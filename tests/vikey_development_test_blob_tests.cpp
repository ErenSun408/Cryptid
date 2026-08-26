#include "card/vikey/vikey_session.h"
#include "random_test_blob.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main() {
  try {
    auto first = usb_vault::card::vikey::development::random_test_blob();
    auto second = usb_vault::card::vikey::development::random_test_blob();

    require(first.size() == usb_vault::card::vikey::kVikeyBlobBytes &&
                second.size() == usb_vault::card::vikey::kVikeyBlobBytes,
            "development test blob has the wrong size");
    require(first.data() != nullptr && second.data() != nullptr,
            "development test blob has no locked allocation");
    require(!std::ranges::equal(first.bytes(), second.bytes()),
            "independent development test blobs unexpectedly match");

    std::cout << "vikey_development_test_blob_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vikey_development_test_blob_tests failed: " << error.what()
              << '\n';
    return 1;
  }
}
