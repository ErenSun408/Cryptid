#include "card/diagnostic_error.h"
#include "card/verified_local_artifact.h"

#include "usb_vault/card/card_error.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace usb_vault::card {
namespace {

constexpr std::size_t kReadBufferBytes = 64U * 1024U;

[[noreturn]] void fail(const CardError error, const char* const context,
    const DiagnosticStage stage = DiagnosticStage::none) {
  throw DiagnosticError(make_error_code(error), stage, context);
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
      fail(CardError::backend_unavailable, "open SHA-256 provider for local artifact verification", DiagnosticStage::file_hash);
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
      fail(CardError::backend_unavailable, "create SHA-256 state for local artifact verification", DiagnosticStage::file_hash);
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

[[nodiscard]] std::filesystem::path normalized_local_file(
    const std::filesystem::path& input) {
  if (input.empty() || !input.is_absolute() ||
      input.native().find(L'\0') != std::wstring::npos) {
    fail(CardError::invalid_configuration, "local artifact path must be absolute", DiagnosticStage::path_check);
  }

  std::filesystem::path path = input.lexically_normal();
  path.make_preferred();
  const std::wstring native = path.native();
  if (path.filename().empty() || native.find(L':', 2) != std::wstring::npos) {
    fail(CardError::invalid_configuration, "local artifact path must name a plain file", DiagnosticStage::path_check);
  }
  const std::filesystem::path root = path.root_path();
  if (root.empty() || ::GetDriveTypeW(root.c_str()) != DRIVE_FIXED) {
    fail(CardError::invalid_configuration, "local artifact must reside on a fixed local drive", DiagnosticStage::path_check);
  }
  return path;
}

[[nodiscard]] std::wstring final_path(const HANDLE handle) {
  std::wstring value(32768, L'\0');
  const DWORD length = ::GetFinalPathNameByHandleW(
      handle, value.data(), static_cast<DWORD>(value.size()),
      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (length == 0 || length >= value.size()) {
    fail(CardError::backend_unavailable, "resolve local artifact path", DiagnosticStage::path_check);
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
    fail(CardError::backend_unavailable, context, DiagnosticStage::path_check);
  }
}

[[nodiscard]] FILE_ATTRIBUTE_TAG_INFO attributes(const HANDLE handle) {
  FILE_ATTRIBUTE_TAG_INFO value{};
  if (::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &value,
                                     sizeof(value)) == FALSE) {
    fail(CardError::backend_unavailable, "inspect local artifact attributes", DiagnosticStage::file_state);
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
    throw DiagnosticError(make_error_code(CardError::backend_unavailable), DiagnosticStage::file_open, "lock local artifact directory", ::GetLastError(), NativeCodeKind::win32);
  }

  const FILE_ATTRIBUTE_TAG_INFO info = attributes(handle.get());
  if ((info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    fail(CardError::backend_unavailable, "local artifact directory is not a plain directory", DiagnosticStage::path_check);
  }
  require_final_path(handle.get(), directory,
                     "local artifact directory resolves through a redirection");
  return handle;
}

[[nodiscard]] Handle lock_file(const std::filesystem::path& path) {
  Handle handle(::CreateFileW(
      path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
          FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (handle.get() == INVALID_HANDLE_VALUE) {
    fail(CardError::backend_unavailable, "lock local artifact for verification", DiagnosticStage::file_open);
  }

  const FILE_ATTRIBUTE_TAG_INFO info = attributes(handle.get());
  if ((info.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
    fail(CardError::backend_unavailable, "local artifact is not a plain file", DiagnosticStage::path_check);
  }
  require_final_path(handle.get(), path,
                     "local artifact resolves through a redirection");
  return handle;
}

[[nodiscard]] std::uint64_t require_bounded_size(
    const HANDLE file, const std::uint64_t maximum_bytes) {
  FILE_STANDARD_INFO info{};
  if (maximum_bytes == 0 ||
      ::GetFileInformationByHandleEx(file, FileStandardInfo, &info,
                                     sizeof(info)) == FALSE ||
      info.Directory != FALSE || info.DeletePending != FALSE ||
      info.EndOfFile.QuadPart <= 0 ||
      static_cast<std::uint64_t>(info.EndOfFile.QuadPart) > maximum_bytes) {
    fail(CardError::backend_unavailable, "local artifact size or state is invalid", DiagnosticStage::file_state);
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
    fail(CardError::backend_unavailable, "query SHA-256 provider for local artifact verification", DiagnosticStage::file_hash);
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
    fail(CardError::backend_unavailable, "SHA-256 provider returned invalid dimensions", DiagnosticStage::file_hash);
  }

  std::vector<UCHAR> object(object_bytes);
  Hash hash(algorithm.get(), object);
  std::vector<std::byte> buffer(kReadBufferBytes);
  std::uint64_t total = 0;
  for (;;) {
    DWORD bytes_read = 0;
    if (::ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                   &bytes_read, nullptr) == FALSE) {
      fail(CardError::backend_unavailable, "read local artifact for verification", DiagnosticStage::file_state);
    }
    if (bytes_read == 0) break;
    total += bytes_read;
    if (total > expected_size ||
        !BCRYPT_SUCCESS(::BCryptHashData(
            hash.get(), reinterpret_cast<PUCHAR>(buffer.data()), bytes_read,
            0))) {
      fail(CardError::backend_unavailable, "hash local artifact for verification", DiagnosticStage::file_state);
    }
  }
  if (total != expected_size) {
    fail(CardError::backend_unavailable, "local artifact changed during verification", DiagnosticStage::file_state);
  }

  Sha256Digest digest{};
  if (!BCRYPT_SUCCESS(::BCryptFinishHash(
          hash.get(), reinterpret_cast<PUCHAR>(digest.data()),
          static_cast<ULONG>(digest.size()), 0))) {
    fail(CardError::backend_unavailable, "finish local artifact verification", DiagnosticStage::file_state);
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

VerifiedLocalArtifact::VerifiedLocalArtifact(
    std::filesystem::path path, void* const directory_handle,
    void* const file_handle) noexcept
    : path_(std::move(path)),
      directory_handle_(directory_handle),
      file_handle_(file_handle) {}

VerifiedLocalArtifact::~VerifiedLocalArtifact() noexcept {
  close_owned_handle(file_handle_);
  close_owned_handle(directory_handle_);
}

VerifiedLocalArtifact::VerifiedLocalArtifact(
    VerifiedLocalArtifact&& other) noexcept
    : path_(std::move(other.path_)),
      directory_handle_(std::exchange(other.directory_handle_, nullptr)),
      file_handle_(std::exchange(other.file_handle_, nullptr)) {}

const std::filesystem::path& VerifiedLocalArtifact::path() const noexcept {
  return path_;
}

VerifiedLocalArtifact verify_local_artifact(
    const std::filesystem::path& input,
    const Sha256Digest& expected_digest,
    const std::uint64_t maximum_bytes) {
  const std::filesystem::path path = normalized_local_file(input);
  Handle directory = lock_directory(path.parent_path());
  Handle file = lock_file(path);
  const std::uint64_t size = require_bounded_size(file.get(), maximum_bytes);
  const Sha256Digest actual_digest = hash_file(file.get(), size);
  if (!std::equal(actual_digest.begin(), actual_digest.end(),
                  expected_digest.begin(), expected_digest.end())) {
    fail(CardError::backend_unavailable, "local artifact digest does not match the deployment whitelist", DiagnosticStage::file_hash);
  }

  return VerifiedLocalArtifact(path, directory.release(), file.release());
}

}  // namespace usb_vault::card
