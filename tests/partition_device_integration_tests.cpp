// Opt-in feasibility evidence. Never accepts an existing disk or partition.
#include "usb_vault/security/secure_buffer.h"
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
  const std::wstring& target() const { return target_; }
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
  void format(const OwnedDisk& disk, const usb_vault::security::SecureBuffer& password) {
    disk.verify();
    usb_vault::volume::VolumeProvider provider;
    for (const auto& mounted : provider.mounted_volumes())
      require(mounted.volume_path != disk.target(), "owned native path already mounted");
    fmt::Options options{};
    options.path = disk.target().c_str(); options.is_device = TRUE;
    options.password = reinterpret_cast<const char*>(password.data()); options.size = kPartitionBytes;
    options.encryption_algorithm = L"AES"; options.hash_algorithm = L"SHA-512";
    options.filesystem = L"FAT"; options.quick_format = TRUE;
    const int code = format_(&options, static_cast<std::uint32_t>(password.size()));
    require(code == fmt::kSuccess, "owned partition format failed");
  }
 private: HMODULE module_{}; fmt::Shutdown shutdown_{}; fmt::FormatBinary format_{}; bool initialized_{};
};

// Experiment-only driver request: the product mount_file path policy stays intact.
void mount_owned(const OwnedDisk& disk, const usb_vault::security::SecureBuffer& password, wchar_t drive) {
  disk.verify();
  usb_vault::volume::detail::LockedDriverBuffer<abi::MountRequest> buffer;
  auto& request = buffer.get();
  usb_vault::volume::detail::populate_mount_request(request, L"C:\\unused-fixture-placeholder", password,
      {.drive_letter = drive, .notify_shell = false});
  std::fill(std::begin(request.volume_path), std::end(request.volume_path), L'\0');
  require(disk.target().size() < std::size(request.volume_path), "native target too long");
  std::ranges::copy(disk.target(), request.volume_path);
  request.preserve_timestamp = 0;
  Handle driver(::CreateFileW(L"\\\\.\\VeraCrypt", 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
  require(driver.get() != INVALID_HANDLE_VALUE, "VeraCrypt driver unavailable");
  ioctl(driver.get(), CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS),
      &request, sizeof(request), &request, sizeof(request));
  require(request.return_code == 0, "owned partition mount failed");
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
  if (argc != 2 || std::wstring_view(argv[1]) != L"--run-owned-vhd") {
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
    OwnedDisk disk; disk.create(); backend.format(disk, password); roundtrip(disk, password); disk.cleanup();
    std::cout << "PASS: owned MBR/512 VHD partition format, mount, write, unmount, remount and read; fixture removed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
