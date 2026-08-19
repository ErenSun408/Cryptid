/*
 * probe_bitmap —— FSCTL_GET_VOLUME_BITMAP 可用性探针
 *
 * 背景：Spike 3b 里程碑 2 的阶段 2b 中，用 FILE_READ_ATTRIBUTES|SYNCHRONIZE 打开卷后
 *       DeviceIoControl(FSCTL_GET_VOLUME_BITMAP) 返回 err=1 (ERROR_INVALID_FUNCTION)，
 *       harness 降级为 GetDiskFreeSpace 估算。而官方 ScanVolClusterBitmap()
 *       (Format/Tcformat.c:9997) 用的是 GENERIC_READ。
 *
 * 目的：确定失败到底是「访问掩码不足」还是「文件系统不支持该 FSCTL」，
 *       并确定在**普通用户权限**下哪一种掩码可行。
 *
 * 用法：probe_bitmap <盘符>      例: probe_bitmap Y
 *
 * ⚠️ 一次性验证代码，非产品代码。
 */

#include <windows.h>
#include <winioctl.h>
#include <stdio.h>

static void probe(wchar_t letter, DWORD access, const char *name)
{
    wchar_t path[8] = { L'\\', L'\\', L'.', L'\\', letter, L':', 0 };
    HANDLE h;
    DWORD err, ret = 0;
    STARTING_LCN_INPUT_BUFFER in;
    BYTE *buf;
    const DWORD BUFSZ = 1 << 20;

    printf("  %-34s ", name);

    h = CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("CreateFile 失败 err=%lu\n", GetLastError());
        return;
    }

    buf = (BYTE *)malloc(BUFSZ);
    if (!buf) { CloseHandle(h); printf("malloc 失败\n"); return; }

    in.StartingLcn.QuadPart = 0;
    if (DeviceIoControl(h, FSCTL_GET_VOLUME_BITMAP, &in, sizeof(in),
                        buf, BUFSZ, &ret, NULL)) {
        err = ERROR_SUCCESS;
    } else {
        err = GetLastError();
    }

    if (err == ERROR_SUCCESS || err == ERROR_MORE_DATA) {
        VOLUME_BITMAP_BUFFER *vb = (VOLUME_BITMAP_BUFFER *)buf;
        printf("OK  err=%lu(%s)  BitmapSize=%lld  StartingLcn=%lld\n",
               err, err == ERROR_MORE_DATA ? "MORE_DATA" : "SUCCESS",
               vb->BitmapSize.QuadPart, vb->StartingLcn.QuadPart);
    } else {
        /* 2026-08-19 实测订正：err=1 与文件系统无关，也与管理员权限无关 ——
           FAT32 与 NTFS、普通用户与管理员，四种组合下用不带读数据权限的掩码
           打开时**一律** err=1。即该 FSCTL 要求句柄具备 FILE_READ_DATA。 */
        const char *hint =
            err == 1 ? "ERROR_INVALID_FUNCTION —— 句柄缺少 FILE_READ_DATA（与文件系统/权限无关）" :
            err == 5 ? "ERROR_ACCESS_DENIED —— 需要管理员权限才能带读数据权限打开卷" :
                       "";
        printf("失败 err=%lu %s\n", err, hint);
    }

    free(buf);
    CloseHandle(h);
}

int main(int argc, char **argv)
{
    wchar_t letter;
    wchar_t root[8];
    wchar_t fsname[32] = { 0 };
    BOOL admin = FALSE;
    HANDLE tok = NULL;
    TOKEN_ELEVATION el;
    DWORD cb = 0;

    if (argc < 2) { printf("用法: probe_bitmap <盘符>\n"); return 1; }
    letter = (wchar_t)argv[1][0];
    if (letter >= L'a' && letter <= L'z') letter -= 32;

    root[0] = letter; root[1] = L':'; root[2] = L'\\'; root[3] = 0;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &cb))
            admin = el.TokenIsElevated ? TRUE : FALSE;
        CloseHandle(tok);
    }

    if (!GetVolumeInformationW(root, NULL, 0, NULL, NULL, NULL, fsname, 32))
        wcscpy_s(fsname, 32, L"<未知>");

    printf("=== FSCTL_GET_VOLUME_BITMAP 探针 ===\n");
    printf("卷        : %C:\n", (char)letter);
    printf("文件系统  : %ls\n", fsname);
    printf("管理员    : %s\n\n", admin ? "是" : "否");

    probe(letter, GENERIC_READ,                        "GENERIC_READ (官方用法)");
    probe(letter, FILE_READ_ATTRIBUTES | SYNCHRONIZE,  "FILE_READ_ATTRIBUTES|SYNCHRONIZE");
    probe(letter, FILE_READ_DATA | SYNCHRONIZE,        "FILE_READ_DATA|SYNCHRONIZE");
    probe(letter, 0,                                   "0 (仅查询设备属性)");

    return 0;
}
