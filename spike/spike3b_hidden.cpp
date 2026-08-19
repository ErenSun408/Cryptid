/*
 * Spike 3b 里程碑 2 —— 隐藏卷建卷两阶段编排验证
 *
 * 目的：证明改造后的 FormatDLL 能在无界面条件下建出带隐藏卷的容器。
 *
 * 流程：
 *   阶段 1  建外层卷（普通卷，hiddenVol=FALSE）
 *   阶段 2  挂载外层卷 -> 写入诱饵 -> 扫簇位图求尾部连续空闲块
 *           -> 算隐藏卷最大尺寸 -> 建隐藏卷（hiddenVol=TRUE）
 *
 * 挂载/卸载复用已验证的 vcspike（外部进程），本程序只负责编排与位图扫描。
 *
 * ⚠️ 一次性验证代码，非产品代码。产品实现应按结论重写。
 */

#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdint.h>
#include <string>

#include "VeraCryptFormatSDK.h"

/* 与 Common/Volumes.h 一致，此处独立声明以保持 clean-room 边界 */
#define TC_VOLUME_HEADER_SIZE                             (64 * 1024LL)
#define TC_VOLUME_HEADER_GROUP_SIZE                       (2 * TC_VOLUME_HEADER_SIZE)
#define TC_TOTAL_VOLUME_HEADERS_SIZE                      (4 * TC_VOLUME_HEADER_SIZE)
#define TC_HIDDEN_VOLUME_HOST_FS_RESERVED_END_AREA_SIZE   4096LL

static const wchar_t *CONTAINER   = L"D:\\Desktop\\usb-vault\\spike\\spike3b.hc";
static const char    *OUTER_PWD   = "OuterPass123456";
static const char    *HIDDEN_PWD  = "HiddenPass123456";
static const uint64_t HOST_SIZE   = 200ULL * 1024 * 1024;
static const wchar_t  MOUNT_LETTER = L'Y';

static const char *VCSPIKE = "D:\\Desktop\\usb-vault\\spike\\vcspike.exe";

/* ---------- 小工具 ---------- */

static BOOL CALLBACK OnProgress(int pct, void *user)
{
    const char *tag = (const char *)user;
    static int last = -1;
    if (pct / 20 != last / 20) { printf("      %s %d%%\n", tag, pct); last = pct; }
    return TRUE;
}

static int RunCmd(const std::string &cmd)
{
    return system(cmd.c_str());
}

static std::string MountCmd(const char *pwd, wchar_t letter)
{
    char buf[1024];
    sprintf_s(buf, "set VCSPIKE_PRF=1&& set VCSPIKE_PASSWORD=%s&& \"%s\" mount \"D:\\Desktop\\usb-vault\\spike\\spike3b.hc\" %c >nul",
              pwd, VCSPIKE, (char)letter);
    return buf;
}

static std::string UnmountCmd(wchar_t letter)
{
    char buf[512];
    sprintf_s(buf, "set VCSPIKE_PASSWORD=x&& \"%s\" unmount %c -f >nul", VCSPIKE, (char)letter);
    return buf;
}

/* ---------- 簇位图扫描：求"末端与卷尾对齐的连续空闲块" ---------- */
/* 对应 Tcformat.c 的 ScanVolClusterBitmap()，此处独立实现 */

static BOOL ScanTailFreeClusters(wchar_t letter, uint64_t *outTailFreeBytes, uint32_t *outClusterSize)
{
    wchar_t path[8] = { L'\\', L'\\', L'.', L'\\', letter, L':', 0 };
    wchar_t root[8] = { letter, L':', L'\\', 0 };

    DWORD spc = 0, bps = 0, freeClusters = 0, totalClusters = 0;
    if (!GetDiskFreeSpaceW(root, &spc, &bps, &freeClusters, &totalClusters)) {
        printf("      [X] GetDiskFreeSpace 失败 %lu\n", GetLastError());
        return FALSE;
    }
    *outClusterSize = spc * bps;

    /* 🔴 必须用 GENERIC_READ —— 与官方 ScanVolClusterBitmap() (Tcformat.c:9997) 一致。
       2026-08-19 探针实测（probe_bitmap.c，FAT32/NTFS × 普通用户/管理员 四组）：
         - FSCTL_GET_VOLUME_BITMAP 要求句柄具备 FILE_READ_DATA；
           不具备时一律 err=1，与文件系统、与是否管理员均无关
         - 带读数据权限打开卷设备需要管理员权限，普通用户 err=5
       => 本步骤必然需要提权。见主文档 N-08。

       原先的 FILE_READ_ATTRIBUTES 降级路径已删除：它退回 GetDiskFreeSpace 的
       空闲簇总数，**忽略碎片、会高估尾部连续空间**，可能把隐藏卷建到已占用区域。 */
    HANDLE h = CreateFileW(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        printf("      [X] 打开卷失败 err=%lu%s\n", e,
               e == ERROR_ACCESS_DENIED ? "  —— 需要管理员权限运行本程序" : "");
        return FALSE;
    }

    /* 位图可能很大，分块取回；只关心尾部连续空闲长度，故整块扫完后回溯 */
    const DWORD BUFSZ = 1 << 20;
    BYTE *buf = (BYTE *)malloc(BUFSZ);
    if (!buf) { CloseHandle(h); return FALSE; }

    uint64_t tailFree = 0;      /* 尾部连续空闲簇数 */
    uint64_t scanned  = 0;
    STARTING_LCN_INPUT_BUFFER in;
    in.StartingLcn.QuadPart = 0;

    for (;;) {
        DWORD ret = 0;
        BOOL ok = DeviceIoControl(h, FSCTL_GET_VOLUME_BITMAP, &in, sizeof(in),
                                  buf, BUFSZ, &ret, NULL);
        DWORD err = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && err != ERROR_MORE_DATA) {
            /* 🔴 硬失败，不降级。降级估算会高估尾部连续空间，可能摧毁隐藏卷。 */
            printf("      [X] FSCTL_GET_VOLUME_BITMAP 失败 err=%lu\n", err);
            if (err == ERROR_INVALID_FUNCTION)
                printf("      [X] 句柄缺少 FILE_READ_DATA —— 不应发生（本函数已用 GENERIC_READ）\n");
            free(buf); CloseHandle(h); return FALSE;
        }

        VOLUME_BITMAP_BUFFER *vb = (VOLUME_BITMAP_BUFFER *)buf;
        uint64_t bits = vb->BitmapSize.QuadPart;
        uint64_t avail = (uint64_t)(ret - FIELD_OFFSET(VOLUME_BITMAP_BUFFER, Buffer)) * 8;
        if (bits > avail) bits = avail;

        for (uint64_t i = 0; i < bits; i++) {
            BYTE b = vb->Buffer[i >> 3];
            BOOL used = (b >> (i & 7)) & 1;
            if (used) tailFree = 0; else tailFree++;
        }
        scanned += bits;

        if (err != ERROR_MORE_DATA) break;
        in.StartingLcn.QuadPart = vb->StartingLcn.QuadPart + bits;
    }

    free(buf);
    CloseHandle(h);

    printf("      簇大小 %u B，总簇 %llu，空闲簇 %lu，扫描 %llu 簇\n",
           *outClusterSize, (unsigned long long)totalClusters,
           (unsigned long)freeClusters, (unsigned long long)scanned);

    *outTailFreeBytes = tailFree * (uint64_t)*outClusterSize;

    /* 量化对比：真实的尾部连续空闲块 vs 朴素的"空闲簇总数"。
       后者正是被删掉的降级路径所用的值。差值 = 若按降级路径建卷会
       多出多少字节落在**已被占用**的区域上，即隐藏卷被破坏的量。 */
    double trueMB  = tailFree      * (double)*outClusterSize / 1048576.0;
    double naiveMB = freeClusters  * (double)*outClusterSize / 1048576.0;
    printf("      尾部连续空闲簇 = %llu  (%.2f MB)   <- 真实值，唯一可用\n",
           (unsigned long long)tailFree, trueMB);
    printf("      空闲簇总数     = %lu  (%.2f MB)   <- 朴素估算（已废弃的降级路径）\n",
           (unsigned long)freeClusters, naiveMB);
    printf("      => 高估 %.2f MB", naiveMB - trueMB);
    if (trueMB > 0.0) printf("（%.1f%%）", (naiveMB - trueMB) / trueMB * 100.0);
    printf("%s\n", (naiveMB - trueMB) > 0.01 ? "  [!!] 降级路径会摧毁隐藏卷" : "  （本次恰好接近）");

    return TRUE;
}

/* ---------- 主流程 ---------- */

static int CreateVolume(BOOL hidden, uint64_t size, uint64_t hostSize, const char *pwd, const char *tag)
{
    VeraCryptFormatOptions o = { 0 };
    o.path                = CONTAINER;
    o.isDevice            = FALSE;
    o.password            = pwd;
    o.keyfiles            = NULL;
    o.pim                 = 0;
    o.size                = size;
    o.encryptionAlgorithm = L"AES";
    o.hashAlgorithm       = L"SHA-512";
    o.filesystem          = L"FAT";
    o.clusterSize         = 0;
    o.quickFormat         = TRUE;
    o.dynamicFormat       = FALSE;
    o.fastCreateFile      = FALSE;
    o.progressCallback    = OnProgress;
    o.progressUserData    = (void *)tag;
    o.hiddenVol           = hidden;
    o.hiddenVolHostSize   = hostSize;

    printf("   -> VeraCryptFormat(hiddenVol=%s, size=%.2f MB, hostSize=%.2f MB)\n",
           hidden ? "TRUE" : "FALSE",
           size / 1048576.0, hostSize / 1048576.0);
    int rc = VeraCryptFormat(&o);
    printf("   <- 返回 %d %s\n", rc, rc == VCF_SUCCESS ? "(VCF_SUCCESS)" : "(失败)");
    return rc;
}

/* 提权自检：阶段 2b 的 FSCTL_GET_VOLUME_BITMAP 必然需要管理员权限（见 N-08）。
   与其跑到一半才失败，不如开头就拦住。 */
static BOOL IsElevated(void)
{
    BOOL r = FALSE; HANDLE tok = NULL; TOKEN_ELEVATION el; DWORD cb = 0;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &cb))
            r = el.TokenIsElevated ? TRUE : FALSE;
        CloseHandle(tok);
    }
    return r;
}

int main(void)
{
    printf("=== Spike 3b 里程碑 2：隐藏卷建卷两阶段编排 ===\n\n");

    if (!IsElevated()) {
        printf("[X] 本程序必须以**管理员权限**运行。\n");
        printf("    阶段 2b 的 FSCTL_GET_VOLUME_BITMAP 要求句柄具备 FILE_READ_DATA，\n");
        printf("    而带该权限打开卷设备需要提权。详见主文档 N-08。\n");
        return 1;
    }

    DeleteFileW(CONTAINER);

    int rc = VeraCryptFormat_Initialize();
    if (rc != VCF_SUCCESS) { printf("[X] SDK 初始化失败 %d\n", rc); return 1; }
    printf("[OK] SDK 已初始化\n\n");

    /* ---- 阶段 1：建外层卷 ---- */
    printf("[阶段 1] 建外层卷 200MB / FAT / AES / SHA-512\n");
    if (CreateVolume(FALSE, HOST_SIZE, 0, OUTER_PWD, "outer") != VCF_SUCCESS) {
        VeraCryptFormat_Shutdown(); return 1;
    }
    printf("\n");

    /* ---- 阶段 2a：挂载外层卷并写入诱饵 ---- */
    printf("[阶段 2a] 挂载外层卷并写入诱饵\n");
    if (RunCmd(MountCmd(OUTER_PWD, MOUNT_LETTER)) != 0) {
        printf("   [X] 挂载失败\n"); VeraCryptFormat_Shutdown(); return 1;
    }
    printf("   [OK] 已挂载到 %c:\n", (char)MOUNT_LETTER);

    for (int i = 0; i < 3; i++) {
        wchar_t f[64];
        swprintf_s(f, L"%c:\\decoy_%d.bin", MOUNT_LETTER, i);
        HANDLE h = CreateFileW(f, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            BYTE *blk = (BYTE *)malloc(1 << 20);
            memset(blk, 0x5A + i, 1 << 20);
            DWORD w;
            for (int k = 0; k < 10; k++) WriteFile(h, blk, 1 << 20, &w, NULL);
            free(blk);
            CloseHandle(h);
        }
    }
    printf("   [OK] 已写入 3 x 10MB 诱饵文件\n");

    /* ---- 阶段 2b：扫簇位图 ---- */
    printf("\n[阶段 2b] 扫描簇位图求尾部连续空闲块\n");
    uint64_t tailFree = 0; uint32_t clusterSize = 0;
    if (!ScanTailFreeClusters(MOUNT_LETTER, &tailFree, &clusterSize)) {
        RunCmd(UnmountCmd(MOUNT_LETTER)); VeraCryptFormat_Shutdown(); return 1;
    }

    /* 余量：对齐官方 DetermineMaxHiddenVolSize —— 0.5% 且封顶 10MB */
    uint64_t reserve = HOST_SIZE / 200;
    if (reserve > 10ULL * 1024 * 1024) reserve = 10ULL * 1024 * 1024;

    int64_t maxHidden = (int64_t)tailFree
                      - TC_HIDDEN_VOLUME_HOST_FS_RESERVED_END_AREA_SIZE
                      - (int64_t)reserve;
    if (maxHidden <= 0) { printf("   [X] 尾部空闲不足\n"); RunCmd(UnmountCmd(MOUNT_LETTER)); VeraCryptFormat_Shutdown(); return 1; }
    maxHidden -= maxHidden % clusterSize;

    printf("      自留余量 %.2f MB（0.5%% 封顶 10MB）\n", reserve / 1048576.0);
    printf("      => 隐藏卷最大尺寸 = %.2f MB\n", maxHidden / 1048576.0);

    RunCmd(UnmountCmd(MOUNT_LETTER));
    printf("   [OK] 外层卷已卸载\n\n");

    /* ---- 阶段 2c：建隐藏卷 ---- */
    printf("[阶段 2c] 在同一容器内建隐藏卷\n");
    rc = CreateVolume(TRUE, (uint64_t)maxHidden, HOST_SIZE, HIDDEN_PWD, "hidden");

    VeraCryptFormat_Shutdown();

    if (rc != VCF_SUCCESS) { printf("\n[X] 隐藏卷创建失败\n"); return 1; }
    printf("\n[OK] 两阶段建卷完成 -> %ls\n\n", CONTAINER);

    /* ---- 阶段 3：验证诱饵未被隐藏卷覆盖 ---- */
    printf("[阶段 3] 挂回外层卷，验证诱饵文件是否存活\n");
    if (RunCmd(MountCmd(OUTER_PWD, MOUNT_LETTER)) != 0) {
        printf("   [X] 外层卷挂载失败\n"); return 1;
    }
    int okCount = 0;
    for (int i = 0; i < 3; i++) {
        wchar_t f[64];
        swprintf_s(f, L"%c:\\decoy_%d.bin", MOUNT_LETTER, i);
        HANDLE h = CreateFileW(f, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) { printf("   [X] decoy_%d.bin 打不开\n", i); continue; }
        LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
        /* 校验全文件内容是否仍为写入时的填充字节 */
        BYTE *blk = (BYTE *)malloc(1 << 20);
        DWORD r; BOOL good = TRUE;
        for (int k = 0; k < 10 && good; k++) {
            if (!ReadFile(h, blk, 1 << 20, &r, NULL) || r != (1 << 20)) { good = FALSE; break; }
            for (DWORD n = 0; n < r; n++) if (blk[n] != (BYTE)(0x5A + i)) { good = FALSE; break; }
        }
        free(blk); CloseHandle(h);
        printf("   %s decoy_%d.bin  %lld B  内容校验%s\n", good ? "[OK]" : "[X] ",
               i, sz.QuadPart, good ? "一致" : "**不一致**");
        if (good) okCount++;
    }
    RunCmd(UnmountCmd(MOUNT_LETTER));
    printf("   -> 诱饵存活 %d/3\n", okCount);

    printf("\n下一步：用 vcspike 分别以两个密码挂载验证 type 与容量。\n");
    return okCount == 3 ? 0 : 1;
}
