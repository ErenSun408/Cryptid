#include "usb_vault/platform/abi_checks.h"
#include "usb_vault/security/secure_buffer.h"

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <type_traits>
#include <utility>

namespace {

using usb_vault::security::SecureBuffer;

static_assert(!std::is_copy_constructible_v<SecureBuffer>);
static_assert(!std::is_copy_assignable_v<SecureBuffer>);
static_assert(std::is_nothrow_move_constructible_v<SecureBuffer>);
static_assert(std::is_nothrow_move_assignable_v<SecureBuffer>);
static_assert(std::is_nothrow_destructible_v<SecureBuffer>);

struct AbiFixture {
  std::uint32_t size;
  std::uint16_t flags;
  std::uint16_t reserved;
};

USB_VAULT_ASSERT_STANDARD_LAYOUT(AbiFixture);
USB_VAULT_ASSERT_SIZE(AbiFixture, 8);
USB_VAULT_ASSERT_OFFSET(AbiFixture, size, 0);
USB_VAULT_ASSERT_OFFSET(AbiFixture, flags, 4);
USB_VAULT_ASSERT_OFFSET(AbiFixture, reserved, 6);

[[noreturn]] void fail(const char* const message) {
  std::cerr << "FAILED: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void expect(const bool condition, const char* const message) {
  if (!condition) {
    fail(message);
  }
}

void expect_zeroed(const std::span<const std::byte> bytes,
                   const char* const message) {
  const bool all_zero = std::all_of(bytes.begin(), bytes.end(), [](const auto b) {
    return b == std::byte{0};
  });
  expect(all_zero, message);
}

void expect_locked(SecureBuffer& buffer, const char* const message) {
  PSAPI_WORKING_SET_EX_INFORMATION information{};
  information.VirtualAddress = buffer.data();

  const BOOL queried = ::QueryWorkingSetEx(
      ::GetCurrentProcess(), &information, sizeof(information));
  expect(queried != FALSE, "QueryWorkingSetEx must succeed");
  expect(information.VirtualAttributes.Valid != 0,
         "allocated buffer page must be resident");
  expect(information.VirtualAttributes.Locked != 0, message);
}

void test_default_and_zero_length_buffers_are_empty() {
  SecureBuffer default_buffer;
  expect(default_buffer.empty(), "default buffer must be empty");
  expect(default_buffer.size() == 0, "default buffer size must be zero");
  expect(default_buffer.data() == nullptr,
         "default buffer data must be null");

  SecureBuffer zero_length_buffer(0);
  expect(zero_length_buffer.empty(), "zero-length buffer must be empty");
  expect(zero_length_buffer.data() == nullptr,
         "zero-length buffer data must be null");
}

void test_allocation_is_zero_initialized_and_writable() {
  SecureBuffer buffer(64);
  expect(!buffer.empty(), "allocated buffer must not be empty");
  expect(buffer.size() == 64, "allocated buffer size must be preserved");
  expect(buffer.data() != nullptr, "allocated buffer data must not be null");
  expect_zeroed(buffer.bytes(), "new allocation must be zero initialized");
  expect_locked(buffer, "allocated buffer page must be locked");

  std::ranges::fill(buffer.bytes(), std::byte{0xA5});
  const bool all_written =
      std::ranges::all_of(buffer.bytes(), [](const auto b) {
        return b == std::byte{0xA5};
      });
  expect(all_written, "allocated buffer must be writable");
}

void test_clear_zeroes_without_reallocating() {
  SecureBuffer buffer(37);
  std::ranges::fill(buffer.bytes(), std::byte{0x5A});
  const auto* const original_data = buffer.data();

  buffer.clear();

  expect(buffer.data() == original_data, "clear must not reallocate");
  expect(buffer.size() == 37, "clear must preserve the fixed size");
  expect_zeroed(buffer.bytes(), "clear must erase every requested byte");
  expect_locked(buffer, "clear must not unlock the buffer page");
}

void test_move_construction_transfers_ownership() {
  SecureBuffer source(32);
  std::ranges::fill(source.bytes(), std::byte{0xC3});
  const auto* const original_data = source.data();

  SecureBuffer destination(std::move(source));

  expect(source.empty(), "moved-from source must be empty");
  expect(source.data() == nullptr, "moved-from source data must be null");
  expect(destination.data() == original_data,
         "move construction must transfer the allocation");
  expect(destination.size() == 32,
         "move construction must preserve the size");
}

void test_move_assignment_releases_destination_and_transfers_ownership() {
  SecureBuffer source(48);
  std::ranges::fill(source.bytes(), std::byte{0x7E});
  const auto* const source_data = source.data();
  SecureBuffer destination(16);

  destination = std::move(source);

  expect(source.empty(), "move-assigned source must be empty");
  expect(source.data() == nullptr,
         "move-assigned source data must be null");
  expect(destination.data() == source_data,
         "move assignment must transfer the source allocation");
  expect(destination.size() == 48,
         "move assignment must preserve the source size");
}

void test_self_move_assignment_preserves_buffer() {
  SecureBuffer buffer(24);
  std::ranges::fill(buffer.bytes(), std::byte{0x1D});
  const auto* const original_data = buffer.data();

#pragma warning(push)
#pragma warning(disable : 26800)
  buffer = std::move(buffer);
#pragma warning(pop)

  expect(buffer.data() == original_data,
         "self move assignment must preserve the allocation");
  expect(buffer.size() == 24, "self move assignment must preserve the size");
}

}  // namespace

int main() {
  test_default_and_zero_length_buffers_are_empty();
  test_allocation_is_zero_initialized_and_writable();
  test_clear_zeroes_without_reallocating();
  test_move_construction_transfers_ownership();
  test_move_assignment_releases_destination_and_transfers_ownership();
  test_self_move_assignment_preserves_buffer();

  std::cout << "secure_buffer_tests: PASS\n";
  return EXIT_SUCCESS;
}
