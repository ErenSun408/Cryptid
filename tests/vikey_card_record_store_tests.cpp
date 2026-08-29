#include "card/vikey/vikey_card_record_store.h"

#include "usb_vault/card/card_error.h"
#include "usb_vault/card/card_record_v1.h"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <system_error>

namespace {

using usb_vault::card::CardError;
using usb_vault::card::kCardRecordV1Size;
using usb_vault::card::make_error_code;
using usb_vault::card::vikey::IVikeyCardRecordBackend;
using usb_vault::card::vikey::VikeyCardRecordStore;
using usb_vault::security::SecureBuffer;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

[[nodiscard]] std::byte expected_byte(const std::size_t index) {
  return static_cast<std::byte>((index * 17U) ^ 0xa5U);
}

class FakeBackend final : public IVikeyCardRecordBackend {
 public:
  [[nodiscard]] SecureBuffer read_record() override {
    ++read_calls;
    if (read_error != CardError{}) {
      throw std::system_error(make_error_code(read_error));
    }
    SecureBuffer result(read_size);
    for (std::size_t index = 0; index < result.size(); ++index) {
      result.bytes()[index] = expected_byte(index);
    }
    return result;
  }

  void write_record(const SecureBuffer& record) override {
    ++write_calls;
    if (write_error != CardError{}) {
      throw std::system_error(make_error_code(write_error));
    }
    require(record.size() == kCardRecordV1Size,
            "backend received a wrong-sized card record");
    for (std::size_t index = 0; index < record.size(); ++index) {
      require(record.bytes()[index] == expected_byte(index),
              "backend received changed card record data");
    }
  }

  std::size_t read_size{kCardRecordV1Size};
  CardError read_error{};
  CardError write_error{};
  std::size_t read_calls{};
  std::size_t write_calls{};
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

[[nodiscard]] SecureBuffer test_record() {
  SecureBuffer result(kCardRecordV1Size);
  for (std::size_t index = 0; index < result.size(); ++index) {
    result.bytes()[index] = expected_byte(index);
  }
  return result;
}

void test_read_preserves_binary_data() {
  FakeBackend backend;
  VikeyCardRecordStore store(backend);

  SecureBuffer record = store.get_blob();

  require(backend.read_calls == 1,
          "read did not reach the ViKey card-record backend");
  require(record.size() == kCardRecordV1Size,
          "read returned the wrong card record size");
  for (std::size_t index = 0; index < record.size(); ++index) {
    require(record.bytes()[index] == expected_byte(index),
            "read changed the backend card record");
  }
  require(store.capabilities().bits == 0,
          "ViKey card-record adapter advertised an unsupported capability");
}

void test_write_preserves_caller_ownership() {
  FakeBackend backend;
  VikeyCardRecordStore store(backend);
  SecureBuffer record = test_record();

  store.put_blob(record);

  require(backend.write_calls == 1,
          "write did not reach the ViKey card-record backend");
  for (std::size_t index = 0; index < record.size(); ++index) {
    require(record.bytes()[index] == expected_byte(index),
            "write changed the caller card record");
  }
}

void test_invalid_record_shapes_fail_closed() {
  FakeBackend backend;
  VikeyCardRecordStore store(backend);
  SecureBuffer short_record(kCardRecordV1Size - 1);

  require_card_error([&] { store.put_blob(short_record); },
                     CardError::invalid_configuration,
                     "short write returned the wrong error");
  require(backend.write_calls == 0,
          "short write reached the ViKey card-record backend");

  SecureBuffer long_record(kCardRecordV1Size + 1);
  require_card_error([&] { store.put_blob(long_record); },
                     CardError::invalid_configuration,
                     "long write returned the wrong error");
  require(backend.write_calls == 0,
          "long write reached the ViKey card-record backend");

  backend.read_size = kCardRecordV1Size - 1;
  require_card_error([&] { static_cast<void>(store.get_blob()); },
                     CardError::device_failure,
                     "short backend read returned the wrong error");
  require(backend.read_calls == 1,
          "short backend read was not observed");

  backend.read_size = kCardRecordV1Size + 1;
  require_card_error([&] { static_cast<void>(store.get_blob()); },
                     CardError::device_failure,
                     "long backend read returned the wrong error");
  require(backend.read_calls == 2,
          "long backend read was not observed");
}

void test_backend_errors_remain_in_common_domain() {
  FakeBackend backend;
  VikeyCardRecordStore store(backend);
  backend.read_error = CardError::device_not_found;
  require_card_error([&] { static_cast<void>(store.get_blob()); },
                     CardError::device_not_found,
                     "read backend error changed at the adapter");

  backend.read_error = CardError{};
  backend.write_error = CardError::communication_failed;
  SecureBuffer record = test_record();
  require_card_error([&] { store.put_blob(record); },
                     CardError::communication_failed,
                     "write backend error changed at the adapter");
}

}  // namespace

int main() {
  try {
    test_read_preserves_binary_data();
    test_write_preserves_caller_ownership();
    test_invalid_record_shapes_fail_closed();
    test_backend_errors_remain_in_common_domain();
    std::cout << "vikey_card_record_store_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vikey_card_record_store_tests failed: " << error.what()
              << '\n';
    return 1;
  }
}
