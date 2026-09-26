#include "usb_vault/application/container_index_view_v1.h"

#include "usb_vault/application/container_index_v1.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using usb_vault::application::add_container_index_path_v1;
using usb_vault::application::container_index_v1_mutable_slot;
using usb_vault::application::container_index_v1_slot;
using usb_vault::application::ContainerIndexWriteV1;
using usb_vault::application::derive_shared_container_index_key_v1;
using usb_vault::application::fill_container_index_slot_v1;
using usb_vault::application::kContainerIndexV1FileSize;
using usb_vault::application::kContainerIndexV1KeySize;
using usb_vault::application::kContainerIndexV1SlotCount;
using usb_vault::application::list_container_index_paths_v1;
using usb_vault::application::remove_container_index_path_v1;
using usb_vault::application::seal_container_index_entry_v1;
using usb_vault::application::try_open_container_index_entry_v1;
using usb_vault::security::SecureBuffer;

[[noreturn]] void fail(const char *const message) {
  std::cerr << "FAILED: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void expect(const bool condition, const char *const message) {
  if (!condition) {
    fail(message);
  }
}

template <typename Callable>
void expect_invalid_argument(Callable &&callable, const char *const message) {
  try {
    std::forward<Callable>(callable)();
  } catch (const std::invalid_argument &) {
    return;
  }
  fail(message);
}

[[nodiscard]] std::vector<std::byte> test_root(const unsigned char seed) {
  std::vector<std::byte> root(32);
  for (std::size_t index = 0; index < root.size(); ++index) {
    root[index] =
        static_cast<std::byte>(static_cast<unsigned char>(seed + (index * 3U)));
  }
  return root;
}

[[nodiscard]] std::vector<std::byte> fresh_file() {
  std::vector<std::byte> file(kContainerIndexV1FileSize);
  for (std::size_t index = 0; index < kContainerIndexV1SlotCount; ++index) {
    fill_container_index_slot_v1(container_index_v1_mutable_slot(file, index));
  }
  return file;
}

[[nodiscard]] std::wstring numbered_path(const std::size_t number) {
  return L"D:\\vaults\\container-" + std::to_wstring(number) + L".hc";
}

// Where the one entry in this file sits. Only the reader can say, which is the
// point: nothing outside a sealed slot records its position.
[[nodiscard]] std::size_t
sole_occupied_slot(const SecureBuffer &key,
                   const std::span<const std::byte> file) {
  std::size_t found = kContainerIndexV1SlotCount;
  for (std::size_t index = 0; index < kContainerIndexV1SlotCount; ++index) {
    if (try_open_container_index_entry_v1(
            key.bytes(), container_index_v1_slot(file, index))
            .has_value()) {
      expect(found == kContainerIndexV1SlotCount,
             "the file holds more than one entry");
      found = index;
    }
  }
  expect(found != kContainerIndexV1SlotCount, "the file holds no entry");
  return found;
}

void test_the_shared_key_follows_the_root_and_nothing_else() {
  const std::vector<std::byte> root = test_root(0x10);
  const SecureBuffer first = derive_shared_container_index_key_v1(root);
  const SecureBuffer second = derive_shared_container_index_key_v1(root);
  expect(first.size() == kContainerIndexV1KeySize,
         "the shared key is not the size a slot takes");
  expect(std::ranges::equal(first.bytes(), second.bytes()),
         "the same root gave two different shared keys");

  const SecureBuffer other =
      derive_shared_container_index_key_v1(test_root(0x11));
  expect(!std::ranges::equal(first.bytes(), other.bytes()),
         "two different roots gave the same shared key");

  expect_invalid_argument(
      [] {
        const std::vector<std::byte> short_root(31);
        static_cast<void>(derive_shared_container_index_key_v1(short_root));
      },
      "a root secret of the wrong size was accepted");
}

void test_a_path_that_was_added_can_be_listed() {
  const SecureBuffer key = derive_shared_container_index_key_v1(test_root(0x20));
  std::vector<std::byte> file = fresh_file();

  expect(list_container_index_paths_v1(key.bytes(), file).empty(),
         "a file of pure filler listed something");

  const std::wstring path = L"D:\\vaults\\holiday.hc";
  expect(add_container_index_path_v1(key.bytes(), file, path) ==
             ContainerIndexWriteV1::added,
         "adding a path to an empty file did not report adding it");

  const std::vector<std::wstring> listed =
      list_container_index_paths_v1(key.bytes(), file);
  expect(listed.size() == 1 && listed.front() == path,
         "the path that was added did not come back");
}

void test_the_same_path_never_takes_two_slots() {
  const SecureBuffer key = derive_shared_container_index_key_v1(test_root(0x21));
  std::vector<std::byte> file = fresh_file();
  const std::wstring path = L"D:\\Vaults\\Holiday.hc";

  expect(add_container_index_path_v1(key.bytes(), file, path) ==
             ContainerIndexWriteV1::added,
         "the first add did not report adding");
  const std::vector<std::byte> after_first = file;

  expect(add_container_index_path_v1(key.bytes(), file, path) ==
             ContainerIndexWriteV1::already_present,
         "adding the same path twice did not report it was already there");
  // Windows names files without regard to case, so this is the same container
  // reached through the same dialog, not a second one.
  expect(add_container_index_path_v1(key.bytes(), file,
                                     L"d:\\vaults\\holiday.HC") ==
             ContainerIndexWriteV1::already_present,
         "a spelling differing only in case was taken for a second container");

  expect(std::ranges::equal(after_first, file),
         "an add that changed nothing still rewrote the file");
  expect(list_container_index_paths_v1(key.bytes(), file).size() == 1,
         "one container ended up listed more than once");
}

void test_every_path_added_survives_the_ones_after_it() {
  // Under one shared key every occupied slot is visible to the writer, so a
  // new entry can never land on an existing one. This is what that buys.
  const SecureBuffer key = derive_shared_container_index_key_v1(test_root(0x22));
  std::vector<std::byte> file = fresh_file();

  constexpr std::size_t kCount = 50;
  for (std::size_t index = 0; index < kCount; ++index) {
    expect(add_container_index_path_v1(key.bytes(), file,
                                       numbered_path(index)) ==
               ContainerIndexWriteV1::added,
           "one of a run of adds did not report adding");
  }

  const std::vector<std::wstring> listed =
      list_container_index_paths_v1(key.bytes(), file);
  const std::set<std::wstring> found(listed.begin(), listed.end());
  expect(found.size() == kCount, "adding paths in a run lost one of them");
  for (std::size_t index = 0; index < kCount; ++index) {
    expect(found.contains(numbered_path(index)),
           "a path added earlier went missing");
  }
}

void test_placement_does_not_favour_a_corner_of_the_file() {
  // Position must say nothing about an entry, so the slot a lone entry lands
  // in has to move around. Thirty-two files cannot all fall in one half by
  // chance.
  const SecureBuffer key = derive_shared_container_index_key_v1(test_root(0x23));
  std::set<std::size_t> chosen;
  bool below_middle = false;
  bool above_middle = false;

  constexpr std::size_t kTrials = 32;
  for (std::size_t trial = 0; trial < kTrials; ++trial) {
    std::vector<std::byte> file = fresh_file();
    expect(add_container_index_path_v1(key.bytes(), file, numbered_path(trial)) ==
               ContainerIndexWriteV1::added,
           "a trial add did not report adding");
    const std::size_t slot = sole_occupied_slot(key, file);
    chosen.insert(slot);
    below_middle = below_middle || slot < (kContainerIndexV1SlotCount / 2);
    above_middle = above_middle || slot >= (kContainerIndexV1SlotCount / 2);
  }

  expect(chosen.size() >= 20, "the chosen slots repeated far too often");
  expect(below_middle && above_middle,
         "every entry landed in the same half of the file");
}

void test_a_removed_path_goes_and_the_rest_stay() {
  const SecureBuffer key = derive_shared_container_index_key_v1(test_root(0x24));
  std::vector<std::byte> file = fresh_file();
  for (std::size_t index = 0; index < 5; ++index) {
    static_cast<void>(
        add_container_index_path_v1(key.bytes(), file, numbered_path(index)));
  }

  expect(remove_container_index_path_v1(key.bytes(), file, numbered_path(2)),
         "removing a path that was there reported nothing to remove");
  const std::vector<std::wstring> listed =
      list_container_index_paths_v1(key.bytes(), file);
  const std::set<std::wstring> found(listed.begin(), listed.end());
  expect(found.size() == 4, "removing one path took more than one with it");
  expect(!found.contains(numbered_path(2)), "the removed path is still listed");

  // The slot it left has to be filler again, not a hole anyone can see.
  expect(!remove_container_index_path_v1(key.bytes(), file, numbered_path(2)),
         "the removed path could be removed a second time");
  expect(!remove_container_index_path_v1(key.bytes(), file,
                                         L"D:\\vaults\\never-added.hc"),
         "removing a path that was never there reported a removal");

  // Case is matched the same way on the way out as on the way in.
  expect(remove_container_index_path_v1(key.bytes(), file,
                                        L"D:\\VAULTS\\CONTAINER-3.HC"),
         "a removal did not match the way an add matches");
}

void test_a_full_file_is_refused_rather_than_made_room_in() {
  const SecureBuffer key = derive_shared_container_index_key_v1(test_root(0x25));
  std::vector<std::byte> file = fresh_file();
  for (std::size_t index = 0; index < kContainerIndexV1SlotCount; ++index) {
    const SecureBuffer sealed =
        seal_container_index_entry_v1(key.bytes(), numbered_path(index));
    std::ranges::copy(sealed.bytes(),
                      container_index_v1_mutable_slot(file, index).begin());
  }

  const std::vector<std::byte> before = file;
  expect(add_container_index_path_v1(key.bytes(), file,
                                     L"D:\\vaults\\one-too-many.hc") ==
             ContainerIndexWriteV1::full,
         "a full file accepted one more entry");
  expect(std::ranges::equal(before, file),
         "a refused add still changed the file");
  expect(list_container_index_paths_v1(key.bytes(), file).size() ==
             kContainerIndexV1SlotCount,
         "a refused add cost the file an entry");
}

void test_another_root_sees_an_empty_index() {
  const SecureBuffer key = derive_shared_container_index_key_v1(test_root(0x26));
  const SecureBuffer stranger =
      derive_shared_container_index_key_v1(test_root(0x27));
  std::vector<std::byte> file = fresh_file();
  static_cast<void>(
      add_container_index_path_v1(key.bytes(), file, L"D:\\vaults\\mine.hc"));

  expect(list_container_index_paths_v1(stranger.bytes(), file).empty(),
         "an index sealed under one root listed under another");
}

void test_order_survives_reload_and_preserves_unselected_and_opaque_slots() {
  using usb_vault::application::reorder_container_index_paths_v1;
  const auto key = derive_shared_container_index_key_v1(test_root(0x31));
  const auto other = derive_shared_container_index_key_v1(test_root(0x32));
  auto file = fresh_file();
  // Fixed physical positions make legacy fallback and opaque preservation
  // deterministic, independently of the random-placement writer.
  for (std::size_t index = 0; index < 4; ++index) {
    const auto sealed = seal_container_index_entry_v1(key.bytes(), numbered_path(index));
    std::ranges::copy(sealed.bytes(), container_index_v1_mutable_slot(file, index).begin());
  }
  const auto foreign = seal_container_index_entry_v1(other.bytes(), L"E:\\other.hc");
  std::ranges::copy(foreign.bytes(), container_index_v1_mutable_slot(file, 9).begin());
  const auto before = file;
  const std::vector<std::wstring> requested{numbered_path(3), numbered_path(1)};
  reorder_container_index_paths_v1(key.bytes(), file, requested);
  const std::vector<std::byte> reloaded(file);
  const std::vector<std::wstring> expected{
      numbered_path(3), numbered_path(1), numbered_path(0), numbered_path(2)};
  expect(list_container_index_paths_v1(key.bytes(), reloaded) == expected,
         "reload lost order or omitted records");
  for (std::size_t index = 4; index < kContainerIndexV1SlotCount; ++index) {
    expect(std::ranges::equal(container_index_v1_slot(before, index),
                              container_index_v1_slot(file, index)),
           "reordering changed an opaque slot");
  }
  expect(list_container_index_paths_v1(other.bytes(), file) ==
             std::vector<std::wstring>{L"E:\\other.hc"},
         "reordering damaged another key's entry");
  const auto saved = file;
  const std::vector<std::wstring> duplicate{numbered_path(0), L"d:\\VAULTS\\CONTAINER-0.HC"};
  expect_invalid_argument([&] { reorder_container_index_paths_v1(key.bytes(), file, duplicate); },
                          "case-insensitive duplicate order was accepted");
  const std::vector<std::wstring> unknown{numbered_path(0), numbered_path(99)};
  expect_invalid_argument([&] { reorder_container_index_paths_v1(key.bytes(), file, unknown); },
                          "unknown path was accepted");
  expect(file == saved, "invalid ordering partially modified the buffer");
}

void test_new_paths_go_first_without_changing_existing_relative_order() {
  using usb_vault::application::prepend_container_index_path_v1;
  const auto key = derive_shared_container_index_key_v1(test_root(0x33));
  auto file = fresh_file();
  for (std::size_t i = 0; i < 3; ++i)
    static_cast<void>(prepend_container_index_path_v1(key.bytes(), file, numbered_path(i)));
  expect(list_container_index_paths_v1(key.bytes(), file) ==
             std::vector<std::wstring>{numbered_path(2), numbered_path(1), numbered_path(0)},
         "new paths did not go first");
  const auto before = file;
  static_cast<void>(prepend_container_index_path_v1(key.bytes(), file, numbered_path(0)));
  expect(file == before, "reimporting a known path changed its order");
}

void test_partition_conversion_states() {
  using namespace usb_vault::application;
  const auto key=derive_shared_container_index_key_v1(test_root(0x71));
  ContainerIndexPartition partition; partition.device_id.fill(0x31); partition.offset_bytes=1048576; partition.length_bytes=64*1024*1024;
  auto file=fresh_file();
  expect(add_container_index_partition(key.bytes(),file,partition,PartitionConversionState::uncertain)==ContainerIndexWriteV1::added,"conversion insertion failed");
  expect(add_container_index_path_v1(key.bytes(),file,L"D:\\other.hc")==ContainerIndexWriteV1::added,"mixed file insertion failed");
  for(auto state:{PartitionConversionState::paused,PartitionConversionState::uncertain,PartitionConversionState::normal}) {
    const auto before=file;
    expect(set_container_index_partition_state(key.bytes(),file,partition,state),"state update failed");
    const auto entries=list_container_index_partition_entries(key.bytes(),file);
    expect(entries.size()==1 && entries[0].conversion==state,"state did not survive re-read");
    for(std::size_t i=0;i<kContainerIndexV1SlotCount;++i) {
      const auto old=try_open_container_index_entry(key.bytes(),container_index_v1_slot(before,i));
      if(!old || !old->partition)expect(std::ranges::equal(container_index_v1_slot(before,i),container_index_v1_slot(file,i)),"unrelated slot changed");
    }
  }
  const auto before=file;
  expect_invalid_argument([&]{static_cast<void>(set_container_index_partition_state(key.bytes(),file,partition,static_cast<PartitionConversionState>(3)));},"unknown conversion state sealed");
  expect(file==before,"invalid state changed index");
  const auto sealed=seal_container_index_partition(key.bytes(),partition,{},PartitionConversionState::paused);
  for(std::size_t i=0;i<sealed.size();++i) {
    std::vector<std::byte> corrupt(sealed.bytes().begin(),sealed.bytes().end()); corrupt[i]^=std::byte{1};
    expect(!try_open_container_index_entry(key.bytes(),corrupt),"conversion slot tampering accepted");
  }
  std::ranges::copy(sealed.bytes(),container_index_v1_mutable_slot(file,0).begin());
  std::ranges::copy(sealed.bytes(),container_index_v1_mutable_slot(file,1).begin());
  const auto duplicate=file;
  expect_invalid_argument([&]{static_cast<void>(set_container_index_partition_state(key.bytes(),file,partition,PartitionConversionState::normal));},"duplicate conversion identity accepted");
  expect(file==duplicate,"ambiguous update changed index");
}

void test_mixed_partition_and_file_index_preserves_both_kinds() {
  using namespace usb_vault::application;
  const auto key = derive_shared_container_index_key_v1(test_root(0x41));
  const auto stranger = derive_shared_container_index_key_v1(test_root(0x42));
  ContainerIndexPartition partition;
  partition.device_id.fill(0x51);
  partition.offset_bytes = 1024 * 1024;
  partition.length_bytes = 64 * 1024 * 1024;
  auto file = fresh_file();
  expect(add_container_index_partition(key.bytes(), file, partition) == ContainerIndexWriteV1::added,
         "partition was not added");
  const auto first = file;
  expect(add_container_index_partition(key.bytes(), file, partition) == ContainerIndexWriteV1::already_present && file == first,
         "duplicate partition modified the index");
  std::size_t slot = kContainerIndexV1SlotCount;
  for (std::size_t i = 0; i < kContainerIndexV1SlotCount; ++i) {
    if (try_open_container_index_entry(key.bytes(), container_index_v1_slot(file, i))) slot = i;
  }
  expect(slot < kContainerIndexV1SlotCount, "partition slot missing");
  expect(list_container_index_paths_v1(key.bytes(), file).empty(), "partition leaked into file view");
  expect(list_container_index_partitions(stranger.bytes(), file).empty(), "foreign key saw a partition");
  static_cast<void>(prepend_container_index_path_v1(key.bytes(), file, L"D:\\first.hc"));
  static_cast<void>(prepend_container_index_path_v1(key.bytes(), file, L"D:\\second.hc"));
  const std::vector<std::wstring> reordered{L"D:\\first.hc"};
  reorder_container_index_paths_v1(key.bytes(), file, reordered);
  expect(remove_container_index_path_v1(key.bytes(), file, L"D:\\second.hc"), "file remove failed");
  expect(std::ranges::equal(container_index_v1_slot(first, slot), container_index_v1_slot(file, slot)),
         "file insertion, sorting or removal rewrote the partition slot");
  const auto reloaded = file;
  expect(list_container_index_partitions(key.bytes(), reloaded) == std::vector{partition},
         "partition reload lost stable identity");
  auto other = partition;
  other.offset_bytes += other.length_bytes;
  expect(add_container_index_partition(key.bytes(), file, other) == ContainerIndexWriteV1::added,
         "second partition on same device was not distinct");
  const auto before_remove = file;
  expect(remove_container_index_partition(key.bytes(), file, partition), "partition removal failed");
  for (std::size_t i = 0; i < kContainerIndexV1SlotCount; ++i)
    if (i != slot) expect(std::ranges::equal(container_index_v1_slot(file, i), container_index_v1_slot(before_remove, i)),
                         "partition removal changed another slot");
  expect(list_container_index_paths_v1(key.bytes(), file) == reordered,
         "partition removal lost the file record");
  expect(list_container_index_partitions(key.bytes(), file) == std::vector{other}, "partition removal lost its neighbour");
  const auto unchanged = file;
  expect(!remove_container_index_partition(key.bytes(), file, partition) && file == unchanged,
         "missing partition removal modified the file");
  auto invalid = partition;
  invalid.length_bytes = 0;
  expect_invalid_argument([&] { static_cast<void>(add_container_index_partition(key.bytes(), file, invalid)); },
                          "invalid partition was inserted");
  expect(file == unchanged, "failed partition insertion modified the file");
  // A full partition index must not look empty to the legacy file writer.
  const auto sealed = seal_container_index_partition(key.bytes(), partition);
  for (std::size_t i = 0; i < kContainerIndexV1SlotCount; ++i)
    std::ranges::copy(sealed.bytes(), container_index_v1_mutable_slot(file, i).begin());
  const auto full = file;
  expect(add_container_index_path_v1(key.bytes(), file, L"D:\\new.hc") == ContainerIndexWriteV1::full && file == full,
         "file writer overwrote a partition as filler");
  expect(add_container_index_partition(key.bytes(), file, other) == ContainerIndexWriteV1::full && file == full,
         "partition writer evicted an existing record");
}

void test_a_creation_sequence_is_recorded_and_never_moves() {
  using namespace usb_vault::application;
  const auto key = derive_shared_container_index_key_v1(test_root(0x44));
  const auto make = [](std::uint8_t tag, std::uint64_t offset) {
    ContainerIndexPartition partition;
    partition.device_id.fill(tag);
    partition.offset_bytes = offset;
    partition.length_bytes = 64 * 1024 * 1024;
    return partition;
  };
  const auto sequence_of = [&](const std::vector<std::byte>& file,
                               const ContainerIndexPartition& partition) {
    for (const auto& entry : list_container_index_partition_entries(key.bytes(), file))
      if (entry.partition == partition) return entry.sequence;
    return 0u;
  };
  const auto one = make(0x71, 1024 * 1024);
  const auto two = make(0x71, 96ULL * 1024 * 1024);
  const auto three = make(0x71, 192ULL * 1024 * 1024);
  auto file = fresh_file();

  expect(add_container_index_partition(key.bytes(), file, one) == ContainerIndexWriteV1::added &&
             add_container_index_partition(key.bytes(), file, two) == ContainerIndexWriteV1::added &&
             add_container_index_partition(key.bytes(), file, three) == ContainerIndexWriteV1::added,
         "partitions were not recorded");
  expect(sequence_of(file, one) == 1 && sequence_of(file, two) == 2 &&
             sequence_of(file, three) == 3,
         "creation sequences were not handed out in order");

  // The position may change freely; the sequence is the container's own.
  const std::vector<ContainerIndexRef> wanted{{.partition = three}, {.partition = one},
                                              {.partition = two}};
  reorder_container_index_v1(key.bytes(), file, wanted);
  expect(sequence_of(file, one) == 1 && sequence_of(file, two) == 2 &&
             sequence_of(file, three) == 3,
         "reordering moved a creation sequence");
  const auto refs = list_container_index_refs_v1(key.bytes(), file);
  expect(refs.size() == 3 && refs[0].partition == three && refs[1].partition == one,
         "reordering did not take effect");

  // A conversion-state update must not drop it either.
  expect(set_container_index_partition_state(key.bytes(), file, two,
                                            PartitionConversionState::paused),
         "conversion state could not be set");
  expect(sequence_of(file, two) == 2, "a conversion-state update lost the sequence");

  // A gap in the middle is kept rather than handed to something new.
  expect(remove_container_index_partition(key.bytes(), file, two), "removal failed");
  const auto four = make(0x71, 288ULL * 1024 * 1024);
  expect(add_container_index_partition(key.bytes(), file, four) == ContainerIndexWriteV1::added,
         "a replacement partition was not recorded");
  expect(sequence_of(file, four) == 4, "a middle gap was reused");

  // Records written before the sequence existed get one once, and only once.
  auto legacy = fresh_file();
  const auto legacy_sealed = seal_container_index_partition(key.bytes(), one);
  std::ranges::copy(legacy_sealed.bytes(),
                    container_index_v1_mutable_slot(legacy, 3).begin());
  expect(sequence_of(legacy, one) == 0, "a legacy record already carried a sequence");
  expect(list_container_index_partitions(key.bytes(), legacy) == std::vector{one},
         "a legacy record stopped being readable");
  expect(number_container_index_partitions_v1(key.bytes(), legacy),
         "a legacy record was not given a sequence");
  expect(sequence_of(legacy, one) == 1, "the numbering did not take");
  const auto settled = legacy;
  expect(!number_container_index_partitions_v1(key.bytes(), legacy) && legacy == settled,
         "numbering ran a second time");
}

void test_one_order_covers_both_kinds_and_survives_reload() {
  using namespace usb_vault::application;
  const auto key = derive_shared_container_index_key_v1(test_root(0x43));
  const auto make = [](std::uint8_t tag, std::uint64_t offset) {
    ContainerIndexPartition partition;
    partition.device_id.fill(tag);
    partition.offset_bytes = offset;
    partition.length_bytes = 64 * 1024 * 1024;
    return partition;
  };
  const auto older = make(0x61, 1024 * 1024);
  const auto newer = make(0x61, 96ULL * 1024 * 1024);
  auto file = fresh_file();

  // A newly recorded disk container goes first, the way a new path does.
  expect(prepend_container_index_partition(key.bytes(), file, older) ==
             ContainerIndexWriteV1::added, "first partition was not recorded");
  expect(prepend_container_index_partition(key.bytes(), file, newer) ==
             ContainerIndexWriteV1::added, "second partition was not recorded");
  auto refs = list_container_index_refs_v1(key.bytes(), file);
  expect(refs.size() == 2 && refs[0].partition == newer && refs[1].partition == older,
         "a newly recorded disk container did not go first");
  expect(prepend_container_index_partition(key.bytes(), file, newer) ==
             ContainerIndexWriteV1::already_present,
         "an existing partition was recorded twice");

  static_cast<void>(prepend_container_index_path_v1(key.bytes(), file, L"D:\\a.hc"));
  static_cast<void>(prepend_container_index_path_v1(key.bytes(), file, L"D:\\b.hc"));

  // One arrangement naming both kinds, in an order neither grouping would give.
  const std::vector<ContainerIndexRef> wanted{
      {.partition = older}, {.path = L"D:\\b.hc"}, {.partition = newer},
      {.path = L"D:\\a.hc"}};
  reorder_container_index_v1(key.bytes(), file, wanted);
  const auto reloaded = file;
  refs = list_container_index_refs_v1(key.bytes(), reloaded);
  expect(refs.size() == 4 && refs[0].partition == older &&
             refs[1].partition == std::nullopt && refs[1].path == L"D:\\b.hc" &&
             refs[2].partition == newer &&
             refs[3].partition == std::nullopt && refs[3].path == L"D:\\a.hc",
         "a mixed arrangement did not survive the reload");
  // The file-only view keeps its own relative order out of the same records.
  expect(list_container_index_paths_v1(key.bytes(), reloaded) ==
             std::vector<std::wstring>{L"D:\\b.hc", L"D:\\a.hc"},
         "the file-only view disagrees with the arrangement");

  // Re-sealing a partition to give it a position must not lose what else the
  // record carries.
  expect(set_container_index_partition_state(key.bytes(), file, newer,
                                            PartitionConversionState::paused),
         "conversion state could not be set");
  const std::vector<ContainerIndexRef> again{{.partition = newer}};
  reorder_container_index_v1(key.bytes(), file, again);
  const auto entries = list_container_index_partition_entries(key.bytes(), file);
  const auto found = std::ranges::find_if(entries, [&](const auto& item) {
    return item.partition == newer;
  });
  expect(found != entries.end() &&
             found->conversion == PartitionConversionState::paused,
         "reordering a disk container lost its conversion state");

  // Rejected before anything is written, both kinds alike.
  const auto untouched = file;
  const std::vector<ContainerIndexRef> unknown{{.partition = make(0x62, 4096)}};
  expect_invalid_argument([&] { reorder_container_index_v1(key.bytes(), file, unknown); },
                          "an unknown disk container was ordered");
  expect(file == untouched, "a refused arrangement modified the index");
  const std::vector<ContainerIndexRef> twice{{.partition = newer}, {.partition = newer}};
  expect_invalid_argument([&] { reorder_container_index_v1(key.bytes(), file, twice); },
                          "a repeated disk container was ordered");
  expect(file == untouched, "a repeated arrangement modified the index");
  const std::vector<ContainerIndexRef> neither{{}};
  expect_invalid_argument([&] { reorder_container_index_v1(key.bytes(), file, neither); },
                          "an ordering naming nothing was accepted");
  expect(file == untouched, "an empty reference modified the index");
}

} // namespace

int main() {
  test_a_creation_sequence_is_recorded_and_never_moves();
  test_one_order_covers_both_kinds_and_survives_reload();
  test_partition_conversion_states();
  test_mixed_partition_and_file_index_preserves_both_kinds();
  test_order_survives_reload_and_preserves_unselected_and_opaque_slots();
  test_new_paths_go_first_without_changing_existing_relative_order();
  test_the_shared_key_follows_the_root_and_nothing_else();
  test_a_path_that_was_added_can_be_listed();
  test_the_same_path_never_takes_two_slots();
  test_every_path_added_survives_the_ones_after_it();
  test_placement_does_not_favour_a_corner_of_the_file();
  test_a_removed_path_goes_and_the_rest_stay();
  test_a_full_file_is_refused_rather_than_made_room_in();
  test_another_root_sees_an_empty_index();
  std::cout << "container_index_view_v1_tests: PASS\n";
  return EXIT_SUCCESS;
}
