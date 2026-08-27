#include "development_container_fixture.h"

#include "usb_vault/application/container_layout_v1.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using usb_vault::development::IDevelopmentContainerFixtureRuntime;
using usb_vault::security::SecureBuffer;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    std::array<wchar_t, MAX_PATH + 1> temp_root{};
    const DWORD length = ::GetTempPathW(
        static_cast<DWORD>(temp_root.size()), temp_root.data());
    if (length == 0 || length >= temp_root.size()) {
      throw std::runtime_error("resolve temporary directory for fixture test");
    }

    std::array<wchar_t, MAX_PATH + 1> unique{};
    if (::GetTempFileNameW(temp_root.data(), L"uvf", 0, unique.data()) == 0) {
      throw std::runtime_error("reserve unique fixture test path");
    }
    if (::DeleteFileW(unique.data()) == FALSE) {
      throw std::runtime_error("release unique fixture test path");
    }

    root_ = std::wstring(unique.data()) + L".dir";
    std::filesystem::create_directories(root_);
  }

  ~TemporaryDirectory() noexcept {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  [[nodiscard]] std::filesystem::path child(
      const std::wstring_view name) const {
    return root_ / name;
  }

 private:
  std::filesystem::path root_;
};

void write_placeholder(const std::filesystem::path& path) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.put('x');
  if (!output) throw std::runtime_error("write fixture test placeholder");
}

[[nodiscard]] bool path_exists(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::exists(path, error) && !error;
}

template <typename Operation>
void require_throws(Operation&& operation, const char* const message) {
  try {
    operation();
  } catch (const std::exception&) {
    return;
  }
  throw std::runtime_error(message);
}

class FakeCardStore final : public usb_vault::card::ICardStore {
 public:
  explicit FakeCardStore(const std::size_t blob_size = 32)
      : blob_size_(blob_size) {}

  [[nodiscard]] SecureBuffer get_blob() override {
    ++get_calls;
    SecureBuffer result(blob_size_);
    for (std::size_t index = 0; index < result.size(); ++index) {
      result.bytes()[index] = static_cast<std::byte>(index);
    }
    return result;
  }

  void put_blob(const SecureBuffer&) override { ++put_calls; }

  [[nodiscard]] usb_vault::card::CardCapabilities capabilities()
      const noexcept override {
    return {};
  }

  int get_calls{};
  int put_calls{};

 private:
  std::size_t blob_size_{};
};

class ProbeRuntime final : public IDevelopmentContainerFixtureRuntime {
 public:
  [[nodiscard]] SecureBuffer derive(
      const SecureBuffer& password_input,
      const SecureBuffer& card_secret,
      const usb_vault::security::KdfV1Salt& salt) override {
    require(card_secret.size() == 32, "fixture changed card blob size");
    for (std::size_t index = 0; index < card_secret.size(); ++index) {
      require(card_secret.bytes()[index] == static_cast<std::byte>(index),
              "fixture changed card blob bytes");
    }
    inputs.emplace_back(
        reinterpret_cast<const char*>(password_input.data()),
        password_input.size());
    salts.push_back(salt);

    SecureBuffer result(usb_vault::security::kKdfV1OutputSize);
    std::ranges::fill(result.bytes(),
                      static_cast<std::byte>(0xA0U + derive_calls));
    ++derive_calls;
    return result;
  }

  void create_standard(const std::wstring_view target_path,
                       const std::uint64_t size_bytes,
                       const SecureBuffer& volume_password) override {
    paths.emplace_back(target_path);
    require(size_bytes ==
                usb_vault::development::kP17ContainerABytes,
            "fixture changed container A size");
    require_password(volume_password, std::byte{0xA0});
    calls.emplace_back("standard");
    if (fail_standard) {
      throw std::runtime_error("injected container A creation failure");
    }
  }

  void create_with_hidden(
      const std::wstring_view target_path,
      const std::uint64_t outer_size_bytes,
      const SecureBuffer& outer_password,
      const SecureBuffer& hidden_password) override {
    paths.emplace_back(target_path);
    require(outer_size_bytes ==
                usb_vault::development::kP17ContainerBBytes,
            "fixture changed container B size");
    require_password(outer_password, std::byte{0xA1});
    require_password(hidden_password, std::byte{0xA2});
    calls.emplace_back("hidden");
  }

  bool fail_standard{false};
  int derive_calls{};
  std::vector<std::string> inputs;
  std::vector<usb_vault::security::KdfV1Salt> salts;
  std::vector<std::string> calls;
  std::vector<std::wstring> paths;

 private:
  static void require_password(const SecureBuffer& password,
                               const std::byte expected) {
    require(password.size() == usb_vault::security::kKdfV1OutputSize,
            "fixture changed derived password size");
    require(std::ranges::all_of(password.bytes(),
                                [expected](const std::byte value) {
                                  return value == expected;
                                }),
            "fixture routed the wrong derived password");
  }
};

void test_fixture_reads_once_and_derives_matching_layout() {
  const TemporaryDirectory directory;
  const std::filesystem::path stage_a = directory.child(L"a.preparing");
  const std::filesystem::path stage_b = directory.child(L"b.preparing");
  FakeCardStore card;
  ProbeRuntime runtime;
  usb_vault::development::FixtureCleanup cleanup(
      stage_a, stage_b, directory.child(L"container_a.hc"),
      directory.child(L"container_b.hc"));
  usb_vault::development::prepare_matching_development_containers(
      card, runtime, cleanup, stage_a.native(), stage_b.native());

  require(runtime.paths ==
              std::vector<std::wstring>{stage_a.native(), stage_b.native()},
          "fixture created containers at unexpected paths");

  require(card.get_calls == 1 && card.put_calls == 0,
          "fixture did not remain a single read-only card operation");
  require(runtime.inputs ==
              std::vector<std::string>{
                  "diskA",
                  std::string(
                      usb_vault::development::kP17DuressTestPassword),
                  std::string(usb_vault::development::kP17RealTestPassword)},
          "fixture derived passwords from the wrong inputs or order");
  require(runtime.salts.size() == 3 &&
              runtime.salts[0] ==
                  usb_vault::application::kContainerASalt &&
              runtime.salts[1] ==
                  usb_vault::application::kContainerBSalt &&
              runtime.salts[2] ==
                  usb_vault::application::kContainerBSalt,
          "fixture used salts that do not match the application layout");
  require(runtime.calls == std::vector<std::string>{"standard", "hidden"},
          "fixture creation order changed");
}

void test_fixture_rejects_wrong_card_blob_size_before_derivation() {
  const TemporaryDirectory directory;
  FakeCardStore card(31);
  ProbeRuntime runtime;
  usb_vault::development::FixtureCleanup cleanup(
      directory.child(L"a.preparing"), directory.child(L"b.preparing"),
      directory.child(L"container_a.hc"), directory.child(L"container_b.hc"));
  try {
    usb_vault::development::prepare_matching_development_containers(
        card, runtime, cleanup, directory.child(L"a.preparing").native(),
        directory.child(L"b.preparing").native());
  } catch (const std::invalid_argument&) {
    require(card.get_calls == 1 && card.put_calls == 0,
            "invalid blob path wrote the card");
    require(runtime.derive_calls == 0 && runtime.calls.empty(),
            "invalid blob reached derivation or file creation");
    return;
  }
  throw std::runtime_error("fixture accepted an invalid card blob size");
}

void test_partially_written_container_is_removed_when_creation_fails() {
  const TemporaryDirectory directory;
  const std::filesystem::path stage_a = directory.child(L"a.preparing");
  const std::filesystem::path stage_b = directory.child(L"b.preparing");
  FakeCardStore card;
  ProbeRuntime runtime;
  runtime.fail_standard = true;
  // Stands in for a container the creation helper had already started
  // writing when it failed.
  write_placeholder(stage_a);

  {
    usb_vault::development::FixtureCleanup cleanup(
        stage_a, stage_b, directory.child(L"container_a.hc"),
        directory.child(L"container_b.hc"));
    require_throws(
        [&] {
          usb_vault::development::prepare_matching_development_containers(
              card, runtime, cleanup, stage_a.native(), stage_b.native());
        },
        "fixture hid a container creation failure");
    require(runtime.calls == std::vector<std::string>{"standard"},
            "fixture continued after a container creation failure");
  }
  require(!path_exists(stage_a),
          "a partially written container survived the failure");
}

void test_require_absent_rejects_any_existing_entry() {
  const TemporaryDirectory directory;
  const std::filesystem::path missing = directory.child(L"container_a.hc");
  usb_vault::development::require_absent(missing);

  write_placeholder(missing);
  require_throws([&] { usb_vault::development::require_absent(missing); },
                 "require_absent accepted an existing file");

  const std::filesystem::path occupied_directory =
      directory.child(L"container_b.hc");
  std::filesystem::create_directory(occupied_directory);
  require_throws(
      [&] { usb_vault::development::require_absent(occupied_directory); },
      "require_absent accepted an existing directory");
}

void test_require_regular_file_rejects_missing_and_directory() {
  const TemporaryDirectory directory;
  const std::filesystem::path present = directory.child(L"helper.exe");
  write_placeholder(present);
  usb_vault::development::require_regular_file(present, "helper");

  require_throws(
      [&] {
        usb_vault::development::require_regular_file(
            directory.child(L"absent.exe"), "helper");
      },
      "require_regular_file accepted a missing artifact");

  const std::filesystem::path nested = directory.child(L"nested");
  std::filesystem::create_directory(nested);
  require_throws(
      [&] {
        usb_vault::development::require_regular_file(nested, "helper");
      },
      "require_regular_file accepted a directory");
}

void test_cleanup_removes_only_claimed_staging_files() {
  const TemporaryDirectory directory;
  const std::filesystem::path stage_a = directory.child(L"a.preparing");
  const std::filesystem::path stage_b = directory.child(L"b.preparing");
  const std::filesystem::path final_a = directory.child(L"container_a.hc");
  const std::filesystem::path final_b = directory.child(L"container_b.hc");
  write_placeholder(stage_a);
  write_placeholder(stage_b);

  {
    usb_vault::development::FixtureCleanup cleanup(stage_a, stage_b, final_a,
                                                   final_b);
    cleanup.own_stage_a();
  }
  require(!path_exists(stage_a), "cleanup kept a claimed staging container");
  require(path_exists(stage_b),
          "cleanup removed an unclaimed staging container");
  require(!path_exists(final_a) && !path_exists(final_b),
          "cleanup published a container it never staged");
}

void test_failed_second_publication_rolls_back_the_first() {
  const TemporaryDirectory directory;
  const std::filesystem::path stage_a = directory.child(L"a.preparing");
  const std::filesystem::path stage_b = directory.child(L"b.preparing");
  const std::filesystem::path final_a = directory.child(L"container_a.hc");
  const std::filesystem::path final_b = directory.child(L"container_b.hc");
  write_placeholder(stage_b);

  {
    usb_vault::development::FixtureCleanup cleanup(stage_a, stage_b, final_a,
                                                   final_b);
    cleanup.own_stage_a();
    cleanup.own_stage_b();
    cleanup.publish_b();
    require(path_exists(final_b),
            "publication did not move the staged container");
    // Container A was never written, so publishing it must fail and the
    // already published container B must not survive the failure.
    require_throws([&] { cleanup.publish_a(); },
                   "publication accepted a missing staging container");
  }
  require(!path_exists(final_a) && !path_exists(final_b),
          "a half-published layout survived the failure");
  require(!path_exists(stage_a) && !path_exists(stage_b),
          "a staging container survived the failure");
}

void test_release_keeps_a_fully_published_layout() {
  const TemporaryDirectory directory;
  const std::filesystem::path stage_a = directory.child(L"a.preparing");
  const std::filesystem::path stage_b = directory.child(L"b.preparing");
  const std::filesystem::path final_a = directory.child(L"container_a.hc");
  const std::filesystem::path final_b = directory.child(L"container_b.hc");
  write_placeholder(stage_a);
  write_placeholder(stage_b);

  {
    usb_vault::development::FixtureCleanup cleanup(stage_a, stage_b, final_a,
                                                   final_b);
    cleanup.own_stage_a();
    cleanup.own_stage_b();
    cleanup.publish_b();
    cleanup.publish_a();
    cleanup.release();
  }
  require(path_exists(final_a) && path_exists(final_b),
          "release discarded the published layout");
  require(!path_exists(stage_a) && !path_exists(stage_b),
          "publication left a staging container behind");
}

}  // namespace

int main() {
  try {
    test_fixture_reads_once_and_derives_matching_layout();
    test_fixture_rejects_wrong_card_blob_size_before_derivation();
    test_partially_written_container_is_removed_when_creation_fails();
    test_require_absent_rejects_any_existing_entry();
    test_require_regular_file_rejects_missing_and_directory();
    test_cleanup_removes_only_claimed_staging_files();
    test_failed_second_publication_rolls_back_the_first();
    test_release_keeps_a_fully_published_layout();
  } catch (const std::exception& error) {
    std::cerr << "development_container_fixture_tests: FAIL: " << error.what()
              << '\n';
    return 1;
  }

  std::cout << "development_container_fixture_tests: PASS\n";
  return 0;
}
