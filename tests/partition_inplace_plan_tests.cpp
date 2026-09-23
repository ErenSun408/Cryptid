#include "usb_vault/volume/partition_inplace_plan.h"
#include "volume/partition_inplace_plan_detail.h"
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace usb_vault::volume;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F action) {
  bool rejected = false;
  try { action(); } catch (const std::invalid_argument&) { rejected = true; }
  require(rejected, "invalid geometry or progress accepted");
}
NtfsInplaceFacts facts(std::uint64_t size) {
  return {.sectors = static_cast<std::int64_t>(size / 512 - 1),
      .clusters = static_cast<std::int64_t>(size / 4096),
      .free_clusters = static_cast<std::int64_t>(size / 4096 / 2),
      .sector_bytes = 512, .cluster_bytes = 4096};
}
void test_geometry() {
  constexpr auto size = 256ULL * 1024 * 1024;
  const auto original = facts(size);
  const auto plan = plan_partition_inplace(size, original);
  require(plan.shrink_to_sectors == static_cast<std::uint64_t>(original.sectors) - 512 &&
      plan.data_offset_bytes == 131072 && plan.data_bytes == size - 262144 &&
      plan.backup_header_offset_bytes == size - 131072, "pinned layout mismatch");
  for (int scenario = 0; scenario < 12; ++scenario) {
    auto bad = original;
    switch (scenario) {
      case 0: bad.sectors = -1; break;
      case 1: bad.sectors = (std::numeric_limits<std::int64_t>::max)(); break;
      case 2: bad.clusters = -1; break;
      case 3: bad.free_clusters = -1; break;
      case 4: bad.free_clusters = bad.clusters + 1; break;
      case 5: bad.free_clusters = 63; break;
      case 6: bad.cluster_bytes = 1536; break;
      case 7: bad.cluster_bytes = 131072; break;
      case 8: bad.sector_bytes = 4096; break;
      case 9: bad.sectors = 512; break;
      case 10: bad.clusters += 100; break;
      case 11: bad.clusters /= 2; break;
    }
    rejects([&] { static_cast<void>(plan_partition_inplace(size, bad)); });
  }
  rejects([&] { static_cast<void>(plan_partition_inplace(size + 1, original)); });
  rejects([&] { static_cast<void>(plan_partition_inplace(1ULL << 41, original)); });
  rejects([&] { static_cast<void>(detail::inspect_partition_inplace(INVALID_HANDLE_VALUE, {})); });
}
void test_backwards_copy(std::uint64_t size) {
  const auto plan = plan_partition_inplace(size, facts(size));
  std::vector<unsigned char> disk(static_cast<std::size_t>(size), 0xA5);
  std::vector<unsigned char> original(static_cast<std::size_t>(plan.data_bytes));
  for (std::size_t i = 0; i < original.size(); ++i) original[i] = static_cast<unsigned char>((i + i / 512 * 37) % 251);
  std::copy(original.begin(), original.end(), disk.begin());
  auto start = plan.backup_header_offset_bytes;
  std::uint64_t length = 0;
  unsigned chunks = 0;
  while (const auto chunk = next_partition_inplace_chunk(size, start, length)) {
    require(chunk->bytes > 0 && chunk->bytes <= 2 * 1024 * 1024 && chunk->bytes % 512 == 0 &&
        chunk->destination_offset_bytes == chunk->source_offset_bytes + 131072 &&
        chunk->encryption_unit * 512 == chunk->destination_offset_bytes, "invalid backwards chunk");
    // Memory model of full read before overlapping write. XOR is only a test
    // marker, not encryption. A forward scheduler would corrupt later reads.
    std::vector<unsigned char> buffer(disk.begin() + chunk->source_offset_bytes,
        disk.begin() + chunk->source_offset_bytes + chunk->bytes);
    for (auto& byte : buffer) byte ^= 0x5A;
    std::copy(buffer.begin(), buffer.end(), disk.begin() + chunk->destination_offset_bytes);
    start -= chunk->bytes; length += chunk->bytes; ++chunks;
    // Reconstruct solely from saved range, as a later invocation must do.
    const auto again = next_partition_inplace_chunk(size, start, length);
    require(!again || again->destination_offset_bytes + again->bytes == start, "resume introduced a gap");
  }
  require(chunks > 1 && length == plan.data_bytes && start == plan.data_offset_bytes, "premature completion");
  for (std::size_t i = 0; i < original.size(); ++i)
    require(disk[i + plan.data_offset_bytes] == (original[i] ^ 0x5A), "unread plaintext overwritten or misplaced");
  require(std::all_of(disk.begin() + plan.backup_header_offset_bytes, disk.end(), [](auto b) { return b == 0xA5; }), "chunk touched backup header");
  rejects([&] { static_cast<void>(next_partition_inplace_chunk(size, start, length - 512)); });
  rejects([&] { static_cast<void>(next_partition_inplace_chunk(size, start - 512, length + 512)); });
  rejects([&] { static_cast<void>(next_partition_inplace_chunk(size, start + 1, length - 1)); });
  rejects([&] { static_cast<void>(next_partition_inplace_chunk(size, UINT64_MAX, 0)); });
}
}
int main() {
  try {
    test_geometry();
    test_backwards_copy(20ULL * 1024 * 1024);
    test_backwards_copy(31ULL * 1024 * 1024 + 512);
    const auto largest = 1ULL << 40;
    require(next_partition_inplace_chunk(largest, largest - 131072, 0)->bytes == 2097152,
        "maximum supported size overflowed");
    std::cout << "PASS: NTFS bounds, backwards overlap safety, saved-range continuation and header exclusion\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
