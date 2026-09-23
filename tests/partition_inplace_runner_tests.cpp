#include "volume/partition_inplace_runner.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace usb_vault::volume::detail;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
constexpr std::uint64_t size = 20ULL * 1024 * 1024 + 512;
constexpr std::uint64_t header = 131072;
struct Backend final : InplaceBackend {
  std::vector<std::byte> disk = std::vector<std::byte>(size, std::byte{0xA5});
  std::vector<std::byte> original = std::vector<std::byte>(size - 2 * header);
  InplaceRange saved{size, size - header, 0};
  std::vector<std::string> calls;
  std::string fail;
  int fail_at{-1};
  int writes{}, pause_after{-1}, revoke_after{-1}, closes{};
  bool short_read{}, short_write{}, revoke_before_write{}, pause_before_write{},
      revoke_on_save{}, finalized{}, dirty_data{}, dirty_header{}, initial_pause{};
  Backend() {
    for (std::size_t i = 0; i < original.size(); ++i) original[i] = std::byte((i + i / 512 * 37) % 251);
    std::copy(original.begin(), original.end(), disk.begin());
  }
  void call(const char* name) {
    calls.emplace_back(name);
    if (fail == name || static_cast<int>(calls.size()) == fail_at) throw std::runtime_error("injected failure");
  }
  InplaceControl control() override {
    call("control");
    if ((revoke_after >= 0 && writes >= revoke_after) || revoke_before_write) return InplaceControl::revoked;
    if (initial_pause || (pause_after >= 0 && writes >= pause_after) || pause_before_write) return InplaceControl::pause;
    return InplaceControl::proceed;
  }
  InplaceRange open_authenticated() override { call("open"); return saved; }
  std::size_t read(std::uint64_t offset, std::span<std::byte> buffer) override {
    call("read");
    require(offset + buffer.size() <= size - 2 * header, "read outside data source");
    std::copy_n(disk.begin() + offset, buffer.size(), buffer.begin());
    return buffer.size() - (short_read ? 512 : 0);
  }
  void encrypt(std::uint64_t unit, std::span<std::byte> buffer) override {
    call("encrypt");
    require(unit * 512 >= header, "invalid unit");
    for (auto& byte : buffer) byte ^= std::byte{0x5A}; // Marker only; not cryptography.
    if (fail == "revoke-during-encrypt") revoke_before_write = true;
    if (fail == "pause-during-encrypt") pause_before_write = true;
  }
  std::size_t write(std::uint64_t offset, std::span<const std::byte> buffer) override {
    call("write");
    require(offset >= header && offset + buffer.size() <= size - header, "write touched header");
    const auto bytes = buffer.size() - (short_write ? 512 : 0);
    std::copy_n(buffer.begin(), bytes, disk.begin() + offset);
    ++writes; dirty_data = true;
    return bytes;
  }
  void save_progress(const InplaceRange& range) override {
    call("save"); require(!dirty_data, "checkpoint before data flush");
    saved = range; dirty_header = true;
    if (revoke_on_save) revoke_before_write = true;
  }
  void flush() override { call("flush"); dirty_data = false; dirty_header = false; }
  void finalize() override {
    call("finalize");
    require(!dirty_data && !dirty_header && saved.start == header && saved.length == size - 2 * header,
        "premature finalization");
    finalized = true;
  }
  void close() noexcept override { ++closes; }
  bool called(const char* name) const { return std::find(calls.begin(), calls.end(), name) != calls.end(); }
};
void resume() {
  Backend backend;
  backend.pause_after = 2;
  require(run_partition_inplace(backend) == InplaceResult::paused, "pause failed");
  require(backend.writes == 2 && backend.saved.length == 131072 && !backend.finalized &&
      !backend.dirty_data && !backend.dirty_header && backend.closes == 1, "pause was not durable");
  backend.pause_after = -1;
  require(run_partition_inplace(backend) == InplaceResult::completed, "resume failed");
  require(backend.finalized && backend.closes == 2, "completion or close missing");
  for (std::size_t i = 0; i < backend.original.size(); ++i)
    require(backend.disk[i + header] == (backend.original[i] ^ std::byte{0x5A}), "overlap/resume corrupted data");
  require(std::all_of(backend.disk.begin() + size - header, backend.disk.end(),
      [](auto byte) { return byte == std::byte{0xA5}; }), "data writes touched backup header");
}
void failures() {
  for (const auto* operation : {"control", "open", "read", "encrypt", "write", "save", "flush", "finalize"}) {
    Backend backend; backend.fail = operation;
    require(run_partition_inplace(backend) == InplaceResult::failed && backend.closes == 1, "failure reported success");
    if (std::string(operation) == "write" || std::string(operation) == "read" || std::string(operation) == "encrypt")
      require(!backend.called("save") && !backend.called("finalize"), "failed block published progress");
  }
  for (bool writing : {false, true}) {
    Backend backend; backend.short_write = writing; backend.short_read = !writing;
    require(run_partition_inplace(backend) == InplaceResult::failed && !backend.called("save") &&
        !backend.finalized && backend.writes <= 1, "short IO retried or checkpointed");
  }
  Backend invalid; invalid.saved.length = 512;
  require(run_partition_inplace(invalid) == InplaceResult::failed && !invalid.called("read") &&
      !invalid.called("save"), "malformed authenticated range accepted");
}
void every_failure_boundary() {
  Backend baseline;
  baseline.saved.start = header + 3 * 65536 + 512;
  baseline.saved.length = size - header - baseline.saved.start;
  require(run_partition_inplace(baseline) == InplaceResult::completed, "baseline failed");
  const auto steps = static_cast<int>(baseline.calls.size());
  for (int i = 1; i <= steps; ++i) {
    Backend backend; backend.saved = {size, header + 3 * 65536 + 512, size - 2 * header - 3 * 65536 - 512}; backend.fail_at = i;
    require(run_partition_inplace(backend) == InplaceResult::failed && backend.closes == 1 &&
        static_cast<int>(backend.calls.size()) == i, "failed operation retried or reported completion");
  }
  Backend paused; paused.pause_after = 1;
  require(run_partition_inplace(paused) == InplaceResult::paused, "pause baseline failed");
  for (int i = 1; i <= static_cast<int>(paused.calls.size()); ++i) {
    Backend backend; backend.pause_after = 1; backend.fail_at = i;
    require(run_partition_inplace(backend) == InplaceResult::failed && backend.closes == 1,
        "pause boundary failure reported success");
  }
}
void controls() {
  Backend early; early.revoke_after = 0;
  require(run_partition_inplace(early) == InplaceResult::revoked && !early.called("open"), "revoked session opened");
  Backend pause; pause.initial_pause = true;
  require(run_partition_inplace(pause) == InplaceResult::paused && pause.writes == 0 && pause.called("save"), "initial pause lost");
  Backend revoked; revoked.fail = "revoke-during-encrypt";
  require(run_partition_inplace(revoked) == InplaceResult::revoked && revoked.writes == 0, "write after observed revocation");
  Backend during; during.fail = "pause-during-encrypt";
  require(run_partition_inplace(during) == InplaceResult::paused && during.writes == 1, "pause interrupted block");
  Backend after; after.revoke_after = 1;
  require(run_partition_inplace(after) == InplaceResult::revoked && after.writes == 1 && !after.finalized, "revocation continued work");
  Backend saving; saving.initial_pause = true; saving.revoke_on_save = true;
  require(run_partition_inplace(saving) == InplaceResult::revoked, "revocation during checkpoint reported pause");
  Backend badpause; badpause.pause_after = 1; badpause.fail = "save";
  require(run_partition_inplace(badpause) == InplaceResult::failed, "failed pause reported resumable");
}
}
int main() {
  try { resume(); failures(); every_failure_boundary(); controls(); std::cout << "PASS: backwards conversion, durable pause ordering, resume, IO failures and revocation\n"; return 0; }
  catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
