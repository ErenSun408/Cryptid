#include "usb_vault/volume/partition_catalog.h"
#include "volume/partition_catalog_detail.h"
#include "volume/partition_write_policy.h"
#include "usb_vault/volume_creation/volume_creator.h"
#include <filesystem>
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
  value.device_known = true;
  value.device.Version = value.device.Size = sizeof(STORAGE_DEVICE_NUMBER_EX);
  value.device.DeviceType = FILE_DEVICE_DISK;
  value.device.DeviceNumber = value.extents.Extents[0].DiskNumber;
  value.device.PartitionNumber = value.partition.PartitionNumber;
  value.device.DeviceGuid.Data1 = 123;
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
  for (int bad = 0; bad < 12; ++bad) {
    value = valid();
    if (bad == 0) value.device_known = false;
    if (bad == 1) value.device.Version = 0;
    if (bad == 2) --value.device.Size;
    if (bad == 3) ++value.device.Size;
    if (bad == 4) value.device.DeviceType = FILE_DEVICE_CD_ROM;
    if (bad == 5) value.device.Flags = STORAGE_DEVICE_FLAGS_RANDOM_DEVICEGUID_REASON_CONFLICT;
    if (bad == 6) value.device.Flags = STORAGE_DEVICE_FLAGS_RANDOM_DEVICEGUID_REASON_NOHWID;
    if (bad == 7) value.device.Flags = 0x80000000;
    if (bad == 8) value.device.DeviceGuid = {};
    if (bad == 9) ++value.device.DeviceNumber;
    if (bad == 10) ++value.device.PartitionNumber;
    if (bad == 11) value.device.DeviceNumber = MAXDWORD;
    check(value, A::device_identity_unavailable, "unreliable device identity accepted");
  }
  value = valid(); value.device.Flags = STORAGE_DEVICE_FLAGS_PAGE_83_DEVICEGUID;
  check(value, A::candidate, "page83 identity rejected");
  value.device.Flags |= STORAGE_DEVICE_FLAGS_RANDOM_DEVICEGUID_REASON_CONFLICT;
  check(value, A::device_identity_unavailable, "page83 masked conflict flag");
  std::cout << "PASS: " << cases << " partition assessment cases\n";
}
void snapshot_tests() {
  PartitionCandidate selected{};
  selected.volume_path = L"native-test-identity";
  selected.disk_number = 2; selected.partition_number = 3;
  selected.offset_bytes = 1048576; selected.length_bytes = 1073741824;
  selected.logical_sector_bytes = 512; selected.availability = A::candidate;
  selected.layout = PartitionLayout::gpt; selected.partition_id[0] = 7;
  selected.filesystem_serial = 123;
  selected.device_id[0] = 9;
  std::vector<PartitionCandidate> current{selected};
  require(matches_partition_snapshot(selected, current), "unchanged snapshot rejected");
  for (int change = 0; change < 14; ++change) {
    auto altered = selected;
    if (change == 0) altered.volume_path += L"other";
    if (change == 1) ++altered.disk_number;
    if (change == 2) ++altered.partition_number;
    if (change == 3) altered.offset_bytes += 512;
    if (change == 4) altered.length_bytes -= 512;
    if (change == 5) altered.logical_sector_bytes = 4096;
    if (change == 6) altered.availability = A::windows_volume;
    if (change == 7) altered.query_error = ERROR_ACCESS_DENIED;
    if (change == 8) altered.layout = PartitionLayout::mbr;
    if (change == 9) ++altered.partition_id[0];
    if (change == 10) ++altered.filesystem_serial;
    if (change == 11) altered.layout = PartitionLayout::unknown;
    if (change == 12) ++altered.device_id[0];
    if (change == 13) altered.device_id_flags = STORAGE_DEVICE_FLAGS_RANDOM_DEVICEGUID_REASON_NOHWID;
    current = {altered};
    require(!matches_partition_snapshot(selected, current), "changed snapshot accepted");
    require(!matches_partition_snapshot(altered, {&selected, 1}), "reverse mismatch accepted");
  }
  require(!matches_partition_snapshot(selected, {}), "missing target accepted");
  current = {selected, selected};
  require(!matches_partition_snapshot(selected, current), "duplicate identity accepted");
  current.resize(4097, selected);
  require(!matches_partition_snapshot(selected, current), "oversized inventory accepted");
  selected.layout = PartitionLayout::unknown;
  require(!matches_partition_snapshot(selected, {&selected, 1}), "unknown layout accepted");
  selected.layout = PartitionLayout::gpt; selected.partition_id = {};
  require(!matches_partition_snapshot(selected, {&selected, 1}), "missing GPT identity accepted");
  selected.layout = PartitionLayout::mbr;
  require(matches_partition_snapshot(selected, {&selected, 1}), "MBR snapshot rejected");
  std::cout << "PASS: partition snapshot replacement, format, ambiguity and bounds checks\n";
}
void write_policy_tests() {
  using detail::PartitionWriteFacts;
  // Exhaustive unknown/false/true flags, with every documented conversion state
  // and unknown/out-of-range values. Only fully known, unprotected data passes.
  unsigned cases{};
  for (int boot = -1; boot <= 1; ++boot)
    for (int system = -1; system <= 1; ++system)
      for (int paging = -1; paging <= 1; ++paging)
        for (int conversion = -1; conversion <= 6; ++conversion) {
          PartitionWriteFacts facts;
          if (boot >= 0) facts.boot_volume = boot != 0;
          if (system >= 0) facts.system_volume = system != 0;
          if (paging >= 0) facts.page_file_present = paging != 0;
          if (conversion >= 0) facts.conversion_status = static_cast<std::uint32_t>(conversion);
          require(detail::permits_partition_write(facts) ==
              (boot == 0 && system == 0 && paging == 0 && conversion == 0),
              "unsafe/unknown volume write policy accepted");
          ++cases;
        }
  require(!detail::check_partition_write_policy(nullptr, {}), "invalid policy handle accepted");
  require(!detail::check_partition_write_policy(INVALID_HANDLE_VALUE, {}), "invalid policy handle accepted");
  std::cout << "PASS: " << cases << " write-policy combinations and invalid handles\n";
}
void rejected_handles() {
  PartitionCandidate expected{};
  expected.volume_path = L"native-only-label";
  expected.partition_number = 1; expected.offset_bytes = 1048576;
  expected.length_bytes = 1073741824; expected.logical_sector_bytes = 512;
  expected.layout = PartitionLayout::mbr; expected.device_id[0] = 1;
  expected.availability = A::candidate;
  require(!detail::matches_partition_handle_identity(nullptr, expected), "null handle accepted");
  require(!detail::matches_partition_handle_identity(INVALID_HANDLE_VALUE, expected), "invalid handle accepted");
  const HANDLE event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  require(event != nullptr, "create test event");
  const bool event_rejected = !detail::matches_partition_handle_identity(event, expected);
  const bool event_retained = ::SetEvent(event) != FALSE;
  ::CloseHandle(event);
  require(event_rejected && event_retained, "non-disk handle accepted or consumed");
  std::array<wchar_t, 32768> name{};
  const auto count = ::GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
  require(count && count < name.size(), "locate test executable");
  const HANDLE file = ::CreateFileW(name.data(), GENERIC_READ, FILE_SHARE_READ,
      nullptr, OPEN_EXISTING, 0, nullptr);
  require(file != INVALID_HANDLE_VALUE, "open test executable");
  LARGE_INTEGER position{}; position.QuadPart = 17;
  require(::SetFilePointerEx(file, position, nullptr, FILE_BEGIN) != FALSE, "set test cursor");
  const bool file_rejected = !detail::matches_partition_handle_identity(file, expected);
  LARGE_INTEGER zero{}, after{};
  const bool file_retained = ::SetFilePointerEx(file, zero, &after, FILE_CURRENT) != FALSE;
  ::CloseHandle(file);
  require(file_rejected && file_retained && after.QuadPart == 17, "file accepted or handle cursor changed");
  std::cout << "PASS: invalid and non-device handles rejected without ownership or cursor changes\n";
}
void scan(bool handles = false) {
  // Explicit opt-in read-only native smoke. Never print identities or paths.
  const auto first = enumerate_partition_candidates();
  const auto second = enumerate_partition_candidates();
  require(!first.empty() && !second.empty(), "empty Windows volume inventory");
  std::size_t candidates{}, blocked{}, windows{}, verified_handles{};
  for (const auto& entry : second) {
    if (entry.availability == A::candidate) {
      ++candidates;
      if (handles) {
        auto device = entry.volume_path;
        device.pop_back();
        const HANDLE handle = ::CreateFileW(device.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr);
        require(handle != INVALID_HANDLE_VALUE, "open read-only candidate");
        const bool matched = detail::matches_partition_handle_identity(handle, entry);
        auto changed = entry; changed.device_id[0] ^= 1;
        const bool replaced_rejected = !detail::matches_partition_handle_identity(handle, changed);
        changed = entry; changed.offset_bytes += 512;
        const bool moved_rejected = !detail::matches_partition_handle_identity(handle, changed);
        changed = entry; ++changed.filesystem_serial;
        const bool reformatted_rejected = !detail::matches_partition_handle_identity(handle, changed);
        DWORD flags{};
        const bool retained = ::GetHandleInformation(handle, &flags) != FALSE;
        ::CloseHandle(handle);
        require(matched && replaced_rejected && moved_rejected && reformatted_rejected && retained,
            "same-handle identity verification failed");
        ++verified_handles;
      }
      require(matches_partition_snapshot(entry, first), "candidate changed between read-only scans");
      require(entry.partition_number && entry.offset_bytes && entry.length_bytes &&
          entry.logical_sector_bytes == 512 && !entry.query_error, "incomplete candidate");
    } else ++blocked;
    if (entry.availability == A::windows_volume) ++windows;
  }
  require(candidates > 0, "no eligible candidate exercised same-handle checks");
  require(windows == 1, "Windows volume not excluded exactly once");
  std::cout << "PASS: read-only refresh; volumes=" << second.size()
            << " candidates=" << candidates << " blocked=" << blocked
            << " windows_excluded=" << windows << " handle_checks=" << verified_handles << '\n';
}
}
int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--probe-helper") {
      const auto candidates = enumerate_partition_candidates();
      for (const auto& candidate : candidates) {
        if (candidate.availability != A::candidate || candidate.layout != PartitionLayout::mbr ||
            candidate.length_bytes < 20ULL * 1024 * 1024 || candidate.length_bytes > (1ULL << 40)) continue;
        std::array<wchar_t, 32768> module{};
        const auto n = ::GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
        require(n && n < module.size(), "locate helper test");
        const auto helper = std::filesystem::path(module.data()).parent_path().parent_path() / L"usb_vault_volume_creator.exe";
        usb_vault::volume_creation::VolumeCreator creator(helper);
        creator.check_partition(candidate);
        auto stale = candidate; stale.device_id[0] ^= 1;
        bool rejected{};
        try { creator.check_partition(stale); } catch (const std::system_error& error) {
          rejected = error.code() == usb_vault::volume_creation::make_error_code(usb_vault::volume_creation::CreationError::target_access_denied);
        }
        require(rejected, "helper accepted replaced device");
        std::cout << "PASS: read-only helper request and changed-identity rejection\n";
        return 0;
      }
      std::cout << "SKIP: no supported MBR candidate for read-only helper integration\n";
      return 77;
    }
    if (argc == 1) { unit_tests(); snapshot_tests(); rejected_handles(); write_policy_tests(); }
    else if (argc == 2 && std::string_view(argv[1]) == "--scan") scan();
    else if (argc == 2 && std::string_view(argv[1]) == "--scan-handles") scan(true);
    else throw std::runtime_error("expected no arguments, --scan or --scan-handles");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "partition_catalog_tests failed: " << error.what() << '\n';
    return 1;
  }
}
