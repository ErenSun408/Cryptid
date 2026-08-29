#include "card/card_record_journal_v1.h"
#include "card/vikey/vikey_card_record_io.h"
#include "card/vikey/vikey_session.h"

#include "usb_vault/card/card_error.h"
#include "usb_vault/card/card_record_v1.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <ranges>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace {

namespace sdk = usb_vault::card::vikey::sdk;
using usb_vault::card::CardError;
using usb_vault::card::ICardRecordJournalStorage;
using usb_vault::card::kCardRecordJournalV1Slot0Offset;
using usb_vault::card::kCardRecordJournalV1StorageSize;
using usb_vault::card::kCardRecordV1Size;
using usb_vault::card::make_error_code;
using usb_vault::card::provision_card_record_journal_v1;
using usb_vault::card::provision_uninitialized_card_record_v1;
using usb_vault::card::vikey::inspect_vikey_card_media;
using usb_vault::card::vikey::read_vikey_card_record;
using usb_vault::card::vikey::VikeyCardMediaState;
using usb_vault::card::vikey::VikeySession;
using usb_vault::card::vikey::write_vikey_card_record;
using usb_vault::security::SecureBuffer;

constexpr std::size_t kVikeyTestUserDataSize = 8192;

struct FakeSdk final {
  std::array<std::byte, kVikeyTestUserDataSize> data{};
  sdk::Length reported_capacity{
      static_cast<sdk::Length>(kVikeyTestUserDataSize)};
  std::size_t reads{};
  std::size_t writes{};
  bool ranges_valid{true};
  bool mutate_write_arguments{true};
};

FakeSdk* active_fake = nullptr;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

FakeSdk& fake() {
  require(active_fake != nullptr, "no active fake SDK");
  return *active_fake;
}

sdk::Status __stdcall set_user_sn(char*) { return sdk::kSuccess; }

sdk::Status __stdcall find_ex(std::uint32_t* const count) {
  if (count != nullptr) *count = 1;
  return sdk::kSuccess;
}

sdk::Status __stdcall set_api_key(sdk::Index, char*) {
  return sdk::kSuccess;
}

sdk::Status __stdcall uninitialization() { return sdk::kSuccess; }

sdk::Status __stdcall get_hid(sdk::Index,
                              std::uint32_t* const hardware_id) {
  if (hardware_id != nullptr) *hardware_id = 0x12345678U;
  return sdk::kSuccess;
}

sdk::Status __stdcall get_level(sdk::Index, sdk::Byte* const level) {
  if (level != nullptr) *level = 1;
  return sdk::kSuccess;
}

sdk::Status __stdcall get_user_data_size(sdk::Index,
                                         sdk::Length* const bytes) {
  if (bytes != nullptr) *bytes = fake().reported_capacity;
  return sdk::kSuccess;
}

sdk::Status __stdcall get_attempts(sdk::Index, sdk::Byte* const current,
                                   sdk::Byte* const maximum) {
  if (current != nullptr) *current = 200;
  if (maximum != nullptr) *maximum = 200;
  return sdk::kSuccess;
}

sdk::Status __stdcall user_login(sdk::Index, char*) {
  return sdk::kSuccess;
}

sdk::Status __stdcall logoff(sdk::Index) { return sdk::kSuccess; }

sdk::Status __stdcall read_data(const sdk::Index index,
                                const sdk::Address address,
                                const sdk::Length length,
                                sdk::Byte* const destination) {
  FakeSdk& state = fake();
  ++state.reads;
  const bool valid = index == 0 && destination != nullptr &&
                     address <= state.data.size() &&
                     length <= state.data.size() - address;
  state.ranges_valid = state.ranges_valid && valid;
  if (!valid) return sdk::kInvalidAddressOrSize;
  for (std::size_t offset = 0; offset < length; ++offset) {
    destination[offset] =
        std::to_integer<sdk::Byte>(state.data[address + offset]);
  }
  return sdk::kSuccess;
}

sdk::Status __stdcall write_data(const sdk::Index index,
                                 const sdk::Address address,
                                 const sdk::Length length,
                                 sdk::Byte* const source) {
  FakeSdk& state = fake();
  ++state.writes;
  const bool valid = index == 0 && source != nullptr &&
                     address <= state.data.size() &&
                     length <= state.data.size() - address;
  state.ranges_valid = state.ranges_valid && valid;
  if (!valid) return sdk::kInvalidAddressOrSize;
  for (std::size_t offset = 0; offset < length; ++offset) {
    state.data[address + offset] = static_cast<std::byte>(source[offset]);
  }
  if (state.mutate_write_arguments && length != 0) source[0] ^= 0xffU;
  return sdk::kSuccess;
}

[[nodiscard]] sdk::Functions functions() noexcept {
  return {.set_user_sn = &set_user_sn,
          .find_ex = &find_ex,
          .set_api_key = &set_api_key,
          .uninitialization = &uninitialization,
          .get_hid = &get_hid,
          .get_level = &get_level,
          .get_user_data_size = &get_user_data_size,
          .get_user_password_attempt = &get_attempts,
          .user_login = &user_login,
          .logoff = &logoff,
          .read_data = &read_data,
          .write_data = &write_data};
}

class DirectStorage final : public ICardRecordJournalStorage {
 public:
  explicit DirectStorage(FakeSdk& state) noexcept : state_(state) {}

  [[nodiscard]] std::size_t capacity() override {
    return state_.data.size();
  }

  [[nodiscard]] SecureBuffer read(const std::size_t offset,
                                  const std::size_t length) override {
    require(offset <= state_.data.size() &&
                length <= state_.data.size() - offset,
            "direct read exceeded fake storage");
    SecureBuffer result(length);
    std::ranges::copy_n(state_.data.begin() + offset, length,
                        result.bytes().begin());
    return result;
  }

  void write(const std::size_t offset,
             const std::span<const std::byte> bytes) override {
    require(offset <= state_.data.size() &&
                bytes.size() <= state_.data.size() - offset,
            "direct write exceeded fake storage");
    std::ranges::copy(bytes, state_.data.begin() + offset);
  }

 private:
  FakeSdk& state_;
};

void store_u32(const std::span<std::byte> bytes, const std::size_t offset,
               const std::uint32_t value) noexcept {
  for (std::size_t index = 0; index < 4; ++index) {
    bytes[offset + index] =
        static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
}

[[nodiscard]] SecureBuffer framed_record() {
  SecureBuffer result(kCardRecordV1Size);
  std::ranges::fill(result.bytes(), std::byte{0xa5});
  constexpr std::array<std::byte, 8> magic{
      std::byte{'U'}, std::byte{'V'}, std::byte{'C'}, std::byte{'R'},
      std::byte{'D'}, std::byte{'2'}, std::byte{},    std::byte{}};
  std::ranges::copy(magic, result.bytes().begin());
  store_u32(result.bytes(), 8, 1);
  store_u32(result.bytes(), 12, static_cast<std::uint32_t>(result.size()));
  store_u32(result.bytes(), 48, 512U * 1024U);
  store_u32(result.bytes(), 52, 2);
  store_u32(result.bytes(), 56, 1);
  store_u32(result.bytes(), 60, 0x13);
  return result;
}

[[nodiscard]] bool same(const SecureBuffer& left,
                        const SecureBuffer& right) noexcept {
  return std::ranges::equal(left.bytes(), right.bytes());
}

template <typename Operation>
void require_card_error(Operation&& operation, const CardError expected,
                        const char* const message) {
  try {
    std::forward<Operation>(operation)();
  } catch (const std::system_error& error) {
    require(error.code() == make_error_code(expected), message);
    return;
  }
  throw std::runtime_error(message);
}

void test_random_access_card_record_path() {
  FakeSdk state;
  DirectStorage direct(state);
  SecureBuffer marker = provision_uninitialized_card_record_v1();
  provision_card_record_journal_v1(direct, marker.bytes());
  active_fake = &state;

  SecureBuffer user_sn(usb_vault::card::vikey::kVikeyUserSnBytes);
  SecureBuffer api_key(usb_vault::card::vikey::kVikeyApiKeyBytes);
  SecureBuffer user_pin(usb_vault::card::vikey::kVikeyUserPinBytes);
  std::ranges::fill(user_sn.bytes(), std::byte{'A'});
  std::ranges::fill(api_key.bytes(), std::byte{'B'});
  std::ranges::fill(user_pin.bytes(), std::byte{'P'});
  VikeySession session =
      VikeySession::open(functions(), user_sn, api_key, user_pin);

  SecureBuffer observed = read_vikey_card_record(session);
  require(same(observed, marker), "ViKey path did not read the marker");
  SecureBuffer replacement = framed_record();
  write_vikey_card_record(session, replacement);
  require(replacement.bytes().front() == std::byte{'U'},
          "vendor mutation escaped the private write copy");
  observed = read_vikey_card_record(session);
  require(same(observed, replacement),
          "ViKey path did not return the committed record");
  require(state.ranges_valid && state.writes >= 4,
          "ViKey path used invalid or incomplete random access");

  state.reported_capacity =
      static_cast<sdk::Length>(kCardRecordJournalV1StorageSize - 1);
  const std::size_t reads_before = state.reads;
  require_card_error(
      [&] { static_cast<void>(read_vikey_card_record(session)); },
      CardError::invalid_configuration,
      "undersized ViKey user area was accepted");
  require(state.reads == reads_before,
          "capacity rejection occurred after a vendor read");
  require(!session.close(), "ViKey card-record session did not close");
  active_fake = nullptr;
}

void test_read_only_media_classification() {
  FakeSdk state;
  active_fake = &state;

  SecureBuffer user_sn(usb_vault::card::vikey::kVikeyUserSnBytes);
  SecureBuffer api_key(usb_vault::card::vikey::kVikeyApiKeyBytes);
  SecureBuffer user_pin(usb_vault::card::vikey::kVikeyUserPinBytes);
  std::ranges::fill(user_sn.bytes(), std::byte{'A'});
  std::ranges::fill(api_key.bytes(), std::byte{'B'});
  std::ranges::fill(user_pin.bytes(), std::byte{'P'});
  VikeySession session =
      VikeySession::open(functions(), user_sn, api_key, user_pin);

  require(inspect_vikey_card_media(session) ==
              VikeyCardMediaState::blank,
          "blank ViKey media was not accepted");

  state.data[0] = std::byte{0xa5};
  require(inspect_vikey_card_media(session) ==
              VikeyCardMediaState::legacy_p1_or_foreign_prefix,
          "P1-shaped ViKey media was not blocked separately");

  state.data.fill(std::byte{});
  state.data.back() = std::byte{0xa5};
  require(inspect_vikey_card_media(session) ==
              VikeyCardMediaState::foreign_data,
          "foreign ViKey media was treated as blank");

  state.data.fill(std::byte{});
  SecureBuffer marker = provision_uninitialized_card_record_v1();
  std::ranges::copy(marker.bytes(),
                    state.data.begin() + kCardRecordJournalV1Slot0Offset);
  require(inspect_vikey_card_media(session) ==
              VikeyCardMediaState::provisioning_interrupted,
          "interrupted ViKey provisioning was not recoverable");

  state.data[kCardRecordJournalV1StorageSize] = std::byte{0x7e};
  require(inspect_vikey_card_media(session) ==
              VikeyCardMediaState::conflicting_evidence,
          "conflicting ViKey provisioning evidence was accepted");

  state.data.fill(std::byte{});
  DirectStorage direct(state);
  provision_card_record_journal_v1(direct, marker.bytes());
  state.data[kCardRecordJournalV1StorageSize] = std::byte{0x7e};
  const std::size_t writes_before = state.writes;
  require(inspect_vikey_card_media(session) ==
              VikeyCardMediaState::product_evidence,
          "existing ViKey product media was not recognized");
  require(state.writes == writes_before,
          "ViKey media inspection wrote through the SDK");

  state.reported_capacity =
      static_cast<sdk::Length>(kCardRecordJournalV1StorageSize - 1);
  const std::size_t reads_before = state.reads;
  require_card_error(
      [&] { static_cast<void>(inspect_vikey_card_media(session)); },
      CardError::invalid_configuration,
      "undersized ViKey media was inspected");
  require(state.reads == reads_before,
          "undersized ViKey media was read before rejection");

  require(!session.close(), "ViKey media inspection session did not close");
  active_fake = nullptr;
}

}  // namespace

int main() {
  try {
    test_random_access_card_record_path();
    test_read_only_media_classification();
    std::cout << "vikey card record IO tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    active_fake = nullptr;
    std::cerr << "FAILED: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
