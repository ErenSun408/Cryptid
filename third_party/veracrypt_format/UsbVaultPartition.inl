// Included only by the separately distributed FormatDLL. No path-based opens.
// Caller authorizes the target and owns the supplied synchronous RW handle.
// This boundary supplies locking and same-handle geometry checks, not card auth.
namespace {
struct UsbVaultPartitionProgress {
    VeraCrypt_Progress_Callback function;
    void* context;
    uint64_t total;
    bool cancelled;
    static BOOL __cdecl update(unsigned __int64 bytes, void* data) {
        UsbVaultPartitionProgress& state = *static_cast<UsbVaultPartitionProgress*>(data);
        if (state.cancelled) return FALSE;
        const int percent = bytes >= state.total ? 100 : static_cast<int>(bytes * 100 / state.total);
        try { if (!state.function(percent, state.context)) state.cancelled = true; }
        catch (...) { state.cancelled = true; }
        return state.cancelled ? FALSE : TRUE;
    }
};
class UsbVaultPartitionHandle {
public:
    HANDLE value;
    bool locked;
    UsbVaultPartitionHandle() : value(INVALID_HANDLE_VALUE), locked(false) {}
    ~UsbVaultPartitionHandle() {
        if (value != INVALID_HANDLE_VALUE) {
            DWORD returned;
            if (locked) DeviceIoControl(value, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &returned, NULL);
            CloseHandle(value);
        }
    }
    UsbVaultPartitionHandle(const UsbVaultPartitionHandle&) = delete;
    UsbVaultPartitionHandle& operator=(const UsbVaultPartitionHandle&) = delete;
};
class UsbVaultPartitionPassword {
public:
    Password* value;
    UsbVaultPartitionPassword() : value(NULL) {
        void* p = VirtualAlloc(NULL, sizeof(Password), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!p) return;
        if (!VirtualLock(p, sizeof(Password))) { VirtualFree(p, 0, MEM_RELEASE); return; }
        value = static_cast<Password*>(p);
    }
    ~UsbVaultPartitionPassword() {
        if (value) { burn(value, sizeof(Password)); VirtualUnlock(value, sizeof(Password)); VirtualFree(value, 0, MEM_RELEASE); }
    }
    UsbVaultPartitionPassword(const UsbVaultPartitionPassword&) = delete;
    UsbVaultPartitionPassword& operator=(const UsbVaultPartitionPassword&) = delete;
};
template<class T> bool UsbVaultPartitionQuery(HANDLE handle, DWORD code, T& output) {
    DWORD returned = 0;
    return DeviceIoControl(handle, code, NULL, 0, &output, sizeof(output), &returned, NULL) && returned >= sizeof(output);
}
bool UsbVaultReadPartitionBoot(HANDLE handle, unsigned char* sector) {
    LARGE_INTEGER beginning = {};
    DWORD read = 0;
    return SetFilePointerEx(handle, beginning, NULL, FILE_BEGIN)
        && ReadFile(handle, sector, 512, &read, NULL) && read == 512;
}
bool UsbVaultPartitionMatches(HANDLE handle, uint64_t offset, uint64_t length) {
    PARTITION_INFORMATION_EX partition = {};
    GET_LENGTH_INFORMATION size = {};
    DISK_GEOMETRY geometry = {};
    STORAGE_DEVICE_NUMBER number = {};
    if (!UsbVaultPartitionQuery(handle, IOCTL_DISK_GET_PARTITION_INFO_EX, partition)
        || !UsbVaultPartitionQuery(handle, IOCTL_DISK_GET_LENGTH_INFO, size)
        || !UsbVaultPartitionQuery(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY, geometry)
        || !UsbVaultPartitionQuery(handle, IOCTL_STORAGE_GET_DEVICE_NUMBER, number)) return false;
    // First experimentally validated layout only. No whole disks or GPT yet.
    return partition.PartitionStyle == PARTITION_STYLE_MBR
        && partition.PartitionNumber > 0 && partition.PartitionNumber == number.PartitionNumber
        && number.DeviceType == FILE_DEVICE_DISK
        && partition.Mbr.PartitionType == PARTITION_IFS && partition.Mbr.RecognizedPartition
        && !partition.Mbr.BootIndicator && geometry.BytesPerSector == 512
        && partition.StartingOffset.QuadPart > 0 && static_cast<uint64_t>(partition.StartingOffset.QuadPart) == offset
        && partition.PartitionLength.QuadPart > 0 && static_cast<uint64_t>(partition.PartitionLength.QuadPart) == length
        && size.Length.QuadPart > 0 && static_cast<uint64_t>(size.Length.QuadPart) == length;
}
}

extern "C" VCF_API int __cdecl VeraCryptFormatPartitionBinary(
    HANDLE target, uint64_t expectedOffset, uint64_t expectedLength,
    const unsigned char* password, uint32_t passwordBytes,
    VeraCrypt_Progress_Callback progress, void* context)
{
    if (!target || target == INVALID_HANDLE_VALUE || !password || passwordBytes != 64
        || expectedOffset == 0 || expectedOffset % 512 || expectedLength % 512
        || expectedLength <= TC_TOTAL_VOLUME_HEADERS_SIZE || expectedLength > INT64_MAX
        || expectedOffset > static_cast<uint64_t>(INT64_MAX) - expectedLength)
        return VCF_ERROR_INVALID_PARAMETER;
    try {
        std::lock_guard<std::mutex> guard(g_sdkMutex);
        if (!g_isInitialized) return VCF_ERROR_NOT_INITIALIZED;
        UsbVaultPartitionHandle disk;
        if (!DuplicateHandle(GetCurrentProcess(), target, GetCurrentProcess(), &disk.value, 0, FALSE, DUPLICATE_SAME_ACCESS))
            return VCF_ERROR_ACCESS_DENIED;
        const uint64_t dataSize = GetVolumeDataAreaSize(FALSE, expectedLength);
        if (dataSize < TC_MIN_FAT_FS_SIZE || dataSize > TC_MAX_FAT_SECTOR_COUNT * uint64_t(512)
            || !UsbVaultPartitionMatches(disk.value, expectedOffset, expectedLength))
            return VCF_ERROR_INVALID_PARAMETER;
        wchar_t filesystem[32] = {};
        if (!GetVolumeInformationByHandleW(disk.value, NULL, 0, NULL, NULL, NULL, filesystem, ARRAYSIZE(filesystem)))
            return VCF_ERROR_OS_ERROR;
        const bool raw = wcscmp(filesystem, L"RAW") == 0;
        if (!raw && wcscmp(filesystem, L"NTFS") != 0) return VCF_ERROR_INVALID_PARAMETER;
        alignas(4096) unsigned char bootBefore[512] = {};
        NTFS_VOLUME_DATA_BUFFER ntfsBefore = {};
        DWORD returned = 0;
        if (!raw) {
            if (!UsbVaultPartitionQuery(disk.value, FSCTL_GET_NTFS_VOLUME_DATA, ntfsBefore)
                || ntfsBefore.BytesPerSector != 512 || ntfsBefore.NumberSectors.QuadPart <= 0
                || static_cast<uint64_t>(ntfsBefore.NumberSectors.QuadPart) > expectedLength / 512
                || !UsbVaultReadPartitionBoot(disk.value, bootBefore)) return VCF_ERROR_OS_ERROR;
            uint64_t bootSerial = 0;
            memcpy(&bootSerial, bootBefore + 72, sizeof(bootSerial));
            if (memcmp(bootBefore + 3, "NTFS    ", 8) != 0
                || bootBefore[11] != 0 || bootBefore[12] != 2
                || bootBefore[510] != 0x55 || bootBefore[511] != 0xaa
                || bootSerial != static_cast<uint64_t>(ntfsBefore.VolumeSerialNumber.QuadPart))
                return VCF_ERROR_INVALID_PARAMETER;
            // Set the handle's extended-I/O flag while NTFS still serves FSCTLs.
            if (!DeviceIoControl(disk.value, FSCTL_ALLOW_EXTENDED_DASD_IO, NULL, 0, NULL, 0, &returned, NULL))
                return VCF_ERROR_OS_ERROR;
        }
        // Never dismount without first proving exclusive filesystem ownership.
        if (!DeviceIoControl(disk.value, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &returned, NULL))
            return VCF_ERROR_ACCESS_DENIED;
        disk.locked = true;
        if (!raw) {
            // Locked NTFS rejects filesystem metadata queries with NOT_READY.
            // Read the unchanged boot sector through the retained DASD handle.
            alignas(4096) unsigned char bootLocked[512] = {};
            if (!UsbVaultReadPartitionBoot(disk.value, bootLocked)
                || memcmp(bootBefore, bootLocked, sizeof(bootBefore)) != 0)
                return VCF_ERROR_OS_ERROR;
        } else {
            wchar_t lockedFilesystem[32] = {};
            if (!GetVolumeInformationByHandleW(disk.value, NULL, 0, NULL, NULL, NULL, lockedFilesystem, ARRAYSIZE(lockedFilesystem))
                || wcscmp(filesystem, lockedFilesystem) != 0) return VCF_ERROR_OS_ERROR;
        }
        // Positively identified RAW has no filesystem boundary to extend.
        if (!DeviceIoControl(disk.value, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &returned, NULL)
            || !UsbVaultPartitionMatches(disk.value, expectedOffset, expectedLength))
            return VCF_ERROR_OS_ERROR;
        UsbVaultPartitionPassword secret;
        if (!secret.value) return VCF_ERROR_OUT_OF_MEMORY;
        memcpy(secret.value->Text, password, passwordBytes); secret.value->Length = passwordBytes;
        wchar_t noPath[1] = {0};
        FORMAT_VOL_PARAMETERS parameters = {};
        parameters.bDevice = TRUE; parameters.volumePath = noPath;
        parameters.usbVaultDeviceHandle = disk.value; parameters.size = expectedLength;
        parameters.ea = MapEncryptionAlgorithm(L"AES"); parameters.pkcs5 = SHA512;
        parameters.fileSystem = FILESYS_FAT; parameters.quickFormat = TRUE;
        parameters.sectorSize = 512; parameters.password = secret.value;
        UsbVaultPartitionProgress callback = {progress, context, dataSize, false};
        if (progress) { parameters.progress_callback = UsbVaultPartitionProgress::update; parameters.progress_callback_user_data = &callback; }
        RandSetHashFunction(SHA512);
        const int result = TCFormatVolume(&parameters);
        if (callback.cancelled || result == ERR_USER_ABORT) return VCF_ERROR_USER_ABORT;
        if (result != ERR_SUCCESS) return VCF_ERROR_OS_ERROR;
        if (!FlushFileBuffers(disk.value)) return VCF_ERROR_OS_ERROR;
        if (!DeviceIoControl(disk.value, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &returned, NULL))
            return VCF_ERROR_OS_ERROR;
        disk.locked = false;
        return VCF_SUCCESS;
    } catch (...) { return VCF_ERROR_OS_ERROR; }
}
