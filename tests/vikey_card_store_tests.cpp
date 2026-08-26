#include "card/vikey/vikey_card_store.h"

#include "usb_vault/card/card_error.h"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <system_error>

namespace {

using usb_vault::card::CardError;
using usb_vault::card::make_error_code;
using usb_vault::card::vikey::IVikeyBlobBackend;
using usb_vault::card::vikey::VikeyCardStore;
using usb_vault::security::SecureBuffer;

constexpr std::size_t kBlobBytes = 32;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

[[nodiscard]] std::byte expected_byte(const std::size_t index) {
  return static_cast<std::byte>(index ^ 0xa5U);
}

class FakeBackend final : public IVikeyBlobBackend {
 public:
  [[nodiscard]] SecureBuffer get_blob() override {
    ++get_calls;
    if (get_error != CardError{}) {
      throw std::system_error(make_error_code(get_error));
    }
    SecureBuffer result(read_size);
    for (std::size_t index = 0; index < result.size(); ++index) {
      result.bytes()[index] = expected_byte(index);
    }
    return result;
  }

  void put_blob(const SecureBuffer& blob) override {
    ++put_calls;
    if (put_error != CardError{}) {
      throw std::system_error(make_error_code(put_error));
    }
    require(blob.size() == kBlobBytes, "backend received a wrong-sized blob");
    for (std::size_t index = 0; index < blob.size(); ++index) {
      require(blob.bytes()[index] == expected_byte(index),
              "backend received changed blob data");
    }
  }

  std::size_t read_size{kBlobBytes};
  CardError get_error{};
  CardError put_error{};
  std::size_t get_calls{};
  std::size_t put_calls{};
};

void require_card_error(const auto& operation, const CardError expected,
                        const char* const message) {
  try {
    operation();
  } catch (const std::system_error& error) {
    require(error.code() == make_error_code(expected), message);
    return;
  }
  throw std::runtime_error(message);
}

[[nodiscard]] SecureBuffer test_blob() {
  SecureBuffer result(kBlobBytes);
  for (std::size_t index = 0; index < result.size(); ++index) {
    result.bytes()[index] = expected_byte(index);
  }
  return result;
}

void test_get_blob_preserves_binary_data() {
  FakeBackend backend;
  VikeyCardStore store(backend);

  SecureBuffer blob = store.get_blob();

  require(backend.get_calls == 1, "get did not reach the ViKey backend");
  require(blob.size() == kBlobBytes, "get returned the wrong blob size");
  for (std::size_t index = 0; index < blob.size(); ++index) {
    require(blob.bytes()[index] == expected_byte(index),
            "get changed the backend blob");
  }
  require(store.capabilities().bits == 0,
          "ViKey adapter advertised an unsupported capability");
}

void test_put_blob_preserves_caller_ownership() {
  FakeBackend backend;
  VikeyCardStore store(backend);
  SecureBuffer blob = test_blob();

  store.put_blob(blob);

  require(backend.put_calls == 1, "put did not reach the ViKey backend");
  for (std::size_t index = 0; index < blob.size(); ++index) {
    require(blob.bytes()[index] == expected_byte(index),
            "put changed the caller blob");
  }
}

void test_invalid_blob_shapes_fail_closed() {
  FakeBackend backend;
  VikeyCardStore store(backend);
  SecureBuffer short_blob(kBlobBytes - 1);

  require_card_error([&] { store.put_blob(short_blob); },
                     CardError::invalid_configuration,
                     "short put returned the wrong error");
  require(backend.put_calls == 0,
          "short put reached the ViKey backend");

  SecureBuffer long_blob(kBlobBytes + 1);
  require_card_error([&] { store.put_blob(long_blob); },
                     CardError::invalid_configuration,
                     "long put returned the wrong error");
  require(backend.put_calls == 0,
          "long put reached the ViKey backend");

  backend.read_size = kBlobBytes - 1;
  require_card_error([&] { static_cast<void>(store.get_blob()); },
                     CardError::device_failure,
                     "short backend read returned the wrong error");
  require(backend.get_calls == 1,
          "invalid backend read was not observed");

  backend.read_size = kBlobBytes + 1;
  require_card_error([&] { static_cast<void>(store.get_blob()); },
                     CardError::device_failure,
                     "long backend read returned the wrong error");
  require(backend.get_calls == 2,
          "long backend read was not observed");
}

void test_backend_errors_remain_in_common_domain() {
  FakeBackend backend;
  VikeyCardStore store(backend);
  backend.get_error = CardError::device_not_found;
  require_card_error([&] { static_cast<void>(store.get_blob()); },
                     CardError::device_not_found,
                     "get backend error changed at the adapter");

  backend.get_error = CardError{};
  backend.put_error = CardError::communication_failed;
  SecureBuffer blob = test_blob();
  require_card_error([&] { store.put_blob(blob); },
                     CardError::communication_failed,
                     "put backend error changed at the adapter");
}

}  // namespace

int main() {
  try {
    test_get_blob_preserves_binary_data();
    test_put_blob_preserves_caller_ownership();
    test_invalid_blob_shapes_fail_closed();
    test_backend_errors_remain_in_common_domain();
    std::cout << "vikey_card_store_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vikey_card_store_tests failed: " << error.what() << '\n';
    return 1;
  }
}
