#include "platform/windows/hid_card_device_notifications.h"

#include <hidsdi.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace {

using usb_vault::application::CardPresenceController;
using usb_vault::application::CardPresenceTransition;
using usb_vault::application::CardDeviceLifecycleController;
using usb_vault::application::CardEntryContextController;
using usb_vault::application::CardEntryMediaEvidence;
using usb_vault::application::CardEntryRoute;
using usb_vault::application::IApplicationSessionActions;
using usb_vault::application::ICardEntryInspectionSource;
using usb_vault::application::IReadOnlyCardEntryInspection;
using usb_vault::card::CardRecordState;
using usb_vault::platform::windows::HidDeviceIdentity;
using usb_vault::platform::windows::handle_hid_device_change;
using usb_vault::platform::windows::matches_hid_device_path;
using usb_vault::security::SecureBuffer;

constexpr HidDeviceIdentity kTarget{.vendor_id = 0x1987,
                                    .product_id = 0x0331};

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

class FakeSession final : public IApplicationSessionActions {
 public:
  void authenticate_and_mount(SecureBuffer) override { ++authenticate_calls; }
  void unmount_active() override {
    ++unmount_calls;
    mounted_state = false;
  }
  [[nodiscard]] bool force_unmount_active() override {
    ++force_unmount_calls;
    mounted_state = false;
    return false;
  }
  [[nodiscard]] bool mounted() const noexcept override {
    return mounted_state;
  }

  bool mounted_state{};
  int authenticate_calls{};
  int unmount_calls{};
  int force_unmount_calls{};
};

class FakeInspection final : public IReadOnlyCardEntryInspection {
 public:
  [[nodiscard]] CardEntryMediaEvidence inspect_media() override {
    return CardEntryMediaEvidence::product_record;
  }

  [[nodiscard]] CardRecordState
  inspect_product_record_read_only() override {
    return CardRecordState::initialized_v1;
  }
};

class FakeSource final : public ICardEntryInspectionSource {
 public:
  [[nodiscard]] std::unique_ptr<IReadOnlyCardEntryInspection>
  inspect_inserted_card() override {
    ++calls;
    return std::make_unique<FakeInspection>();
  }

  int calls{};
};

class BroadcastPayload final {
 public:
  explicit BroadcastPayload(const std::wstring_view path) {
    const std::size_t required =
        offsetof(DEV_BROADCAST_DEVICEINTERFACE_W, dbcc_name) +
        (path.size() + 1) * sizeof(wchar_t);
    require(required <= storage_.size(), "test interface path is too long");
    auto* const data = get();
    data->dbcc_size = static_cast<DWORD>(required);
    data->dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    ::HidD_GetHidGuid(&data->dbcc_classguid);
    std::memcpy(data->dbcc_name, path.data(),
                path.size() * sizeof(wchar_t));
    data->dbcc_name[path.size()] = L'\0';
  }

  [[nodiscard]] DEV_BROADCAST_DEVICEINTERFACE_W* get() noexcept {
    return reinterpret_cast<DEV_BROADCAST_DEVICEINTERFACE_W*>(
        storage_.data());
  }

  [[nodiscard]] LPARAM parameter() noexcept {
    return reinterpret_cast<LPARAM>(get());
  }

 private:
  alignas(DEV_BROADCAST_DEVICEINTERFACE_W)
      std::array<std::byte, 1024> storage_{};
};

void test_exact_case_insensitive_path_matching() {
  require(matches_hid_device_path(
              LR"(\\?\hid#VID_1987&PID_0331&mi_00#fixture)", kTarget),
          "valid target path was rejected");
  require(!matches_hid_device_path(
              LR"(\\?\hid#vid_1987&pid_03310#fixture)", kTarget),
          "overlong product token was accepted");
  require(!matches_hid_device_path(
              LR"(\\?\hid#vid_1987&pid_0332#fixture)", kTarget),
          "wrong product was accepted");
  require(!matches_hid_device_path(
              LR"(\\?\hid#vid_1988&pid_0331#fixture)", kTarget),
          "wrong vendor was accepted");
  require(!matches_hid_device_path(
              LR"(\\?\hid#prefix_vid_1987&pid_0331#fixture)", kTarget),
          "embedded vendor substring was accepted");
}

void test_target_arrival_and_removal_are_routed() {
  FakeSession session;
  CardPresenceController controller(session);
  BroadcastPayload payload(
      LR"(\\?\hid#vid_1987&pid_0331&mi_00#fixture)");
  require(handle_hid_device_change(controller, DBT_DEVICEARRIVAL,
                                   payload.parameter(), kTarget) ==
              CardPresenceTransition::card_available,
          "target arrival was not routed");
  require(session.authenticate_calls == 0,
          "target arrival attempted authentication");
  session.mounted_state = true;
  require(handle_hid_device_change(controller, DBT_DEVICEREMOVECOMPLETE,
                                   payload.parameter(), kTarget) ==
              CardPresenceTransition::active_session_unmounted,
          "target removal was not routed");
  require(session.force_unmount_calls == 1 && !session.mounted_state,
          "target removal did not unmount once");
}

void test_unrelated_and_malformed_payloads_are_ignored() {
  FakeSession session;
  session.mounted_state = true;
  CardPresenceController controller(session, true);

  BroadcastPayload unrelated(
      LR"(\\?\hid#vid_1234&pid_0331&mi_00#fixture)");
  require(handle_hid_device_change(controller, DBT_DEVICEREMOVECOMPLETE,
                                   unrelated.parameter(), kTarget) ==
              CardPresenceTransition::no_change,
          "unrelated HID removal was routed");

  BroadcastPayload wrong_class(
      LR"(\\?\hid#vid_1987&pid_0331&mi_00#fixture)");
  wrong_class.get()->dbcc_classguid = GUID{};
  require(handle_hid_device_change(controller, DBT_DEVICEREMOVECOMPLETE,
                                   wrong_class.parameter(), kTarget) ==
              CardPresenceTransition::no_change,
          "non-HID class payload was routed");

  BroadcastPayload too_small(
      LR"(\\?\hid#vid_1987&pid_0331&mi_00#fixture)");
  too_small.get()->dbcc_size = sizeof(DEV_BROADCAST_HDR);
  require(handle_hid_device_change(controller, DBT_DEVICEREMOVECOMPLETE,
                                   too_small.parameter(), kTarget) ==
              CardPresenceTransition::no_change,
          "undersized payload was routed");

  BroadcastPayload odd_size(
      LR"(\\?\hid#vid_1987&pid_0331&mi_00#fixture)");
  odd_size.get()->dbcc_size = static_cast<DWORD>(
      offsetof(DEV_BROADCAST_DEVICEINTERFACE_W, dbcc_name) + 3);
  require(handle_hid_device_change(controller, DBT_DEVICEREMOVECOMPLETE,
                                   odd_size.parameter(), kTarget) ==
              CardPresenceTransition::no_change,
          "odd-sized UTF-16 payload was routed");

  BroadcastPayload oversized(
      LR"(\\?\hid#vid_1987&pid_0331&mi_00#fixture)");
  oversized.get()->dbcc_size = static_cast<DWORD>(
      offsetof(DEV_BROADCAST_DEVICEINTERFACE_W, dbcc_name) +
      32769U * sizeof(wchar_t));
  require(handle_hid_device_change(controller, DBT_DEVICEREMOVECOMPLETE,
                                   oversized.parameter(), kTarget) ==
              CardPresenceTransition::no_change,
          "oversized interface payload was routed");

  BroadcastPayload unterminated(
      LR"(\\?\hid#vid_1987&pid_0331&mi_00#fixture)");
  const std::size_t last =
      (unterminated.get()->dbcc_size -
       offsetof(DEV_BROADCAST_DEVICEINTERFACE_W, dbcc_name)) /
          sizeof(wchar_t) -
      1;
  unterminated.get()->dbcc_name[last] = L'x';
  require(handle_hid_device_change(controller, DBT_DEVICEREMOVECOMPLETE,
                                   unterminated.parameter(), kTarget) ==
              CardPresenceTransition::no_change,
          "unterminated payload was routed");

  require(controller.card_present() && session.force_unmount_calls == 0,
          "ignored payload changed session or presence state");
  require(handle_hid_device_change(controller, DBT_DEVNODES_CHANGED, 0,
                                   kTarget) ==
              CardPresenceTransition::no_change,
          "unrelated device event was routed");
}

void test_p2_lifecycle_overload_uses_the_same_strict_filter() {
  FakeSession session;
  FakeSource source;
  CardPresenceController presence(session);
  CardEntryContextController entry(source);
  CardDeviceLifecycleController lifecycle(presence, entry);
  BroadcastPayload target(
      LR"(\\?\hid#vid_1987&pid_0331&mi_00#fixture)");

  const auto arrival = handle_hid_device_change(
      lifecycle, DBT_DEVICEARRIVAL, target.parameter(), kTarget);
  require(arrival.presence == CardPresenceTransition::card_available &&
              arrival.entry_route == CardEntryRoute::login_required &&
              arrival.entry_changed && source.calls == 1,
          "P2 target arrival did not establish one entry context");

  BroadcastPayload unrelated(
      LR"(\\?\hid#vid_1234&pid_0331&mi_00#fixture)");
  const auto ignored = handle_hid_device_change(
      lifecycle, DBT_DEVICEREMOVECOMPLETE, unrelated.parameter(), kTarget);
  require(ignored.presence == CardPresenceTransition::no_change &&
              ignored.entry_route == CardEntryRoute::login_required &&
              !ignored.entry_changed && source.calls == 1 &&
              entry.has_context(),
          "unrelated HID event changed the P2 entry lifecycle");

  session.mounted_state = true;
  const auto removal = handle_hid_device_change(
      lifecycle, DBT_DEVICEREMOVECOMPLETE, target.parameter(), kTarget);
  require(removal.presence ==
              CardPresenceTransition::active_session_unmounted &&
              !removal.entry_route.has_value() && removal.entry_changed &&
              !entry.has_context() && session.force_unmount_calls == 1,
          "P2 target removal did not clear entry and unmount once");
}

}  // namespace

int main() {
  try {
    test_exact_case_insensitive_path_matching();
    test_target_arrival_and_removal_are_routed();
    test_unrelated_and_malformed_payloads_are_ignored();
    test_p2_lifecycle_overload_uses_the_same_strict_filter();
    std::cout << "hid_card_device_notifications_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "hid_card_device_notifications_tests failed: "
              << error.what() << '\n';
    return 1;
  }
}
