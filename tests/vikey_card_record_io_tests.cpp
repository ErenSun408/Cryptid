#include "card/card_record_journal_v1.h"
#include "card/card_record_journal_v2.h"
#include "card/vikey/vikey_card_record_io.h"
#include "card/vikey/vikey_session.h"

#include "usb_vault/card/card_error.h"
#include "card/capture_failure.h"
#include "usb_vault/card/card_record_v1.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <ranges>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace sdk = usb_vault::card::vikey::sdk;
using usb_vault::card::CardError;
using usb_vault::card::CardRecordState;
using usb_vault::card::ICardRecordJournalStorage;
using usb_vault::card::kCardRecordJournalV1Slot0Offset;
using usb_vault::card::kCardRecordJournalV1StorageSize;
using usb_vault::card::kCardRecordV1Size;
using usb_vault::card::make_error_code;
using usb_vault::card::provision_card_record_journal_v1;
using usb_vault::card::provision_uninitialized_card_record_v1;
using usb_vault::card::vikey::bootstrap_vikey_card_media;
using usb_vault::card::vikey::inspect_vikey_card_media;
using usb_vault::card::vikey::inspect_vikey_card_record_state_read_only;
using usb_vault::card::vikey::read_vikey_card_record;
using usb_vault::card::vikey::reset_vikey_development_legacy_media;
using usb_vault::card::vikey::VikeyCardMediaState;
using usb_vault::card::vikey::VikeySession;
using usb_vault::card::vikey::write_vikey_card_record;
using usb_vault::security::SecureBuffer;

constexpr std::size_t kVikeyTestUserDataSize = 8192;

struct FakeSdk final {
  std::uint32_t hardware_id{0x12345678U};
  std::array<std::byte, kVikeyTestUserDataSize> data{};
  sdk::Length reported_capacity{
      static_cast<sdk::Length>(kVikeyTestUserDataSize)};
  std::size_t reads{};
  std::size_t writes{};
  bool ranges_valid{true};
  bool mutate_write_arguments{true};
  bool swap_after_read{};
  bool corrupt_other_byte{};
  std::size_t fail_write_call{std::numeric_limits<std::size_t>::max()};
  std::size_t bytes_before_failure{};
  std::vector<sdk::Address> write_addresses;
  std::vector<sdk::Length> write_lengths;
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
  if (hardware_id != nullptr) *hardware_id = fake().hardware_id;
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
  if (state.swap_after_read) ++state.hardware_id;
  return sdk::kSuccess;
}

sdk::Status __stdcall write_data(const sdk::Index index,
                                 const sdk::Address address,
                                 const sdk::Length length,
                                 sdk::Byte* const source) {
  FakeSdk& state = fake();
  const std::size_t call = state.writes++;
  state.write_addresses.push_back(address);
  state.write_lengths.push_back(length);
  const bool valid = index == 0 && source != nullptr &&
                     address <= state.data.size() &&
                     length <= state.data.size() - address;
  state.ranges_valid = state.ranges_valid && valid;
  if (!valid) return sdk::kInvalidAddressOrSize;
  const std::size_t copied =
      call == state.fail_write_call
          ? std::min<std::size_t>(length, state.bytes_before_failure)
          : length;
  for (std::size_t offset = 0; offset < copied; ++offset) {
    state.data[address + offset] = static_cast<std::byte>(source[offset]);
  }
  if (call == state.fail_write_call) return sdk::kCommunication;
  if (state.mutate_write_arguments && length != 0) source[0] ^= 0xffU;
  if (state.corrupt_other_byte) state.data[1807] ^= std::byte{1};
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
  require(inspect_vikey_card_record_state_read_only(session) ==
              CardRecordState::provisioned_uninitialized,
          "ViKey product marker returned the wrong logical state");
  require(state.writes == writes_before,
          "ViKey media or logical-state inspection wrote through the SDK");

  std::ranges::fill(std::span(state.data).first(304), std::byte{});
  const std::array<std::byte, 6> anchor_prefix{
      std::byte{'U'}, std::byte{'V'}, std::byte{'C'}, std::byte{'R'}, std::byte{'D'}, std::byte{'2'}};
  std::ranges::copy(anchor_prefix, state.data.begin() + kCardRecordJournalV1Slot0Offset);
  require(inspect_vikey_card_media(session) == VikeyCardMediaState::product_evidence &&
      inspect_vikey_card_record_state_read_only(session) == CardRecordState::malformed &&
      state.writes == writes_before,
      "revoked v2 descriptors blocked read-only context or promoted residue");

  state.reported_capacity =
      static_cast<sdk::Length>(kCardRecordJournalV1StorageSize - 1);
  const std::size_t reads_before = state.reads;
  bool capacity_rejected = false;
  try { static_cast<void>(inspect_vikey_card_media(session)); }
  catch (...) {
    const auto failure = usb_vault::card::capture_failure(std::current_exception()).primary;
    capacity_rejected = failure.code == 2029 && failure.reason == 2;
  }
  require(capacity_rejected, "undersized startup media lost capacity diagnostic");
  require(state.reads == reads_before,
          "undersized ViKey media was read before rejection");

  require(!session.close(), "ViKey media inspection session did not close");
  active_fake = nullptr;
}

void test_media_bootstrap_revalidates_before_writing() {
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
  SecureBuffer marker = provision_uninitialized_card_record_v1();

  bootstrap_vikey_card_media(session, VikeyCardMediaState::blank);
  SecureBuffer observed = read_vikey_card_record(session);
  require(same(observed, marker),
          "blank ViKey bootstrap did not publish the marker");
  require(std::ranges::all_of(
              std::span(state.data).subspan(
                  kCardRecordJournalV1StorageSize),
              [](const std::byte value) { return value == std::byte{}; }),
          "blank ViKey bootstrap changed data outside the journal");

  state.data.fill(std::byte{});
  std::ranges::copy(marker.bytes(),
                    state.data.begin() + kCardRecordJournalV1Slot0Offset);
  bootstrap_vikey_card_media(
      session, VikeyCardMediaState::provisioning_interrupted);
  observed = read_vikey_card_record(session);
  require(same(observed, marker),
          "interrupted ViKey bootstrap did not resume the marker");

  state.data.fill(std::byte{});
  std::ranges::copy(marker.bytes(),
                    state.data.begin() + kCardRecordJournalV1Slot0Offset);
  const std::size_t writes_before_mismatch = state.writes;
  require_card_error(
      [&] {
        bootstrap_vikey_card_media(session, VikeyCardMediaState::blank);
      },
      CardError::device_failure,
      "changed ViKey bootstrap state was accepted");
  require(state.writes == writes_before_mismatch,
          "changed ViKey bootstrap state was written");

  state.data.fill(std::byte{});
  state.data.back() = std::byte{0x7e};
  const std::size_t writes_before_foreign = state.writes;
  require_card_error(
      [&] {
        bootstrap_vikey_card_media(session, VikeyCardMediaState::blank);
      },
      CardError::device_failure,
      "foreign ViKey media was bootstrapped");
  require(state.writes == writes_before_foreign,
          "foreign ViKey media was written");

  state.data.fill(std::byte{});
  const std::array<VikeyCardMediaState, 4> ineligible_states{
      VikeyCardMediaState::legacy_p1_or_foreign_prefix,
      VikeyCardMediaState::foreign_data,
      VikeyCardMediaState::conflicting_evidence,
      VikeyCardMediaState::product_evidence,
  };
  for (const VikeyCardMediaState ineligible : ineligible_states) {
    const std::size_t reads_before = state.reads;
    const std::size_t writes_before = state.writes;
    require_card_error(
        [&] { bootstrap_vikey_card_media(session, ineligible); },
        CardError::invalid_configuration,
        "ineligible expected ViKey bootstrap state was accepted");
    require(state.reads == reads_before && state.writes == writes_before,
            "ineligible expected state touched ViKey media");
  }

  require(!session.close(), "ViKey media bootstrap session did not close");
  active_fake = nullptr;
}

void test_development_legacy_reset_is_explicit_and_recoverable() {
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

  state.data.fill(std::byte{});
  std::ranges::fill_n(state.data.begin(),
                      usb_vault::card::vikey::kVikeyBlobBytes,
                      std::byte{0xa5});
  reset_vikey_development_legacy_media(
      session, VikeyCardMediaState::legacy_p1_or_foreign_prefix);
  require(inspect_vikey_card_media(session) == VikeyCardMediaState::blank,
          "development reset did not produce blank ViKey media");
  require(!state.write_addresses.empty() &&
              state.write_addresses.back() == 0 &&
              state.write_lengths.back() ==
                  usb_vault::card::vikey::kVikeyBlobBytes,
          "development reset did not clear the legacy prefix last");
  require(std::ranges::all_of(state.data, [](const std::byte value) {
            return value == std::byte{};
          }),
          "development reset did not verify the complete user-data area");

  const std::array<VikeyCardMediaState, 5> forbidden_expected_states{
      VikeyCardMediaState::blank,
      VikeyCardMediaState::foreign_data,
      VikeyCardMediaState::provisioning_interrupted,
      VikeyCardMediaState::conflicting_evidence,
      VikeyCardMediaState::product_evidence,
  };
  for (const VikeyCardMediaState forbidden : forbidden_expected_states) {
    const std::size_t reads_before = state.reads;
    const std::size_t writes_before = state.writes;
    require_card_error(
        [&] { reset_vikey_development_legacy_media(session, forbidden); },
        CardError::invalid_configuration,
        "development reset accepted an ineligible expected state");
    require(state.reads == reads_before && state.writes == writes_before,
            "ineligible development reset touched ViKey media");
  }

  state.data.fill(std::byte{});
  state.data.back() = std::byte{0x7e};
  const std::size_t writes_before_changed_state = state.writes;
  require_card_error(
      [&] {
        reset_vikey_development_legacy_media(
            session,
            VikeyCardMediaState::legacy_p1_or_foreign_prefix);
      },
      CardError::device_failure,
      "changed foreign media was accepted by development reset");
  require(state.writes == writes_before_changed_state,
          "changed foreign media was written by development reset");

  state.data.fill(std::byte{});
  std::ranges::fill_n(state.data.begin(),
                      usb_vault::card::vikey::kVikeyBlobBytes,
                      std::byte{0xa5});
  state.fail_write_call = state.writes + 2;
  state.bytes_before_failure =
      usb_vault::card::vikey::kVikeyBlobBytes / 2;
  require_card_error(
      [&] {
        reset_vikey_development_legacy_media(
            session,
            VikeyCardMediaState::legacy_p1_or_foreign_prefix);
      },
      CardError::device_failure,
      "partial final reset write was reported as successful");
  require(inspect_vikey_card_media(session) ==
              VikeyCardMediaState::legacy_p1_or_foreign_prefix,
          "partial final reset write did not remain explicitly retryable");

  state.fail_write_call = std::numeric_limits<std::size_t>::max();
  state.bytes_before_failure = 0;
  reset_vikey_development_legacy_media(
      session, VikeyCardMediaState::legacy_p1_or_foreign_prefix);
  require(inspect_vikey_card_media(session) == VikeyCardMediaState::blank,
          "retry did not finish the interrupted development reset");

  state.data.fill(std::byte{});
  std::ranges::fill_n(state.data.begin(),
                      usb_vault::card::vikey::kVikeyBlobBytes,
                      std::byte{0xa5});
  state.fail_write_call = state.writes + 2;
  state.bytes_before_failure = usb_vault::card::vikey::kVikeyBlobBytes;
  reset_vikey_development_legacy_media(
      session, VikeyCardMediaState::legacy_p1_or_foreign_prefix);
  require(inspect_vikey_card_media(session) == VikeyCardMediaState::blank,
          "verified full reset write failure was not accepted");

  require(!session.close(), "ViKey development reset session did not close");
  active_fake = nullptr;
}

void test_reserved_journal_snapshot_is_read_only() {
  FakeSdk state;
  state.data.fill(std::byte{0xa5});
  state.data[0] = std::byte{0x11};
  state.data[735] = std::byte{0x22};
  state.data[736] = std::byte{0x33};
  state.data[1807] = std::byte{0x44};
  state.data[1808] = std::byte{0x55};
  active_fake = &state;
  SecureBuffer user_sn(usb_vault::card::vikey::kVikeyUserSnBytes);
  SecureBuffer api_key(usb_vault::card::vikey::kVikeyApiKeyBytes);
  SecureBuffer pin(usb_vault::card::vikey::kVikeyUserPinBytes);
  std::ranges::fill(user_sn.bytes(), std::byte{'A'});
  std::ranges::fill(api_key.bytes(), std::byte{'B'});
  std::ranges::fill(pin.bytes(), std::byte{'P'});
  auto session = VikeySession::open(functions(), user_sn, api_key, pin);
  const auto before = state.data;
  const auto snapshot = usb_vault::card::vikey::snapshot_vikey_card_journal(session);
  constexpr auto size = usb_vault::card::kCardRecordJournalV2StorageSize;
  require(snapshot.size() == size && std::ranges::equal(snapshot.bytes(), std::span(state.data).first(size)),
          "reserved journal snapshot did not preserve ciphertext bytes");
  require(state.reads == 1 && state.writes == 0 && state.data == before,
          "reserved journal snapshot normalized or modified media");
  state.reported_capacity = static_cast<sdk::Length>(size - 1);
  require_card_error([&] {
    static_cast<void>(usb_vault::card::vikey::snapshot_vikey_card_journal(session));
  }, CardError::invalid_configuration, "undersized snapshot was accepted");
  require(state.reads == 1 && state.writes == 0, "undersized snapshot accessed card bytes");
  state.reported_capacity = static_cast<sdk::Length>(size);
  require(usb_vault::card::vikey::snapshot_vikey_card_journal(session).size() == size,
          "exactly sized journal area was rejected");
  require(state.writes == 0 && state.data == before, "snapshot changed out-of-region bytes");
  ++state.hardware_id;
  const auto reads_before_swap = state.reads;
  require_card_error([&] {
    static_cast<void>(usb_vault::card::vikey::snapshot_vikey_card_journal(session));
  }, CardError::device_failure, "journal snapshot accepted swapped hardware identity");
  require(state.reads == reads_before_swap && state.writes == 0,
          "swapped card was read or written");
  require(!session.close(), "journal snapshot session failed to close");
  active_fake = nullptr;
}

void test_conditional_journal_blocks() {
  using usb_vault::card::vikey::write_vikey_card_journal_block;
  FakeSdk state;
  state.data.fill(std::byte{0xa5});
  active_fake = &state;
  SecureBuffer user_sn(usb_vault::card::vikey::kVikeyUserSnBytes);
  SecureBuffer api_key(usb_vault::card::vikey::kVikeyApiKeyBytes);
  SecureBuffer pin(usb_vault::card::vikey::kVikeyUserPinBytes);
  std::ranges::fill(user_sn.bytes(), std::byte{'A'});
  std::ranges::fill(api_key.bytes(), std::byte{'B'});
  std::ranges::fill(pin.bytes(), std::byte{'P'});
  auto session = VikeySession::open(functions(), user_sn, api_key, pin);
  const auto snapshot = [&] {
    SecureBuffer bytes(1808);
    std::ranges::copy_n(state.data.begin(), bytes.size(), bytes.bytes().begin());
    return bytes;
  };
  constexpr std::array<std::pair<std::size_t, std::size_t>, 8> blocks{{
      {0, 152}, {152, 152}, {304, 216}, {520, 216},
      {736, 152}, {888, 152}, {1040, 384}, {1424, 384}}};
  for (const auto [offset, size] : blocks) {
    const auto expected = snapshot();
    auto result = state.data;
    SecureBuffer replacement(size);
    std::ranges::fill(replacement.bytes(), std::byte{0x39});
    std::ranges::copy(replacement.bytes(), result.begin() + offset);
    const auto writes = state.writes;
    write_vikey_card_journal_block(session, expected, offset, replacement);
    require(state.writes == writes + 1 && state.data == result &&
                state.write_addresses.back() == offset && state.write_lengths.back() == size,
            "conditional write changed more than its fixed block");
    require(replacement.bytes().front() == std::byte{0x39}, "vendor mutated caller replacement");
  }
  const auto expected = snapshot();
  SecureBuffer replacement(152);
  std::ranges::fill(replacement.bytes(), std::byte{0x42});
  auto writes = state.writes;
  state.data[1807] ^= std::byte{1};
  require_card_error([&] { write_vikey_card_journal_block(session, expected, 0, replacement); },
      CardError::invalid_operation, "change outside target did not expire snapshot");
  state.data[1807] ^= std::byte{1};
  SecureBuffer short_snapshot(1807);
  require_card_error([&] { write_vikey_card_journal_block(session, short_snapshot, 0, replacement); },
      CardError::invalid_configuration, "short conditional snapshot accepted");
  for (const auto offset : {1u, 304u, 735u, 1808u, std::numeric_limits<unsigned int>::max()}) {
    require_card_error([&] { write_vikey_card_journal_block(session, expected, offset, replacement); },
        CardError::invalid_configuration, "arbitrary journal write range accepted");
  }
  state.reported_capacity = 1807;
  require_card_error([&] { write_vikey_card_journal_block(session, expected, 0, replacement); },
      CardError::invalid_configuration, "undersized conditional storage accepted");
  state.reported_capacity = static_cast<sdk::Length>(state.data.size());
  state.swap_after_read = true;
  require_card_error([&] { write_vikey_card_journal_block(session, expected, 0, replacement); },
      CardError::device_failure, "swap after comparison was accepted");
  state.swap_after_read = false;
  --state.hardware_id;
  require(state.writes == writes, "rejected conditional writes touched media");
  const auto before = state.data;
  for (const auto length : {0u, 76u, 152u}) {
    state.data = before;
    state.fail_write_call = state.writes;
    state.bytes_before_failure = length;
    writes = state.writes;
    require_card_error([&] { write_vikey_card_journal_block(session, expected, 0, replacement); },
        CardError::communication_failed, "indeterminate write was reported as successful");
    auto partial = before;
    std::ranges::copy_n(replacement.bytes().begin(), length, partial.begin());
    require(state.writes == writes + 1 && state.data == partial,
            "indeterminate write retried or changed unexpected bytes");
  }
  state.fail_write_call = std::numeric_limits<std::size_t>::max();
  state.data = before;
  state.corrupt_other_byte = true;
  require_card_error([&] { write_vikey_card_journal_block(session, expected, 0, replacement); },
      CardError::device_failure, "full-region readback missed corruption outside target");
  require(!session.close(), "conditional journal session failed to close");
  active_fake = nullptr;
}

}  // namespace

int main() {
  try {
    test_random_access_card_record_path();
    test_reserved_journal_snapshot_is_read_only();
    test_conditional_journal_blocks();
    test_read_only_media_classification();
    test_media_bootstrap_revalidates_before_writing();
    test_development_legacy_reset_is_explicit_and_recoverable();
    std::cout << "vikey card record IO tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    active_fake = nullptr;
    std::cerr << "FAILED: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
