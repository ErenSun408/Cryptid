// Opt-in feasibility evidence. Never accepts an existing disk or partition.
#include "usb_vault/security/secure_buffer.h"
#include "usb_vault/volume/partition_catalog.h"
#include "volume/partition_catalog_detail.h"
#include "usb_vault/volume_creation/volume_creator.h"
#include "usb_vault/volume/volume_provider.h"
#include "usb_vault/volume/unmount_retry.h"
#include "volume/locked_driver_buffer.h"
#include "volume/mount_request.h"
#include "volume/veracrypt_driver_abi.h"
#include "volume_creation/veracrypt_format_abi.h"
#include <Windows.h>
#include <winioctl.h>
#include <initguid.h>
#include <virtdisk.h>
#include <objbase.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#include <utility>

namespace {
namespace abi = usb_vault::volume::detail::abi;
namespace fmt = usb_vault::volume_creation::detail::format_abi;
constexpr LONGLONG kDiskBytes = 256LL * 1024 * 1024;
constexpr LONGLONG kOffset = 1024 * 1024;
constexpr LONGLONG kPartitionBytes = 254LL * 1024 * 1024;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void check(DWORD code, const char* message) {
  if (code != ERROR_SUCCESS) throw std::system_error(static_cast<int>(code), std::system_category(), message);
}
class Handle {
 public:
  explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
  ~Handle() { if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) ::CloseHandle(value_); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  HANDLE get() const { return value_; }
 private: HANDLE value_;
};
void ioctl(HANDLE h, DWORD code, void* input, DWORD input_size, void* output, DWORD output_size) {
  DWORD returned{};
  if (!::DeviceIoControl(h, code, input, input_size, output, output_size, &returned, nullptr))
    check(::GetLastError(), "fixture device control");
  require(!output_size || returned >= output_size, "short fixture query");
}
template<class T> T query(HANDLE h, DWORD code) {
  T value{}; ioctl(h, code, nullptr, 0, &value, sizeof(value)); return value;
}
std::filesystem::path executable_directory() {
  std::vector<wchar_t> path(32768);
  const DWORD count = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  require(count && count < path.size(), "executable path unavailable");
  return std::filesystem::path(std::wstring(path.data(), count)).parent_path();
}
bool elevated() {
  HANDLE raw{};
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw)) check(::GetLastError(), "process token");
  Handle token(raw); TOKEN_ELEVATION value{}; DWORD count{};
  if (!::GetTokenInformation(token.get(), TokenElevation, &value, sizeof(value), &count)) check(::GetLastError(), "elevation query");
  return value.TokenIsElevated != 0;
}
struct Layout { DRIVE_LAYOUT_INFORMATION_EX header; PARTITION_INFORMATION_EX extra[3]; };
static_assert(sizeof(Layout) == offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) + 4 * sizeof(PARTITION_INFORMATION_EX));

class OwnedDisk {
 public:
  OwnedDisk() = default;
  OwnedDisk(const OwnedDisk&) = delete;
  OwnedDisk& operator=(const OwnedDisk&) = delete;
  ~OwnedDisk() {
    // Keep failed evidence; never delete an unknown or still-mounted image.
    if (disk_ != INVALID_HANDLE_VALUE) {
      if (attached_) ::DetachVirtualDisk(disk_, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
      ::CloseHandle(disk_);
    }
  }
  void create() {
    GUID unique{}; require(SUCCEEDED(::CoCreateGuid(&unique)), "fixture GUID");
    std::array<wchar_t, 40> name{}; require(::StringFromGUID2(unique, name.data(), static_cast<int>(name.size())) != 0, "fixture name");
    directory_ = executable_directory() / (std::wstring(L"partition-fixture-") + name.data());
    require(::CreateDirectoryW(directory_.c_str(), nullptr) != FALSE, "new fixture directory required");
    image_ = directory_ / L"owned.vhd";
    VIRTUAL_STORAGE_TYPE type{VIRTUAL_STORAGE_TYPE_DEVICE_VHD, VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT};
    CREATE_VIRTUAL_DISK_PARAMETERS parameters{};
    parameters.Version = CREATE_VIRTUAL_DISK_VERSION_1;
    parameters.Version1.UniqueId = unique;
    parameters.Version1.MaximumSize = kDiskBytes;
    parameters.Version1.SectorSizeInBytes = 512;
    HANDLE created{};
    check(::CreateVirtualDisk(&type, image_.c_str(), VIRTUAL_DISK_ACCESS_ALL, nullptr,
        CREATE_VIRTUAL_DISK_FLAG_NONE, 0, &parameters, nullptr, &created), "create owned VHD");
    disk_ = created;
    ATTACH_VIRTUAL_DISK_PARAMETERS attach{}; attach.Version = ATTACH_VIRTUAL_DISK_VERSION_1;
    check(::AttachVirtualDisk(disk_, nullptr, ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER, 0, &attach, nullptr), "attach owned VHD");
    attached_ = true;
    physical_ = physical_path();
    Handle physical(::CreateFileW(physical_.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    require(physical.get() != INVALID_HANDLE_VALUE, "open owned disk");
    const auto number = query<STORAGE_DEVICE_NUMBER>(physical.get(), IOCTL_STORAGE_GET_DEVICE_NUMBER);
    require(number.DeviceType == FILE_DEVICE_DISK, "fixture is not a disk");
    disk_number_ = number.DeviceNumber;
    require(query<GET_LENGTH_INFORMATION>(physical.get(), IOCTL_DISK_GET_LENGTH_INFO).Length.QuadPart == kDiskBytes, "fixture length mismatch");
    require(query<DISK_GEOMETRY>(physical.get(), IOCTL_DISK_GET_DRIVE_GEOMETRY).BytesPerSector == 512, "fixture sector mismatch");
    require(physical_ == physical_path(), "owned disk changed before initialization");
    require(::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&signature_), sizeof(signature_), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 && signature_, "disk signature");
    CREATE_DISK initialize{}; initialize.PartitionStyle = PARTITION_STYLE_MBR; initialize.Mbr.Signature = signature_;
    ioctl(physical.get(), IOCTL_DISK_CREATE_DISK, &initialize, sizeof(initialize), nullptr, 0);
    Layout layout{}; layout.header.PartitionStyle = PARTITION_STYLE_MBR;
    layout.header.PartitionCount = 4; layout.header.Mbr.Signature = signature_;
    auto& partition = layout.header.PartitionEntry[0];
    partition.PartitionStyle = PARTITION_STYLE_MBR; partition.StartingOffset.QuadPart = kOffset;
    partition.PartitionLength.QuadPart = kPartitionBytes; partition.PartitionNumber = 1;
    partition.RewritePartition = TRUE; partition.Mbr.PartitionType = PARTITION_IFS;
    partition.Mbr.RecognizedPartition = TRUE; partition.Mbr.HiddenSectors = static_cast<DWORD>(kOffset / 512);
    for (auto& unused : layout.extra) { unused.PartitionStyle = PARTITION_STYLE_MBR; unused.RewritePartition = TRUE; }
    ioctl(physical.get(), IOCTL_DISK_SET_DRIVE_LAYOUT_EX, &layout, sizeof(layout), nullptr, 0);
    ioctl(physical.get(), IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0);
    target_ = L"\\Device\\Harddisk" + std::to_wstring(disk_number_) + L"\\Partition1";
    for (int attempt = 0; attempt < 100; ++attempt) {
      Handle part(::CreateFileW((L"\\\\?\\GLOBALROOT" + target_).c_str(), 0,
          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
      if (part.get() != INVALID_HANDLE_VALUE) { verify(); return; }
      ::Sleep(50);
    }
    throw std::runtime_error("owned partition arrival timed out");
  }
  void verify() const {
    require(attached_ && physical_path() == physical_, "owned attachment changed");
    Handle disk(::CreateFileW(physical_.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    require(disk.get() != INVALID_HANDLE_VALUE, "owned disk missing");
    const auto layout = query<Layout>(disk.get(), IOCTL_DISK_GET_DRIVE_LAYOUT_EX);
    require(layout.header.PartitionStyle == PARTITION_STYLE_MBR && layout.header.Mbr.Signature == signature_ && layout.header.PartitionCount == 4, "owned layout changed");
    Handle part(::CreateFileW((L"\\\\?\\GLOBALROOT" + target_).c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    require(part.get() != INVALID_HANDLE_VALUE, "owned partition missing");
    const auto number = query<STORAGE_DEVICE_NUMBER>(part.get(), IOCTL_STORAGE_GET_DEVICE_NUMBER);
    const auto info = query<PARTITION_INFORMATION_EX>(part.get(), IOCTL_DISK_GET_PARTITION_INFO_EX);
    require(number.DeviceNumber == disk_number_ && number.PartitionNumber == 1 &&
        info.PartitionStyle == PARTITION_STYLE_MBR && info.PartitionNumber == 1 &&
        info.StartingOffset.QuadPart == kOffset && info.PartitionLength.QuadPart == kPartitionBytes &&
        info.Mbr.PartitionType == PARTITION_IFS && !info.Mbr.BootIndicator, "owned target identity mismatch");
  }
  usb_vault::volume::PartitionCandidate prepare_ntfs() const {
    verify();
    std::array<wchar_t, 64> name{};
    const HANDLE search = ::FindFirstVolumeW(name.data(), static_cast<DWORD>(name.size()));
    require(search != INVALID_HANDLE_VALUE, "fixture volume search");
    std::wstring volume;
    try {
      do {
        std::wstring path(name.data()); path.pop_back();
        Handle handle(::CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
        STORAGE_DEVICE_NUMBER number{}; DWORD count{};
        if (handle.get() != INVALID_HANDLE_VALUE && ::DeviceIoControl(handle.get(), IOCTL_STORAGE_GET_DEVICE_NUMBER,
            nullptr, 0, &number, sizeof(number), &count, nullptr) && count >= sizeof(number) &&
            number.DeviceType == FILE_DEVICE_DISK && number.DeviceNumber == disk_number_ && number.PartitionNumber == 1) {
          require(volume.empty(), "ambiguous owned volume"); volume = name.data();
        }
      } while (::FindNextVolumeW(search, name.data(), static_cast<DWORD>(name.size())));
      require(::GetLastError() == ERROR_NO_MORE_FILES, "fixture enumeration failed");
    } catch (...) { ::FindVolumeClose(search); throw; }
    ::FindVolumeClose(search);
    require(volume.size() == 49 && volume.starts_with(LR"(\\?\Volume{)"), "owned volume missing");
    verify();
    std::array<wchar_t, MAX_PATH> system{};
    const UINT length = ::GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    require(length && length < system.size(), "system formatter path");
    const auto program = std::filesystem::path(system.data()) / L"format.com";
    // Only the freshly created VHD's verified volume; no user-supplied target.
    auto argument = volume; argument.pop_back();
    std::wstring command = L"\"" + program.native() + L"\" " + argument + L" /FS:NTFS /Q /Y /V:UVFixture";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    require(::CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &process) != FALSE, "start owned NTFS preparation");
    Handle child(process.hProcess), thread(process.hThread);
    require(::WaitForSingleObject(child.get(), INFINITE) == WAIT_OBJECT_0, "wait NTFS preparation");
    DWORD code{}; require(::GetExitCodeProcess(child.get(), &code) && code == 0, "owned NTFS preparation failed");
    verify();
    for (const auto& candidate : usb_vault::volume::enumerate_partition_candidates()) {
      if (candidate.volume_path == volume && candidate.disk_number == disk_number_ && candidate.partition_number == 1 &&
          candidate.offset_bytes == kOffset && candidate.length_bytes == kPartitionBytes &&
          candidate.availability == usb_vault::volume::PartitionAvailability::candidate) return candidate;
    }
    throw std::runtime_error("prepared NTFS fixture not eligible");
  }
  const std::wstring& target() const { return target_; }
  std::filesystem::path container_path() const { return directory_ / L"regression.hc"; }
  void cleanup() {
    check(::DetachVirtualDisk(disk_, DETACH_VIRTUAL_DISK_FLAG_NONE, 0), "detach fixture"); attached_ = false;
    static_cast<void>(::CloseHandle(std::exchange(disk_, INVALID_HANDLE_VALUE)));
    require(::DeleteFileW(image_.c_str()) != FALSE, "remove owned image");
    require(::RemoveDirectoryW(directory_.c_str()) != FALSE, "remove fixture directory");
  }
 private:
  std::wstring physical_path() const {
    std::array<wchar_t, 256> value{}; ULONG size = sizeof(value);
    check(::GetVirtualDiskPhysicalPath(disk_, &size, value.data()), "resolve owned disk");
    require(value.back() == L'\0', "unterminated device path"); return value.data();
  }
  HANDLE disk_{INVALID_HANDLE_VALUE}; bool attached_{}; DWORD disk_number_{}; DWORD signature_{};
  std::filesystem::path directory_, image_; std::wstring physical_, target_;
};

struct FormatProgress {
  int calls{};
  bool invalid{};
  bool cancel{};
  static BOOL CALLBACK observe(int percent, void* data) {
    auto& state = *static_cast<FormatProgress*>(data);
    ++state.calls; state.invalid = state.invalid || percent < 0 || percent > 100;
    return state.cancel ? FALSE : TRUE;
  }
};
class Backend {
 public:
  Backend() {
    module_ = ::LoadLibraryExW((executable_directory() / L"VeraCryptFormat.dll").c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    require(module_ != nullptr, "patched FormatDLL unavailable beside fixture");
  }
  ~Backend() { if (initialized_) shutdown_(); if (module_) ::FreeLibrary(module_); }
  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;
  void initialize() {
    const auto initialize = reinterpret_cast<fmt::Initialize>(::GetProcAddress(module_, "VeraCryptFormat_Initialize"));
    shutdown_ = reinterpret_cast<fmt::Shutdown>(::GetProcAddress(module_, "VeraCryptFormat_Shutdown"));
    format_ = reinterpret_cast<fmt::FormatBinary>(::GetProcAddress(module_, "VeraCryptFormatBinary"));
    if (!initialize || !shutdown_ || !format_) throw std::runtime_error("FormatDLL export missing");
    require(initialize() == fmt::kSuccess, "FormatDLL initialization failed"); initialized_ = true;
  }
  void format(const OwnedDisk& disk, const usb_vault::security::SecureBuffer& password, bool same_handle, bool cancel_only = false) {
    disk.verify();
    usb_vault::volume::VolumeProvider provider;
    for (const auto& mounted : provider.mounted_volumes())
      require(mounted.volume_path != disk.target(), "owned native path already mounted");
    if (same_handle) { format_handle(disk, password, cancel_only); return; }
    fmt::Options options{};
    options.path = disk.target().c_str(); options.is_device = TRUE;
    options.password = reinterpret_cast<const char*>(password.data()); options.size = kPartitionBytes;
    options.encryption_algorithm = L"AES"; options.hash_algorithm = L"SHA-512";
    options.filesystem = L"FAT"; options.quick_format = TRUE;
    const int code = format_(&options, static_cast<std::uint32_t>(password.size()));
    require(code == fmt::kSuccess, "owned partition format failed");
  }
  void file_regression(const OwnedDisk& disk, const usb_vault::security::SecureBuffer& password) {
    const auto file = disk.container_path();
    require(!std::filesystem::exists(file), "regression file already exists");
    fmt::Options options{};
    options.path = file.c_str(); options.password = reinterpret_cast<const char*>(password.data());
    options.size = 64ULL * 1024 * 1024; options.encryption_algorithm = L"AES";
    options.hash_algorithm = L"SHA-512"; options.filesystem = L"FAT"; options.quick_format = TRUE;
    require(format_(&options, 64) == fmt::kSuccess, "file-format regression");
    usb_vault::security::SecureBuffer hidden(64);
    std::copy(password.bytes().begin(), password.bytes().end(), hidden.bytes().begin());
    hidden.bytes().back() ^= std::byte{0x80};
    usb_vault::volume::VolumeProvider provider;
    const auto verify_mount = [&](const auto& key, usb_vault::volume::VolumeType type) {
      const DWORD used = ::GetLogicalDrives(); require(used, "regression drive list");
      wchar_t drive = L'Z'; while (drive >= L'D' && (used & (DWORD{1} << (drive - L'A')))) --drive;
      require(drive >= L'D', "no regression drive");
      static_cast<void>(provider.mount_file(file.native(), key, {.drive_letter = drive, .notify_shell = false}));
      const auto identity = provider.mount_identity(drive);
      try {
        const auto list = provider.mounted_volumes();
        const auto item = std::ranges::find(list, drive, &usb_vault::volume::MountedVolume::drive_letter);
        require(item != list.end() && item->type == type, "regression mounted wrong volume type");
        static_cast<void>(usb_vault::volume::unmount_with_retry(provider, drive, {.notify_shell = false}));
      } catch (...) {
        if (provider.mount_identity(drive) == identity)
          static_cast<void>(provider.unmount(drive, {.force = true, .notify_shell = false}));
        throw;
      }
    };
    verify_mount(password, usb_vault::volume::VolumeType::normal);
    options.password = reinterpret_cast<const char*>(hidden.data());
    options.hidden_volume = TRUE; options.hidden_volume_host_size = options.size;
    options.size = 16ULL * 1024 * 1024;
    require(format_(&options, 64) == fmt::kSuccess, "hidden file-format regression");
    verify_mount(hidden, usb_vault::volume::VolumeType::hidden);
    require(::DeleteFileW(file.c_str()) != FALSE, "regression file cleanup failed");
    std::cout << "PASS: existing normal and hidden file-container formats and mounts\n";
  }
  void format_handle(const OwnedDisk& disk, const usb_vault::security::SecureBuffer& password, bool cancel_only) {
    const auto format = reinterpret_cast<fmt::FormatPartitionBinary>(::GetProcAddress(module_, "VeraCryptFormatPartitionBinary"));
    if (!format) throw std::runtime_error("candidate same-handle export missing");
    const auto* secret = reinterpret_cast<const unsigned char*>(password.data());
    const std::wstring path = LR"(\\?\GLOBALROOT)" + disk.target();
    Handle target(::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    require(target.get() != INVALID_HANDLE_VALUE, "open fixture partition for same-handle format");
    const auto sample = [&] {
      // Aligned sector reads also work for DASD handles treated as noncached.
      alignas(4096) std::array<unsigned char, 512> sector{};
      std::array<unsigned char, 1024> result{};
      for (int i = 0; i < 2; ++i) {
        LARGE_INTEGER offset{}; offset.QuadPart = i ? kPartitionBytes - 512 : 0;
        DWORD read{};
        require(::SetFilePointerEx(target.get(), offset, nullptr, FILE_BEGIN) &&
            ::ReadFile(target.get(), sector.data(), static_cast<DWORD>(sector.size()), &read, nullptr) && read == sector.size(), "read fixture boundary sectors");
        std::copy(sector.begin(), sector.end(), result.begin() + i * 512);
      }
      return result;
    };
    const auto before = sample();
    std::array<wchar_t, 32> filesystem{};
    const BOOL known_filesystem = ::GetVolumeInformationByHandleW(target.get(), nullptr, 0, nullptr, nullptr, nullptr,
        filesystem.data(), static_cast<DWORD>(filesystem.size()));
    require(known_filesystem && std::wstring_view(filesystem.data()) == L"RAW", "new fixture is not explicitly RAW");
    require(format(nullptr, kOffset, kPartitionBytes, secret, 64, nullptr, nullptr) == fmt::kInvalidParameter, "null target accepted");
    Handle event(::CreateEventW(nullptr, FALSE, FALSE, nullptr));
    require(event.get() != nullptr, "fixture event unavailable");
    require(format(event.get(), kOffset, kPartitionBytes, secret, 64, nullptr, nullptr) == fmt::kInvalidParameter, "non-disk handle accepted");
    require(format(target.get(), 0, kPartitionBytes, secret, 64, nullptr, nullptr) == fmt::kInvalidParameter, "zero offset accepted");
    require(format(target.get(), kOffset, kPartitionBytes + 1, secret, 64, nullptr, nullptr) == fmt::kInvalidParameter, "unaligned length accepted");
    require(format(target.get(), kOffset, kPartitionBytes, secret, 63, nullptr, nullptr) == fmt::kInvalidParameter, "password length accepted");
    require(format(target.get(), kOffset + 512, kPartitionBytes, secret, 64, nullptr, nullptr) == fmt::kInvalidParameter, "wrong offset accepted");
    require(format(target.get(), kOffset, kPartitionBytes + 512, secret, 64, nullptr, nullptr) == fmt::kInvalidParameter, "wrong length accepted");
    require(sample() == before, "invalid request modified boundary sectors");
    {
      Handle busy(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
      require(busy.get() != INVALID_HANDLE_VALUE, "busy fixture handle unavailable");
      require(format(target.get(), kOffset, kPartitionBytes, secret, 64, nullptr, nullptr) == fmt::kAccessDenied, "busy target did not reject before format");
    }
    require(sample() == before, "busy rejection modified boundary sectors");
    std::cout << "PASS: same-handle shape, offset, length and busy guards; boundary sectors unchanged\n";
    if (cancel_only) {
      FormatProgress cancelled; cancelled.cancel = true;
      require(format(target.get(), kOffset, kPartitionBytes, secret, 64, &FormatProgress::observe, &cancelled) == fmt::kUserAbort &&
          cancelled.calls > 0 && !cancelled.invalid, "cancellation did not report abort");
      DWORD flags{};
      require(::GetHandleInformation(target.get(), &flags) != FALSE, "cancel consumed caller handle");
      std::cout << "PASS: cancellation returns abort and retains caller handle; partial fixture will be discarded\n";
      return;
    }
    FormatProgress completed;
    const int result = format(target.get(), kOffset, kPartitionBytes, secret, 64, &FormatProgress::observe, &completed);
    if (result != fmt::kSuccess) { std::cerr << "same-handle format code " << result << '\n'; throw std::runtime_error("same-handle format failed"); }
    require(completed.calls > 0 && !completed.invalid, "format progress invalid");
    DWORD flags{};
    require(::GetHandleInformation(target.get(), &flags) != FALSE, "backend consumed caller handle");
    std::cout << "PASS: bounded progress and caller handle retained after same-handle format\n";
  }
 private: HMODULE module_{}; fmt::Shutdown shutdown_{}; fmt::FormatBinary format_{}; bool initialized_{};
};

void helper_format(OwnedDisk& disk, const usb_vault::security::SecureBuffer& password) {
  using namespace usb_vault::volume_creation;
  const auto candidate = disk.prepare_ntfs();
  const usb_vault::volume::PartitionIdentity saved{candidate.device_id, candidate.device_id_flags,
      1, candidate.logical_sector_bytes, candidate.offset_bytes, candidate.length_bytes};
  require(usb_vault::volume::resolve_partition_location(saved,
      usb_vault::volume::enumerate_partition_locations()).has_value(), "NTFS location not found");
  VolumeCreator creator(executable_directory() / L"usb_vault_volume_creator.exe");
  const auto read_header = [&] {
    auto path = candidate.volume_path; path.pop_back();
    Handle handle(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr));
    require(handle.get() != INVALID_HANDLE_VALUE, "open NTFS header probe");
    std::vector<std::byte> bytes(65536); DWORD count{};
    require(::ReadFile(handle.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) &&
        count == bytes.size(), "read NTFS header probe");
    return bytes;
  };
  // Give only this owned VHD an original access path, then prove consent
  // refusal preserves it and successful creation removes it.
  std::wstring original_root;
  const DWORD used = ::GetLogicalDrives();
  require(used != 0, "drive inventory failed");
  for (wchar_t letter = L'Z'; letter >= L'D'; --letter) {
    if (!(used & (1UL << (letter - L'A')))) {
      original_root = std::wstring(1, letter) + L":\\";
      if (::SetVolumeMountPointW(original_root.c_str(), candidate.volume_path.c_str())) break;
      original_root.clear();
    }
  }
  require(!original_root.empty(), "cannot assign owned NTFS access path");
  const auto before = read_header();
  // Declining and throwing must both leave the original NTFS identity intact.
  for (int decision = 0; decision < 2; ++decision) {
    unsigned calls{}; bool cancelled{};
    try { creator.format_partition(candidate, password, [&]() -> bool {
      ++calls; if (decision == 1) throw std::runtime_error("fixture declined"); return false;
    }); } catch (const std::system_error& error) { cancelled = error.code() == make_error_code(CreationError::operation_cancelled); }
    require(cancelled && calls == 1, "declined partition confirmation did not stop");
    require(read_header() == before, "declined confirmation changed volume header");
    wchar_t original_volume[64]{};
    require(::GetVolumeNameForVolumeMountPointW(original_root.c_str(), original_volume, 64) &&
        candidate.volume_path == original_volume, "declined confirmation removed original access");
    require(usb_vault::volume::matches_partition_snapshot(candidate,
        usb_vault::volume::enumerate_partition_candidates()), "declined format modified NTFS identity");
  }
  std::cout << "PASS: declined and throwing confirmation preserve NTFS identity and first 64 KiB" << std::endl;
  unsigned confirmations{}, reports{};
  try {
    creator.format_partition(candidate, password, [&] { disk.verify(); ++confirmations; return true; },
        [&](const CreationProgress& progress) {
          require(progress.percent <= 100 && (progress.stage == CreationStage::authorized ||
              progress.stage == CreationStage::formatting), "unexpected partition progress"); ++reports;
        });
  } catch (const std::exception& error) {
    throw std::runtime_error(std::string("confirmed NTFS helper format: ") + error.what());
  }
  require(confirmations == 1 && reports > 1, "partition confirmation/progress missing");
  wchar_t remaining_paths[32768]{}; DWORD needed{};
  require(::GetVolumePathNamesForVolumeNameW(candidate.volume_path.c_str(), remaining_paths, 32768, &needed) &&
      !remaining_paths[0], "original access path survived creation");
  std::cout << "PASS: consent refusal preserves original drive; creation removes all original access paths" << std::endl;
  const auto located = usb_vault::volume::resolve_partition_location(saved,
      usb_vault::volume::enumerate_partition_locations());
  require(located.has_value(), "ciphertext location lost after formatting");
  auto path = located->volume_path; path.pop_back();
  Handle retained(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr, OPEN_EXISTING, 0, nullptr));
  require(retained.get() != INVALID_HANDLE_VALUE, "open discovered ciphertext location");
  require(usb_vault::volume::detail::matches_partition_location_handle(retained.get(), *located),
      "ciphertext retained-handle identity mismatch");
  auto replaced = *located; replaced.identity.device_id[0] ^= 1;
  require(!usb_vault::volume::detail::matches_partition_location_handle(retained.get(), replaced),
      "ciphertext handle accepted replaced identity");
  require(!usb_vault::volume::matches_partition_snapshot(candidate,
      usb_vault::volume::enumerate_partition_candidates()), "ciphertext remained a format candidate");
  std::cout << "PASS: saved identity rediscovers ciphertext; retained handle verifies; format selection stays strict" << std::endl;
  std::cout << "PASS: elevated NTFS partition format, declined/throwing consent and single-use confirmation\n";
}

// Experiment-only driver request: the product mount_file path policy stays intact.
void mount_owned(const OwnedDisk& disk, const usb_vault::security::SecureBuffer& password, wchar_t drive) {
  disk.verify();
  const auto locations = usb_vault::volume::enumerate_partition_locations();
  std::optional<usb_vault::volume::PartitionIdentity> identity;
  for (const auto& location : locations) {
    const auto native = L"\\Device\\Harddisk" + std::to_wstring(location.disk_number) +
        L"\\Partition" + std::to_wstring(location.partition_number);
    if (native == disk.target()) {
      require(!identity, "owned partition identity ambiguous");
      identity = location.identity;
    }
  }
  require(identity.has_value(), "owned partition not discovered");
  usb_vault::volume::VolumeProvider provider;
  static_cast<void>(provider.mount_partition(*identity, password,
      {.drive_letter = drive, .notify_shell = false}));
}
wchar_t free_letter() {
  const DWORD used = ::GetLogicalDrives(); require(used != 0, "drive list unavailable");
  for (wchar_t drive = L'Z'; drive >= L'D'; --drive)
    if ((used & (DWORD{1} << (drive - L'A'))) == 0) return drive;
  throw std::runtime_error("no free fixture drive");
}
void roundtrip(OwnedDisk& disk, const usb_vault::security::SecureBuffer& password) {
  usb_vault::volume::VolumeProvider provider;
  const wchar_t drive = free_letter(); bool mounted = false;
  std::optional<usb_vault::volume::MountIdentity> identity;
  const auto unmount = [&] {
    require(identity && identity->volume_path == disk.target() && provider.mount_identity(drive) == *identity,
        "fixture mount identity changed before cleanup");
    static_cast<void>(usb_vault::volume::unmount_with_retry(provider, drive, {.notify_shell = false}));
    mounted = false; identity.reset();
  };
  try {
    mount_owned(disk, password, drive); mounted = true; identity = provider.mount_identity(drive);
    require(provider.mount_identity(drive).volume_path == disk.target(), "mounted target mismatch");
    const std::wstring file = std::wstring(1, drive) + L":\\probe.bin";
    const std::array<unsigned char, 8> expected{0, 1, 17, 33, 0, 127, 254, 255};
    {
      Handle output(::CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
      require(output.get() != INVALID_HANDLE_VALUE, "create probe"); DWORD written{};
      require(::WriteFile(output.get(), expected.data(), static_cast<DWORD>(expected.size()), &written, nullptr) && written == expected.size(), "write probe");
      require(::FlushFileBuffers(output.get()) != FALSE, "flush probe");
    }
    unmount(); mount_owned(disk, password, drive); mounted = true; identity = provider.mount_identity(drive);
    {
      Handle input(::CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
      require(input.get() != INVALID_HANDLE_VALUE, "reopen probe"); std::array<unsigned char, 8> actual{}; DWORD read{};
      require(::ReadFile(input.get(), actual.data(), static_cast<DWORD>(actual.size()), &read, nullptr) && read == actual.size() && actual == expected, "remounted bytes mismatch");
    }
    unmount();
  } catch (...) {
    if (mounted) {
      try { unmount(); }
      catch (...) {
        // Failure cleanup is limited to this exact ephemeral test mount, whose
        // only content was generated above. Never force by drive letter alone.
        try {
          require(identity && identity->volume_path == disk.target() && provider.mount_identity(drive) == *identity,
              "fixture cleanup identity changed");
          static_cast<void>(provider.unmount(drive, {.force = true, .notify_shell = false}));
        } catch (...) { std::cerr << "fixture cleanup failed; image retained for inspection\n"; }
      }
    }
    throw;
  }
}
}
int wmain(int argc, wchar_t** argv) {
  if (argc != 2 || (std::wstring_view(argv[1]) != L"--run-owned-vhd" && std::wstring_view(argv[1]) != L"--run-owned-vhd-handle" && std::wstring_view(argv[1]) != L"--run-owned-vhd-helper")) {
    std::cout << "SKIP: explicit --run-owned-vhd required; no existing disk arguments accepted\n"; return 77;
  }
  try {
    if (!elevated()) { std::cout << "SKIP: administrator token required; no disk created\n"; return 77; }
    usb_vault::volume::VolumeProvider provider;
    require(provider.driver_version() == abi::kSupportedDriverVersion, "unsupported driver version");
    Backend backend; backend.initialize();
    usb_vault::security::SecureBuffer password(64);
    require(::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(password.data()), static_cast<ULONG>(password.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0, "fixture password generation");
    password.bytes()[0] = std::byte{0}; // Exercise explicit binary length.
    if (std::wstring_view(argv[1]) == L"--run-owned-vhd-handle") {
      OwnedDisk cancelled; cancelled.create(); backend.format(cancelled, password, true, true); cancelled.cleanup();
    }
    OwnedDisk disk; disk.create();
    if (std::wstring_view(argv[1]) == L"--run-owned-vhd-helper") helper_format(disk, password);
    else backend.format(disk, password, std::wstring_view(argv[1]) == L"--run-owned-vhd-handle");
    roundtrip(disk, password); if (std::wstring_view(argv[1]) == L"--run-owned-vhd-handle") backend.file_regression(disk, password); disk.cleanup();
    std::cout << "PASS: owned MBR/512 VHD partition format, mount, write, unmount, remount and read; fixture removed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
