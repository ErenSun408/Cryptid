#include "usb_vault/volume/mount_state_monitor.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
using namespace std::chrono_literals;
using namespace usb_vault::volume;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
WatchedMount target(std::uint32_t id=1, wchar_t drive=L'M') { return {id,drive,42,L"\\??\\D:\\test.hc",1024}; }
MountIdentity identity(const WatchedMount& t) { return {t.drive_letter,t.unique_id,t.volume_path,t.disk_length}; }
struct Reader final : IMountStateReader {
  bool fail_inventory{};
  std::vector<MountedVolume> volumes;
  std::map<wchar_t,MountIdentity> identities;
  std::vector<wchar_t> queried;
  std::vector<MountedVolume> inventory() override {
    if (fail_inventory) throw std::runtime_error("driver unavailable");
    return volumes;
  }
  MountIdentity identity(wchar_t drive) override { queried.push_back(drive); return identities.at(drive); }
};
void test_observations() {
  Reader reader;
  const std::vector<WatchedMount> targets{target(),target(2,L'N')};
  reader.volumes={{.drive_letter=L'M'},{.drive_letter=L'N'},{.drive_letter=L'Z'}};
  reader.identities[L'M']=identity(targets[0]); reader.identities[L'N']=identity(targets[1]);
  auto values=inspect_mount_states(reader,targets);
  require(values[0].presence==MountPresence::present && values[1].presence==MountPresence::present, "live mounts lost");
  require(reader.queried==std::vector<wchar_t>{L'M',L'N'}, "unrelated mount inspected");
  reader.identities[L'M'].unique_id++;
  require(inspect_mount_states(reader,targets)[0].presence==MountPresence::replaced, "same path remount missed");
  reader.identities[L'M']=identity(targets[0]); reader.identities[L'M'].volume_path=L"other";
  require(inspect_mount_states(reader,targets)[0].presence==MountPresence::replaced, "reused drive missed");
  reader.identities[L'M']=identity(targets[0]); reader.identities[L'M'].disk_length++;
  require(inspect_mount_states(reader,targets)[0].presence==MountPresence::replaced, "length changed unnoticed");
  reader.identities[L'M']=identity(targets[0]); reader.identities[L'M'].protected_outer_writable=true;
  require(inspect_mount_states(reader,targets)[0].presence==MountPresence::present, "protection state mistaken for replacement");
  reader.identities.erase(L'M');
  values=inspect_mount_states(reader,targets);
  require(values[0].presence==MountPresence::unknown && values[1].presence==MountPresence::present, "per-drive error contaminated another volume");
  reader.volumes.erase(reader.volumes.begin());
  require(inspect_mount_states(reader,targets)[0].presence==MountPresence::missing, "external unmount missed");
  reader.volumes.clear();
  require(inspect_mount_states(reader,targets)[1].presence==MountPresence::missing, "empty inventory is not absence");
  reader.fail_inventory=true;
  values=inspect_mount_states(reader,targets);
  require(values[0].presence==MountPresence::unknown && values[1].presence==MountPresence::unknown, "failed inventory reported unmount");
  reader.fail_inventory=false; reader.volumes={{.drive_letter=L'M'},{.drive_letter=L'M'}};
  require(inspect_mount_states(reader,targets)[1].presence==MountPresence::unknown, "malformed inventory accepted");
  reader.volumes={{.drive_letter=L'M'}}; reader.identities[L'M']=identity(targets[0]); reader.identities[L'M'].drive_letter=L'Q';
  require(inspect_mount_states(reader,targets)[0].presence==MountPresence::unknown, "wrong-drive identity accepted");
}
MountStateUpdate await_update(MountStateMonitor& monitor) {
  const auto end=std::chrono::steady_clock::now()+2s;
  while (std::chrono::steady_clock::now()<end) {
    if (auto update=monitor.take_update()) return std::move(*update);
    std::this_thread::sleep_for(1ms);
  }
  throw std::runtime_error("monitor did not publish");
}
void test_polling_and_errors() {
  std::atomic<int> calls{}; std::atomic<int> mode{};
  const auto owner=std::this_thread::get_id(); std::atomic<bool> wrong_thread{};
  MountStateMonitor monitor([&](auto targets) {
    wrong_thread.store(std::this_thread::get_id()==owner); ++calls;
    if(mode==2) throw std::runtime_error("query failed");
    if(mode==3) return std::vector<MountObservation>{};
    return std::vector<MountObservation>{{targets[0].container_id,mode==1?MountPresence::missing:MountPresence::present}};
  }, 10ms);
  require(calls==0, "empty watch queried driver");
  monitor.watch(7,{target()});
  auto first=await_update(monitor);
  require(first.epoch==7 && first.observations[0].presence==MountPresence::present && !wrong_thread, "worker did not publish initial state independently");
  mode=1; // No explicit refresh: periodic checking must detect external changes.
  require(await_update(monitor).observations[0].presence==MountPresence::missing, "periodic poll failed");
  mode=2; monitor.request_refresh();
  require(await_update(monitor).observations[0].presence==MountPresence::unknown, "exception became disappearance");
  mode=0; monitor.request_refresh();
  require(await_update(monitor).observations[0].presence==MountPresence::present, "query did not recover");
  mode=3; monitor.request_refresh();
  require(await_update(monitor).observations[0].presence==MountPresence::unknown, "malformed callback accepted");
  monitor.watch(8,{});
  require(!monitor.take_update(), "cleared watch retained mailbox");
  bool rejected=false;
  try { monitor.watch(9,{target(),target(2)}); } catch(const std::invalid_argument&) { rejected=true; }
  require(rejected, "duplicate drive accepted");
}
void test_generation_and_coalescing() {
  std::promise<void> started, release, second_started, second_release;
  auto gate=release.get_future().share(); auto second_gate=second_release.get_future().share();
  std::atomic<int> calls{};
  MountStateMonitor monitor([&](auto targets) {
    const int call=++calls;
    if(call==1) { started.set_value(); gate.wait_for(2s); }
    if(call==2) { second_started.set_value(); second_gate.wait_for(2s); }
    return std::vector<MountObservation>{{targets[0].container_id,MountPresence::missing}};
  }, 1h);
  monitor.watch(10,{target()});
  require(started.get_future().wait_for(2s)==std::future_status::ready,"first query not started");
  // A session replacement must not wait for a blocked driver query.
  monitor.watch(11,{target(2,L'N')});
  release.set_value();
  require(second_started.get_future().wait_for(2s)==std::future_status::ready,"new watch not queried");
  require(!monitor.take_update(),"old session result escaped revision guard");
  second_release.set_value();
  auto update=await_update(monitor);
  require(update.epoch==11 && update.observations[0].container_id==2,"wrong session published");
  monitor.request_refresh();
  const auto deadline=std::chrono::steady_clock::now()+2s;
  while(calls<3 && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(1ms);
  require(calls>=3,"explicit wake failed");
  // Clearing also drops any in-flight result with the same public epoch.
  monitor.watch(11,{});
  require(!monitor.take_update(),"clearing left stale results");
}
void test_unchanged_not_republished() {
  std::promise<void> second_started, third_started, third_release;
  auto gate=third_release.get_future().share();
  std::atomic<int> calls{};
  MountStateMonitor monitor([&](auto targets) {
    const int call=++calls;
    if(call==2) second_started.set_value();
    if(call==3) { third_started.set_value(); gate.wait_for(2s); }
    return std::vector<MountObservation>{{targets[0].container_id,MountPresence::present}};
  },1h);
  monitor.watch(1,{target()}); static_cast<void>(await_update(monitor));
  monitor.request_refresh();
  require(second_started.get_future().wait_for(2s)==std::future_status::ready,"second poll missing");
  monitor.request_refresh();
  require(third_started.get_future().wait_for(2s)==std::future_status::ready,"third poll missing");
  require(!monitor.take_update(),"unchanged state republished");
  third_release.set_value();
}
void test_shutdown_wakes_sleep() {
  const auto before=std::chrono::steady_clock::now();
  { MountStateMonitor monitor([](auto targets) { return std::vector<MountObservation>{{targets[0].container_id,MountPresence::present}}; },1h);
    monitor.watch(1,{target()}); static_cast<void>(await_update(monitor)); }
  require(std::chrono::steady_clock::now()-before<2s,"shutdown waited for polling interval");
}
}
int main() {
  try { test_observations(); test_polling_and_errors(); test_generation_and_coalescing(); test_unchanged_not_republished(); test_shutdown_wakes_sleep(); }
  catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
  std::cout<<"mount state monitor: observation, worker, epoch, error and shutdown tests passed\n";
}
