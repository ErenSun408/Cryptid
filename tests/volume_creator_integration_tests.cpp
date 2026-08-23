#include "usb_vault/security/secure_buffer.h"
#include "usb_vault/volume/unmount_retry.h"
#include "usb_vault/volume/volume_provider.h"
#include "usb_vault/volume_creation/volume_creator.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using usb_vault::security::SecureBuffer;
using usb_vault::volume::MountOptions;
using usb_vault::volume::Pkcs5Prf;
using usb_vault::volume::VolumeProvider;
using usb_vault::volume::VolumeType;
using usb_vault::volume_creation::CreationProgress;
using usb_vault::volume_creation::CreationStage;
using usb_vault::volume_creation::VolumeCreator;

constexpr int kSkip = 77;

[[noreturn]] void fail(const char* const message) {
  throw std::runtime_error(message);
}

void require(const bool condition, const char* const message) {
  if (!condition) fail(message);
}

[[nodiscard]] SecureBuffer binary_password() {
  SecureBuffer result(64);
  for (std::size_t index = 0; index < result.size(); ++index) {
    result.bytes()[index] = static_cast<std::byte>((index * 37U) & 0xffU);
  }
  require(result.bytes()[0] == std::byte{0},
          "binary password fixture must contain an embedded NUL");
  return result;
}

[[nodiscard]] SecureBuffer text_password(const std::string_view text) {
  SecureBuffer result(text.size());
  std::memcpy(result.data(), text.data(), text.size());
  return result;
}

[[nodiscard]] wchar_t free_drive_letter() {
  const DWORD used = ::GetLogicalDrives();
  for (wchar_t letter = L'Z'; letter >= L'D'; --letter) {
    const unsigned index = static_cast<unsigned>(letter - L'A');
    if ((used & (DWORD{1} << index)) == 0) return letter;
  }
  fail("no free drive letter for integration test");
}

class FixtureCleanup final {
 public:
  FixtureCleanup(std::filesystem::path first, std::filesystem::path second)
      : first_(std::move(first)), second_(std::move(second)) {}
  ~FixtureCleanup() noexcept {
    try {
      VolumeProvider provider;
      for (const auto& mounted : provider.mounted_volumes()) {
        if (mounted.volume_path == first_.native() ||
            mounted.volume_path == second_.native()) {
          try {
            static_cast<void>(provider.unmount(
                mounted.drive_letter,
                {.force = true, .notify_shell = false}));
          } catch (...) {
          }
        }
      }
    } catch (...) {
    }
    static_cast<void>(::DeleteFileW(first_.c_str()));
    static_cast<void>(::DeleteFileW(second_.c_str()));
  }

 private:
  std::filesystem::path first_;
  std::filesystem::path second_;
};

void mount_and_expect(const std::filesystem::path& path,
                      const SecureBuffer& password, const VolumeType type) {
  VolumeProvider provider;
  const wchar_t drive = free_drive_letter();
  static_cast<void>(provider.mount_file(
      path.native(), password,
      MountOptions{.drive_letter = drive,
                   .prf = Pkcs5Prf::sha512,
                   .pim = 0,
                   .read_only = false,
                   .notify_shell = false}));
  try {
    const auto volumes = provider.mounted_volumes();
    const auto found = std::ranges::find_if(
        volumes, [drive](const auto& volume) {
          return volume.drive_letter == drive;
        });
    require(found != volumes.end(), "newly created volume was not listed");
    require(found->type == type, "newly created volume reported wrong type");
    static_cast<void>(usb_vault::volume::unmount_with_retry(
        provider, drive, {.notify_shell = false}));
  } catch (...) {
    try {
      static_cast<void>(provider.unmount(
          drive, {.force = true, .notify_shell = false}));
    } catch (...) {
    }
    throw;
  }
}

class EnvironmentValue final {
 public:
  EnvironmentValue(const wchar_t* name, const wchar_t* value) : name_(name) {
    const DWORD required = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (required != 0) {
      previous_.resize(required);
      const DWORD copied =
          ::GetEnvironmentVariableW(name, previous_.data(), required);
      previous_.resize(copied);
      had_previous_ = true;
    }
    if (::SetEnvironmentVariableW(name, value) == FALSE) {
      throw std::system_error(static_cast<int>(::GetLastError()),
                              std::system_category(), "set test environment");
    }
  }
  ~EnvironmentValue() noexcept {
    static_cast<void>(::SetEnvironmentVariableW(
        name_.c_str(), had_previous_ ? previous_.c_str() : nullptr));
  }
  EnvironmentValue(const EnvironmentValue&) = delete;
  EnvironmentValue& operator=(const EnvironmentValue&) = delete;

 private:
  std::wstring name_;
  std::wstring previous_;
  bool had_previous_{false};
};

void mount_outer_protected_and_expect(
    const std::filesystem::path& vcspike,
    const std::filesystem::path& container) {
  const wchar_t drive = free_drive_letter();
  const EnvironmentValue outer(L"VCSPIKE_PASSWORD", L"OuterPass123456");
  const EnvironmentValue hidden(L"VCSPIKE_HIDDEN_PASSWORD",
                                L"HiddenPass123456");
  const EnvironmentValue prf(L"VCSPIKE_PRF", L"1");
  std::wstring command = L"\"" + vcspike.native() + L"\" mount-outer \"" +
                         container.native() + L"\" " + drive;
  STARTUPINFOW startup{.cb = sizeof(STARTUPINFOW)};
  PROCESS_INFORMATION process{};
  if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                       &process) == FALSE) {
    throw std::system_error(static_cast<int>(::GetLastError()),
                            std::system_category(), "launch vcspike");
  }
  static_cast<void>(::CloseHandle(process.hThread));
  static_cast<void>(::WaitForSingleObject(process.hProcess, 30000));
  DWORD exit_code = 1;
  static_cast<void>(::GetExitCodeProcess(process.hProcess, &exit_code));
  static_cast<void>(::CloseHandle(process.hProcess));
  require(exit_code == 0, "protected outer-volume mount failed");

  VolumeProvider provider;
  try {
    const auto volumes = provider.mounted_volumes();
    const auto found = std::ranges::find_if(
        volumes, [drive](const auto& volume) {
          return volume.drive_letter == drive;
        });
    require(found != volumes.end(), "protected outer volume was not listed");
    require(found->type == VolumeType::outer,
            "protected outer volume did not report type=2");
    static_cast<void>(usb_vault::volume::unmount_with_retry(
        provider, drive, {.notify_shell = false}));
  } catch (...) {
    try {
      static_cast<void>(provider.unmount(
          drive, {.force = true, .notify_shell = false}));
    } catch (...) {
    }
    throw;
  }
}

}  // namespace

int wmain(const int argc, wchar_t** const argv) {
  if (argc != 5) {
    std::cerr << "usage: volume_creator_integration_tests "
                 "<helper> <A> <B> <vcspike>\n";
    return 2;
  }
  const std::filesystem::path helper =
      std::filesystem::absolute(argv[1]).lexically_normal();
  const std::filesystem::path container_a =
      std::filesystem::absolute(argv[2]).lexically_normal();
  const std::filesystem::path container_b =
      std::filesystem::absolute(argv[3]).lexically_normal();
  const std::filesystem::path vcspike =
      std::filesystem::absolute(argv[4]).lexically_normal();
  if (!std::filesystem::exists(helper) ||
      !std::filesystem::exists(helper.parent_path() / L"VeraCryptFormat.dll") ||
      !std::filesystem::exists(vcspike)) {
    std::cout << "SKIP: helper, patched FormatDLL, or vcspike is missing\n";
    return kSkip;
  }

  static_cast<void>(::DeleteFileW(container_a.c_str()));
  static_cast<void>(::DeleteFileW(container_b.c_str()));
  FixtureCleanup cleanup(container_a, container_b);
  try {
    VolumeProvider provider;
    static_cast<void>(provider.driver_version());

    const SecureBuffer password_a = binary_password();
    const SecureBuffer outer_password = text_password("OuterPass123456");
    const SecureBuffer hidden_password = text_password("HiddenPass123456");
    VolumeCreator creator(helper);

    std::vector<CreationProgress> standard_progress;
    creator.create_standard(
        container_a.native(), 20ULL * 1024ULL * 1024ULL, password_a,
        [&](const CreationProgress& value) { standard_progress.push_back(value); });
    require(!standard_progress.empty(), "standard creation sent no progress");
    mount_and_expect(container_a, password_a, VolumeType::normal);

    std::vector<CreationProgress> hidden_progress;
    creator.create_with_hidden(
        container_b.native(), 100ULL * 1024ULL * 1024ULL, 0,
        outer_password, hidden_password,
        [&](const CreationProgress& value) { hidden_progress.push_back(value); });
    require(std::ranges::any_of(hidden_progress, [](const auto& value) {
              return value.stage == CreationStage::scanning_tail_space;
            }),
            "hidden creation did not report a bitmap scan");
    mount_and_expect(container_b, hidden_password, VolumeType::hidden);
    mount_and_expect(container_b, outer_password, VolumeType::normal);
    mount_outer_protected_and_expect(vcspike, container_b);

    std::cout << "volume_creator_integration_tests passed: type=0/1/2\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "volume_creator_integration_tests failed: " << error.what()
              << '\n';
    return 1;
  }
}
