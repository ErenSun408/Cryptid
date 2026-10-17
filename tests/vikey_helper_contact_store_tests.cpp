#include "card/vikey/vikey_helper_contact_store.h"

#include "usb_vault/card/card_error.h"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace {

using usb_vault::card::CardError;
using usb_vault::card::make_error_code;
using usb_vault::card::vikey::IVikeyCredentialProvider;
using usb_vault::card::vikey::VikeyCredentialSet;
using usb_vault::card::vikey::VikeyHelperBackendConfig;
using usb_vault::card::vikey::VikeyHelperContactStoreStorage;
using usb_vault::contacts::kContactStoreV1CopyOffsets;
using usb_vault::contacts::kContactStoreV1CopySize;
using usb_vault::contacts::kContactStoreV1RegionEnd;
using usb_vault::contacts::kContactStoreV1RegionOffset;
using usb_vault::contacts::kContactStoreV1RegionSize;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

// Counts credential loads: each helper session starts with one, so zero loads
// proves a rejected range never reached the helper.
class CountingCredentials final : public IVikeyCredentialProvider {
 public:
  VikeyCredentialSet load_credentials() override {
    ++loads;
    throw std::system_error(make_error_code(CardError::backend_unavailable),
                            "no credentials in this test");
  }
  int loads{0};
};

template <typename F>
[[nodiscard]] bool throws_invalid_argument(F&& f) {
  try {
    f();
  } catch (const std::invalid_argument&) {
    return true;
  }
  return false;
}

void test_identity_required() {
  CountingCredentials credentials;
  bool rejected = false;
  try {
    VikeyHelperContactStoreStorage storage(VikeyHelperBackendConfig{},
                                           credentials, 0);
  } catch (const std::system_error& error) {
    rejected = error.code() == make_error_code(CardError::invalid_configuration);
  }
  require(rejected, "a zero hardware identity was accepted");
}

void test_only_fixed_ranges_reach_the_helper() {
  CountingCredentials credentials;
  VikeyHelperContactStoreStorage storage(VikeyHelperBackendConfig{},
                                         credentials, 0x12345678U);
  require(storage.capacity() == kContactStoreV1RegionEnd,
          "the adapter reported an unexpected capacity");

  require(throws_invalid_argument([&] {
            static_cast<void>(storage.read(0, kContactStoreV1RegionSize));
          }),
          "a read below the region was accepted");
  require(throws_invalid_argument([&] {
            static_cast<void>(storage.read(kContactStoreV1RegionOffset,
                                           kContactStoreV1CopySize));
          }),
          "a partial region read was accepted");

  const std::vector<std::byte> copy(kContactStoreV1CopySize);
  const std::vector<std::byte> short_copy(kContactStoreV1CopySize - 1);
  require(throws_invalid_argument(
              [&] { storage.write(kContactStoreV1RegionOffset + 1, copy); }),
          "a misaligned copy write was accepted");
  require(throws_invalid_argument([&] { storage.write(0, copy); }),
          "a copy write into the card journal was accepted");
  require(throws_invalid_argument(
              [&] { storage.write(kContactStoreV1CopyOffsets[1], short_copy); }),
          "a short copy write was accepted");
  require(credentials.loads == 0, "a rejected range started a helper session");

  // A valid range does start a session; failures surface as card errors.
  bool card_error = false;
  try {
    storage.write(kContactStoreV1CopyOffsets[0], copy);
  } catch (const std::system_error& error) {
    card_error = error.code() == make_error_code(CardError::backend_unavailable);
  }
  require(card_error && credentials.loads == 1,
          "a valid copy write did not start exactly one helper session");
}

}  // namespace

int main() {
  try {
    test_identity_required();
    test_only_fixed_ranges_reach_the_helper();
  } catch (const std::exception& error) {
    std::cerr << "vikey_helper_contact_store_tests failed: " << error.what()
              << '\n';
    return 1;
  }
  std::cout << "vikey_helper_contact_store_tests passed\n";
  return 0;
}
