#include "volume_creation/creator_helper_engine.h"

#include "usb_vault/volume/unmount_retry.h"
#include "usb_vault/volume/volume_provider.h"
#include "usb_vault/volume/volume_space.h"
#include "volume_creation/veracrypt_format_abi.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace usb_vault::volume_creation::detail {
namespace {

constexpr std::uint64_t kSectorSize = 512;
constexpr std::uint64_t kVolumeHeaderSize = 64ULL * 1024ULL;
constexpr std::uint64_t kTotalVolumeHeadersSize = 4ULL * kVolumeHeaderSize;
constexpr std::uint64_t kHiddenHostReservedEndBytes = 4096;
constexpr std::uint64_t kMaxReserveBytes = 10ULL * 1024ULL * 1024ULL;
constexpr DWORD kBitmapBufferBytes = 1U << 20U;

class Failure final : public std::system_error {
 public:
  Failure(const CreationError error, const char* const context)
      : std::system_error(make_error_code(error), context) {}
};

[[noreturn]] void fail(const CreationError error, const char* const context) {
  throw Failure(error, context);
}

class Handle final {
 public:
  explicit Handle(const HANDLE handle = INVALID_HANDLE_VALUE) noexcept
      : handle_(handle) {}
  ~Handle() noexcept {
    if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
      static_cast<void>(::CloseHandle(handle_));
    }
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  [[nodiscard]] HANDLE get() const noexcept { return handle_; }

 private:
  HANDLE handle_;
};

class Impersonation final {
 public:
  explicit Impersonation(const HANDLE pipe) {
    if (::ImpersonateNamedPipeClient(pipe) == FALSE) {
      throw std::system_error(static_cast<int>(::GetLastError()),
                              std::system_category(),
                              "impersonate volume-creation caller");
    }
  }
  ~Impersonation() noexcept { static_cast<void>(::RevertToSelf()); }
  Impersonation(const Impersonation&) = delete;
  Impersonation& operator=(const Impersonation&) = delete;
};

class Module final {
 public:
  explicit Module(const HMODULE module) noexcept : module_(module) {}
  ~Module() noexcept {
    if (module_ != nullptr) {
      static_cast<void>(::FreeLibrary(module_));
    }
  }
  Module(const Module&) = delete;
  Module& operator=(const Module&) = delete;
  [[nodiscard]] HMODULE get() const noexcept { return module_; }

 private:
  HMODULE module_;
};

[[nodiscard]] std::filesystem::path executable_directory() {
  std::wstring buffer(32768, L'\0');
  const DWORD length = ::GetModuleFileNameW(
      nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) {
    fail(CreationError::format_backend_unavailable,
         "locate volume-creation helper");
  }
  buffer.resize(length);
  return std::filesystem::path(buffer).parent_path();
}

class FormatBackend final {
 public:
  FormatBackend(const HANDLE pipe,
                const std::array<std::byte, kNonceSize>& nonce)
      : pipe_(pipe),
        nonce_(nonce),
        module_(::LoadLibraryExW(
            (executable_directory() / L"VeraCryptFormat.dll").c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)) {
    if (module_.get() == nullptr) {
      fail(CreationError::format_backend_unavailable,
           "load patched VeraCrypt FormatDLL");
    }
    initialize_ = symbol<format_abi::Initialize>("VeraCryptFormat_Initialize");
    shutdown_ = symbol<format_abi::Shutdown>("VeraCryptFormat_Shutdown");
    format_binary_ =
        symbol<format_abi::FormatBinary>("VeraCryptFormatBinary");
    const int result = initialize_();
    if (result != format_abi::kSuccess) {
      fail(CreationError::format_initialization_failed,
           "initialize VeraCrypt FormatDLL");
    }
    initialized_ = true;
  }

  ~FormatBackend() noexcept {
    if (initialized_) {
      shutdown_();
    }
  }

  FormatBackend(const FormatBackend&) = delete;
  FormatBackend& operator=(const FormatBackend&) = delete;

  void format(const std::wstring& path, const std::uint64_t size,
              const security::SecureBuffer& password,
              const bool hidden_volume,
              const std::uint64_t hidden_volume_host_size,
              const CreationStage stage) {
    stage_ = stage;
    target_path_ = path;
    expected_size_ = hidden_volume ? hidden_volume_host_size : size;
    format_abi::Options options{
        .path = path.c_str(),
        .is_device = FALSE,
        .password = reinterpret_cast<const char*>(password.data()),
        .keyfiles = nullptr,
        .pim = 0,
        .size = size,
        .encryption_algorithm = L"AES",
        .hash_algorithm = L"SHA-512",
        .filesystem = L"FAT",
        .cluster_size = 0,
        .quick_format = TRUE,
        .dynamic_format = FALSE,
        .fast_create_file = FALSE,
        .progress_callback = &FormatBackend::progress,
        .progress_user_data = this,
        .hidden_volume = hidden_volume ? TRUE : FALSE,
        .hidden_volume_host_size = hidden_volume_host_size,
    };
    const int result = format_binary_(
        &options, static_cast<std::uint32_t>(password.size()));
    if (result == format_abi::kUserAbort) {
      fail(CreationError::operation_cancelled,
           "VeraCrypt format callback aborted");
    }
    if (result != format_abi::kSuccess) {
      fail(format_abi::map_error(result), "VeraCrypt rejected volume creation");
    }
  }

 private:
  template <typename Function>
  [[nodiscard]] Function symbol(const char* const name) {
    const FARPROC address = ::GetProcAddress(module_.get(), name);
    if (address == nullptr) {
      fail(CreationError::format_backend_unavailable,
           "patched VeraCrypt FormatDLL export is missing");
    }
    return reinterpret_cast<Function>(address);
  }

  static BOOL CALLBACK progress(const int percent, void* const user_data) {
    auto& self = *static_cast<FormatBackend*>(user_data);
    try {
      // Asked first, and on every tick: answering FALSE here is the only way
      // to stop the format from inside, and it leaves the unwinding to the
      // caller below, which still holds a live pipe and can therefore delete
      // the half-written container as the user who asked for it.
      if (cancel_requested(self.pipe_, self.nonce_)) {
        return FALSE;
      }
      if (!self.allocated_) {
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (::GetFileAttributesExW(self.target_path_.c_str(), GetFileExInfoStandard, &attributes)) {
          const std::uint64_t length = (static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
          if (length >= self.expected_size_) {
            write_response(self.pipe_, Response{.type = MessageType::progress,
                .stage = CreationStage::allocated, .percent = 100});
            self.allocated_ = true;
          }
        }
      }
      write_response(self.pipe_, Response{
                                     .type = MessageType::progress,
                                     .stage = self.stage_,
                                     .percent = static_cast<std::uint32_t>(
                                         std::clamp(percent, 0, 100)),
                                 });
      return TRUE;
    } catch (...) {
      return FALSE;
    }
  }

  HANDLE pipe_;
  std::array<std::byte, kNonceSize> nonce_{};
  Module module_;
  format_abi::Initialize initialize_{};
  format_abi::Shutdown shutdown_{};
  format_abi::FormatBinary format_binary_{};
  CreationStage stage_{CreationStage::formatting};
  std::wstring target_path_;
  std::uint64_t expected_size_{};
  bool allocated_{false};
  bool initialized_{false};
};

void validate_request(const Request& request) {
  if (request.target_path.empty() ||
      !std::filesystem::path(request.target_path).is_absolute()) {
    fail(CreationError::invalid_request, "invalid creation request values");
  }
  if (request.operation == Operation::scan_tail_space) {
    // A volume root and nothing else: a request carrying a size or a password
    // is not a measurement, whatever it calls itself.
    if (request.target_path.size() != 3 || request.target_path[1] != L':' ||
        request.target_path[2] != L'\\' || request.target_path[0] < L'A' ||
        request.target_path[0] > L'Z' || request.outer_size_bytes != 0 ||
        request.hidden_size_bytes != 0 || !request.first_password.empty() ||
        !request.second_password.empty()) {
      fail(CreationError::invalid_request, "invalid tail-space request values");
    }
    return;
  }
  if (request.outer_size_bytes < kTotalVolumeHeadersSize ||
      request.outer_size_bytes % kSectorSize != 0 ||
      request.first_password.empty() ||
      request.first_password.size() > kMaxPasswordBytes ||
      request.second_password.size() > kMaxPasswordBytes) {
    fail(CreationError::invalid_request, "invalid creation request values");
  }
  if (request.operation == Operation::create_standard) {
    if (!request.second_password.empty() || request.hidden_size_bytes != 0) {
      fail(CreationError::invalid_request,
           "standard request contains hidden-volume fields");
    }
  } else if (request.operation == Operation::create_with_hidden) {
    if (request.second_password.empty() ||
        request.hidden_size_bytes % kSectorSize != 0) {
      fail(CreationError::invalid_request,
           "invalid hidden-volume request values");
    }
  } else {
    fail(CreationError::invalid_request, "unknown creation operation");
  }
}

void create_new_target_as_caller(const HANDLE pipe,
                                 const std::wstring& path) {
  const Impersonation caller(pipe);
  const Handle target(::CreateFileW(
      path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
      nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (target.get() == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
      fail(CreationError::target_exists, "target container already exists");
    }
    if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION) {
      fail(CreationError::target_access_denied,
           "caller cannot create target container");
    }
    throw std::system_error(static_cast<int>(error), std::system_category(),
                            "create target container as caller");
  }
}

[[nodiscard]] bool delete_target_as_caller(const HANDLE pipe,
                                           const std::wstring& path) noexcept {
  try {
    const Impersonation caller(pipe);
    if (::DeleteFileW(path.c_str()) != FALSE) {
      return true;
    }
    const DWORD error = ::GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
  } catch (...) {
    return false;
  }
}

template <typename Action>
void as_caller(const HANDLE pipe, Action&& action) {
  const Impersonation caller(pipe);
  std::forward<Action>(action)();
}

[[nodiscard]] wchar_t choose_temporary_drive(
    const volume::VolumeProvider& provider) {
  std::uint32_t used = ::GetLogicalDrives();
  for (const volume::MountedVolume& mounted : provider.mounted_volumes()) {
    const unsigned index =
        static_cast<unsigned>(mounted.drive_letter - L'A');
    if (index < 26) {
      used |= std::uint32_t{1} << index;
    }
  }
  for (wchar_t letter = L'Z'; letter >= L'D'; --letter) {
    const auto index = static_cast<unsigned>(letter - L'A');
    if ((used & (std::uint32_t{1} << index)) == 0) {
      return letter;
    }
  }
  fail(CreationError::mount_failed, "no temporary drive letter is available");
}

[[nodiscard]] volume::TailSpace scan_tail_space(const wchar_t drive_letter) {
  try {
    return volume::scan_volume_tail_space(drive_letter);
  } catch (const std::exception&) {
    fail(CreationError::bitmap_scan_failed, "scan outer-volume free tail");
  }
}

[[nodiscard]] std::uint64_t safe_hidden_size(
    const volume::TailSpace& tail_space, const std::uint64_t host_size,
    const std::uint64_t requested_size) {
  const std::uint64_t reserve =
      std::min<std::uint64_t>(host_size / 200ULL, kMaxReserveBytes);
  if (tail_space.bytes <= kHiddenHostReservedEndBytes + reserve) {
    fail(CreationError::insufficient_tail_space,
         "outer volume has no safe tail space");
  }
  std::uint64_t maximum =
      tail_space.bytes - kHiddenHostReservedEndBytes - reserve;
  maximum -= maximum % tail_space.cluster_size;
  if (maximum <= kTotalVolumeHeadersSize) {
    fail(CreationError::insufficient_tail_space,
         "safe hidden-volume size is too small");
  }
  if (requested_size == 0) {
    return maximum;
  }
  if (requested_size > maximum ||
      requested_size % tail_space.cluster_size != 0) {
    fail(CreationError::insufficient_tail_space,
         "requested hidden-volume size exceeds safe tail space");
  }
  return requested_size;
}

void send_stage(const HANDLE pipe, const CreationStage stage,
                const std::uint32_t percent) {
  write_response(pipe, Response{.type = MessageType::progress,
                                .stage = stage,
                                .percent = percent});
}

void create_standard(const HANDLE pipe, Request& request,
                     FormatBackend& backend) {
  create_new_target_as_caller(pipe, request.target_path);
  try {
    as_caller(pipe, [&] {
      backend.format(request.target_path, request.outer_size_bytes,
                     request.first_password, false, 0,
                     CreationStage::formatting);
    });
  } catch (...) {
    if (!delete_target_as_caller(pipe, request.target_path)) {
      fail(CreationError::cleanup_failed,
           "failed to remove incomplete standard container");
    }
    throw;
  }
}

void create_with_hidden(const HANDLE pipe, Request& request,
                        FormatBackend& backend) {
  create_new_target_as_caller(pipe, request.target_path);
  bool mounted = false;
  wchar_t drive_letter = L'\0';
  volume::VolumeProvider provider;
  try {
    as_caller(pipe, [&] {
      backend.format(request.target_path, request.outer_size_bytes,
                     request.first_password, false, 0,
                     CreationStage::formatting_outer);
    });

    drive_letter = choose_temporary_drive(provider);
    as_caller(pipe, [&] {
      static_cast<void>(provider.mount_file(
          request.target_path, request.first_password,
          {.drive_letter = drive_letter,
           .prf = volume::Pkcs5Prf::sha512,
           .pim = 0,
           .read_only = false,
           .notify_shell = false}));
    });
    mounted = true;
    send_stage(pipe, CreationStage::scanning_tail_space, 0);
    const volume::TailSpace tail = scan_tail_space(drive_letter);
    send_stage(pipe, CreationStage::scanning_tail_space, 100);
    const std::uint64_t hidden_size = safe_hidden_size(
        tail, request.outer_size_bytes, request.hidden_size_bytes);

    try {
      static_cast<void>(volume::unmount_with_retry(
          provider, drive_letter, {.notify_shell = false}));
    } catch (...) {
      static_cast<void>(provider.unmount(
          drive_letter, {.force = true, .notify_shell = false}));
    }
    mounted = false;

    as_caller(pipe, [&] {
      backend.format(request.target_path, hidden_size,
                     request.second_password, true,
                     request.outer_size_bytes,
                     CreationStage::formatting_hidden);
    });
  } catch (...) {
    if (mounted) {
      try {
        static_cast<void>(provider.unmount(
            drive_letter, {.force = true, .notify_shell = false}));
        mounted = false;
      } catch (...) {
        throw Failure(CreationError::cleanup_failed,
                      "failed to remove temporary outer-volume mount");
      }
    }
    if (!delete_target_as_caller(pipe, request.target_path)) {
      fail(CreationError::cleanup_failed,
           "failed to remove incomplete hidden container");
    }
    throw;
  }
}

}  // namespace

std::uint64_t execute_request(const HANDLE pipe, Request& request) {
  validate_request(request);
  if (request.operation == Operation::scan_tail_space) {
    // Nothing is created, so the format backend is never loaded and there is
    // no progress to report: the number is the whole exchange.
    return scan_tail_space(request.target_path[0]).bytes;
  }
  FormatBackend backend(pipe, request.nonce);
  if (request.operation == Operation::create_standard) {
    create_standard(pipe, request, backend);
  } else {
    create_with_hidden(pipe, request, backend);
  }
  return 0;
}

}  // namespace usb_vault::volume_creation::detail
