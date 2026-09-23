// Separately distributed VeraCrypt adapter. Same-handle MBR/512 only.
#include <memory>
namespace {
const uint64_t UvIpHeader = 131072;
const uint64_t UvIpConceal = 8192;
struct UvIpState {
    UsbVaultPartitionHandle disk;
    UsbVaultPartitionPassword secret;
    PCRYPTO_INFO crypto;
    uint64_t size, start, length;
    UvIpState() : crypto(NULL), size(0), start(0), length(0) {}
    ~UvIpState() { if (crypto) crypto_close(crypto); }
};
std::unique_ptr<UvIpState> uvIp;
bool UvIpIo(HANDLE disk, uint64_t offset, void* bytes, DWORD count, bool write) {
    LARGE_INTEGER pos; pos.QuadPart = offset; DWORD done = 0;
    return SetFilePointerEx(disk, pos, NULL, FILE_BEGIN) &&
        (write ? WriteFile(disk, bytes, count, &done, NULL) : ReadFile(disk, bytes, count, &done, NULL)) && done == count;
}
bool UvIpRange(uint64_t size, uint64_t start, uint64_t length) {
    return size >= 20ULL*1024*1024 && size <= (1ULL<<40) && size % 512 == 0 &&
        start >= UvIpHeader && start <= size-UvIpHeader && start % 512 == 0 && length == size-UvIpHeader-start;
}
bool UvIpSupported(PCRYPTO_INFO c, uint64_t size) {
    return c && c->ea == MapEncryptionAlgorithm(L"AES") && c->mode == FIRST_MODE_OF_OPERATION_ID &&
        c->HeaderVersion == 5 && c->HeaderFlags == TC_HEADER_FLAG_NONSYS_INPLACE_ENC &&
        c->SectorSize == 512 && !c->hiddenVolume && c->VolumeSize.Value == size-2*UvIpHeader &&
        UvIpRange(size, c->EncryptedAreaStart.Value, c->EncryptedAreaLength.Value);
}
bool UvIpFakeHidden(UvIpState& s, uint64_t base) {
    UsbVaultLockedHeaders storage;
    return storage.states && RandgetBytesFull(NULL, storage.states[0].encrypted, 512, FALSE, TRUE) &&
        UvIpIo(s.disk.value, base + 65536, storage.states[0].encrypted, 512, true);
}
// Freshly salt the encrypted header; keys never leave the DLL.
bool UvIpHeaderWrite(UvIpState& s, uint64_t offset, uint64_t start, uint64_t length) {
    UsbVaultLockedHeaders storage;
    if (!storage.states) return false;
    auto& h = storage.states[0];
    if (CreateVolumeHeaderInMemory(NULL, FALSE, h.encrypted, MapEncryptionAlgorithm(L"AES"),
        FIRST_MODE_OF_OPERATION_ID, s.secret.value, SHA512, 0,
        s.crypto ? reinterpret_cast<char*>(s.crypto->master_keydata) : NULL, &h.crypto,
        s.size-2*UvIpHeader, 0, start, length, 0, TC_HEADER_FLAG_NONSYS_INPLACE_ENC, 512, FALSE) != ERR_SUCCESS)
        return false;
    if (!UvIpIo(s.disk.value, offset, h.encrypted, 512, true)) return false;
    if (!s.crypto) { s.crypto = h.crypto; h.crypto = NULL; }
    return true;
}
}
static int UsbVaultInplaceOpen(HANDLE target, uint64_t offset, uint64_t size,
    const unsigned char* password, uint32_t passwordBytes, BOOL begin, uint64_t* range, uint32_t rangeBytes, BOOL (CALLBACK *allowed)(void*), void* context, const unsigned char* gptId) {
    if (range && rangeBytes == 3*sizeof(uint64_t)) memset(range, 0, rangeBytes);
    if (!target || target == INVALID_HANDLE_VALUE || !password || passwordBytes != 64 || !range ||
        rangeBytes != 3*sizeof(uint64_t) || !allowed || (begin != TRUE && begin != FALSE) ||
        !UvIpRange(size, size >= UvIpHeader ? size-UvIpHeader : 0, 0) || !offset || offset%512 || offset > INT64_MAX-size)
        return VCF_ERROR_INVALID_PARAMETER;
    try {
        std::lock_guard<std::mutex> guard(g_sdkMutex);
        if (!g_isInitialized || uvIp) return VCF_ERROR_INVALID_PARAMETER;
        std::unique_ptr<UvIpState> s(new UvIpState);
        if (!s->secret.value) return VCF_ERROR_OUT_OF_MEMORY;
        s->size=size;
        memcpy(s->secret.value->Text,password,64); s->secret.value->Length=64;
        if (!DuplicateHandle(GetCurrentProcess(),target,GetCurrentProcess(),&s->disk.value,0,FALSE,DUPLICATE_SAME_ACCESS) ||
            !UsbVaultPartitionMatches(s->disk.value,offset,size,gptId)) return VCF_ERROR_INVALID_PARAMETER;
        if (!allowed(context)) return VCF_ERROR_USER_ABORT;
        DWORD returned=0;
        unsigned char boot[512] = {};
        if (begin) {
            NTFS_VOLUME_DATA_BUFFER n = {}; DWORD dirty=0;
            if (!UsbVaultPartitionQuery(s->disk.value,FSCTL_IS_VOLUME_DIRTY,dirty) || dirty ||
                !UsbVaultPartitionQuery(s->disk.value,FSCTL_GET_NTFS_VOLUME_DATA,n) || n.BytesPerSector!=512 ||
                n.BytesPerCluster<512 || n.BytesPerCluster>65536 || (n.BytesPerCluster&(n.BytesPerCluster-1)) ||
                n.NumberSectors.QuadPart<=static_cast<int64>(2*UvIpHeader/512) ||
                static_cast<uint64_t>(n.NumberSectors.QuadPart)>size/512 || n.FreeClusters.QuadPart<0 ||
                static_cast<uint64_t>(n.FreeClusters.QuadPart)<2*UvIpHeader/n.BytesPerCluster)
                return VCF_ERROR_INVALID_PARAMETER;
            if (!DeviceIoControl(s->disk.value,FSCTL_ALLOW_EXTENDED_DASD_IO,NULL,0,NULL,0,&returned,NULL)) return VCF_ERROR_OS_ERROR;
            if (!allowed(context)) return VCF_ERROR_USER_ABORT;
            SHRINK_VOLUME_INFORMATION shrink = {};
            shrink.ShrinkRequestType=ShrinkPrepare;
            shrink.NewNumberOfSectors=n.NumberSectors.QuadPart-2*UvIpHeader/512;
            const uint64_t targetSectors=shrink.NewNumberOfSectors;
            if (!DeviceIoControl(s->disk.value,FSCTL_SHRINK_VOLUME,&shrink,sizeof(shrink),NULL,0,&returned,NULL)) return VCF_ERROR_OS_ERROR;
            shrink.ShrinkRequestType=ShrinkCommit; shrink.NewNumberOfSectors=0;
            if (!DeviceIoControl(s->disk.value,FSCTL_SHRINK_VOLUME,&shrink,sizeof(shrink),NULL,0,&returned,NULL)) {
                shrink.ShrinkRequestType=ShrinkAbort;
                DeviceIoControl(s->disk.value,FSCTL_SHRINK_VOLUME,&shrink,sizeof(shrink),NULL,0,&returned,NULL);
                return VCF_ERROR_OS_ERROR;
            }
            if (!UsbVaultPartitionQuery(s->disk.value,FSCTL_GET_NTFS_VOLUME_DATA,n) || n.NumberSectors.QuadPart<=0 ||
                static_cast<uint64_t>(n.NumberSectors.QuadPart)>targetSectors || !UvIpIo(s->disk.value,0,boot,512,false) ||
                memcmp(boot+3,"NTFS    ",8)!=0) return VCF_ERROR_OS_ERROR;
        }
        if (!allowed(context)) return VCF_ERROR_USER_ABORT;
        if (!DeviceIoControl(s->disk.value,FSCTL_LOCK_VOLUME,NULL,0,NULL,0,&returned,NULL)) return VCF_ERROR_ACCESS_DENIED;
        s->disk.locked=true;
        if (begin) {
            unsigned char locked[512] = {};
            if (!UvIpIo(s->disk.value,0,locked,512,false) || memcmp(locked,boot,512)) return VCF_ERROR_OS_ERROR;
        }
        if (!DeviceIoControl(s->disk.value,FSCTL_DISMOUNT_VOLUME,NULL,0,NULL,0,&returned,NULL) ||
            !UsbVaultPartitionMatches(s->disk.value,offset,size,gptId)) return VCF_ERROR_OS_ERROR;
        if (!begin) {
            // A paused operation retains the concealed NTFS boot sector until
            // finalization. Reject BitLocker, ordinary RAW and finalized volumes
            // even if an old in-place backup header happens to remain at the tail.
            unsigned char pausedBoot[512] = {};
            if (!UvIpIo(s->disk.value,0,pausedBoot,512,false)) return VCF_ERROR_OS_ERROR;
            for (unsigned i=0;i<512;++i) pausedBoot[i]^=0xff;
            uint64_t sectors=0; memcpy(&sectors,pausedBoot+40,sizeof(sectors));
            if (memcmp(pausedBoot+3,"NTFS    ",8) || pausedBoot[11]!=0 || pausedBoot[12]!=2 ||
                pausedBoot[510]!=0x55 || pausedBoot[511]!=0xaa || !sectors || sectors>(size-2*UvIpHeader)/512)
                return VCF_ERROR_INVALID_PARAMETER;
        }
        UsbVaultLockedHeaders headers;
        if (!headers.states) return VCF_ERROR_OUT_OF_MEMORY;
        auto& h=headers.states[0];
        if (!allowed(context)) return VCF_ERROR_USER_ABORT;
        if (begin) {
            RandSetHashFunction(SHA512);
            if (!UvIpHeaderWrite(*s,size-UvIpHeader,size-UvIpHeader,0) ||
                WriteRandomDataToReservedHeaderAreas(NULL,s->disk.value,s->crypto,size-2*UvIpHeader,FALSE,TRUE)!=ERR_SUCCESS ||
                !UvIpFakeHidden(*s,size-UvIpHeader) || !FlushFileBuffers(s->disk.value)) return VCF_ERROR_OS_ERROR;
        }
        if (!UvIpIo(s->disk.value,size-UvIpHeader,h.encrypted,512,false)) return VCF_ERROR_OS_ERROR;
        memcpy(&h.password,s->secret.value,sizeof(Password));
        if (ReadVolumeHeader(FALSE,h.encrypted,&h.password,SHA512,0,&h.crypto,NULL)!=ERR_SUCCESS || !UvIpSupported(h.crypto,size))
            return VCF_ERROR_INVALID_PARAMETER;
        if (s->crypto) crypto_close(s->crypto);
        s->crypto=h.crypto; h.crypto=NULL;
        s->start=s->crypto->EncryptedAreaStart.Value; s->length=s->crypto->EncryptedAreaLength.Value;
        if (begin) {
            // Disable the original filesystem before releasing an incomplete volume.
            unsigned char* b=static_cast<unsigned char*>(VirtualAlloc(NULL,UvIpConceal,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
            if (!b) return VCF_ERROR_OUT_OF_MEMORY;
            const bool locked=VirtualLock(b,UvIpConceal)!=FALSE;
            bool ok=locked && UvIpIo(s->disk.value,0,b,static_cast<DWORD>(UvIpConceal),false);
            if (ok) { for (uint64_t i=0;i<UvIpConceal;++i) b[i]^=0xff; ok=UvIpIo(s->disk.value,0,b,static_cast<DWORD>(UvIpConceal),true) && FlushFileBuffers(s->disk.value); }
            burn(b,UvIpConceal); if(locked) VirtualUnlock(b,UvIpConceal); VirtualFree(b,0,MEM_RELEASE);
            if (!ok) return VCF_ERROR_OS_ERROR;
        }
        range[0]=size; range[1]=s->start; range[2]=s->length;
        uvIp=std::move(s); return VCF_SUCCESS;
    } catch (...) { return VCF_ERROR_OS_ERROR; }
}
extern "C" VCF_API int __cdecl VeraCryptInplaceEncrypt(uint64_t unit, unsigned char* bytes, uint32_t count) {
    std::lock_guard<std::mutex> guard(g_sdkMutex);
    if (!uvIp || !bytes || !count || count>65536 || count%512 || unit<256 || unit>uvIp->size/512 ||
        count>uvIp->size-unit*512 || unit*512+count>uvIp->start) return VCF_ERROR_INVALID_PARAMETER;
    const uint64_t source=unit*512-UvIpHeader;
    for(uint64_t i=0; i<count && source+i<UvIpConceal; ++i) bytes[i]^=0xff;
    UINT64_STRUCT number; number.Value=unit;
    EncryptDataUnits(bytes,&number,count/512,uvIp->crypto);
    return VCF_SUCCESS;
}
extern "C" VCF_API int __cdecl VeraCryptInplaceSave(uint64_t start,uint64_t length) {
    std::lock_guard<std::mutex> guard(g_sdkMutex);
    if (!uvIp || !UvIpRange(uvIp->size,start,length) || start>uvIp->start || length<uvIp->length) return VCF_ERROR_INVALID_PARAMETER;
    if (!UvIpHeaderWrite(*uvIp,uvIp->size-UvIpHeader,start,length)) return VCF_ERROR_OS_ERROR;
    uvIp->start=start; uvIp->length=length; return VCF_SUCCESS;
}
extern "C" VCF_API int __cdecl VeraCryptInplaceFinalize() {
    std::lock_guard<std::mutex> guard(g_sdkMutex);
    if (!uvIp || uvIp->start!=UvIpHeader || uvIp->length!=uvIp->size-2*UvIpHeader) return VCF_ERROR_INVALID_PARAMETER;
    if (!UvIpHeaderWrite(*uvIp,0,uvIp->start,uvIp->length) ||
        WriteRandomDataToReservedHeaderAreas(NULL,uvIp->disk.value,uvIp->crypto,uvIp->length,TRUE,FALSE)!=ERR_SUCCESS || !UvIpFakeHidden(*uvIp,0))
        return VCF_ERROR_OS_ERROR;
    return VCF_SUCCESS;
}
extern "C" VCF_API void __cdecl VeraCryptInplaceClose() {
    std::lock_guard<std::mutex> guard(g_sdkMutex); uvIp.reset();
}

extern "C" VCF_API int __cdecl VeraCryptInplaceOpenBinary(HANDLE target, uint64_t offset, uint64_t size,
    const unsigned char* password, uint32_t bytes, BOOL begin, uint64_t* range, uint32_t rangeBytes,
    BOOL (CALLBACK *allowed)(void*), void* context) {
    return UsbVaultInplaceOpen(target,offset,size,password,bytes,begin,range,rangeBytes,allowed,context,NULL);
}
extern "C" VCF_API int __cdecl VeraCryptInplaceOpenGptBinary(HANDLE target, uint64_t offset, uint64_t size,
    const unsigned char* password, uint32_t bytes, BOOL begin, uint64_t* range, uint32_t rangeBytes,
    BOOL (CALLBACK *allowed)(void*), void* context, const unsigned char* partitionId, uint32_t idBytes) {
    if (range && rangeBytes == 3*sizeof(uint64_t)) memset(range, 0, rangeBytes);
    if (!partitionId || idBytes != 16) return VCF_ERROR_INVALID_PARAMETER;
    unsigned char id[16]; memcpy(id,partitionId,sizeof(id));
    return UsbVaultInplaceOpen(target,offset,size,password,bytes,begin,range,rangeBytes,allowed,context,id);
}
