#include "card/vikey/vikey_artifact.h"

#include "usb_vault/card/card_error.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace usb_vault::card::vikey {
namespace {

constexpr wchar_t kDllName[] = L"ViKey.dll";
constexpr std::uint64_t kMaximumDllBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::size_t kReadBufferBytes = 64U * 1024U;

[[noreturn]] void fail(const CardError error, const char* const context) {
  throw std::system_error(make_error_code(error), context);
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
  Handle(Handle&& other) noexcept
      : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
  Handle& operator=(Handle&&) = delete;

  [[nodiscard]] HANDLE get() const noexcept { return handle_; }

  [[nodiscard]] HANDLE release() noexcept {
    return std::exchange(handle_, INVALID_HANDLE_VALUE);
  }

 private:
  HANDLE handle_;
};

class Algorithm final {
 public:
  Algorithm() {
    if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(
            &handle_, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
      fail(CardError::backend_unavailable,
           "open SHA-256 provider for ViKey DLL verification");
    }
  }

  ~Algorithm() noexcept {
    if (handle_ != nullptr) {
      static_cast<void>(::BCryptCloseAlgorithmProvider(handle_, 0));
    }
  }

  Algorithm(const Algorithm&) = delete;
  Algorithm& operator=(const Algorithm&) = delete;

  [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return handle_; }

 private:
  BCRYPT_ALG_HANDLE handle_{};
};

class Hash final {
 public:
  Hash(const BCRYPT_ALG_HANDLE algorithm, std::vector<UCHAR>& object) {
    if (!BCRYPT_SUCCESS(::BCryptCreateHash(
            algorithm, &handle_, object.data(),
            static_cast<ULONG>(object.size()), nullptr, 0, 0))) {
      fail(CardError::backend_unavailable,
           "create SHA-256 state for ViKey DLL verification");
    }
  }

  ~Hash() noexcept {
    if (handle_ != nullptr) {
      static_cast<void>(::BCryptDestroyHash(handle_));
    }
  }

  Hash(const Hash&) = delete;
  Hash& operator=(const Hash&) = delete;

  [[nodiscard]] BCRYPT_HASH_HANDLE get() const noexcept { return handle_; }

 private:
  BCRYPT_HASH_HANDLE handle_{};
};

[[nodiscard]] bool ordinal_equal(const std::wstring& left,
                                 const std::wstring& right) noexcept {
  return ::CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) ==
         CSTR_EQUAL;
}

[[nodiscard]] std::filesystem::path normalize_deployment_directory(
    const std::filesystem::path& directory) {
  if (directory.empty() || !directory.is_absolute() ||
      directory.native().find(L'\0') != std::wstring::npos) {
    fail(CardError::invalid_configuration,
         "ViKey deployment directory must be an absolute local path");
  }

  std::filesystem::path normalized = directory.lexically_normal();
  normalized.make_preferred();
  while (normalized != normalized.root_path() &&
         normalized.filename().empty()) {
    normalized = normalized.parent_path();
  }
  const std::filesystem::path root = normalized.root_path();
  if (root.empty() || ::GetDriveTypeW(root.c_str()) != DRIVE_FIXED) {
    fail(CardError::invalid_configuration,
         "ViKey deployment directory must reside on a fixed local drive");
  }
  return normalized;
}

[[nodiscard]] std::wstring final_path(const HANDLE handle) {
  std::wstring value(32768, L'\0');
  const DWORD length = ::GetFinalPathNameByHandleW(
      handle, value.data(), static_cast<DWORD>(value.size()),
      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (length == 0 || length >= value.size()) {
    fail(CardError::backend_unavailable,
         "resolve ViKey DLL deployment path");
  }
  value.resize(length);

  constexpr std::wstring_view extended_prefix = L"\\\\?\\";
  if (value.starts_with(extended_prefix)) {
    value.erase(0, extended_prefix.size());
  }
  return value;
}

void require_final_path(const HANDLE handle,
                        const std::filesystem::path& expected,
                        const char* const context) {
  if (!ordinal_equal(final_path(handle), expected.native())) {
    fail(CardError::backend_unavailable, context);
  }
}

[[nodiscard]] FILE_ATTRIBUTE_TAG_INFO attributes(const HANDLE handle) {
  FILE_ATTRIBUTE_TAG_INFO value{};
  if (::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &value,
                                     sizeof(value)) == FALSE) {
    fail(CardError::backend_unavailable,
         "inspect ViKey DLL deployment attributes");
  }
  return value;
}

[[nodiscard]] Handle lock_directory(
    const std::filesystem::path& directory) {
  Handle handle(::CreateFileW(
      directory.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (handle.get() == INVALID_HANDLE_VALUE) {
    fail(CardError::backend_unavailable,
         "lock ViKey DLL deployment directory");
  }

  const FILE_ATTRIBUTE_TAG_INFO info = attributes(handle.get());
  if ((info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    fail(CardError::backend_unavailable,
         "ViKey DLL deployment directory is not a plain directory");
  }
  require_final_path(handle.get(), directory,
                     "ViKey DLL directory resolves through a redirection");
  return handle;
}

[[nodiscard]] Handle lock_file(const std::filesystem::path& path) {
  Handle handle(::CreateFileW(
      path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
          FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (handle.get() == INVALID_HANDLE_VALUE) {
    fail(CardError::backend_unavailable, "lock ViKey DLL for verification");
  }

  const FILE_ATTRIBUTE_TAG_INFO info = attributes(handle.get());
  if ((info.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
    fail(CardError::backend_unavailable,
         "ViKey DLL is not a plain file");
  }
  require_final_path(handle.get(), path,
                     "ViKey DLL resolves through a redirection");
  return handle;
}

[[nodiscard]] std::uint64_t require_bounded_size(const HANDLE file) {
  FILE_STANDARD_INFO info{};
  if (::GetFileInformationByHandleEx(file, FileStandardInfo, &info,
                                     sizeof(info)) == FALSE ||
      info.Directory != FALSE || info.DeletePending != FALSE ||
      info.EndOfFile.QuadPart <= 0 ||
      static_cast<std::uint64_t>(info.EndOfFile.QuadPart) > kMaximumDllBytes) {
    fail(CardError::backend_unavailable,
         "ViKey DLL size or state is invalid");
  }
  return static_cast<std::uint64_t>(info.EndOfFile.QuadPart);
}

[[nodiscard]] DWORD algorithm_property(const BCRYPT_ALG_HANDLE algorithm,
                                       const wchar_t* const name) {
  DWORD value = 0;
  DWORD returned = 0;
  if (!BCRYPT_SUCCESS(::BCryptGetProperty(
          algorithm, name, reinterpret_cast<PUCHAR>(&value), sizeof(value),
          &returned, 0)) ||
      returned != sizeof(value)) {
    fail(CardError::backend_unavailable,
         "query SHA-256 provider for ViKey DLL verification");
  }
  return value;
}

[[nodiscard]] Sha256Digest hash_file(const HANDLE file,
                                     const std::uint64_t expected_size) {
  Algorithm algorithm;
  const DWORD object_bytes =
      algorithm_property(algorithm.get(), BCRYPT_OBJECT_LENGTH);
  const DWORD digest_bytes =
      algorithm_property(algorithm.get(), BCRYPT_HASH_LENGTH);
  if (object_bytes == 0 ||
      digest_bytes != static_cast<DWORD>(Sha256Digest{}.size())) {
    fail(CardError::backend_unavailable,
         "SHA-256 provider returned invalid dimensions");
  }

  std::vector<UCHAR> object(object_bytes);
  Hash hash(algorithm.get(), object);
  std::vector<std::byte> buffer(kReadBufferBytes);
  std::uint64_t total = 0;
  for (;;) {
    DWORD bytes_read = 0;
    if (::ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                   &bytes_read, nullptr) == FALSE) {
      fail(CardError::backend_unavailable,
           "read ViKey DLL for verification");
    }
    if (bytes_read == 0) {
      break;
    }
    total += bytes_read;
    if (total > expected_size ||
        !BCRYPT_SUCCESS(::BCryptHashData(
            hash.get(), reinterpret_cast<PUCHAR>(buffer.data()), bytes_read,
            0))) {
      fail(CardError::backend_unavailable,
           "hash ViKey DLL for verification");
    }
  }
  if (total != expected_size) {
    fail(CardError::backend_unavailable,
         "ViKey DLL changed during verification");
  }

  Sha256Digest digest{};
  if (!BCRYPT_SUCCESS(::BCryptFinishHash(
          hash.get(), reinterpret_cast<PUCHAR>(digest.data()),
          static_cast<ULONG>(digest.size()), 0))) {
    fail(CardError::backend_unavailable,
         "finish ViKey DLL verification");
  }
  return digest;
}

void close_owned_handle(void*& handle) noexcept {
  if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
    static_cast<void>(::CloseHandle(static_cast<HANDLE>(handle)));
    handle = nullptr;
  }
}

}  // namespace

VerifiedVikeyDll::VerifiedVikeyDll(std::filesystem::path path,
                                   void* const directory_handle,
                                   void* const file_handle) noexcept
    : path_(std::move(path)),
      directory_handle_(directory_handle),
      file_handle_(file_handle) {}

VerifiedVikeyDll::~VerifiedVikeyDll() noexcept {
  close_owned_handle(file_handle_);
  close_owned_handle(directory_handle_);
}

VerifiedVikeyDll::VerifiedVikeyDll(VerifiedVikeyDll&& other) noexcept
    : path_(std::move(other.path_)),
      directory_handle_(std::exchange(other.directory_handle_, nullptr)),
      file_handle_(std::exchange(other.file_handle_, nullptr)) {}

const std::filesystem::path& VerifiedVikeyDll::path() const noexcept {
  return path_;
}

VerifiedVikeyDll verify_vikey_dll(
    const std::filesystem::path& deployment_directory,
    const Sha256Digest& expected_digest) {
  const std::filesystem::path directory =
      normalize_deployment_directory(deployment_directory);
  const std::filesystem::path path = directory / kDllName;
  Handle directory_handle = lock_directory(directory);
  Handle file = lock_file(path);
  const std::uint64_t size = require_bounded_size(file.get());
  const Sha256Digest actual_digest = hash_file(file.get(), size);
  if (!std::equal(actual_digest.begin(), actual_digest.end(),
                  expected_digest.begin(), expected_digest.end())) {
    fail(CardError::backend_unavailable,
         "ViKey DLL digest does not match the deployment whitelist");
  }

  return VerifiedVikeyDll(path, directory_handle.release(), file.release());
}

}  // namespace usb_vault::card::vikey
