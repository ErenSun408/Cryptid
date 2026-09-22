#include "usb_vault/volume/partition_catalog.h"
#include "volume/partition_catalog_detail.h"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
using namespace usb_vault::volume;
using detail::PartitionObservation;
using A = PartitionAvailability;
void require(bool condition, const char* text) {
  if (!condition) throw std::runtime_error(text);
}
PartitionObservation valid() {
  PartitionObservation value;
  value.partition_known = value.extent_known = value.geometry_known = value.filesystem_known = true;
  value.partition.PartitionStyle = PARTITION_STYLE_GPT;
  value.partition.PartitionNumber = 2;
  value.partition.StartingOffset.QuadPart = 1024 * 1024;
  value.partition.PartitionLength.QuadPart = 1024LL * 1024 * 1024;
  value.partition.Gpt.PartitionType = {0xebd0a0a2, 0xb9e5, 0x4433,
      {0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7}};
  value.partition.Gpt.PartitionId.Data1 = 42;
  value.extents.NumberOfDiskExtents = 1;
  value.extents.Extents[0].DiskNumber = 1;
  value.extents.Extents[0].StartingOffset = value.partition.StartingOffset;
  value.extents.Extents[0].ExtentLength = value.partition.PartitionLength;
  value.geometry.BytesPerSector = 512;
  value.filesystem = L"NTFS";
  return value;
}
void unit_tests() {
  unsigned cases{};
  const auto check = [&](const PartitionObservation& value, A expected, const char* message) {
    ++cases; require(detail::assess_partition(value) == expected, message);
  };
  check(valid(), A::candidate, "basic NTFS partition rejected");
  auto value = valid(); value.partition.Gpt.Attributes = GPT_BASIC_DATA_ATTRIBUTE_NO_DRIVE_LETTER;
  check(value, A::candidate, "no-drive-letter data partition rejected");
  value = valid(); value.partition.PartitionStyle = PARTITION_STYLE_MBR;
  value.partition.Mbr = {}; value.partition.Mbr.PartitionType = PARTITION_IFS;
  value.partition.Mbr.RecognizedPartition = TRUE;
  check(value, A::candidate, "basic MBR NTFS rejected");
  value.partition.Mbr.BootIndicator = TRUE;
  check(value, A::protected_partition, "active boot partition accepted");
  value.partition.Mbr.BootIndicator = FALSE;
  value.partition.Mbr.PartitionType = PARTITION_LDM;
  check(value, A::unsupported_layout, "dynamic partition accepted");
  value.partition.Mbr.PartitionType = PARTITION_IFS; value.partition.Mbr.RecognizedPartition = FALSE;
  check(value, A::unsupported_layout, "unrecognized MBR accepted");
  value = {}; value.windows_volume = true;
  check(value, A::windows_volume, "Windows exclusion needs metadata");
  for (int missing = 0; missing < 4; ++missing) {
    value = valid();
    if (missing == 0) value.partition_known = false;
    if (missing == 1) value.extent_known = false;
    if (missing == 2) value.geometry_known = false;
    if (missing == 3) value.filesystem_known = false;
    check(value, A::metadata_unavailable, "missing metadata accepted");
  }
  // EFI, Microsoft reserved and Windows recovery are never data candidates.
  for (DWORD type : {0xc12a7328u, 0xe3c9e316u, 0xde94bba4u}) {
    value = valid(); value.partition.Gpt.PartitionType.Data1 = type;
    check(value, A::protected_partition, "protected GPT type accepted");
  }
  for (ULONGLONG flag : {1ULL, 0x1000000000000000ULL, 0x2000000000000000ULL,
                         0x4000000000000000ULL, 0x10ULL}) {
    value = valid(); value.partition.Gpt.Attributes = flag;
    check(value, A::protected_partition, "protected/unknown GPT flag accepted");
  }
  value = valid(); value.partition.Gpt.PartitionId = {};
  check(value, A::metadata_unavailable, "empty GPT identity accepted");
  value = valid(); value.partition.PartitionStyle = PARTITION_STYLE_RAW;
  check(value, A::unsupported_layout, "raw disk accepted");
  for (DWORD count : {0u, 2u, 0xffffffffu}) {
    value = valid(); value.extents.NumberOfDiskExtents = count;
    check(value, A::inconsistent_extent, "multi/missing extent accepted");
  }
  for (int bad = 0; bad < 8; ++bad) {
    value = valid();
    if (bad == 0) value.partition.PartitionNumber = 0;
    if (bad == 1) value.partition.StartingOffset.QuadPart = -512;
    if (bad == 2) value.partition.PartitionLength.QuadPart = 0;
    if (bad == 3) value.partition.StartingOffset.QuadPart = (std::numeric_limits<LONGLONG>::max)();
    if (bad == 4) value.extents.Extents[0].StartingOffset.QuadPart += 512;
    if (bad == 5) value.extents.Extents[0].ExtentLength.QuadPart -= 512;
    if (bad == 6) { value.partition.StartingOffset.QuadPart++; value.extents.Extents[0].StartingOffset = value.partition.StartingOffset; }
    if (bad == 7) { value.partition.PartitionLength.QuadPart++; value.extents.Extents[0].ExtentLength = value.partition.PartitionLength; }
    check(value, A::inconsistent_extent, "unsafe partition range accepted");
  }
  for (DWORD sector : {0u, 4096u, 511u}) {
    value = valid(); value.geometry.BytesPerSector = sector;
    check(value, A::unsupported_sector_size, "unsupported sector accepted");
  }
  for (const auto fs : {L"", L"RAW", L"FAT32", L"exFAT", L"ReFS"}) {
    value = valid(); value.filesystem = fs;
    check(value, A::unsupported_filesystem, "unvalidated filesystem accepted");
  }
  std::cout << "PASS: " << cases << " partition assessment cases\n";
}
void scan() {
  // Explicit opt-in read-only native smoke. Never print identities or paths.
  const auto first = enumerate_partition_candidates();
  const auto second = enumerate_partition_candidates();
  require(!first.empty() && !second.empty(), "empty Windows volume inventory");
  std::size_t candidates{}, blocked{}, windows{};
  for (const auto& entry : second) {
    if (entry.availability == A::candidate) {
      ++candidates;
      require(entry.partition_number && entry.offset_bytes && entry.length_bytes &&
          entry.logical_sector_bytes == 512 && !entry.query_error, "incomplete candidate");
    } else ++blocked;
    if (entry.availability == A::windows_volume) ++windows;
  }
  require(windows == 1, "Windows volume not excluded exactly once");
  std::cout << "PASS: read-only refresh; volumes=" << second.size()
            << " candidates=" << candidates << " blocked=" << blocked
            << " windows_excluded=" << windows << '\n';
}
}
int main(int argc, char** argv) {
  try {
    if (argc == 1) unit_tests();
    else if (argc == 2 && std::string_view(argv[1]) == "--scan") scan();
    else throw std::runtime_error("expected no arguments or --scan");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "partition_catalog_tests failed: " << error.what() << '\n';
    return 1;
  }
}
