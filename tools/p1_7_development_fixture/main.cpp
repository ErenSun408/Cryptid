#include "development_container_fixture.h"

#include "card/vikey/vikey_card_store.h"
#include "card/vikey/vikey_development_app_config.h"
#include "card/vikey/vikey_development_credentials.h"
#include "card/vikey/vikey_helper_blob_backend.h"
#include "usb_vault/application/container_layout_v1.h"
#include "usb_vault/security/kdf_v1.h"
#include "usb_vault/volume_creation/volume_creator.h"

#include <Windows.h>

#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fixture = usb_vault::development;

namespace {

constexpr std::wstring_view kCreationAcknowledgement =
    L"--create-disposable-p1.7-containers";
constexpr wchar_t kPreparingSuffix[] = L".p1-7-preparing";

[[nodiscard]] std::filesystem::path executable_directory() {
  std::vector<wchar_t> path(32768);
  const DWORD length = ::GetModuleFileNameW(
      nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) {
    throw std::system_error(static_cast<int>(::GetLastError()),
                            std::system_category(),
                            "resolve fixture executable path");
  }
  return std::filesystem::path(std::wstring_view(path.data(), length))
      .parent_path();
}

class LocalFixtureRuntime final
    : public fixture::IDevelopmentContainerFixtureRuntime {
 public:
  explicit LocalFixtureRuntime(std::filesystem::path creator_helper)
      : creator_(std::move(creator_helper)) {}

  [[nodiscard]] usb_vault::security::SecureBuffer derive(
      const usb_vault::security::SecureBuffer& password_input,
      const usb_vault::security::SecureBuffer& card_secret,
      const usb_vault::security::KdfV1Salt& salt) override {
    return usb_vault::security::derive_volume_password_v1(
        password_input, card_secret, salt);
  }

  void create_standard(
      const std::wstring_view target_path, const std::uint64_t size_bytes,
      const usb_vault::security::SecureBuffer& volume_password) override {
    creator_.create_standard(target_path, size_bytes, volume_password);
  }

  void create_with_hidden(
      const std::wstring_view target_path,
      const std::uint64_t outer_size_bytes,
      const usb_vault::security::SecureBuffer& outer_password,
      const usb_vault::security::SecureBuffer& hidden_password) override {
    creator_.create_with_hidden(target_path, outer_size_bytes, 0,
                                outer_password, hidden_password);
  }

 private:
  usb_vault::volume_creation::VolumeCreator creator_;
};

}  // namespace

int wmain(const int argc, wchar_t** const argv) {
  try {
    if (argc != 6 || argv[5] == nullptr ||
        std::wstring_view(argv[5]) != kCreationAcknowledgement) {
      throw std::invalid_argument(
          "usage: fixture <helper.exe> <helper-sha256> <vendor-dir> "
          "<vendor-sha256> --create-disposable-p1.7-containers");
    }

    const std::array<std::wstring_view, 6> app_arguments{
        argv[0],
        usb_vault::card::vikey::development::
            kNetworkEnabledDevelopmentAcknowledgement,
        argv[1], argv[2], argv[3], argv[4]};
    auto backend_config =
        usb_vault::card::vikey::development::parse_app_config(app_arguments);

    const std::filesystem::path directory = executable_directory();
    const std::filesystem::path creator_helper =
        directory / L"usb_vault_volume_creator.exe";
    const std::filesystem::path format_backend =
        directory / L"VeraCryptFormat.dll";
    const std::filesystem::path development_app =
        directory / L"usb_vault_vikey_development_app.exe";
    fixture::require_regular_file(creator_helper, "volume creator helper");
    fixture::require_regular_file(format_backend, "format backend");
    fixture::require_regular_file(development_app,
                                  "development application");

    const std::filesystem::path final_a =
        directory / usb_vault::application::kContainerAFilename;
    const std::filesystem::path final_b =
        directory / usb_vault::application::kContainerBFilename;
    const std::filesystem::path stage_a =
        std::filesystem::path(final_a.native() + kPreparingSuffix);
    const std::filesystem::path stage_b =
        std::filesystem::path(final_b.native() + kPreparingSuffix);
    fixture::require_absent(final_a);
    fixture::require_absent(final_b);
    fixture::require_absent(stage_a);
    fixture::require_absent(stage_b);

    fixture::FixtureCleanup cleanup(stage_a, stage_b, final_a, final_b);
    usb_vault::card::vikey::development::WindowsCredentialProvider
        credential_provider;
    usb_vault::card::vikey::VikeyHelperBlobBackend backend(
        std::move(backend_config), credential_provider);
    usb_vault::card::vikey::VikeyCardStore store(backend);
    LocalFixtureRuntime runtime(creator_helper);
    fixture::prepare_matching_development_containers(
        store, runtime, cleanup, stage_a.native(), stage_b.native());
    cleanup.publish_b();
    cleanup.publish_a();
    cleanup.release();

    std::cout << "P1.7 disposable matching containers created\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "P1.7 development container fixture failed: "
              << error.what() << '\n';
    return 1;
  }
}
