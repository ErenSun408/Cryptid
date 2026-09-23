#include "volume_creation/partition_inplace_backend.h"
#include "volume_creation/creator_helper_engine.h"
#include "volume_creation/import_unique_name.h"
#include "volume/partition_catalog_detail.h"
#include "volume/partition_write_policy.h"

#include "usb_vault/volume/unmount_retry.h"
#include "usb_vault/volume/volume_provider.h"
#include "usb_vault/volume/volume_space.h"
#include "usb_vault/volume/fat32_safe_space.h"
#include "usb_vault/volume/fat32_file_transfer.h"
#include "usb_vault/volume/volume_entry_path.h"
#include "volume_creation/veracrypt_format_abi.h"

#include <Windows.h>
#include <winioctl.h>
#include <winternl.h>
#include <bcrypt.h>
#include <cstring>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <mutex>
#include <system_error>
#include <thread>
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

// Only remove access paths of the consented, retained volume. Never change
// the partition table or system-wide automount policy, and never restore an
// access path after a potentially partial encrypted write.
void remove_partition_access_paths(HANDLE handle, const volume::PartitionCandidate& expected, const volume::PartitionLocation* location = nullptr) {
  const auto matches = [&] { return location ? volume::detail::matches_partition_location_handle(handle,*location) : volume::detail::matches_partition_handle_identity(handle,expected); };
  if (!matches())
    fail(CreationError::target_access_denied, "partition changed before access removal");
  std::vector<wchar_t> paths(32768);
  DWORD required{};
  if (!::GetVolumePathNamesForVolumeNameW(expected.volume_path.c_str(), paths.data(),
      static_cast<DWORD>(paths.size()), &required))
    fail(CreationError::target_access_denied, "cannot enumerate partition access paths");
  for (std::size_t offset = 0; offset < paths.size() && paths[offset];) {
    const auto end = std::find(paths.begin() + offset, paths.end(), L'\0');
    if (end == paths.end()) fail(CreationError::target_access_denied, "invalid access path list");
    const std::wstring path(paths.begin() + offset, end);
    wchar_t volume_name[64]{};
    if (!::GetVolumeNameForVolumeMountPointW(path.c_str(), volume_name, 64) ||
        ::CompareStringOrdinal(volume_name, -1, expected.volume_path.c_str(), -1, TRUE) != CSTR_EQUAL ||
        !matches() ||
        !::DeleteVolumeMountPointW(path.c_str()))
      fail(CreationError::target_access_denied, "cannot remove verified partition access path");
    offset += path.size() + 1;
  }
  paths.assign(32768, L'\0');
  if (!::GetVolumePathNamesForVolumeNameW(expected.volume_path.c_str(), paths.data(),
      static_cast<DWORD>(paths.size()), &required) || paths[0])
    fail(CreationError::target_access_denied, "partition access path remains");
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
  void reset(HANDLE value) noexcept {
    if(handle_!=INVALID_HANDLE_VALUE&&handle_!=nullptr)static_cast<void>(::CloseHandle(handle_));
    handle_=value;
  }

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

  security::SecureBuffer geometry(
      const std::array<unsigned char, 512>& outer,
      const std::array<unsigned char, 512>& hidden,
      std::uint64_t host_bytes, const security::SecureBuffer& first,
      const security::SecureBuffer& second) {
    const auto query = symbol<format_abi::ReadHiddenGeometry>("VeraCryptReadHiddenGeometry");
    security::SecureBuffer result(format_abi::kGeometryOutputBytes);
    if (query(outer.data(), 512, hidden.data(), 512,
              reinterpret_cast<const unsigned char*>(first.data()), static_cast<std::uint32_t>(first.size()),
              reinterpret_cast<const unsigned char*>(second.data()), static_cast<std::uint32_t>(second.size()),
              host_bytes, reinterpret_cast<std::uint64_t*>(result.data()),
              format_abi::kGeometryOutputBytes) != format_abi::kSuccess)
      fail(CreationError::format_rejected, "protected geometry unavailable");
    return result;
  }

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

  void require_inplace_exports(bool gpt) {
    if (gpt) static_cast<void>(symbol<format_abi::InplaceOpenGpt>("VeraCryptInplaceOpenGptBinary"));
    static_cast<void>(symbol<format_abi::InplaceOpen>("VeraCryptInplaceOpenBinary"));
    static_cast<void>(symbol<format_abi::InplaceEncrypt>("VeraCryptInplaceEncrypt"));
    static_cast<void>(symbol<format_abi::InplaceSave>("VeraCryptInplaceSave"));
    static_cast<void>(symbol<format_abi::InplaceFinalize>("VeraCryptInplaceFinalize"));
    static_cast<void>(symbol<format_abi::InplaceClose>("VeraCryptInplaceClose"));
  }
  bool convert_partition(HANDLE handle, const PartitionTarget& target, const security::SecureBuffer& password, bool begin) {
    unsigned control = 0, reported = UINT_MAX;
    PartitionInplaceBackend backend(module_.get(),handle,target.offset_bytes,target.length_bytes,password,begin,
        [&] {
          for (auto next=inplace_control_requested(pipe_,nonce_); next; next=inplace_control_requested(pipe_,nonce_))
            control=(std::max)(control,next);
          return control==2 ? volume::detail::InplaceControl::revoked : control==1 ? volume::detail::InplaceControl::pause : volume::detail::InplaceControl::proceed;
        }, [&](const CreationProgress& p) {
          if (p.percent!=reported) { reported=p.percent; write_response(pipe_,Response{.type=MessageType::progress,.stage=CreationStage::formatting,.percent=p.percent}); }
        }, target.partition_id);
    const auto result=volume::detail::run_partition_inplace(backend);
    if (result==volume::detail::InplaceResult::paused) return true;
    if (result==volume::detail::InplaceResult::completed) return false;
    fail(result==volume::detail::InplaceResult::revoked || control==2 ? CreationError::operation_cancelled : CreationError::io_error,"partition conversion incomplete");
  }

  void require_partition_export(bool gpt) {
    if (gpt) partition_gpt_binary_ = symbol<format_abi::FormatPartitionGptBinary>("VeraCryptFormatPartitionGptBinary");
    partition_binary_ = symbol<format_abi::FormatPartitionBinary>("VeraCryptFormatPartitionBinary");
  }
  void format_partition(HANDLE handle, const PartitionTarget& target, const security::SecureBuffer& password) {
    if (!partition_binary_ || (target.layout == 2 && !partition_gpt_binary_)) fail(CreationError::format_backend_unavailable, "partition export unavailable");
    stage_ = CreationStage::formatting;
    allocated_ = true; // Device targets never use file allocation or deletion paths.
    const int code = target.layout == 2 ? partition_gpt_binary_(handle, target.offset_bytes, target.length_bytes,
        reinterpret_cast<const unsigned char*>(password.data()), static_cast<std::uint32_t>(password.size()),
        &FormatBackend::progress, this, target.partition_id.data(), 16) : partition_binary_(handle, target.offset_bytes, target.length_bytes,
        reinterpret_cast<const unsigned char*>(password.data()), static_cast<std::uint32_t>(password.size()),
        &FormatBackend::progress, this);
    if (code == format_abi::kUserAbort) fail(CreationError::operation_cancelled, "partition format cancelled");
    if (code != format_abi::kSuccess) fail(format_abi::map_error(code), "partition format failed");
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
      // the half-written file as the user who asked for it. Partition
      // formatting has no deletion cleanup and keeps its partial result.
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
  format_abi::FormatPartitionBinary partition_binary_{};
  format_abi::FormatPartitionGptBinary partition_gpt_binary_{};
  CreationStage stage_{CreationStage::formatting};
  std::wstring target_path_;
  std::uint64_t expected_size_{};
  bool allocated_{false};
  bool initialized_{false};
};

void validate_request(const Request& request) {
  if (partition_operation(request.operation)) {
    if (!request.partition_target || !valid_partition_target(*request.partition_target) ||
        !valid_partition_path(request.target_path) || request.outer_size_bytes || request.hidden_size_bytes ||
        request.mounted_drive || request.mount_id ||
        request.first_password.size() != (request.operation != Operation::check_partition && request.operation != Operation::inspect_partition_bitlocker ? 64u : 0u) || !request.second_password.empty() ||
        !request.source_path.empty() || !request.destination_relative.empty())
      fail(CreationError::invalid_request, "invalid partition identity request");
    return;
  }
  if (request.partition_target) fail(CreationError::invalid_request, "partition facts in file request");
  if (request.target_path.empty() ||
      !std::filesystem::path(request.target_path).is_absolute()) {
    fail(CreationError::invalid_request, "invalid creation request values");
  }
  if (request.operation == Operation::scan_protected_space || request.operation == Operation::scan_maintenance_space || request.operation == Operation::import_protected_file) {
    if (request.mounted_drive < 1 || request.mounted_drive > 26 || request.mount_id < 0 ||
        request.outer_size_bytes == 0 || request.hidden_size_bytes != 0 ||
        request.first_password.empty() || request.second_password.empty() ||
        request.first_password.size() > kMaxPasswordBytes || request.second_password.size() > kMaxPasswordBytes)
      fail(CreationError::invalid_request, "invalid protected-space request");
    if (request.operation == Operation::import_protected_file) {
      const auto source = std::filesystem::path(request.source_path);
      static_cast<void>(volume::resolve_exported_entry_path(source.parent_path().native(), source.filename().native()));
      const auto destination = volume::resolve_volume_entry_path(static_cast<wchar_t>(L'A' + request.mounted_drive - 1), request.destination_relative);
      if (destination.size() <= 3) fail(CreationError::invalid_request, "missing import destination");
    }
    return;
  }
  if (request.mounted_drive != 0 || request.mount_id != 0)
    fail(CreationError::invalid_request, "unexpected mounted identity");
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

// Neither caller paths nor DOS drive mappings select a privileged raw device.
// Paths are opened while impersonating the caller; the raw device is a fixed
// VeraCrypt namespace derived only from a validated drive number.
template<class Action>
std::uint64_t with_protected_volume(HANDLE pipe, Request& request, Action&& action) {
  const auto drive = static_cast<wchar_t>(L'A' + request.mounted_drive - 1);
  volume::VolumeProvider provider;
  volume::MountIdentity before;
  {
    const Impersonation caller(pipe);
    before = provider.mount_identity(drive);
  }
  if (!before.protected_outer_writable || before.unique_id != request.mount_id ||
      before.disk_length != request.outer_size_bytes)
    fail(CreationError::invalid_request, "protected mount identity changed");

  auto open_file = [&](std::wstring path) {
    if (std::filesystem::path(path).filename().wstring().find(L':' ) != std::wstring::npos)
      fail(CreationError::invalid_request, "container streams are not supported");
    if (path.starts_with(LR"(\??\)")) path.replace(0, 4, LR"(\\?\)");
    const Impersonation caller(pipe);
    const auto h = ::CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE)
      fail(CreationError::target_access_denied, "caller cannot read container");
    return Handle(h);
  };
  const auto target = open_file(request.target_path);
  const auto mounted_file = open_file(before.volume_path);
  auto identity = [](HANDLE h) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (::GetFileType(h) != FILE_TYPE_DISK || !::GetFileInformationByHandle(h, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
      fail(CreationError::invalid_request, "container is not a regular file");
    return info;
  };
  const auto a = identity(target.get()), b = identity(mounted_file.get());
  if (a.dwVolumeSerialNumber != b.dwVolumeSerialNumber ||
      a.nFileIndexHigh != b.nFileIndexHigh || a.nFileIndexLow != b.nFileIndexLow)
    fail(CreationError::invalid_request, "container file identity differs");
  const auto host_bytes = (static_cast<std::uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
  auto header = [&](std::int64_t offset) {
    std::array<unsigned char, 512> data{};
    LARGE_INTEGER pos{}; pos.QuadPart = offset;
    DWORD count = 0;
    if (!::SetFilePointerEx(target.get(), pos, nullptr, FILE_BEGIN) ||
        !::ReadFile(target.get(), data.data(), static_cast<DWORD>(data.size()), &count, nullptr) || count != data.size())
      fail(CreationError::io_error, "cannot read container headers");
    return data;
  };
  const auto outer = header(0), hidden = header(65536);
  // Pinned driver overwrites volumeID after authenticating the protection header.
  // For a protected outer mount it identifies the hidden header, not the outer.
  std::array<unsigned char, 32> hash{};
  if (::BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
        const_cast<PUCHAR>(hidden.data()), static_cast<ULONG>(hidden.size()),
        hash.data(), static_cast<ULONG>(hash.size())) < 0 || hash != before.header_digest)
    fail(CreationError::invalid_request, "mounted header identity differs");
  FormatBackend backend(pipe, request.nonce);
  auto geometry = backend.geometry(outer, hidden, host_bytes,
                                    request.first_password, request.second_password);
  const auto* g = reinterpret_cast<const std::uint64_t*>(geometry.data());
  if (g[0] >= host_bytes || g[1] != before.disk_length || g[1] > host_bytes - g[0] ||
      g[2] <= g[0] || g[2] - g[0] >= g[1] || g[3] == 0 || g[3] > g[1] - (g[2] - g[0]))
    fail(CreationError::invalid_request, "protected geometry does not match mount");
  const auto device = std::wstring(LR"(\\?\GLOBALROOT\Device\VeraCryptVolume)") + drive;
  const Handle raw(::CreateFileW(device.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
  if (raw.get() == INVALID_HANDLE_VALUE)
    fail(CreationError::bitmap_scan_failed, "cannot read protected volume");
  if (header(0) != outer || header(65536) != hidden)
    fail(CreationError::invalid_request, "container headers changed before operation");
  { const Impersonation caller(pipe);
    if (provider.mount_identity(drive) != before)
      fail(CreationError::invalid_request, "mount changed before operation"); }
  const auto value = action(raw.get(), before, g[2] - g[0]);
  // A measurement that overlaps a header update, file replacement or remount
  // is discarded; no previous value is returned on failure.
  if (header(0) != outer || header(65536) != hidden)
    fail(CreationError::invalid_request, "container headers changed during scan");
  {
    const Impersonation caller(pipe);
    if (provider.mount_identity(drive) != before)
      fail(CreationError::invalid_request, "mount changed during scan");
  }
  return value;
}

std::uint64_t scan_protected_space(HANDLE pipe, Request& request) {
  return with_protected_volume(pipe, request, [](HANDLE raw, const volume::MountIdentity& identity, std::uint64_t boundary) {
    return volume::scan_fat32_safe_space(raw, identity.disk_length, boundary).free_bytes;
  });
}

// FILE_CREATE returns ownership atomically; CreateDirectory followed by opening
// a pathname would leave a replacement race before rollback ownership existed.
HANDLE create_directory_handle(HANDLE parent, std::wstring_view name) {
  using Create = NTSTATUS (NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
      PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
  using ToError = ULONG (WINAPI*)(NTSTATUS);
  const auto module = ::GetModuleHandleW(L"ntdll.dll");
  if (!module) fail(CreationError::invalid_request, "directory creation module unavailable");
  const auto create = reinterpret_cast<Create>(::GetProcAddress(module, "NtCreateFile"));
  const auto to_error = reinterpret_cast<ToError>(::GetProcAddress(module, "RtlNtStatusToDosError"));
  if (!create || !to_error || name.empty() || name.size() > 255)
    fail(CreationError::invalid_request, "directory creation unavailable");
  UNICODE_STRING text{};
  text.Buffer=const_cast<PWSTR>(name.data());text.Length=static_cast<USHORT>(name.size()*sizeof(wchar_t));text.MaximumLength=text.Length;
  OBJECT_ATTRIBUTES attributes{};attributes.Length=sizeof(attributes);attributes.RootDirectory=parent;
  attributes.ObjectName=&text;attributes.Attributes=OBJ_CASE_INSENSITIVE;
  IO_STATUS_BLOCK status{};HANDLE result=INVALID_HANDLE_VALUE;
  // FILE_CREATE, FILE_DIRECTORY_FILE, FILE_WRITE_THROUGH, synchronous nonalert.
  const auto code=create(&result,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|DELETE|SYNCHRONIZE,
      &attributes,&status,nullptr,FILE_ATTRIBUTE_DIRECTORY,FILE_SHARE_READ,2,0x1|0x2|0x20,nullptr,0);
  if(code<0)throw std::system_error(static_cast<int>(to_error(code)),std::system_category(),"create owned import directory");
  return result;
}

struct ImportTreeEntry final {
  std::wstring relative;
  std::unique_ptr<Handle> source;
  std::uint64_t bytes{};
  bool directory{};
  std::size_t parent{};
};

std::uint64_t import_tree(HANDLE pipe, Request& request, HANDLE raw,
    const volume::MountIdentity& identity, std::uint64_t boundary,
    const std::function<bool()>& cancelled,
    const std::function<void(std::uint64_t,std::uint64_t)>& progress) {
  const auto space=volume::scan_fat32_safe_space(raw,identity.disk_length,boundary);
  auto destination=volume::resolve_volume_entry_path(identity.drive_letter,request.destination_relative).substr(3);
  const auto root=std::wstring(LR"(\\?\GLOBALROOT\Device\VeraCryptVolume)")+identity.drive_letter+L"\\";
  std::vector<ImportTreeEntry> entries;
  std::uint64_t total=0,allocation=0;
  const auto stop=[&] {if(cancelled())fail(CreationError::operation_cancelled,"tree import cancelled");};
  std::function<void(std::wstring,std::size_t,unsigned)> survey;
  survey=[&](std::wstring relative,std::size_t parent,unsigned depth) {
    stop();
    if(entries.size()>=1024 || depth>32)fail(CreationError::invalid_request,"import tree limit exceeded");
    static_cast<void>(volume::resolve_volume_entry_path(identity.drive_letter,destination+(relative.empty()?L"":L"\\"+relative)));
    const auto path=request.source_path+(relative.empty()?L"":L"\\"+relative);
    std::unique_ptr<Handle> handle;
    {
      const Impersonation caller(pipe);
      handle=std::make_unique<Handle>(::CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
          FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if(handle->get()==INVALID_HANDLE_VALUE || ::GetFileType(handle->get())!=FILE_TYPE_DISK ||
       !::GetFileInformationByHandle(handle->get(),&info) || (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))
      fail(CreationError::target_access_denied,"tree source unavailable or linked");
    const bool directory=(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
    const auto bytes=(static_cast<std::uint64_t>(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
    if(!directory && bytes>UINT32_MAX)fail(CreationError::invalid_request,"tree file exceeds FAT32 limit");
    const auto index=entries.size();
    entries.push_back({relative,std::move(handle),directory?0:bytes,directory,parent});
    if(!directory){total+=bytes;allocation+=(bytes/space.cluster_size+(bytes%space.cluster_size!=0))*space.cluster_size;return;}
    WIN32_FIND_DATAW found{};
    const auto search=[&] {const Impersonation caller(pipe);return ::FindFirstFileW((path+L"\\*").c_str(),&found);}();
    if(search==INVALID_HANDLE_VALUE){if(::GetLastError()==ERROR_FILE_NOT_FOUND)return;fail(CreationError::io_error,"enumerate import source");}
    struct FindClose {HANDLE value;~FindClose(){::FindClose(value);}} closer{search};
    do {
      const std::wstring name=found.cFileName;
      if(name==L"."||name==L"..")continue;
      // Enumerate under caller rights, but recursive opens manage their own
      // impersonation scope; no privileged filesystem action occurs here.
      survey(relative.empty()?name:relative+L"\\"+name,index,depth+1);
    } while(::FindNextFileW(search,&found));
    if(::GetLastError()!=ERROR_NO_MORE_FILES)fail(CreationError::io_error,"finish import enumeration");
  };
  survey(L"",0,0);
  if(allocation>space.free_bytes)fail(CreationError::insufficient_tail_space,"insufficient safe space for import tree");
  std::vector<std::unique_ptr<Handle>> pinned;
  std::vector<std::unique_ptr<Handle>> directories(entries.size());
  std::vector<std::unique_ptr<volume::Fat32TransferTarget>> files;
  files.reserve(entries.size());
  const auto guard=[&] {stop();volume::VolumeProvider provider;
    if(provider.mount_identity(identity.drive_letter)!=identity)fail(CreationError::invalid_request,"tree mount changed");};
  {
    const Impersonation caller(pipe);
    const auto pin=[&](std::wstring path){auto h=std::make_unique<Handle>(::CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
      BY_HANDLE_FILE_INFORMATION info{};
      if(h->get()==INVALID_HANDLE_VALUE||!::GetFileInformationByHandle(h->get(),&info)||!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))
        fail(CreationError::target_access_denied,"tree parent unavailable");
      pinned.push_back(std::move(h));};
    pin(root);
    for(auto at=destination.find(L'\\');at!=std::wstring::npos;at=destination.find(L'\\',at+1))pin(root+destination.substr(0,at));
  }
  std::uint64_t copied=0;
  try {
    for(std::size_t i=0;i<entries.size();++i) {
      guard();const auto& entry=entries[i];
      const auto relative=destination+(entry.relative.empty()?L"":L"\\"+entry.relative);
      if(entry.directory) {
        const auto name=std::filesystem::path(relative).filename().native();
        const Impersonation caller(pipe);
        directories[i]=std::make_unique<Handle>();
        if (i == 0) {
          const auto prefix = destination.substr(0, destination.size() - name.size());
          directories[i]->reset(detail::create_unique_import(name, true, [&](const std::wstring& candidate) {
            guard();
            // Allocate the path before acquiring ownership of a new directory.
            auto selected = prefix + candidate;
            const auto handle = create_directory_handle(pinned.back()->get(), candidate);
            destination.swap(selected);
            return handle;
          }));
        } else directories[i]->reset(create_directory_handle(directories[entry.parent]->get(),name));
        guard();
      } else {
        const auto plan=volume::plan_fat32_allocation(raw,identity.disk_length,boundary,entry.bytes);
        {const Impersonation caller(pipe);files.push_back(std::make_unique<volume::Fat32TransferTarget>(root+relative));}
        const auto result=volume::transfer_fat32_file(entry.source->get(),*files.back(),identity,boundary,plan,
            cancelled,[&](std::uint64_t done){progress(copied+done,total);},true);
        if(result.failure)std::rethrow_exception(result.failure);
        copied+=entry.bytes;
      }
    }
    guard();
    const auto device=std::wstring(LR"(\\?\GLOBALROOT\Device\VeraCryptVolume)")+identity.drive_letter;
    const Handle flush(::CreateFileW(device.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr));
    if(flush.get()==INVALID_HANDLE_VALUE||!::FlushFileBuffers(flush.get()))
      throw std::system_error(static_cast<int>(::GetLastError()),std::system_category(),"flush imported tree");
    guard();
  } catch(...) {
    const auto original=std::current_exception();
    for(auto& file:files)static_cast<void>(file->discard());
    files.clear();
    for(auto i=directories.size();i>0;--i)if(directories[i-1]) {
      FILE_DISPOSITION_INFO remove{TRUE};
      static_cast<void>(::SetFileInformationByHandle(directories[i-1]->get(),FileDispositionInfo,&remove,sizeof(remove)));
      directories[i-1].reset();
    }
    std::rethrow_exception(original);
  }
  return total;
}

std::uint64_t import_protected(HANDLE pipe, Request& request) {
  return with_protected_volume(pipe, request, [&](HANDLE raw, const volume::MountIdentity& identity, std::uint64_t boundary) -> std::uint64_t {
    std::uint32_t percent = 0;
    auto reported = std::chrono::steady_clock::time_point{};
    const auto cancelled = [&] {
      const auto now = std::chrono::steady_clock::now();
      if (now - reported >= std::chrono::milliseconds(100)) {
        write_response(pipe, Response{.type=MessageType::progress, .stage=CreationStage::copying, .percent=percent});
        reported = now;
      }
      return cancel_requested(pipe, request.nonce);
    };
    if (cancelled()) fail(CreationError::operation_cancelled, "protected import cancelled");
    const auto source = [&] {
      const Impersonation caller(pipe);
      return Handle(::CreateFileW(request.source_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
          OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    }();
    BY_HANDLE_FILE_INFORMATION info{};
    if (source.get() == INVALID_HANDLE_VALUE || ::GetFileType(source.get()) != FILE_TYPE_DISK ||
        !::GetFileInformationByHandle(source.get(), &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
      fail(CreationError::target_access_denied, "caller source is not a readable regular entry");
    if(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      try {
        return import_tree(pipe,request,raw,identity,boundary,cancelled,[&](std::uint64_t done,std::uint64_t total) {
          const auto next=total?static_cast<std::uint32_t>(std::min<std::uint64_t>(99,done*100/total)):99;
          if(next!=percent) {
            percent=next;
            write_response(pipe,Response{.type=MessageType::progress,.stage=CreationStage::copying,.percent=percent});
            reported=std::chrono::steady_clock::now();
          }
        });
      } catch(const std::system_error& error) {
        if(error.code()==std::make_error_code(std::errc::operation_canceled))fail(CreationError::operation_cancelled,"tree import cancelled; recovery required");
        throw;
      }
    }
    const auto size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    const auto plan = volume::plan_fat32_allocation(raw, identity.disk_length, boundary, size);
    // FAT32 geometry has been validated before creating any filesystem entry.
    // Pin every existing parent, reject links, and use the fixed driver device.
    const auto relative = volume::resolve_volume_entry_path(identity.drive_letter, request.destination_relative).substr(3);
    const auto root = std::wstring(LR"(\\?\GLOBALROOT\Device\VeraCryptVolume)") + identity.drive_letter + L"\\";
    std::vector<std::unique_ptr<Handle>> parents;
    std::unique_ptr<volume::Fat32TransferTarget> target;
    {
      const Impersonation caller(pipe);
      const auto pin = [&](const std::wstring& path) {
        auto parent = std::make_unique<Handle>(::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION attributes{};
        if (parent->get() == INVALID_HANDLE_VALUE || !::GetFileInformationByHandle(parent->get(), &attributes) ||
            !(attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
          fail(CreationError::target_access_denied, "caller import parent is unavailable");
        parents.push_back(std::move(parent));
      };
      pin(root);
      for (std::size_t at = relative.find(L'\\'); at != std::wstring::npos; at = relative.find(L'\\', at + 1))
        pin(root + relative.substr(0, at));
      volume::VolumeProvider provider;
      if (provider.mount_identity(identity.drive_letter) != identity)
        fail(CreationError::invalid_request, "mount changed before target creation");
      const auto name = std::filesystem::path(relative).filename().native();
      const auto prefix = root + relative.substr(0, relative.size() - name.size());
      target = detail::create_unique_import(name, false, [&](const std::wstring& candidate) {
        if (cancelled()) fail(CreationError::operation_cancelled, "import cancelled before creation");
        if (provider.mount_identity(identity.drive_letter) != identity)
          fail(CreationError::invalid_request, "mount changed during target naming");
        return std::make_unique<volume::Fat32TransferTarget>(prefix + candidate);
      });
    }
    const auto result = volume::transfer_fat32_file(source.get(), *target, identity, boundary, plan,
        cancelled, [&](std::uint64_t done) {
          const auto next = size ? static_cast<std::uint32_t>(std::min<std::uint64_t>(99, done * 100 / size)) : 99;
          if (next != percent) {
            percent = next;
            write_response(pipe, Response{.type=MessageType::progress, .stage=CreationStage::copying, .percent=percent});
            reported = std::chrono::steady_clock::now();
          }
        });
    if (result.failure) {
      // Preserve the initiating failure; never turn accepted deletion into an
      // assurance of durable cleanup. The caller must stop this mount's queue.
      try { std::rethrow_exception(result.failure); }
      catch (const std::system_error& error) {
        if (error.code() == std::make_error_code(std::errc::operation_canceled))
          fail(CreationError::operation_cancelled, "protected import cancelled; recovery required");
        throw;
      }
    }
    return size;
  });
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

static void serve_session(const HANDLE pipe,
                               const std::array<std::byte, kNonceSize>& nonce,
                               Request& first, bool importing, bool maintenance = false) {
  // Idle time is the only thing that ends this session other than the caller
  // closing the pipe, and something has to end it if the caller never does.
  std::atomic<std::int64_t> last_activity{
      std::chrono::steady_clock::now().time_since_epoch().count()};
  std::atomic<bool> idle_wait{false};
  bool finished = false;
  std::mutex finished_mutex;
  std::condition_variable finished_signal;
  std::jthread watchdog([&](std::stop_token stop) {
    constexpr auto kIdleLimit = std::chrono::minutes(15);
    std::unique_lock lock(finished_mutex);
    while (!finished && !stop.stop_requested()) {
      finished_signal.wait_for(lock, std::chrono::seconds(5));
      if (finished) break;
      const std::chrono::steady_clock::duration idle{
          std::chrono::steady_clock::now().time_since_epoch().count() -
          last_activity.load()};
      // Nothing here is worth flushing, and the caller reads a closed pipe as
      // the end of the session either way.
      if (idle_wait.load() && idle > kIdleLimit) ::ExitProcess(0);
    }
  });

  const auto bound_path = first.target_path;
  const auto bound_drive = first.mounted_drive;
  const auto bound_id = first.mount_id;
  const auto bound_bytes = first.outer_size_bytes;
  Request request = std::move(first);
  for (;;) {
    last_activity.store(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const bool allowed = maintenance
        ? matches_maintenance_binding(request, bound_path, bound_drive, bound_id, bound_bytes)
        : importing
            ? request.operation == Operation::import_protected_file &&
                request.target_path == bound_path && request.mounted_drive == bound_drive &&
                request.mount_id == bound_id && request.outer_size_bytes == bound_bytes
            : request.operation == Operation::scan_tail_space || request.operation == Operation::scan_protected_space;
    if (!allowed) {
      // A session that measures never creates, however the request is
      // labelled: one consent covered measuring and nothing else.
      write_response(pipe,
                     Response{.type = MessageType::result,
                              .error = static_cast<std::int32_t>(
                                  CreationError::invalid_request)});
      break;
    }
    Response response{.type = MessageType::result};
    try {
      validate_request(request);
      response.value = request.operation == Operation::import_protected_file ? import_protected(pipe, request) : (request.operation == Operation::scan_protected_space || request.operation == Operation::scan_maintenance_space)
          ? scan_protected_space(pipe, request) : scan_tail_space(request.target_path[0]).bytes;
    } catch (const std::system_error& error) {
      response.error = error.code().category() == creation_error_category()
                           ? error.code().value()
                           : static_cast<std::int32_t>(CreationError::io_error);
    } catch (...) {
      response.error = static_cast<std::int32_t>(CreationError::io_error);
    }
    request.first_password.clear();
    request.second_password.clear();
    write_response(pipe, response);
    if (importing && response.error != 0) break;
    try {
      last_activity.store(std::chrono::steady_clock::now().time_since_epoch().count());
      idle_wait.store(true);
      request = read_request(pipe, nonce);
      idle_wait.store(false);
    } catch (...) {
      // The caller closed the pipe, which is how a session ends.
      break;
    }
  }
  {
    std::lock_guard lock(finished_mutex);
    finished = true;
  }
  finished_signal.notify_all();
  watchdog.join();
}

void serve_measurement_session(HANDLE pipe, const std::array<std::byte, kNonceSize>& nonce, Request& first) {
  serve_session(pipe, nonce, first, false);
}
void serve_import_session(HANDLE pipe, const std::array<std::byte, kNonceSize>& nonce, Request& first) {
  serve_session(pipe, nonce, first, true);
}

void serve_maintenance_session(HANDLE pipe, const std::array<std::byte, kNonceSize>& nonce, Request& first) {
  serve_session(pipe, nonce, first, true, true);
}

std::uint64_t execute_request(const HANDLE pipe, Request& request) {
  validate_request(request);
  if (partition_operation(request.operation)) {
    const auto& target = *request.partition_target;
    volume::PartitionCandidate expected{};
    expected.volume_path = request.target_path;
    expected.disk_number = target.disk_number;
    expected.partition_number = target.partition_number;
    expected.offset_bytes = target.offset_bytes;
    expected.length_bytes = target.length_bytes;
    expected.logical_sector_bytes = target.sector_bytes;
    expected.layout = static_cast<volume::PartitionLayout>(target.layout);
    expected.partition_id = target.partition_id;
    expected.filesystem_serial = target.filesystem_serial;
    expected.device_id = target.device_id;
    expected.device_id_flags = target.device_flags;
    expected.availability = volume::PartitionAvailability::candidate;
    const bool resume = request.operation == Operation::resume_partition_inplace;
    if (resume) {
      const auto native=L"\\Device\\Harddisk"+std::to_wstring(target.disk_number)+L"\\Partition"+std::to_wstring(target.partition_number);
      for(const auto& mounted: volume::VolumeProvider{}.mounted_volumes())
        if (::CompareStringOrdinal(mounted.volume_path.c_str(),-1,native.c_str(),-1,TRUE)==CSTR_EQUAL)
          fail(CreationError::target_access_denied,"resume target already mounted");
    }
    const bool converting = inplace_operation(request.operation);
    const volume::PartitionLocation location{{target.device_id,target.device_flags,target.layout,target.sector_bytes,target.offset_bytes,target.length_bytes,target.partition_id},request.target_path,target.disk_number,target.partition_number};
    // Independent enumeration excludes the current Windows volume before any
    // target handle is opened. Passing enumeration alone grants no write authorization.
    if (resume ? volume::resolve_partition_location(location.identity,volume::enumerate_partition_locations()) != location : !volume::matches_partition_snapshot(expected, volume::enumerate_partition_candidates()))
      fail(CreationError::target_access_denied, "partition snapshot no longer matches");
    std::wstring path = request.target_path;
    path.pop_back(); // Only the validated volume GUID name reaches CreateFile.
    const bool formatting = request.operation != Operation::check_partition && request.operation != Operation::inspect_partition_bitlocker;
    const Handle handle(::CreateFileW(path.c_str(), GENERIC_READ | (formatting ? GENERIC_WRITE : 0), FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE || (resume ? !volume::detail::matches_partition_location_handle(handle.get(),location) : !volume::detail::matches_partition_handle_identity(handle.get(), expected)))
      fail(CreationError::target_access_denied, "partition handle identity mismatch");
    if (request.operation == Operation::inspect_partition_bitlocker) {
      const auto state = volume::detail::inspect_partition_bitlocker(handle.get(), expected);
      return (static_cast<std::uint64_t>(state.state) << 8) | state.encrypted_percent;
    }
    const auto policy = [&] { return resume ? volume::detail::check_partition_resume_policy(handle.get(),location) : volume::detail::check_partition_write_policy(handle.get(),expected); };
    if (!policy())
      fail(CreationError::target_access_denied, "partition write policy rejected");
    if (formatting) {
      FormatBackend backend(pipe, request.nonce);
      if (converting) backend.require_inplace_exports(target.layout == 2); else backend.require_partition_export(target.layout == 2);
      write_response(pipe, Response{.type = MessageType::progress, .stage = CreationStage::partition_ready});
      if (!read_partition_confirmation(pipe, request.nonce))
        fail(CreationError::operation_cancelled, "partition erase not confirmed");
      // Confirmation can take time. Re-run every policy gate on the retained
      // handle; the DLL then locks, rechecks geometry and formats that handle.
      if (!policy())
        fail(CreationError::target_access_denied, "partition changed after confirmation");
      if (!converting && cancel_requested(pipe, request.nonce))
        fail(CreationError::operation_cancelled, "partition format cancelled before write");
      remove_partition_access_paths(handle.get(), expected, resume ? &location : nullptr);
      if (!converting && cancel_requested(pipe, request.nonce))
        fail(CreationError::operation_cancelled, "partition cancelled after access removal");
      if (converting) return backend.convert_partition(handle.get(),target,request.first_password, !resume) ? 1u : 0u;
      backend.format_partition(handle.get(), target, request.first_password);
    }
    return 0; // No target deletion, implicit retry or retained consent.
  }
  if (request.operation == Operation::scan_tail_space) {
    // Nothing is created, so the format backend is never loaded and there is
    // no progress to report: the number is the whole exchange.
    return scan_tail_space(request.target_path[0]).bytes;
  }
  if (request.operation == Operation::scan_protected_space || request.operation == Operation::scan_maintenance_space)
    return scan_protected_space(pipe, request);
  if (request.operation == Operation::import_protected_file)
    return import_protected(pipe, request);
  FormatBackend backend(pipe, request.nonce);
  if (request.operation == Operation::create_standard) {
    create_standard(pipe, request, backend);
  } else {
    create_with_hidden(pipe, request, backend);
  }
  return 0;
}

}  // namespace usb_vault::volume_creation::detail
