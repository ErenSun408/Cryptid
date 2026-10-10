#include "usb_vault/security/secure_buffer.h"
#include "usb_vault/volume/volume_provider.h"

#include "volume/locked_driver_buffer.h"
#include "volume/mount_request.h"
#include "volume/mount_label.h"
#include "volume/mount_identity.h"
#include "volume/mount_completion.h"
#include "volume/unmount_notifications.h"
#include <array>
#include "volume/unmount_retry_policy.h"
#include "volume/veracrypt_driver_abi.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using usb_vault::security::SecureBuffer;
using usb_vault::volume::HiddenVolumeProtection;
using usb_vault::volume::MountOptions;
using usb_vault::volume::Pkcs5Prf;
using usb_vault::volume::detail::LockedDriverBuffer;
using usb_vault::volume::detail::abi::MountRequest;

void require(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename Function>
void require_invalid_argument(Function&& function,
                              const char* const message) {
  try {
    function();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error(message);
}

SecureBuffer fixture_password(const std::size_t size,
                              const unsigned int seed = 1U) {
  SecureBuffer password(size);
  for (std::size_t index = 0; index < size; ++index) {
    password.bytes()[index] = static_cast<std::byte>((index + seed) & 0xffU);
  }
  return password;
}

void test_valid_mount_request() {
  LockedDriverBuffer<MountRequest> locked_request;
  MountRequest& request = locked_request.get();
  const SecureBuffer password = fixture_password(64);
  const MountOptions options{
      .drive_letter = L'w',
      .prf = Pkcs5Prf::sha512,
      .pim = 0,
      .read_only = false,
  };

  usb_vault::volume::detail::populate_mount_request(
      request, L"D:/vault/single.hc", password, options);

  require(request.drive_number == 22, "drive letter was not normalized");
  require(request.volume_path[2] == L'\\',
          "Windows path separators were not normalized for the driver");
  require(request.pkcs5_prf == 1, "SHA-512 PRF was not explicit");
  require(request.volume_password.length == 64,
          "password length was not preserved");
  for (std::size_t index = 0; index < password.size(); ++index) {
    require(request.volume_password.text[index] ==
                std::to_integer<std::uint8_t>(password.bytes()[index]),
            "password bytes were not preserved");
  }
  require(request.volume_password.text[64] == 0,
          "password terminator byte was not zero");
  require(request.cache_password == 0, "driver password caching was enabled");
  require(request.cache_pim == 0, "driver PIM caching was enabled");
  require(request.protect_hidden_volume == 0,
          "single-container mount enabled hidden-volume protection");
  require(request.mount_manager == 1, "mount manager was not enabled");
  require(request.preserve_timestamp == 1,
          "container timestamp preservation was not enabled");
  require(request.bytes_per_sector == 512,
          "logical sector size was not locked to the validated value");
  require(request.bytes_per_physical_sector == 512,
          "physical sector size was not locked to the validated value");
}

void test_mount_labels() {
  using usb_vault::volume::detail::mount_display_label;
  require(mount_display_label(L"work.hc") == L"work", "file extension leaked into label");
  for (const auto name : {L"work.sys", L"work.DAT", L"work.bin"})
    require(mount_display_label(name) == L"work", "new file suffix leaked into label");
  require(mount_display_label(L"资料:归档.HC") == L"资料_归档", "label sanitization failed");
  require(mount_display_label(L"1234567890\U0001F512") == L"1234567890", "label split a surrogate pair");
  require(mount_display_label(L"123456789\U0001F512") == L"123456789\U0001F512", "valid surrogate pair was lost");
  require(mount_display_label(L"   ") == L"SecureDrive", "blank label has no bounded fallback");
  const auto password = fixture_password(64);
  for (const auto& label : {std::wstring{}, std::wstring(L"工作资料"), std::wstring(32, L'x')}) {
    LockedDriverBuffer<MountRequest> request;
    const MountOptions options{.drive_letter=L'W', .label=label};
    usb_vault::volume::detail::populate_mount_request(request.get(), L"D:\\vault\\one.hc", password, options);
    require(std::wstring(request.get().label) == label && request.get().label[label.size()] == 0,
            "driver label was not copied and terminated");
  }
  for (const auto& label : {std::wstring(33,L'x'), std::wstring(L"a\0b",3), std::wstring(L"a\nb")}) {
    require_invalid_argument([&] {
      LockedDriverBuffer<MountRequest> request;
      const MountOptions options{.drive_letter=L'W', .label=label};
      usb_vault::volume::detail::populate_mount_request(request.get(), L"D:\\vault\\one.hc", password, options);
    }, "invalid driver label accepted");
  }
}

void test_invalid_mount_requests() {
  const MountOptions valid_options{.drive_letter = L'W'};
  const SecureBuffer valid_password = fixture_password(32);

  require_invalid_argument(
      [&] {
        LockedDriverBuffer<MountRequest> request;
        usb_vault::volume::detail::populate_mount_request(
            request.get(), L"relative.hc", valid_password, valid_options);
      },
      "relative path was accepted");

  require_invalid_argument(
      [&] {
        std::wstring embedded_null = L"D:\\vault\\one.hc";
        embedded_null.push_back(L'\0');
        embedded_null.append(L"other.hc");
        LockedDriverBuffer<MountRequest> request;
        usb_vault::volume::detail::populate_mount_request(
            request.get(), embedded_null, valid_password, valid_options);
      },
      "embedded null path was accepted");

  require_invalid_argument(
      [&] {
        std::wstring too_long = L"D:\\";
        too_long.append(usb_vault::volume::detail::abi::kMaxPath, L'x');
        LockedDriverBuffer<MountRequest> request;
        usb_vault::volume::detail::populate_mount_request(
            request.get(), too_long, valid_password, valid_options);
      },
      "overlong path was silently truncated");

  require_invalid_argument(
      [&] {
        LockedDriverBuffer<MountRequest> request;
        const SecureBuffer empty_password;
        usb_vault::volume::detail::populate_mount_request(
            request.get(), L"D:\\vault\\one.hc", empty_password,
            valid_options);
      },
      "empty password was accepted");

  require_invalid_argument(
      [&] {
        LockedDriverBuffer<MountRequest> request;
        const SecureBuffer too_long_password = fixture_password(129);
        usb_vault::volume::detail::populate_mount_request(
            request.get(), L"D:\\vault\\one.hc", too_long_password,
            valid_options);
      },
      "overlong password was accepted");

  require_invalid_argument(
      [&] {
        LockedDriverBuffer<MountRequest> request;
        MountOptions options = valid_options;
        options.drive_letter = L'1';
        usb_vault::volume::detail::populate_mount_request(
            request.get(), L"D:\\vault\\one.hc", valid_password, options);
      },
      "invalid drive letter was accepted");

  require_invalid_argument(
      [&] {
        LockedDriverBuffer<MountRequest> request;
        MountOptions options = valid_options;
        options.prf = static_cast<Pkcs5Prf>(0);
        usb_vault::volume::detail::populate_mount_request(
            request.get(), L"D:\\vault\\one.hc", valid_password, options);
      },
      "automatic PRF probing was accepted");

  require_invalid_argument(
      [&] {
        LockedDriverBuffer<MountRequest> request;
        MountOptions options = valid_options;
        options.pim = usb_vault::volume::detail::abi::kMaxPimValue + 1;
        usb_vault::volume::detail::populate_mount_request(
            request.get(), L"D:\\vault\\one.hc", valid_password, options);
      },
      "out-of-range PIM was accepted");

  require_invalid_argument(
      [&] {
        LockedDriverBuffer<MountRequest> request;
        MountOptions options = valid_options;
        options.hidden_volume_protection = HiddenVolumeProtection{};
        usb_vault::volume::detail::populate_mount_request(
            request.get(), L"D:\\vault\\one.hc", valid_password, options);
      },
      "protection without a password was accepted");

  require_invalid_argument(
      [&] {
        LockedDriverBuffer<MountRequest> request;
        const SecureBuffer empty_password;
        MountOptions options = valid_options;
        options.hidden_volume_protection =
            HiddenVolumeProtection{.password = &empty_password};
        usb_vault::volume::detail::populate_mount_request(
            request.get(), L"D:\\vault\\one.hc", valid_password, options);
      },
      "empty protection password was accepted");

  require_invalid_argument(
      [&] {
        LockedDriverBuffer<MountRequest> request;
        const SecureBuffer too_long_password = fixture_password(129);
        MountOptions options = valid_options;
        options.hidden_volume_protection =
            HiddenVolumeProtection{.password = &too_long_password};
        usb_vault::volume::detail::populate_mount_request(
            request.get(), L"D:\\vault\\one.hc", valid_password, options);
      },
      "overlong protection password was accepted");

  require_invalid_argument(
      [&] {
        LockedDriverBuffer<MountRequest> request;
        MountOptions options = valid_options;
        options.hidden_volume_protection = HiddenVolumeProtection{
            .password = &valid_password, .prf = static_cast<Pkcs5Prf>(0)};
        usb_vault::volume::detail::populate_mount_request(
            request.get(), L"D:\\vault\\one.hc", valid_password, options);
      },
      "automatic PRF probing was accepted for the protected hidden volume");

  require_invalid_argument(
      [&] {
        LockedDriverBuffer<MountRequest> request;
        MountOptions options = valid_options;
        options.hidden_volume_protection = HiddenVolumeProtection{
            .password = &valid_password,
            .pim = usb_vault::volume::detail::abi::kMaxPimValue + 1};
        usb_vault::volume::detail::populate_mount_request(
            request.get(), L"D:\\vault\\one.hc", valid_password, options);
      },
      "out-of-range protection PIM was accepted");
}

void test_protected_mount_request() {
  const SecureBuffer outer_password = fixture_password(48);
  const SecureBuffer hidden_password = fixture_password(72, 0x40U);

  LockedDriverBuffer<MountRequest> locked_request;
  MountRequest& request = locked_request.get();
  const MountOptions options{
      .drive_letter = L'Z',
      .prf = Pkcs5Prf::sha512,
      .pim = 0,
      .read_only = false,
      .notify_shell = false,
      .hidden_volume_protection =
          HiddenVolumeProtection{.password = &hidden_password},
      .label = L"work",
  };

  usb_vault::volume::detail::populate_mount_request(
      request, L"D:\\vault\\pair.hc", outer_password, options);

  require(request.protect_hidden_volume == 1,
          "a maintenance mount left hidden-volume protection off");
  require(std::wstring(request.label) == L"work", "protected mount omitted label");
  require(request.protected_hidden_volume_password.length == 72,
          "protection password length was not preserved");
  for (std::size_t index = 0; index < hidden_password.size(); ++index) {
    require(request.protected_hidden_volume_password.text[index] ==
                std::to_integer<std::uint8_t>(hidden_password.bytes()[index]),
            "protection password bytes were not preserved");
  }
  require(request.protected_hidden_volume_password.text[72] == 0,
          "protection password terminator byte was not zero");
  require(request.protected_hidden_volume_pkcs5_prf == 1,
          "SHA-512 PRF was not explicit for the protected hidden volume");

  // A request that swapped the slots would mount the wrong volume or protect
  // nothing, and the driver refuses both the same way.
  require(request.volume_password.length == 48,
          "the mounted volume password was displaced by the protection one");
  for (std::size_t index = 0; index < outer_password.size(); ++index) {
    require(request.volume_password.text[index] ==
                std::to_integer<std::uint8_t>(outer_password.bytes()[index]),
            "the mounted volume password bytes were not preserved");
  }

  // The product always uses PIM 0; this pins which field each one reaches,
  // since they sit 4 bytes apart.
  LockedDriverBuffer<MountRequest> locked_pim_request;
  MountRequest& pim_request = locked_pim_request.get();
  MountOptions pim_options = options;
  pim_options.hidden_volume_protection =
      HiddenVolumeProtection{.password = &hidden_password, .pim = 485};

  usb_vault::volume::detail::populate_mount_request(
      pim_request, L"D:\\vault\\pair.hc", outer_password, pim_options);

  require(pim_request.protected_hidden_volume_pim == 485,
          "protection PIM did not reach the protected hidden volume");
  require(pim_request.volume_pim == 0,
          "protection PIM displaced the mounted volume PIM");
}

void test_mount_identity_validation() {
  using usb_vault::volume::detail::decode_mount_identity;
  usb_vault::volume::detail::abi::VolumeProperties p{};
  p.drive_number = 24;
  p.unique_id = 0; // The driver's first mount is numbered zero.
  p.volume_path[0] = L'D';
  p.volume_path[1] = L':';
  p.disk_length = 1024;
  p.pkcs5_prf = 1;
  p.hidden_protection = 1;
  const auto valid = decode_mount_identity(p, 24);
  require(valid.protected_outer_writable && valid.unique_id == 0 &&
          valid.drive_letter == L'Y', "valid protected mount identity rejected");
  auto bad = p;
  bad.unique_id = -1;
  require_invalid_argument([&]{decode_mount_identity(bad, 24);}, "negative identity accepted");
  require_invalid_argument([&]{decode_mount_identity(p, 23);}, "another drive accepted");
  bad = p; bad.disk_length = 0;
  require_invalid_argument([&]{decode_mount_identity(bad, 24);}, "empty volume accepted");
  bad = p; std::fill(std::begin(bad.volume_path), std::end(bad.volume_path), L'x');
  require_invalid_argument([&]{decode_mount_identity(bad, 24);}, "unterminated path accepted");
  bad = p; bad.hidden_protection = 3;
  require_invalid_argument([&]{decode_mount_identity(bad, 24);}, "unknown protection accepted");
  bad = p; bad.read_only = 2;
  require_invalid_argument([&]{decode_mount_identity(bad, 24);}, "malformed boolean accepted");
  for (const auto field : {&decltype(p)::hidden_volume, &decltype(p)::read_only,
       &decltype(p)::partition_in_inactive_system_scope, &decltype(p)::mount_disabled,
       &decltype(p)::volume_pim}) {
    bad = p; bad.*field = 1;
    require(!decode_mount_identity(bad, 24).protected_outer_writable,
            "unsupported mount marked writable protected outer");
  }
  for (const int protection : {0, 2}) {
    bad = p; bad.hidden_protection = protection;
    require(!decode_mount_identity(bad, 24).protected_outer_writable,
            "unprotected or triggered volume accepted");
  }
  bad = p; bad.unique_id = 1;
  require(decode_mount_identity(bad, 24) != valid, "remount identity was ignored");
  bad = p; bad.header_flags = 1;
  require(!decode_mount_identity(bad, 24).protected_outer_writable, "header flags ignored");
  bad = p; bad.pkcs5_prf = 2;
  require(!decode_mount_identity(bad, 24).protected_outer_writable, "unsupported PRF ignored");
}

void test_mount_completion_retains_ownership_and_clears_secrets() {
  using namespace usb_vault::volume;
  using usb_vault::volume::detail::complete_mount;
  const auto password = fixture_password(64);
  const auto protection = fixture_password(64, 3);
  const auto cleared = [](const MountRequest& request) {
    const auto raw = std::as_bytes(std::span(&request, 1));
    return std::ranges::all_of(raw, [](std::byte b) { return b == std::byte{}; });
  };
  for (wchar_t drive = L'A'; drive <= L'Z'; ++drive) {
    LockedDriverBuffer<MountRequest> buffer;
    detail::populate_mount_request(buffer.get(), L"D:\\vault\\pair.hc", password,
        {.drive_letter = drive, .hidden_volume_protection = HiddenVolumeProtection{.password = &protection}});
    unsigned calls = 0;
    const auto result = complete_mount(buffer, [&](MountRequest& request) {
      ++calls;
      require(request.volume_password.length == 64 && request.protected_hidden_volume_password.length == 64,
              "mount completion cleared credentials before the transaction");
      request.return_code = 0;
      request.filesystem_dirty = 1;
      request.read_only_after_access_denied = 1;
      request.read_only_after_device_write_protected = 1;
      request.is_ntfs = 1;
      request.master_key_vulnerable = 1;
    });
    require(calls == 1 && result.drive_letter == drive,
            "successful mount lost ownership or repeated the mount transaction");
    require(result.filesystem_dirty && result.read_only_after_access_denied &&
                result.read_only_after_device_write_protected && result.is_ntfs && result.master_key_vulnerable,
            "mount completion lost driver result flags");
    require(cleared(buffer.get()), "successful mount retained credentials");
  }
  for (int scenario = 0; scenario < 3; ++scenario) {
    LockedDriverBuffer<MountRequest> buffer;
    detail::populate_mount_request(buffer.get(), L"D:\\vault\\pair.hc", password, {.drive_letter = L'K'});
    bool rejected = false;
    try {
      static_cast<void>(complete_mount(buffer, [&](MountRequest& request) {
        if (scenario == 0) throw std::system_error(std::make_error_code(std::errc::io_error));
        request.return_code = scenario == 1 ? 3 : 6;
      }));
    } catch (const std::system_error& error) {
      rejected = true;
      const auto expected = scenario == 0 ? std::make_error_code(std::errc::io_error)
          : make_error_code(scenario == 1 ? DriverError::authentication_failed : DriverError::volume_busy);
      require(error.code() == expected, "mount completion changed the failure category");
    }
    require(rejected && cleared(buffer.get()), "failed mount succeeded or retained credentials");
  }
  for (const int drive : {-1, 26}) {
    LockedDriverBuffer<MountRequest> buffer;
    buffer.get().drive_number = drive;
    bool called = false;
    require_invalid_argument([&] {
      static_cast<void>(complete_mount(buffer, [&](MountRequest&) { called = true; }));
    }, "invalid drive reached the mount transaction");
    require(!called && cleared(buffer.get()), "invalid mount was sent or retained its request");
  }
}

void test_driver_error_codes() {
  const std::error_code busy =
      usb_vault::volume::make_error_code(
          usb_vault::volume::DriverError::volume_busy);
  require(busy.value() == 6, "busy driver error value changed");
  require(busy.category() == usb_vault::volume::driver_error_category(),
          "busy driver error category changed");
  require(busy.message() == "volume still has open files",
          "busy driver error message changed");
}

void test_unmount_refresh() {
  using namespace usb_vault::volume::detail;
  using P = DrivePresence;
  struct Scenario {
    std::array<P, 4> states;
    int probes, completions, refreshes, pauses;
  };
  const Scenario scenarios[] = {
    {{P::absent, P::absent, P::absent, P::absent}, 4, 1, 4, 3},
    {{P::present, P::present, P::absent, P::absent}, 4, 1, 2, 3},
    {{P::present, P::present, P::present, P::present}, 4, 0, 0, 3},
    {{P::absent, P::present, P::absent, P::absent}, 2, 1, 1, 1},
    {{P::unknown, P::absent, P::absent, P::absent}, 1, 0, 0, 0},
    {{P::present, P::unknown, P::absent, P::absent}, 2, 0, 0, 1},
    {{P::absent, P::unknown, P::absent, P::absent}, 2, 1, 1, 1},
    {{P::present, P::present, P::present, P::absent}, 4, 1, 1, 3},
  };
  for (const auto& scenario : scenarios) {
    int probes = 0, completions = 0, refreshes = 0, pauses = 0;
    bool premature = false;
    refresh_unmounted_drive([&]() noexcept { return scenario.states[probes++]; },
        [&]() noexcept { ++completions; premature |= scenario.states[probes - 1] != P::absent; },
        [&]() noexcept { ++refreshes; premature |= scenario.states[probes - 1] != P::absent; },
        [&]() noexcept { ++pauses; });
    require(!premature && probes == scenario.probes && completions == scenario.completions &&
            refreshes == scenario.refreshes && pauses == scenario.pauses,
            "unmount refresh ignored disappearance, reuse, query failure or retry bound");
  }
}

void test_unmount_notifications() {
  using namespace usb_vault::volume;
  using detail::UnmountNotification;
  // A simulated Explorer handle is released only by the pending notification.
  // Without that notification the same normal driver operation rejects as busy.
  for (const bool enabled : {false, true}) {
    for (const bool busy : {false, true}) {
      for (const bool present : {false, true}) {
        int events[4]{};
        int count = 0;
        int probes = 0;
        bool shell_handle = true;
        bool rejected = false;
        try {
          detail::unmount_with_notifications(enabled, [&] {
            events[count++] = 2;
            if (busy || shell_handle)
              throw std::system_error(make_error_code(DriverError::volume_busy));
          }, [&](UnmountNotification event) noexcept {
            if (event == UnmountNotification::pending) {
              events[count++] = 1;
              shell_handle = false;
            } else {
              events[count++] = event == UnmountNotification::complete ? 3 : 4;
            }
          }, [&]() noexcept { ++probes; return present; });
        } catch (const std::system_error& error) {
          rejected = true;
          require(error.code() == make_error_code(DriverError::volume_busy),
                  "notification cleanup replaced driver error");
        }
        require(rejected == (busy || !enabled), "normal unmount ignored an open handle");
        if (!enabled) {
          require(count == 1 && events[0] == 2 && probes == 0,
                  "silent cleanup notified Shell or probed drives");
        } else if (!busy) {
          require(count == 3 && events[0] == 1 && events[1] == 2 && events[2] == 3 && probes == 0,
                  "Shell was not released before normal unmount or completion was missing");
        } else {
          require(probes == 1 && count == (present ? 3 : 2) && events[0] == 1 && events[1] == 2,
                  "failed unmount lost notification ordering or drive probe");
          if (present) require(events[2] == 4, "remaining drive was not restored");
        }
      }
    }
  }
  // Transport errors also restore a still-present drive and retain their identity.
  int restores = 0;
  bool rejected = false;
  try {
    detail::unmount_with_notifications(true, [] {
      throw std::system_error(std::make_error_code(std::errc::io_error));
    }, [&](UnmountNotification event) noexcept {
      if (event == UnmountNotification::restored) ++restores;
    }, []() noexcept { return true; });
  } catch (const std::system_error& error) {
    rejected = error.code() == std::make_error_code(std::errc::io_error);
  }
  require(rejected && restores == 1, "transport failure did not restore the remaining drive");
}

void test_refresh_only_after_successful_notified_unmount() {
  using namespace usb_vault::volume::detail;
  for (bool enabled : {false, true}) {
    for (bool fail : {false, true}) {
      int probes = 0, completions = 0, refreshes = 0;
      bool rejected = false;
      try {
        unmount_with_notifications(enabled, [&] {
          if (fail) throw std::runtime_error("unmount failed");
        }, [&](UnmountNotification event) noexcept {
          if (event == UnmountNotification::complete)
            refresh_unmounted_drive([&]() noexcept { ++probes; return DrivePresence::absent; },
                [&]() noexcept { ++completions; }, [&]() noexcept { ++refreshes; }, []() noexcept {});
        }, []() noexcept { return true; });
      } catch (const std::runtime_error&) { rejected = true; }
      require(rejected == fail && probes == (enabled && !fail ? 4 : 0) &&
              completions == int(enabled && !fail) && refreshes == probes,
              "failed or silent unmount entered completion refresh");
    }
  }
}

void test_notification_count_across_actual_retry_policy() {
  using namespace usb_vault::volume;
  using detail::UnmountNotification;
  struct Provider {
    mutable int pending{}, complete{}, restored{}, attempts{}, forced{}, probes{};
    int failures{3};
    bool present{true};
    bool transport_error{false};
    UnmountResult unmount(wchar_t drive, const UnmountOptions& options) const {
      require(drive == L'W' && !options.notify_shell,
              "inner retry changed target or repeated Shell notifications");
      ++attempts;
      if (options.force) ++forced;
      if (transport_error) throw std::system_error(std::make_error_code(std::errc::io_error));
      if (attempts <= failures) throw std::system_error(make_error_code(DriverError::volume_busy));
      return {};
    }
    void run_unmount_batch(wchar_t drive, bool enabled,
                           const std::function<void()>& operation) const {
      require(drive == L'W', "batch changed drive");
      detail::unmount_with_notifications(enabled, operation,
          [&](UnmountNotification event) noexcept {
            if (event == UnmountNotification::pending) ++pending;
            if (event == UnmountNotification::complete) ++complete;
            if (event == UnmountNotification::restored) ++restored;
          }, [&]() noexcept { ++probes; return present; });
    }
  };
  for (bool enabled : {false, true}) {
    Provider provider;
    const auto result = detail::retry_unmount(provider, L'W', {.notify_shell = enabled});
    require(result.attempts == 4 && provider.attempts == 4 && provider.forced == 0,
            "normal retry changed attempt count or escalated");
    require(provider.pending == int(enabled) && provider.complete == int(enabled) &&
            provider.restored == 0 && provider.probes == 0,
            "retry success repeated or rolled back notifications");
  }
  for (bool present : {false, true}) {
    Provider provider;
    provider.failures = 1000;
    provider.present = present;
    bool rejected = false;
    try { static_cast<void>(detail::retry_unmount(provider, L'W', {})); }
    catch (const std::system_error& error) {
      rejected = error.code() == make_error_code(DriverError::volume_busy);
    }
    require(rejected && provider.attempts > 1 && provider.forced == 0 &&
            provider.pending == 1 && provider.complete == 0 &&
            provider.restored == int(present) && provider.probes == 1,
            "exhausted retries repeated notifications or lost final rollback");
  }
  for (bool escalation : {false, true}) {
    Provider provider;
    provider.transport_error = true;
    bool rejected = false;
    try {
      if (escalation) static_cast<void>(detail::escalate_unmount(provider, L'W', {}));
      else static_cast<void>(detail::retry_unmount(provider, L'W', {}));
    } catch (const std::system_error& error) {
      rejected = error.code() == std::make_error_code(std::errc::io_error);
    }
    require(rejected && provider.attempts == 1 && provider.forced == 0 &&
            provider.pending == 1 && provider.complete == 0 && provider.restored == 1,
            "transport failure was retried, forced, or notified twice");
  }
  for (int failures : {0, 1, 2}) {
    Provider provider;
    provider.failures = failures;
    bool rejected = false;
    try {
      const auto result = detail::escalate_unmount(provider, L'W', {});
      require(result.forced == (failures == 1), "incorrect escalation result");
    } catch (const std::system_error& error) {
      rejected = error.code() == make_error_code(DriverError::volume_busy);
    }
    require(rejected == (failures == 2) && provider.pending == 1 &&
            provider.attempts == (failures == 0 ? 1 : 2) &&
            provider.forced == (failures == 0 ? 0 : 1) &&
            provider.complete == int(!rejected) && provider.restored == int(rejected),
            "escalation notified between attempts or lost final state");
  }
}

}  // namespace

int main() {
  try {
    test_notification_count_across_actual_retry_policy();
    test_unmount_notifications();
    test_unmount_refresh();
    test_refresh_only_after_successful_notified_unmount();
    test_mount_completion_retains_ownership_and_clears_secrets();
    test_valid_mount_request();
    test_mount_labels();
    test_protected_mount_request();
    test_invalid_mount_requests();
    test_driver_error_codes();
    test_mount_identity_validation();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }

  return 0;
}
