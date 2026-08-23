#include "usb_vault/card/fixed_blob_card_store.h"

#include <cstddef>
#include <iostream>
#include <stdexcept>

namespace {

void require(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void test_fixed_blob_is_generated_in_secure_memory() {
  usb_vault::card::FixedBlobCardStore store;
  auto first = store.get_blob();
  auto second = store.get_blob();
  require(first.size() == 32 && second.size() == 32,
          "stub returned the wrong blob size");
  require(first.data() != second.data(), "stub retained a shared blob buffer");
  for (std::size_t index = 0; index < first.size(); ++index) {
    const auto expected = static_cast<std::byte>(index);
    require(first.bytes()[index] == expected && second.bytes()[index] == expected,
            "stub returned unexpected blob bytes");
  }
  require(store.capabilities().bits == 0,
          "stub advertised unsupported hardware capabilities");
}

void test_fixed_blob_stub_rejects_writes() {
  usb_vault::card::FixedBlobCardStore store;
  auto blob = store.get_blob();
  try {
    store.put_blob(blob);
  } catch (const std::logic_error&) {
    return;
  }
  throw std::runtime_error("read-only card stub accepted a write");
}

}  // namespace

int main() {
  test_fixed_blob_is_generated_in_secure_memory();
  test_fixed_blob_stub_rejects_writes();
  std::cout << "fixed_blob_card_store_tests: PASS\n";
  return 0;
}
