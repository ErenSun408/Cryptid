#include "usb_vault/card/card_store.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

using usb_vault::card::CardCapabilities;
using usb_vault::card::CardCapability;
using usb_vault::card::ICardStore;
using usb_vault::security::SecureBuffer;

constexpr std::size_t kBlobSize = 32;

void require(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

[[nodiscard]] constexpr std::byte expected_byte(const std::size_t index) {
  return static_cast<std::byte>(index);
}

class FixedBlobCardStore final : public ICardStore {
 public:
  [[nodiscard]] SecureBuffer get_blob() override {
    ++get_calls;
    SecureBuffer blob(kBlobSize);
    for (std::size_t index = 0; index < blob.size(); ++index) {
      blob.bytes()[index] = expected_byte(index);
    }
    return blob;
  }

  void put_blob(const SecureBuffer& blob) override {
    require(blob.size() == kBlobSize, "put blob size differed");
    for (std::size_t index = 0; index < blob.size(); ++index) {
      require(blob.bytes()[index] == expected_byte(index),
              "put blob bytes differed");
    }
    ++put_calls;
  }

  [[nodiscard]] CardCapabilities capabilities() const noexcept override {
    return {.bits = static_cast<std::uint32_t>(
                CardCapability::multiple_pins)};
  }

  std::uint32_t get_calls{};
  std::uint32_t put_calls{};
};

void test_get_returns_binary_blob_in_secure_buffer() {
  FixedBlobCardStore store;
  SecureBuffer blob = store.get_blob();

  require(store.get_calls == 1, "get call was not observed");
  require(blob.size() == kBlobSize, "get blob size differed");
  require(blob.bytes().front() == std::byte{0},
          "binary NUL was not preserved");
  for (std::size_t index = 0; index < blob.size(); ++index) {
    require(blob.bytes()[index] == expected_byte(index),
            "get blob bytes differed");
  }
}

void test_put_accepts_secure_buffer_without_ownership_transfer() {
  FixedBlobCardStore store;
  SecureBuffer blob = store.get_blob();

  store.put_blob(blob);

  require(store.put_calls == 1, "put call was not observed");
  require(blob.size() == kBlobSize,
          "put unexpectedly consumed the caller buffer");
}

void test_capability_bits_do_not_change_blob_contract() {
  FixedBlobCardStore store;
  const CardCapabilities capabilities = store.capabilities();

  require(capabilities.supports(CardCapability::multiple_pins),
          "multiple-pins capability was not reported");
  SecureBuffer blob = store.get_blob();
  require(blob.size() == kBlobSize,
          "capability changed the single-blob contract");
}

}  // namespace

int main() {
  test_get_returns_binary_blob_in_secure_buffer();
  test_put_accepts_secure_buffer_without_ownership_transfer();
  test_capability_bits_do_not_change_blob_contract();

  std::cout << "card_store_contract_tests: PASS\n";
  return 0;
}
