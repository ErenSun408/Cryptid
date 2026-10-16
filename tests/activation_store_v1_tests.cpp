#include "usb_vault/application/activation_store_v1.h"

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using usb_vault::application::ActivationInstallResult;
using usb_vault::application::ActivationStoreV1;
using usb_vault::licensing::ActivationSigner;
using usb_vault::licensing::ActivationState;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

constexpr std::uint32_t kCard = 0x1A2B3C4D;
constexpr std::int64_t kNow = 1790000000;

struct Fixture final {
  Fixture() {
    wchar_t base[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, base);
    directory = std::filesystem::path(base) /
                (L"uv-activation-" + std::to_wstring(::GetCurrentProcessId()));
    std::filesystem::remove_all(directory);
    path = (directory / L"activation.dat").wstring();
  }
  ~Fixture() {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  [[nodiscard]] ActivationStoreV1 store() const {
    return ActivationStoreV1(path, signer.public_key(), [this] { return now; });
  }

  std::filesystem::path directory;
  std::wstring path;
  ActivationSigner signer = ActivationSigner::generate();
  std::int64_t now = kNow;
};

void test_install_and_reload() {
  Fixture f;
  auto store = f.store();
  require(store.state(kCard) == ActivationState::missing, "fresh store was not missing");
  const std::string code = f.signer.sign({kCard, kNow + 3600});
  require(store.install(kCard, code) == ActivationInstallResult::accepted, "valid code rejected");
  require(store.state(kCard) == ActivationState::active, "installed code not active");
  require(store.expiry(kCard) == kNow + 3600, "expiry not reported");

  auto reloaded = f.store();
  require(reloaded.state(kCard) == ActivationState::active, "code did not survive reload");
  require(reloaded.state(kCard + 1) == ActivationState::missing, "another card was active");

  f.now = kNow + 3600;
  require(f.store().state(kCard) == ActivationState::expired, "expiry not enforced");
}

void test_rejections() {
  Fixture f;
  auto store = f.store();
  require(store.install(kCard, "not a code") == ActivationInstallResult::invalid,
          "garbage accepted");
  require(store.install(kCard, f.signer.sign({kCard + 1, kNow + 60})) ==
              ActivationInstallResult::other_card,
          "another card's code accepted");
  require(store.install(kCard, f.signer.sign({kCard, kNow})) ==
              ActivationInstallResult::expired,
          "an expired code accepted");
  const ActivationSigner foreign = ActivationSigner::generate();
  require(store.install(kCard, foreign.sign({kCard, kNow + 60})) ==
              ActivationInstallResult::invalid,
          "a foreign-key code accepted");
  require(store.install(kCard, std::string(600, 'A')) == ActivationInstallResult::invalid,
          "an oversized input accepted");
  require(!std::filesystem::exists(f.path) || store.state(kCard) == ActivationState::missing,
          "a rejected code was kept");
}

void test_rewind_is_persisted() {
  Fixture f;
  auto store = f.store();
  require(store.install(kCard, f.signer.sign({kCard, kNow + 86400})) ==
              ActivationInstallResult::accepted, "valid code rejected");
  f.now = kNow + 7200;
  require(f.store().state(kCard) == ActivationState::active, "later reading not active");
  f.now = kNow;  // Two hours back, beyond the ten-minute tolerance.
  require(f.store().state(kCard) == ActivationState::clock_rewound,
          "a persisted high-water mark did not detect the rewind");
  require(f.store().install(kCard, f.signer.sign({kCard, kNow + 99999})) ==
              ActivationInstallResult::clock_rewound,
          "a code was installed on a rewound clock");
}

void test_replacement_and_canonical_storage() {
  Fixture f;
  auto store = f.store();
  require(store.install(kCard, f.signer.sign({kCard, kNow + 60})) ==
              ActivationInstallResult::accepted, "first code rejected");
  std::string later = f.signer.sign({kCard, kNow + 7200});
  for (char& c : later) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  require(store.install(kCard, "  " + later + "\r\n") == ActivationInstallResult::accepted,
          "a pasted lower-case code was rejected");
  require(f.store().expiry(kCard) == kNow + 7200, "replacement not persisted");
}

void test_unreadable_record_reads_empty() {
  Fixture f;
  std::filesystem::create_directories(f.directory);
  std::ofstream(f.path, std::ios::binary) << "not DPAPI";
  auto store = f.store();
  require(store.state(kCard) == ActivationState::missing, "garbage record trusted");
  require(store.install(kCard, f.signer.sign({kCard, kNow + 60})) ==
              ActivationInstallResult::accepted, "garbage record blocked install");
  require(f.store().state(kCard) == ActivationState::active, "record not rewritten");
}

void test_locked_record_is_never_overwritten() {
  Fixture f;
  require(f.store().install(kCard, f.signer.sign({kCard, kNow + 86400})) ==
              ActivationInstallResult::accepted, "valid code rejected");
  const auto before = std::filesystem::last_write_time(f.path);
  const HANDLE lock = ::CreateFileW(f.path.c_str(), GENERIC_READ, 0, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  require(lock != INVALID_HANDLE_VALUE, "could not lock the record");
  auto store = f.store();
  f.now = kNow + 3600;
  require(store.state(kCard) == ActivationState::missing, "a locked record was trusted");
  bool refused = false;
  try {
    static_cast<void>(store.install(kCard, f.signer.sign({kCard, kNow + 99999})));
  } catch (const std::runtime_error&) {
    refused = true;
  }
  ::CloseHandle(lock);
  require(refused, "an install wrote over a locked record");
  require(std::filesystem::last_write_time(f.path) == before, "a locked record was rewritten");
  require(store.state(kCard) == ActivationState::active,
          "the record was not read once it was released");
}

}  // namespace

int main() {
  try {
    test_install_and_reload();
    test_rejections();
    test_rewind_is_persisted();
    test_replacement_and_canonical_storage();
    test_unreadable_record_reads_empty();
    test_locked_record_is_never_overwritten();
  } catch (const std::exception& error) {
    std::cerr << "activation_store_v1_tests failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "activation_store_v1_tests passed\n";
  return 0;
}
