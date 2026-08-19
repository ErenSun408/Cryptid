/*
 * frag_test —— 验证尾部连续空闲块扫描器在**有碎片**时确实与"空闲簇总数"分离
 *
 * 背景：里程碑 2 阶段 2b 实测得到
 *         尾部连续空闲簇 == 空闲簇总数 == 172205（高估 0.0%）
 *       两种可能导致该结果：
 *         (a) 刚格式化 + 顺序写入 = 零碎片，两值本就相等（best case）
 *         (b) 扫描器有 bug，压根没在算"连续"
 *       输出一模一样，必须用有碎片的卷把二者分开。
 *
 * 做法：建卷 -> 挂载 -> 顺序写 5 个文件 -> 删掉中间的第 2、4 个（制造空洞）
 *       -> 扫描 -> 两值应显著分离
 *
 * 需要管理员权限（FSCTL_GET_VOLUME_BITMAP，见 N-08）。
 *
 * ⚠️ 一次性验证代码，非产品代码。
 */

#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdint.h>
#include <string>

#include "VeraCryptFormatSDK.h"

static const wchar_t *CONTAINER  = L"D:\\Desktop\\usb-vault\\spike\\frag.hc";
static const char    *CONTAINERA = "D:\\Desktop\\usb-vault\\spike\\frag.hc";
static const char    *PWD        = "FragPass12345678";
static const uint64_t VOL_SIZE   = 100ULL * 1024 * 1024;
static const char     LETTER     = 'Y';
static const int      NFILES     = 5;
static const int      FILE_MB    = 10;

static const char *VCSPIKE = "D:\\Desktop\\usb-vault\\spike\\vcspike.exe";

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

/* 与 spike3b_hidden.cpp 的 ScanTailFreeClusters 同一实现，便于对照 */
static BOOL Scan(char letter, uint64_t *outTailClusters, uint64_t *outFreeClusters,
                 uint32_t *outClusterSize)
{
    wchar_t path[8] = { L'\\', L'\\', L'.', L'\\', (wchar_t)letter, L':', 0 };
    wchar_t root[8] = { (wchar_t)letter, L':', L'\\', 0 };

    DWORD spc = 0, bps = 0, freeClusters = 0, totalClusters = 0;
    if (!GetDiskFreeSpaceW(root, &spc, &bps, &freeClusters, &totalClusters)) return FALSE;
    *outClusterSize  = spc * bps;
    *outFreeClusters = freeClusters;

    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("   [X] 打开卷失败 err=%lu\n", GetLastError());
        return FALSE;
    }

    const DWORD BUFSZ = 1 << 20;
    BYTE *buf = (BYTE *)malloc(BUFSZ);
    if (!buf) { CloseHandle(h); return FALSE; }

    uint64_t tailFree = 0, runs = 0, usedSeen = 0;
    STARTING_LCN_INPUT_BUFFER in;
    in.StartingLcn.QuadPart = 0;

    for (;;) {
        DWORD ret = 0;
        BOOL ok = DeviceIoControl(h, FSCTL_GET_VOLUME_BITMAP, &in, sizeof(in),
                                  buf, BUFSZ, &ret, NULL);
        DWORD err = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && err != ERROR_MORE_DATA) {
            printf("   [X] FSCTL 失败 err=%lu\n", err);
            free(buf); CloseHandle(h); return FALSE;
        }

        VOLUME_BITMAP_BUFFER *vb = (VOLUME_BITMAP_BUFFER *)buf;
        uint64_t bits  = vb->BitmapSize.QuadPart;
        uint64_t avail = (uint64_t)(ret - FIELD_OFFSET(VOLUME_BITMAP_BUFFER, Buffer)) * 8;
        if (bits > avail) bits = avail;

        for (uint64_t i = 0; i < bits; i++) {
            BYTE b = vb->Buffer[i >> 3];
            BOOL used = (b >> (i & 7)) & 1;
            if (used) { if (tailFree > 0) runs++; tailFree = 0; usedSeen++; }
            else tailFree++;
        }

        if (err != ERROR_MORE_DATA) break;
        in.StartingLcn.QuadPart = vb->StartingLcn.QuadPart + bits;
    }

    free(buf);
    CloseHandle(h);
    *outTailClusters = tailFree;
    printf("   （诊断：已用簇 %llu，空闲段数 %llu）\n",
           (unsigned long long)usedSeen, (unsigned long long)runs + 1);
    return TRUE;
}

static void Report(const char *tag, char letter)
{
    uint64_t tail = 0, freeC = 0; uint32_t cs = 0;
    printf("\n[%s]\n", tag);
    if (!Scan(letter, &tail, &freeC, &cs)) { printf("   扫描失败\n"); return; }
    double tailMB  = tail  * (double)cs / 1048576.0;
    double naiveMB = freeC * (double)cs / 1048576.0;
    printf("   尾部连续空闲 = %8llu 簇  (%7.2f MB)\n", (unsigned long long)tail, tailMB);
    printf("   空闲簇总数   = %8llu 簇  (%7.2f MB)\n", (unsigned long long)freeC, naiveMB);
    printf("   => 朴素估算高估 %.2f MB", naiveMB - tailMB);
    if (tailMB > 0.0) printf("（%.1f%%）", (naiveMB - tailMB) / tailMB * 100.0);
    printf("\n");
}

static int Mount(void)
{
    char buf[1024];
    sprintf_s(buf, "set VCSPIKE_PASSWORD=%s&& \"%s\" mount \"%s\" %c >nul 2>&1",
              PWD, VCSPIKE, CONTAINERA, LETTER);
    return system(buf);
}

static void Unmount(void)
{
    char buf[512];
    sprintf_s(buf, "set VCSPIKE_PASSWORD=x&& \"%s\" unmount %c -f >nul 2>&1", VCSPIKE, LETTER);
    system(buf);
}

int main(void)
{
    printf("=== 碎片化测试：验证尾部连续扫描器是否真的在算\"连续\" ===\n\n");

    if (!IsElevated()) {
        printf("[X] 需要管理员权限（FSCTL_GET_VOLUME_BITMAP，见 N-08）\n");
        return 1;
    }

    DeleteFileW(CONTAINER);

    if (VeraCryptFormat_Initialize() != VCF_SUCCESS) { printf("[X] SDK 初始化失败\n"); return 1; }

    VeraCryptFormatOptions o = { 0 };
    o.path = CONTAINER; o.password = PWD; o.size = VOL_SIZE;
    o.encryptionAlgorithm = L"AES"; o.hashAlgorithm = L"SHA-512";
    o.filesystem = L"FAT"; o.quickFormat = TRUE;

    printf("[1] 建普通卷 %llu MB\n", (unsigned long long)(VOL_SIZE / 1048576));
    if (VeraCryptFormat(&o) != VCF_SUCCESS) { printf("[X] 建卷失败\n"); VeraCryptFormat_Shutdown(); return 1; }
    VeraCryptFormat_Shutdown();

    if (Mount() != 0) { printf("[X] 挂载失败\n"); return 1; }
    printf("   [OK] 已挂载 %c:\n", LETTER);

    Report("A. 空卷（基准）", LETTER);

    printf("\n[2] 顺序写入 %d 个 %d MB 文件\n", NFILES, FILE_MB);
    BYTE *blk = (BYTE *)malloc(1 << 20);
    for (int i = 0; i < NFILES; i++) {
        wchar_t f[64];
        swprintf_s(f, L"%C:\\f%d.bin", (wchar_t)LETTER, i);
        HANDLE h = CreateFileW(f, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) { printf("   [X] 建文件 %d 失败\n", i); continue; }
        memset(blk, 0x41 + i, 1 << 20);
        DWORD w;
        for (int k = 0; k < FILE_MB; k++) WriteFile(h, blk, 1 << 20, &w, NULL);
        FlushFileBuffers(h);
        CloseHandle(h);
    }
    free(blk);
    printf("   [OK] 已写入\n");

    Report("B. 顺序写满后（应与阶段 2b 同形态：两值相等）", LETTER);

    printf("\n[3] 删除中间的 f1.bin 与 f3.bin，制造空洞\n");
    for (int i = 1; i < NFILES; i += 2) {
        wchar_t f[64];
        swprintf_s(f, L"%C:\\f%d.bin", (wchar_t)LETTER, i);
        if (!DeleteFileW(f)) printf("   [X] 删除 f%d.bin 失败 %lu\n", i, GetLastError());
        else printf("   已删除 f%d.bin\n", i);
    }

    Report("C. 有碎片后（两值必须显著分离，否则扫描器有 bug）", LETTER);

    Unmount();

    printf("\n=== 判读 ===\n");
    printf("若 C 的两值仍然相等 -> 扫描器没在算连续，里程碑 2 的\"真实值\"不可信\n");
    printf("若 C 的两值明显分离 -> 扫描器正确，阶段 2b 的 0%% 差距是零碎片的真实结果\n");
    return 0;
}
