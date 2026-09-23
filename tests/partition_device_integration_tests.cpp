#include "volume_creation/partition_inplace_backend.h"
// Opt-in feasibility evidence. Never accepts an existing disk or partition.
#include "usb_vault/security/secure_buffer.h"
#include "application/card_login_mount_session_internal.h"
#include "usb_vault/application/container_layout_v1.h"
#include "usb_vault/application/container_index_store_v1.h"
#include "usb_vault/security/kdf_v1.h"
#include "usb_vault/volume/volume_file_operations.h"

#include "usb_vault/volume/partition_catalog.h"
#include "volume/partition_catalog_detail.h"
#include "volume/partition_inplace_plan_detail.h"
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
  void create(bool gpt = false) {
    gpt_ = gpt;
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
    CREATE_DISK initialize{};
    Layout layout{};
    auto& partition = layout.header.PartitionEntry[0];
    partition.StartingOffset.QuadPart = kOffset;
    partition.PartitionLength.QuadPart = kPartitionBytes; partition.PartitionNumber = 1;
    partition.RewritePartition = TRUE;
    if (gpt_) {
      initialize.PartitionStyle = PARTITION_STYLE_GPT;
      disk_id_ = unique; initialize.Gpt.DiskId = unique; initialize.Gpt.MaxPartitionCount = 128;
      require(SUCCEEDED(::CoCreateGuid(&partition_id_)), "GPT partition GUID");
      layout.header.PartitionStyle = PARTITION_STYLE_GPT; layout.header.PartitionCount = 1;
      layout.header.Gpt.DiskId = unique; layout.header.Gpt.MaxPartitionCount = 128;
      layout.header.Gpt.StartingUsableOffset.QuadPart = kOffset;
      layout.header.Gpt.UsableLength.QuadPart = kPartitionBytes;
      partition.PartitionStyle = PARTITION_STYLE_GPT;
      partition.Gpt.PartitionType = {0xebd0a0a2,0xb9e5,0x4433,{0x87,0xc0,0x68,0xb6,0xb7,0x26,0x99,0xc7}};
      partition.Gpt.PartitionId = partition_id_;
    } else {
      initialize.PartitionStyle = PARTITION_STYLE_MBR; initialize.Mbr.Signature = signature_;
      layout.header.PartitionStyle = PARTITION_STYLE_MBR; layout.header.PartitionCount = 4;
      layout.header.Mbr.Signature = signature_;
      partition.PartitionStyle = PARTITION_STYLE_MBR; partition.Mbr.PartitionType = PARTITION_IFS;
      partition.Mbr.RecognizedPartition = TRUE; partition.Mbr.HiddenSectors = static_cast<DWORD>(kOffset / 512);
      for (auto& unused : layout.extra) { unused.PartitionStyle = PARTITION_STYLE_MBR; unused.RewritePartition = TRUE; }
    }
    ioctl(physical.get(), IOCTL_DISK_CREATE_DISK, &initialize, sizeof(initialize), nullptr, 0);
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
    if (gpt_) {
      const auto layout = query<DRIVE_LAYOUT_INFORMATION_EX>(disk.get(), IOCTL_DISK_GET_DRIVE_LAYOUT_EX);
      require(layout.PartitionStyle == PARTITION_STYLE_GPT && ::IsEqualGUID(layout.Gpt.DiskId,disk_id_) &&
          layout.PartitionCount == 1, "owned GPT layout changed");
    } else {
      const auto layout = query<Layout>(disk.get(), IOCTL_DISK_GET_DRIVE_LAYOUT_EX);
      require(layout.header.PartitionStyle == PARTITION_STYLE_MBR && layout.header.Mbr.Signature == signature_ && layout.header.PartitionCount == 4, "owned layout changed");
    }
    Handle part(::CreateFileW((L"\\\\?\\GLOBALROOT" + target_).c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    require(part.get() != INVALID_HANDLE_VALUE, "owned partition missing");
    const auto number = query<STORAGE_DEVICE_NUMBER>(part.get(), IOCTL_STORAGE_GET_DEVICE_NUMBER);
    const auto info = query<PARTITION_INFORMATION_EX>(part.get(), IOCTL_DISK_GET_PARTITION_INFO_EX);
    require(number.DeviceNumber == disk_number_ && number.PartitionNumber == 1 &&
        info.PartitionStyle == (gpt_ ? PARTITION_STYLE_GPT : PARTITION_STYLE_MBR) && info.PartitionNumber == 1 &&
        info.StartingOffset.QuadPart == kOffset && info.PartitionLength.QuadPart == kPartitionBytes &&
        (gpt_ ? ::IsEqualGUID(info.Gpt.PartitionId, partition_id_) :
        info.Mbr.PartitionType == PARTITION_IFS && !info.Mbr.BootIndicator), "owned target identity mismatch");
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
  bool gpt() const { return gpt_; }
  const GUID& partition_id() const { return partition_id_; }
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
  bool gpt_{}; GUID partition_id_{}, disk_id_{};
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
    const auto mbr_format = reinterpret_cast<fmt::FormatPartitionBinary>(::GetProcAddress(module_, "VeraCryptFormatPartitionBinary"));
    const auto gpt_format = reinterpret_cast<fmt::FormatPartitionGptBinary>(::GetProcAddress(module_, "VeraCryptFormatPartitionGptBinary"));
    if (!mbr_format || !gpt_format) throw std::runtime_error("candidate same-handle export missing");
    const auto format = [&](HANDLE h, std::uint64_t offset, std::uint64_t length, const unsigned char* pw,
        std::uint32_t bytes, fmt::ProgressCallback progress, void* context) {
      return disk.gpt() ? gpt_format(h,offset,length,pw,bytes,progress,context,
          reinterpret_cast<const unsigned char*>(&disk.partition_id()),16)
          : mbr_format(h,offset,length,pw,bytes,progress,context);
    };
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
    if (disk.gpt()) {
      auto wrong = disk.partition_id(); ++wrong.Data1;
      require(mbr_format(target.get(),kOffset,kPartitionBytes,secret,64,nullptr,nullptr) == fmt::kInvalidParameter, "legacy export accepted GPT");
      require(gpt_format(target.get(),kOffset,kPartitionBytes,secret,64,nullptr,nullptr,
          reinterpret_cast<const unsigned char*>(&wrong),16) == fmt::kInvalidParameter, "wrong GPT GUID accepted");
    }
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

void bitlocker_roundtrip(OwnedDisk& disk) {
  using namespace usb_vault;
  const auto candidate = disk.prepare_ntfs();
  const auto sample = candidate.volume_path + L"bitlocker-check.txt";
  {
    Handle file(::CreateFileW(sample.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr));
    require(file.get()!=INVALID_HANDLE_VALUE,"BitLocker fixture file");
    DWORD done{}; const char bytes[]="preserve this file";
    require(::WriteFile(file.get(),bytes,sizeof(bytes),&done,nullptr) && done==sizeof(bytes),"write BitLocker fixture");
  }
  // Only the freshly created and verified VHD volume; no user target argument.
  disk.verify();
  const std::wstring script = L"$ErrorActionPreference='Stop'; $v=Get-CimInstance -Namespace root/CIMV2/Security/MicrosoftVolumeEncryption -ClassName Win32_EncryptableVolume | Where-Object DeviceID -eq '" + candidate.volume_path +
      L"'; if(!$v){exit 2}; $r=Invoke-CimMethod -InputObject $v -MethodName ProtectKeyWithNumericalPassword -Arguments @{FriendlyName='UVFixture'}; if($r.ReturnValue){exit 3}; $r=Invoke-CimMethod -InputObject $v -MethodName Encrypt -Arguments @{EncryptionMethod=[uint32]7;EncryptionFlags=[uint32]1}; if($r.ReturnValue){exit 4}; for($i=0;$i -lt 240;$i++){ $r=Invoke-CimMethod -InputObject $v -MethodName GetConversionStatus; if($r.ReturnValue){exit 5}; if($r.ConversionStatus -eq 1){exit 0}; Start-Sleep -Seconds 1 };exit 6";
  std::array<wchar_t,MAX_PATH> system{};
  require(::GetSystemDirectoryW(system.data(),static_cast<UINT>(system.size()))>0,"system directory");
  const auto exe=std::filesystem::path(system.data())/L"WindowsPowerShell/v1.0/powershell.exe";
  std::wstring command=L"\""+exe.native()+L"\" -NoProfile -NonInteractive -Command \""+script+L"\"";
  STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
  require(::CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process),"prepare BitLocker fixture");
  Handle child(process.hProcess),thread(process.hThread);
  require(::WaitForSingleObject(child.get(),300000)==WAIT_OBJECT_0,"BitLocker setup timed out");
  DWORD code{};require(::GetExitCodeProcess(child.get(),&code) && code==0,"BitLocker fixture encryption failed");
  volume_creation::VolumeCreator creator(executable_directory()/L"usb_vault_volume_creator.exe");
  require(creator.inspect_partition_bitlocker(candidate).state==1,"encrypted fixture not detected");
  bool declined=false;
  try { creator.decrypt_partition_bitlocker(candidate,[]{return false;}); }
  catch(const std::system_error& e) { declined=e.code()==volume_creation::make_error_code(volume_creation::CreationError::operation_cancelled); }
  require(declined && creator.inspect_partition_bitlocker(candidate).state==1,"declined consent changed encryption");
  creator.decrypt_partition_bitlocker(candidate,[]{return true;});
  bool complete=false;
  for(unsigned i=0;i<120;++i) {
    const auto status=creator.inspect_partition_bitlocker(candidate);
    require(volume::valid_bitlocker_status(status),"invalid decryption progress");
    if(status.state==0){complete=true;break;}
    ::Sleep(1000);
  }
  require(complete,"BitLocker decryption did not complete");
  {
    Handle file(::CreateFileW(sample.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
    require(file.get()!=INVALID_HANDLE_VALUE,"decrypted fixture file missing");
    char bytes[19]{};DWORD done{};
    require(::ReadFile(file.get(),bytes,sizeof(bytes),&done,nullptr) && done==sizeof(bytes) &&
        std::string_view(bytes,18)=="preserve this file","BitLocker file readback mismatch");
  }
  disk.verify();
  std::cout << "PASS: owned BitLocker encryption, declined consent, helper decryption, progress and file readback" << std::endl;
}

void inplace_plan_probe(OwnedDisk& disk) {
  const auto candidate = disk.prepare_ntfs();
  auto path = candidate.volume_path; path.pop_back();
  Handle handle(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr, OPEN_EXISTING, 0, nullptr));
  require(handle.get() != INVALID_HANDLE_VALUE, "in-place readonly probe open");
  const auto sample = [&] {
    LARGE_INTEGER zero{}; DWORD read{};
    std::vector<std::byte> data(65536);
    require(::SetFilePointerEx(handle.get(), zero, nullptr, FILE_BEGIN) &&
        ::ReadFile(handle.get(), data.data(), static_cast<DWORD>(data.size()), &read, nullptr) && read == data.size(), "in-place header sample failed");
    return data;
  };
  const auto before = sample();
  const auto plan = usb_vault::volume::detail::inspect_partition_inplace(handle.get(), candidate);
  LARGE_INTEGER zero{}, position{};
  require(::SetFilePointerEx(handle.get(), zero, &position, FILE_CURRENT) && position.QuadPart == 65536,
      "read-only inspection changed caller position");
  const auto again = usb_vault::volume::detail::inspect_partition_inplace(handle.get(), candidate);
  require(plan == again && sample() == before && plan.partition_bytes == candidate.length_bytes &&
      plan.ntfs_bytes - plan.shrink_to_sectors * 512 == 262144, "NTFS planning inconsistent or modified header");
  auto wrong = candidate; wrong.device_id[0] ^= 1;
  bool refused = false;
  try { static_cast<void>(usb_vault::volume::detail::inspect_partition_inplace(handle.get(), wrong)); }
  catch (const std::invalid_argument&) { refused = true; }
  require(refused, "in-place inspection accepted different device");
  std::cout << "PASS: owned NTFS retained-handle in-place plan; header/position preserved; changed identity rejected\n";
}

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
  // Cancel only after the client has sent its positive confirmation. The
  // following policy query is deliberately silent; cancellation must not
  // wait for destructive progress to reach the helper.
  bool accepted = false, cancelled_after_consent = false;
  unsigned cancellation_queries = 0;
  try {
    creator.format_partition(candidate, password, [&] { accepted = true; return true; }, {},
        [&] { return accepted && ++cancellation_queries >= 2; });
  } catch (const std::system_error& error) {
    cancelled_after_consent = error.code() == make_error_code(CreationError::operation_cancelled);
  }
  require(cancelled_after_consent && read_header() == before, "cancel during silent policy check changed header");
  wchar_t preserved_volume[64]{};
  require(::GetVolumeNameForVolumeMountPointW(original_root.c_str(), preserved_volume, 64) &&
      candidate.volume_path == preserved_volume, "cancel during silent policy check removed access");
  std::cout << "PASS: cancellation after consent reaches silent prewrite policy check without modifying original access or header" << std::endl;
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
  std::vector<wchar_t> remaining_paths(32768); DWORD needed{};
  require(::GetVolumePathNamesForVolumeNameW(candidate.volume_path.c_str(), remaining_paths.data(), static_cast<DWORD>(remaining_paths.size()), &needed) &&
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

// Real crypto, index storage, helper and driver; only card authentication is
// supplied by the fixture. No personal settings or index paths are accessed.
class PartitionSessionRuntime final : public usb_vault::application::detail::CardLoginMountSessionRuntime {
 public:
  using Secret = usb_vault::security::SecureBuffer;
  explicit PartitionSessionRuntime(OwnedDisk& disk)
      : disk_(disk), index_(disk.container_path().replace_extension(L".index")),
        creator_(executable_directory() / L"usb_vault_volume_creator.exe") {
    usb_vault::application::ensure_container_index_v1_file(index_.wstring());
    creation_cancelled = [] { return false; };
  }
  usb_vault::application::MountSessionResult mount_authenticated(const usb_vault::application::ContainerLayout&, const Secret&, const Secret&) override { throw std::logic_error("manual fixture only"); }
  bool container_exists(std::wstring_view) override { return false; }
  std::optional<std::uint64_t> container_size_if_present(std::wstring_view) override { return {}; }
  std::optional<std::uint64_t> mounted_volume_size(wchar_t drive) override {
    for (const auto& volume : provider_.mounted_volumes()) if (volume.drive_letter == drive) return volume.disk_length;
    return {};
  }
  std::optional<usb_vault::volume::VolumeType> mounted_volume_type(wchar_t drive) override {
    for (const auto& volume : provider_.mounted_volumes()) if (volume.drive_letter == drive) return volume.type;
    return {};
  }
  usb_vault::volume::VolumeSpace volume_space(wchar_t drive) override { return usb_vault::volume::volume_space(drive); }
  usb_vault::volume::MountIdentity mount_identity(wchar_t drive) override { return provider_.mount_identity(drive); }
  std::uint64_t measure_protected_space(std::wstring_view, const usb_vault::volume::MountIdentity&, const Secret&, const Secret&) override { throw std::logic_error("file operation outside fixture"); }
  void end_measurements() noexcept override {}
  void end_imports() noexcept override {}
  void import_protected(std::wstring_view, const usb_vault::volume::MountIdentity&, const Secret&, const Secret&, std::wstring_view, std::wstring_view, const usb_vault::volume::TransferCancellation&, const std::function<void(unsigned int)>&) override { throw std::logic_error("file operation outside fixture"); }
  Secret derive_container_password(const Secret& input, const Secret& root, const usb_vault::security::KdfV1Salt& salt) override { return usb_vault::security::derive_volume_password_v1(input, root, salt); }
  void format_partition(const usb_vault::volume::PartitionCandidate& target, const Secret& password,
      const std::function<bool()>& confirm, const usb_vault::application::CreationCancellation& cancelled) override {
    disk_.verify();
    require(L"\\Device\\Harddisk" + std::to_wstring(target.disk_number) + L"\\Partition" + std::to_wstring(target.partition_number) == disk_.target(), "session attempted foreign format");
    creator_.format_partition(target, password, confirm, {}, cancelled);
  }
  bool convert_partition(const usb_vault::volume::PartitionCandidate& target, bool begin, const Secret& password,
      const std::function<bool()>& confirm, const usb_vault::application::CreationCancellation& revoked,
      const usb_vault::application::CreationCancellation& pause) override {
    disk_.verify();
    require(L"\\Device\\Harddisk"+std::to_wstring(target.disk_number)+L"\\Partition"+std::to_wstring(target.partition_number)==disk_.target(),"foreign conversion target");
    return creator_.convert_partition(target,begin,password,confirm,[&](const auto& p){
      if(creation_feedback && p.stage==usb_vault::volume_creation::CreationStage::formatting)creation_feedback(3,p.percent,0);
    },revoked,pause);
  }
  usb_vault::volume::MountResult mount_partition(const usb_vault::volume::PartitionIdentity& identity, const Secret& password, const usb_vault::volume::MountOptions& options) override {
    const auto location = usb_vault::volume::resolve_partition_location(identity, usb_vault::volume::enumerate_partition_locations());
    require(location && L"\\Device\\Harddisk" + std::to_wstring(location->disk_number) + L"\\Partition" + std::to_wstring(location->partition_number) == disk_.target(), "session attempted foreign mount");
    const auto result = provider_.mount_partition(identity, password, options);
    mounted_ = provider_.mount_identity(options.drive_letter);
    require(mounted_->volume_path == disk_.target(), "session mounted wrong target");
    return result;
  }
  usb_vault::volume::UnmountResult unmount(wchar_t drive) override {
    verify_mount(drive);
    const auto result = usb_vault::volume::unmount_with_retry(provider_, drive, {.notify_shell = false});
    mounted_.reset(); return result;
  }
  usb_vault::volume::UnmountEscalationResult unmount_forced(wchar_t drive) override {
    verify_mount(drive);
    const auto result = usb_vault::volume::unmount_with_forced_escalation(provider_, drive, {.notify_shell = false});
    mounted_.reset(); return result;
  }
  void verify_mount(wchar_t drive) {
    disk_.verify();
    require(mounted_ && mounted_->volume_path == disk_.target() && provider_.mount_identity(drive) == *mounted_, "session cleanup target changed");
  }
  void create_standard(std::wstring_view, std::uint64_t, const Secret&) override { throw std::logic_error("file operation outside fixture"); }
  void create_with_hidden(std::wstring_view, std::uint64_t, std::uint64_t, const Secret&, const Secret&) override { throw std::logic_error("file operation outside fixture"); }
  void publish_created(std::wstring_view, std::wstring_view) override { throw std::logic_error("file operation outside fixture"); }
  void remove_incomplete(std::wstring_view) override { throw std::logic_error("file operation outside fixture"); }
  usb_vault::volume::MountResult mount_created(std::wstring_view, const Secret&, const usb_vault::volume::MountOptions&) override { throw std::logic_error("file operation outside fixture"); }
  void notify_mounted(wchar_t drive) override { verify_mount(drive); }
  wchar_t allocate_drive_letter() override { return free_letter(); }
  std::vector<std::byte> read_container_index() override { return usb_vault::application::read_container_index_v1_file(index_.wstring()); }
  void write_container_index(std::span<const std::byte> data) override { usb_vault::application::write_container_index_v1_file(index_.wstring(), data); }
  bool remember_container_paths() override { return true; }
  void set_remember_container_paths(bool) override { throw std::logic_error("fixture preference fixed"); }
  bool mount_imported_containers() override { return false; }
  void set_mount_imported_containers(bool) override { throw std::logic_error("fixture preference fixed"); }
  usb_vault::volume::VolumeDirectoryListing list_directory(std::wstring_view path) override { return usb_vault::volume::list_volume_directory(path); }
  void create_directory(std::wstring_view path) override { usb_vault::volume::create_volume_directory(path); }
  void delete_entry(std::wstring_view) override { throw std::logic_error("unused fixture operation"); }
  void move_entry(std::wstring_view, std::wstring_view) override { throw std::logic_error("unused fixture operation"); }
  void move_entry_by_copying(std::wstring_view, std::wstring_view) override { throw std::logic_error("unused fixture operation"); }
  void copy_entry(std::wstring_view, std::wstring_view, const usb_vault::volume::TransferCancellation&) override { throw std::logic_error("unused fixture operation"); }
 private:
  OwnedDisk& disk_;
  std::filesystem::path index_;
  usb_vault::volume_creation::VolumeCreator creator_;
  usb_vault::volume::VolumeProvider provider_;
  std::optional<usb_vault::volume::MountIdentity> mounted_;
};
void session_roundtrip(OwnedDisk& disk) {
  using namespace usb_vault;
  const auto candidate = disk.prepare_ntfs();
  security::SecureBuffer root(32);
  require(::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(root.data()), 32, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0, "session root generation");
  const auto copy_root = [&] { security::SecureBuffer result(root.size()); std::copy(root.bytes().begin(), root.bytes().end(), result.bytes().begin()); return result; };
  application::ContainerLayout layout;
  layout.container_a.salt = application::kContainerASalt;
  layout.container_b.salt = application::kContainerBSalt;
  const std::array<std::byte, 8> expected{std::byte{1}, std::byte{0}, std::byte{7}, std::byte{31}, std::byte{255}, std::byte{9}, std::byte{2}, std::byte{99}};
  for (int login = 0; login < 2; ++login) {
    application::CardAuthenticationSession auth;
    PartitionSessionRuntime runtime(disk);
    application::detail::CardLoginMountSessionCore session(auth, runtime, layout, application::LoginMountPolicy::manual);
    security::SecureBuffer input(16); std::fill(input.bytes().begin(), input.bytes().end(), login ? std::byte{'b'} : std::byte{'a'});
    session.consume_authenticated_login(copy_root(), std::move(input));
    try {
      std::uint32_t id{};
      if (!login) id = session.create_partition(candidate, [&](const auto& confirmed) { disk.verify(); return confirmed == candidate; });
      else {
        const auto restored = session.container_statuses();
        require(restored.size() == 1 && restored[0].partition && !restored[0].mounted, "session index reload failed");
        id = restored[0].id; session.mount_adopted_container(id);
      }
      const auto status = session.container_statuses().front();
      require(status.partition && status.mounted && status.size_bytes > 0, "session partition status incorrect");
      const std::wstring file = std::wstring(1, status.drive_letter) + L":\\session.bin";
      if (!login) {
        session.create_container_directory(id, L"", L"documents");
        Handle output(::CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        DWORD count{};
        require(output.get() != INVALID_HANDLE_VALUE && ::WriteFile(output.get(), expected.data(), static_cast<DWORD>(expected.size()), &count, nullptr) && count == expected.size() && ::FlushFileBuffers(output.get()), "session write failed");
      } else {
        const auto listing = session.list_container_directory(id, L"");
        require(std::ranges::any_of(listing.entries, [](const auto& entry) { return entry.name == L"documents" && entry.is_directory; }) &&
            std::ranges::any_of(listing.entries, [](const auto& entry) { return entry.name == L"session.bin" && !entry.is_directory; }), "session file manager listing lost contents");
        Handle input_file(::CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
        std::array<std::byte, 8> actual{}; DWORD count{};
        require(input_file.get() != INVALID_HANDLE_VALUE && ::ReadFile(input_file.get(), actual.data(), static_cast<DWORD>(actual.size()), &count, nullptr) && count == actual.size() && actual == expected, "session alternate login readback failed");
      }
      if (!login) session.unmount_container(id);
      else { auth.clear_authentication(); static_cast<void>(session.force_unmount_active()); }
      require(!session.mounted(), "session retained mount");
    } catch (...) {
      try { static_cast<void>(session.force_unmount_active()); } catch (...) { std::cerr << "session fixture cleanup failed; retained evidence\n"; }
      throw;
    }
  }
  root.clear();
  require(::DeleteFileW(disk.container_path().replace_extension(L".index").c_str()), "session index cleanup failed");
  std::cout << "PASS: synthetic authenticated session, real KDF/index/helper/driver, file manager, alternate login remount/readback and card-removal cleanup" << std::endl;
}

void inplace_session_roundtrip(OwnedDisk& disk) {
  using namespace usb_vault;
  const auto candidate=disk.prepare_ntfs();
  const std::wstring original=candidate.volume_path+L"original.bin";
  std::vector<std::byte> expected(1024*1024+317);
  for(std::size_t i=0;i<expected.size();++i)expected[i]=std::byte((i+i/509)%251);
  {
    Handle file(::CreateFileW(original.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr)); DWORD done{};
    require(file.get()!=INVALID_HANDLE_VALUE && ::WriteFile(file.get(),expected.data(),static_cast<DWORD>(expected.size()),&done,nullptr) && done==expected.size() && ::FlushFileBuffers(file.get()),"write original session file");
  }
  security::SecureBuffer root(32);
  require(::BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(root.data()),32,BCRYPT_USE_SYSTEM_PREFERRED_RNG)==0,"generate fixture card root");
  application::ContainerLayout layout; layout.container_a.salt=application::kContainerASalt; layout.container_b.salt=application::kContainerBSalt;
  for(int phase=0;phase<2;++phase) {
    application::CardAuthenticationSession auth; PartitionSessionRuntime runtime(disk);
    application::detail::CardLoginMountSessionCore session(auth,runtime,layout,application::LoginMountPolicy::manual);
    security::SecureBuffer fresh(root.size());std::copy(root.bytes().begin(),root.bytes().end(),fresh.bytes().begin());
    security::SecureBuffer input(16);std::fill(input.bytes().begin(),input.bytes().end(),phase ? std::byte{'b'} : std::byte{'a'});
    session.consume_authenticated_login(std::move(fresh),std::move(input));
    bool pause=false;runtime.creation_cancelled=[&]{return pause;};runtime.creation_revoked=[]{return false;};
    runtime.creation_feedback=[&](unsigned stage,unsigned percent,wchar_t){if(!phase && stage==3 && percent>=2)pause=true;};
    try {
      if(!phase) {
        require(session.begin_partition_conversion(candidate,[&](const auto& target){return target==candidate;}),"session normal pause failed");
        const auto row=session.container_statuses().at(0);
        require(row.conversion_state==2 && !row.mounted,"session did not retain paused state");
        bool refused=false;try{session.mount_adopted_container(row.id);}catch(const std::logic_error&){refused=true;}
        require(refused,"session mounted unfinished conversion");
        auth.clear_authentication();
        std::cout << "PASS: real KDF/helper conversion paused, encrypted state persisted and partial mount blocked" << std::endl;
      } else {
        const auto row=session.container_statuses().at(0);
        require(row.conversion_state==2 && !row.mounted,"reopened session lost pause permission");
        require(!session.resume_partition_conversion(row.id),"session resume did not complete");
        const auto ready=session.container_statuses().at(0);
        require(ready.conversion_state==0 && ready.mounted,"session conversion did not become mountable");
        {
          const auto file=std::wstring(1,ready.drive_letter)+L":\\original.bin";
          Handle input_file(::CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
          std::vector<std::byte> actual(expected.size());DWORD done{};
          require(input_file.get()!=INVALID_HANDLE_VALUE && ::ReadFile(input_file.get(),actual.data(),static_cast<DWORD>(actual.size()),&done,nullptr) && done==actual.size() && actual==expected,"authenticated resumed file content mismatch");
        }
        auth.clear_authentication();static_cast<void>(session.force_unmount_active());require(!session.mounted(),"revocation retained completed mount");
      }
    } catch(...) {try{static_cast<void>(session.force_unmount_active());}catch(...){}throw;}
  }
  root.clear();require(::DeleteFileW(disk.container_path().replace_extension(L".index").c_str()),"remove fixture index");
  std::cout << "PASS: reopened authenticated session, persisted pause permission, separate helper resume, original file readback and revocation cleanup" << std::endl;
}

void inplace_roundtrip(OwnedDisk& disk, const usb_vault::security::SecureBuffer& password, bool helper = false) {
  using namespace usb_vault;
  const auto candidate=disk.prepare_ntfs();
  const auto original=candidate.volume_path+L"preserved.bin";
  std::vector<std::byte> expected(1024*1024+37);
  for(std::size_t i=0;i<expected.size();++i) expected[i]=std::byte((i+i/997)%251);
  {
    Handle file(::CreateFileW(original.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr));
    require(file.get()!=INVALID_HANDLE_VALUE,"create preconversion file"); DWORD done{};
    require(::WriteFile(file.get(),expected.data(),static_cast<DWORD>(expected.size()),&done,nullptr) && done==expected.size() && ::FlushFileBuffers(file.get()),"write preconversion file");
  }
  const auto run=[&](bool begin,bool pause,const security::SecureBuffer& secret) {
    disk.verify();
    if (helper) {
      unsigned percent=0;
      volume_creation::VolumeCreator creator(executable_directory()/L"usb_vault_volume_creator.exe");
      try {
        const bool paused=creator.convert_partition(candidate,begin,secret,[]{return true;},
            [&](const volume_creation::CreationProgress& p){if(p.stage==volume_creation::CreationStage::formatting)percent=p.percent;},
            []{return false;},[&]{return pause && percent>=2;});
        return paused ? volume::detail::InplaceResult::paused : volume::detail::InplaceResult::completed;
      } catch (const std::exception& e) { if (&secret==&password) std::cerr << "conversion helper: " << e.what() << std::endl; return volume::detail::InplaceResult::failed; }
    }
    auto path=candidate.volume_path; path.pop_back();
    Handle raw(::CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr));
    require(raw.get()!=INVALID_HANDLE_VALUE,"open conversion fixture");
    unsigned percent=0;
    volume_creation::detail::PartitionInplaceBackend backend(::GetModuleHandleW(L"VeraCryptFormat.dll"),raw.get(),
        candidate.offset_bytes,candidate.length_bytes,secret,begin,
        [&]{return pause && percent>=2 ? volume::detail::InplaceControl::pause : volume::detail::InplaceControl::proceed;},
        [&](const volume_creation::CreationProgress& p){percent=p.percent;},candidate.partition_id);
    return volume::detail::run_partition_inplace(backend);
  };
  require(run(true,true,password)==volume::detail::InplaceResult::paused,"real conversion pause failed");
  std::cout << "PASS: NTFS preparation and real encrypted checkpoint pause" << std::endl;
  security::SecureBuffer wrong(64); std::fill(wrong.bytes().begin(),wrong.bytes().end(),std::byte{0x31});
  require(run(false,false,wrong)==volume::detail::InplaceResult::failed,"wrong resume key accepted");
  require(run(false,false,password)==volume::detail::InplaceResult::completed,"real conversion resume failed");
  volume::VolumeProvider provider; const auto letter=free_letter();
  mount_owned(disk,password,letter);
  try {
    const auto path=std::wstring(1,letter)+L":\\preserved.bin";
    {
      Handle file(::CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
      require(file.get()!=INVALID_HANDLE_VALUE,"preserved NTFS file missing");
      std::vector<std::byte> actual(expected.size()); DWORD done{};
      require(::ReadFile(file.get(),actual.data(),static_cast<DWORD>(actual.size()),&done,nullptr) && done==actual.size() && actual==expected,"preconversion file content mismatch");
    }
    static_cast<void>(volume::unmount_with_retry(provider,letter,{.notify_shell=false}));
  } catch (...) {
    if(provider.mount_identity(letter).volume_path==disk.target()) static_cast<void>(provider.unmount(letter,{.force=true,.notify_shell=false}));
    throw;
  }
  std::cout << "PASS: authenticated resume, real AES conversion, NTFS mount and original file bytes preserved" << std::endl;
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
  std::wstring mode = argc == 2 ? argv[1] : L"";
  const bool gpt = mode.ends_with(L"-gpt");
  if (gpt) mode.resize(mode.size()-4);
  if (argc != 2 || (mode != L"--run-owned-vhd-bitlocker" && mode != L"--run-owned-vhd" && mode != L"--run-owned-vhd-handle" && mode != L"--run-owned-vhd-helper" && mode != L"--run-owned-vhd-session" && mode != L"--run-owned-vhd-inplace-plan" && mode != L"--run-owned-vhd-inplace" && mode != L"--run-owned-vhd-inplace-helper" && mode != L"--run-owned-vhd-inplace-session")) {
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
    if (mode == L"--run-owned-vhd-handle") {
      OwnedDisk cancelled; cancelled.create(gpt); backend.format(cancelled, password, true, true); cancelled.cleanup();
    }
    OwnedDisk disk; disk.create(gpt);
    if(mode==L"--run-owned-vhd-bitlocker"){bitlocker_roundtrip(disk);disk.cleanup();return 0;}
    if (mode == L"--run-owned-vhd-inplace-session") { inplace_session_roundtrip(disk); disk.cleanup(); return 0; }
    if (mode == L"--run-owned-vhd-inplace-helper") { inplace_roundtrip(disk,password,true); disk.cleanup(); return 0; }
    if (mode == L"--run-owned-vhd-inplace") { inplace_roundtrip(disk,password); disk.cleanup(); return 0; }
    if (mode == L"--run-owned-vhd-inplace-plan") { inplace_plan_probe(disk); disk.cleanup(); return 0; }
    if (mode == L"--run-owned-vhd-session") { session_roundtrip(disk); disk.cleanup(); return 0; }
    if (mode == L"--run-owned-vhd-helper") helper_format(disk, password);
    else backend.format(disk, password, mode == L"--run-owned-vhd-handle");
    roundtrip(disk, password); if (mode == L"--run-owned-vhd-handle") backend.file_regression(disk, password); disk.cleanup();
    std::cout << "PASS: owned 512-byte VHD partition format, mount, write, unmount, remount and read; fixture removed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
