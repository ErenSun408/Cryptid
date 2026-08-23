#include "usb_vault/application/mount_orchestrator.h"

#include "application/mount_orchestrator_internal.h"
#include "usb_vault/volume/unmount_retry.h"
#include "volume/mount_request.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace usb_vault::application {
namespace {

constexpr std::array kContainerAPasswordInput{
    std::byte{'d'}, std::byte{'i'}, std::byte{'s'}, std::byte{'k'},
    std::byte{'A'}};

[[nodiscard]] std::wstring comparable_path(const std::wstring_view path) {
  std::wstring result =
      std::filesystem::path(path).lexically_normal().native();
  std::ranges::replace(result, L'/', L'\\');
  return result;
}

void validate_layout(const ContainerLayout& layout) {
  volume::detail::validate_volume_path(layout.container_a.path);
  volume::detail::validate_volume_path(layout.container_b.path);

  const std::int32_t drive_a =
      volume::detail::drive_number(layout.container_a.drive_letter);
  const std::int32_t drive_b =
      volume::detail::drive_number(layout.container_b.drive_letter);
  if (drive_a == drive_b) {
    throw std::invalid_argument("container drive letters must be distinct");
  }

  const std::wstring path_a = comparable_path(layout.container_a.path);
  const std::wstring path_b = comparable_path(layout.container_b.path);
  const int comparison = ::CompareStringOrdinal(
      path_a.data(), static_cast<int>(path_a.size()), path_b.data(),
      static_cast<int>(path_b.size()), TRUE);
  if (comparison == 0) {
    throw std::system_error(static_cast<int>(::GetLastError()),
                            std::system_category(),
                            "compare container paths");
  }
  if (comparison == CSTR_EQUAL) {
    throw std::invalid_argument("container paths must be distinct");
  }
}

[[nodiscard]] security::SecureBuffer container_a_password_input() {
  security::SecureBuffer input(kContainerAPasswordInput.size());
  std::ranges::copy(kContainerAPasswordInput, input.bytes().begin());
  return input;
}

[[nodiscard]] volume::MountResult derive_and_mount(
    detail::MountRuntime& runtime,
    const ContainerMount& container,
    const security::SecureBuffer& password_input,
    const security::SecureBuffer& card_secret) {
  security::SecureBuffer volume_password =
      runtime.derive(password_input, card_secret, container.salt);
  try {
    volume::MountResult result = runtime.mount(
        container.path, volume_password,
        {.drive_letter = container.drive_letter,
         .prf = volume::Pkcs5Prf::sha512,
         .pim = 0,
         .read_only = false,
         .notify_shell = false});
    volume_password.clear();
    return result;
  } catch (...) {
    volume_password.clear();
    throw;
  }
}

[[noreturn]] void rollback_container_b(
    detail::MountRuntime& runtime,
    const wchar_t drive_letter,
    const std::exception_ptr& operation_failure) {
  try {
    static_cast<void>(runtime.rollback_normal(drive_letter));
  } catch (...) {
    try {
      static_cast<void>(runtime.rollback_force(drive_letter));
    } catch (...) {
      throw MountRollbackError(drive_letter, operation_failure,
                               std::current_exception());
    }
  }
  std::rethrow_exception(operation_failure);
}

class ProductionMountRuntime final : public detail::MountRuntime {
 public:
  explicit ProductionMountRuntime(const volume::VolumeProvider& provider)
      : provider_(provider) {}

  [[nodiscard]] bool container_exists(
      const std::wstring_view path) override {
    volume::detail::validate_volume_path(path);
    const std::wstring owned_path(path);
    const DWORD attributes = ::GetFileAttributesW(owned_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const DWORD error = ::GetLastError();
      if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return false;
      }
      throw std::system_error(static_cast<int>(error), std::system_category(),
                              "inspect container path");
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      throw std::invalid_argument("container path refers to a directory");
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      throw std::invalid_argument("container path must not be a reparse point");
    }
    return true;
  }

  [[nodiscard]] security::SecureBuffer derive(
      const security::SecureBuffer& password_utf8,
      const security::SecureBuffer& card_secret,
      const security::KdfV1Salt& salt) override {
    return security::derive_volume_password_v1(password_utf8, card_secret,
                                                salt);
  }

  [[nodiscard]] volume::MountResult mount(
      const std::wstring_view path,
      const security::SecureBuffer& volume_password,
      const volume::MountOptions& options) override {
    return provider_.mount_file(path, volume_password, options);
  }

  [[nodiscard]] volume::UnmountResult rollback_normal(
      const wchar_t drive_letter) override {
    return volume::unmount_with_retry(
        provider_, drive_letter, {.notify_shell = false});
  }

  [[nodiscard]] volume::UnmountResult rollback_force(
      const wchar_t drive_letter) override {
    return provider_.unmount(
        drive_letter, {.force = true, .notify_shell = false});
  }

  void notify_mounted(const wchar_t drive_letter) override {
    provider_.notify_mounted(drive_letter);
  }

 private:
  const volume::VolumeProvider& provider_;
};

}  // namespace

MountRollbackError::MountRollbackError(
    const wchar_t residual_drive_letter,
    std::exception_ptr operation_failure,
    std::exception_ptr cleanup_failure)
    : std::runtime_error(
          "mount transaction failed and rollback left a mounted drive"),
      residual_drive_letter_(residual_drive_letter),
      operation_failure_(std::move(operation_failure)),
      cleanup_failure_(std::move(cleanup_failure)) {}

wchar_t MountRollbackError::residual_drive_letter() const noexcept {
  return residual_drive_letter_;
}

const std::exception_ptr& MountRollbackError::operation_failure() const noexcept {
  return operation_failure_;
}

const std::exception_ptr& MountRollbackError::cleanup_failure() const noexcept {
  return cleanup_failure_;
}

MountSessionResult mount_session(
    const volume::VolumeProvider& provider,
    const ContainerLayout& layout,
    const security::SecureBuffer& password_utf8,
    const security::SecureBuffer& card_secret) {
  ProductionMountRuntime runtime(provider);
  return detail::mount_session_with_runtime(runtime, layout, password_utf8,
                                            card_secret);
}

namespace detail {

MountSessionResult mount_session_with_runtime(
    MountRuntime& runtime,
    const ContainerLayout& layout,
    const security::SecureBuffer& password_utf8,
    const security::SecureBuffer& card_secret) {
  validate_layout(layout);
  const auto started = std::chrono::steady_clock::now();
  MountSessionResult result;

  if (runtime.container_exists(layout.container_b.path)) {
    result.container_b = derive_and_mount(
        runtime, layout.container_b, password_utf8, card_secret);

    try {
      if (runtime.container_exists(layout.container_a.path)) {
        security::SecureBuffer fixed_input = container_a_password_input();
        result.container_a = derive_and_mount(
            runtime, layout.container_a, fixed_input, card_secret);
        fixed_input.clear();
      }
    } catch (...) {
      rollback_container_b(runtime, layout.container_b.drive_letter,
                           std::current_exception());
    }

    if (result.container_a.has_value()) {
      runtime.notify_mounted(layout.container_a.drive_letter);
    }
    runtime.notify_mounted(layout.container_b.drive_letter);
  } else if (runtime.container_exists(layout.container_a.path)) {
    result.container_a = derive_and_mount(
        runtime, layout.container_a, password_utf8, card_secret);
    runtime.notify_mounted(layout.container_a.drive_letter);
  } else {
    throw std::system_error(
        std::make_error_code(std::errc::no_such_file_or_directory),
        "no container files are available");
  }

  result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  return result;
}

}  // namespace detail
}  // namespace usb_vault::application
