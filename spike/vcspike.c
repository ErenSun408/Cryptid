/*
 * vcspike.c -- VeraCrypt 驱动 IOCTL 验证工具（P0 Spike 1 & 2）
 *
 * 验证目标：
 *   Spike 1: 不经 VeraCrypt.exe，直接向驱动发 IOCTL 完成挂载 / 卸载 / 状态查询
 *   Spike 2: 隐藏卷双模式 —— 隐藏卷密码挂真实数据；外层密码挂外层卷并保护隐藏卷
 *
 * 【许可证说明 —— 重要】
 *   本文件中所有结构体、常量、错误码均为 clean-room 重新声明，
 *   不 #include 任何 VeraCrypt / TrueCrypt 头文件，不含其任何实现代码。
 *   此处描述的仅是驱动对外的二进制接口事实（ABI），用于与已独立安装的
 *   VeraCrypt 驱动通信，属接口调用而非衍生作品。
 *
 * 编译：
 *   MSVC : cl /W4 /O2 vcspike.c
 *   MinGW: gcc -Wall -O2 -o vcspike.exe vcspike.c
 *
 * 运行需管理员权限。
 */

#include <windows.h>
#include <winioctl.h>
#include <dbt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <conio.h>

/* ------------------------------------------------------------------ */
/* 编译期断言：布局对不上就不让编译，绝不把错误的结构体发给内核驱动      */
/* ------------------------------------------------------------------ */
#define STATIC_ASSERT(cond, tag) typedef char static_assert_##tag[(cond) ? 1 : -1]

/* ------------------------------------------------------------------ */
/* 驱动接口常量                                                        */
/* ------------------------------------------------------------------ */
#define VC_DEVICE_PATH      L"\\\\.\\VeraCrypt"

#define VC_IOCTL(code) \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800 + (code), METHOD_BUFFERED, FILE_ANY_ACCESS)

#define VC_IOCTL_GET_DRIVER_VERSION     VC_IOCTL(1)   /* OUT: LONG              */
#define VC_IOCTL_MOUNT_VOLUME           VC_IOCTL(3)   /* IN/OUT: VC_MOUNT       */
#define VC_IOCTL_UNMOUNT_VOLUME         VC_IOCTL(4)   /* IN/OUT: VC_UNMOUNT     */
#define VC_IOCTL_UNMOUNT_ALL_VOLUMES    VC_IOCTL(5)   /* IN/OUT: VC_UNMOUNT     */
#define VC_IOCTL_GET_MOUNTED_VOLUMES    VC_IOCTL(6)   /* IN/OUT: VC_MOUNT_LIST  */

#define VC_MAX_PATH         260
#define VC_MAX_PASSWORD     128
#define VC_VOLUME_ID_SIZE   32      /* = SHA256_DIGESTSIZE */

/* ------------------------------------------------------------------ */
/* 驱动结构体 —— 1 字节对齐（因此 x86 / x64 布局完全一致）              */
/* ------------------------------------------------------------------ */
#pragma pack(push, 1)

typedef struct {
    unsigned int  Length;
    unsigned char Text[VC_MAX_PASSWORD + 1];
    char          Pad[3];
} VC_PASSWORD;

typedef struct {
    int          nReturnCode;
    BOOL         FilesystemDirty;
    BOOL         VolumeMountedReadOnlyAfterAccessDenied;
    BOOL         VolumeMountedReadOnlyAfterDeviceWriteProtected;

    wchar_t      wszVolume[VC_MAX_PATH];
    VC_PASSWORD  VolumePassword;
    BOOL         bCache;
    int          nDosDriveNo;
    unsigned int BytesPerSector;
    BOOL         bMountReadOnly;
    BOOL         bMountRemovable;
    BOOL         bExclusiveAccess;
    BOOL         bMountManager;
    BOOL         bPreserveTimestamp;
    BOOL         bPartitionInInactiveSysEncScope;
    int          nPartitionInInactiveSysEncScopeDriveNo;
    BOOL         SystemFavorite;

    /* 隐藏卷保护 —— Spike 2 的核心 */
    BOOL         bProtectHiddenVolume;
    VC_PASSWORD  ProtectedHidVolPassword;

    BOOL         UseBackupHeader;
    BOOL         RecoveryMode;
    int          pkcs5_prf;                 /* 0 = 自动探测全部 PRF */
    int          ProtectedHidVolPkcs5Prf;
    BOOL         VolumeMountedReadOnlyAfterPartialSysEnc;
    unsigned int BytesPerPhysicalSector;
    int          VolumePim;                 /* 0 = 默认迭代次数 */
    int          ProtectedHidVolPim;
    wchar_t      wszLabel[33];
    BOOL         bIsNTFS;                   /* 仅输出 */
    BOOL         bDriverSetLabel;
    BOOL         bCachePim;
    ULONG        MaximumTransferLength;
    ULONG        MaximumPhysicalPages;
    ULONG        AlignmentMask;
    BOOL         VolumeMasterKeyVulnerable;
} VC_MOUNT;

typedef struct {
    int  nDosDriveNo;
    BOOL ignoreOpenFiles;
    BOOL HiddenVolumeProtectionTriggered;
    int  nReturnCode;
} VC_UNMOUNT;

typedef struct {
    unsigned int     ulMountedDrives;                       /* 已挂载盘符位图 */
    wchar_t          wszVolume[26][VC_MAX_PATH];
    wchar_t          wszLabel[26][33];
    wchar_t          volumeID[26][VC_VOLUME_ID_SIZE];
    unsigned __int64 diskLength[26];
    int              ea[26];
    int              volumeType[26];
    BOOL             reserved[26];
} VC_MOUNT_LIST;

#pragma pack(pop)

/* 关键防线：这三个尺寸必须与驱动预期完全一致 */
STATIC_ASSERT(sizeof(VC_PASSWORD)   == 136,   password_size);
STATIC_ASSERT(sizeof(VC_MOUNT)      == 982,   mount_size);
STATIC_ASSERT(sizeof(VC_UNMOUNT)    == 16,    unmount_size);
STATIC_ASSERT(sizeof(VC_MOUNT_LIST) == 17424, mount_list_size);
STATIC_ASSERT(sizeof(wchar_t)       == 2,     wchar_size);

/* ------------------------------------------------------------------ */
/* 错误码 -> 人话                                                      */
/* ------------------------------------------------------------------ */
static const char *vc_err(int code)
{
    switch (code) {
    case 0:  return "成功";
    case 1:  return "操作系统错误（看 GetLastError）";
    case 2:  return "内存不足";
    case 3:  return "密码错误、PIM 错误、或该卷不是 VeraCrypt 卷";
    case 4:  return "卷格式损坏";
    case 5:  return "找不到驱动器";
    case 6:  return "卷上仍有文件被打开";
    case 7:  return "卷大小不正确";
    case 11: return "卷寻址失败";
    case 12: return "卷写入失败";
    case 13: return "文件被锁定，无法卸载";
    case 14: return "卷读取失败";
    case 15: return "驱动版本不匹配";
    case 16: return "需要更新版本的 VeraCrypt";
    case 17: return "加密算法初始化失败";
    case 20: return "扇区大小不兼容";
    case 21: return "该卷已被挂载";
    case 22: return "没有空闲盘符";
    case 23: return "打开宿主文件/设备失败（路径错？被占用？）";
    case 24: return "挂载失败";
    case 26: return "拒绝访问（是否以管理员运行？）";
    case 30: return "参数不正确";
    default: return "未知错误码";
    }
}

/* volumeType 的取值含义为推测，需在实测中核实（见 README） */
static const char *vc_voltype(int t)
{
    switch (t) {
    case 0:  return "普通卷";
    case 1:  return "隐藏卷";
    case 2:  return "外层卷";
    case 3:  return "外层卷(隐藏卷写保护已生效)";
    case 4:  return "系统卷";
    default: return "?";
    }
}

/* ------------------------------------------------------------------ */
/* 工具函数                                                            */
/* ------------------------------------------------------------------ */
static void secure_wipe(void *p, size_t n)
{
    volatile unsigned char *q = (volatile unsigned char *)p;
    while (n--) *q++ = 0;
}

static BOOL is_elevated(void)
{
    HANDLE tok = NULL;
    TOKEN_ELEVATION el;
    DWORD sz = sizeof(el);
    BOOL ok = FALSE;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &sz))
            ok = el.TokenIsElevated ? TRUE : FALSE;
        CloseHandle(tok);
    }
    return ok;
}

static HANDLE vc_open(void)
{
    HANDLE h = CreateFileW(VC_DEVICE_PATH, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        fprintf(stderr, "[X] 打开 %ls 失败, GetLastError=%lu\n", VC_DEVICE_PATH, e);
        if (e == ERROR_FILE_NOT_FOUND)
            fprintf(stderr, "    -> VeraCrypt 驱动未加载。请先安装并启动 VeraCrypt。\n");
        else if (e == ERROR_ACCESS_DENIED)
            fprintf(stderr, "    -> 权限不足。请用管理员身份运行本程序。\n");
    }
    return h;
}

/*
 * 卷路径原样传给驱动，不加任何前缀。
 *
 * 已实测确认（2026-08-15）：VeraCrypt 官方客户端的 CreateFullVolumePath()
 * （Common/Dlgcode.c:1568）就是原样复制路径，只用开头是否为 "\DEVICE"
 * 来判定是裸设备还是文件容器，路径转换由驱动内部完成。
 * 早期版本这里错误地加了 "\??\" 前缀，导致 IOCTL 返回 ERROR_PATH_NOT_FOUND(3)。
 *
 *   文件容器: "D:\foo\bar.hc"
 *   裸设备  : "\Device\Harddisk0\Partition2"
 */
static void to_driver_path(const wchar_t *in, wchar_t *out, size_t cap)
{
    wcsncpy(out, in, cap - 1);
    out[cap - 1] = L'\0';
}

/*
 * 读取密码。优先级：
 *   1) 环境变量（仅供自动化测试；环境变量不像命令行参数那样出现在进程列表里）
 *   2) 控制台隐藏输入
 * 注意：正式产品中密码来自 USB 卡，这两条通道都不会保留。
 */
static void read_password(const char *prompt, const char *envvar, char *buf, size_t cap)
{
    size_t i = 0;
    int c;

    if (envvar) {
        const char *e = getenv(envvar);
        if (e && *e) {
            strncpy(buf, e, cap - 1);
            buf[cap - 1] = '\0';
            printf("%s[取自环境变量 %s]\n", prompt, envvar);
            return;
        }
    }

    printf("%s", prompt);
    fflush(stdout);

    while ((c = _getch()) != '\r' && c != '\n') {
        if (c == '\b') {
            if (i > 0) { i--; printf("\b \b"); }
        } else if (c == 3) {          /* Ctrl+C */
            printf("\n已取消\n");
            exit(1);
        } else if (i + 1 < cap && c >= 32 && c < 127) {
            buf[i++] = (char)c;
            printf("*");
        }
    }
    buf[i] = '\0';
    printf("\n");
}

static void set_password(VC_PASSWORD *dst, const char *src)
{
    size_t n = strlen(src);
    if (n > VC_MAX_PASSWORD) n = VC_MAX_PASSWORD;
    secure_wipe(dst, sizeof(*dst));
    memcpy(dst->Text, src, n);
    dst->Length = (unsigned int)n;
}

/* 挂载后广播设备变更，促使资源管理器立即刷新出盘符 */
static void broadcast_arrival(int driveNo)
{
    DEV_BROADCAST_VOLUME dbv;
    DWORD recips = BSM_APPLICATIONS;

    memset(&dbv, 0, sizeof(dbv));
    dbv.dbcv_size       = sizeof(dbv);
    dbv.dbcv_devicetype = DBT_DEVTYP_VOLUME;
    dbv.dbcv_unitmask   = 1UL << driveNo;
    dbv.dbcv_flags      = 0;

    BroadcastSystemMessageW(BSF_IGNORECURRENTTASK | BSF_NOHANG | BSF_POSTMESSAGE,
                            &recips, WM_DEVICECHANGE, DBT_DEVICEARRIVAL,
                            (LPARAM)&dbv);
}

static int drive_letter_to_no(const char *s)
{
    char c = s[0];
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c < 'A' || c > 'Z') return -1;
    return c - 'A';
}

/* ------------------------------------------------------------------ */
/* 命令：driver 版本                                                   */
/* ------------------------------------------------------------------ */
static int cmd_version(void)
{
    HANDLE h = vc_open();
    LONG ver = 0;
    DWORD ret = 0;

    if (h == INVALID_HANDLE_VALUE) return 1;

    if (!DeviceIoControl(h, VC_IOCTL_GET_DRIVER_VERSION,
                         NULL, 0, &ver, sizeof(ver), &ret, NULL)) {
        fprintf(stderr, "[X] IOCTL 失败, GetLastError=%lu\n", GetLastError());
        CloseHandle(h);
        return 1;
    }

    printf("[OK] 驱动已连通，版本号 = 0x%lX (%ld)\n", ver, ver);
    printf("     -> Spike 1 第一步通过：用户态可直接与 VeraCrypt 驱动通信。\n");
    CloseHandle(h);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 命令：列出已挂载卷                                                  */
/* ------------------------------------------------------------------ */
static int cmd_list(void)
{
    HANDLE h = vc_open();
    VC_MOUNT_LIST *ml;
    DWORD ret = 0;
    int i, n = 0;

    if (h == INVALID_HANDLE_VALUE) return 1;

    ml = (VC_MOUNT_LIST *)calloc(1, sizeof(VC_MOUNT_LIST));
    if (!ml) { CloseHandle(h); return 1; }

    if (!DeviceIoControl(h, VC_IOCTL_GET_MOUNTED_VOLUMES,
                         ml, sizeof(*ml), ml, sizeof(*ml), &ret, NULL)) {
        fprintf(stderr, "[X] IOCTL 失败, GetLastError=%lu\n", GetLastError());
        free(ml); CloseHandle(h);
        return 1;
    }

    printf("已挂载卷位图 = 0x%08X\n", ml->ulMountedDrives);
    for (i = 0; i < 26; i++) {
        if (ml->ulMountedDrives & (1U << i)) {
            n++;
            printf("  %c:  type=%d(%s)  size=%.2f MB\n",
                   'A' + i, ml->volumeType[i], vc_voltype(ml->volumeType[i]),
                   (double)ml->diskLength[i] / (1024.0 * 1024.0));
            printf("      %ls\n", ml->wszVolume[i]);
        }
    }
    if (n == 0) printf("  (当前没有挂载任何卷)\n");

    free(ml);
    CloseHandle(h);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 命令：挂载                                                          */
/*   protectHidden = TRUE 时进入 Spike 2 的"仅挂外层 + 保护隐藏卷"模式 */
/* ------------------------------------------------------------------ */
static int cmd_mount(const wchar_t *volume, int driveNo, int pim,
                     BOOL readOnly, BOOL protectHidden)
{
    HANDLE h;
    VC_MOUNT *m;
    DWORD ret = 0;
    char pwd[VC_MAX_PASSWORD + 1];
    char hidPwd[VC_MAX_PASSWORD + 1];
    int rc;

    m = (VC_MOUNT *)calloc(1, sizeof(VC_MOUNT));
    if (!m) return 1;

    read_password("请输入卷密码: ", "VCSPIKE_PASSWORD", pwd, sizeof(pwd));
    set_password(&m->VolumePassword, pwd);
    secure_wipe(pwd, sizeof(pwd));

    if (protectHidden) {
        read_password("请输入【被保护的隐藏卷】密码: ",
                      "VCSPIKE_HIDDEN_PASSWORD", hidPwd, sizeof(hidPwd));
        m->bProtectHiddenVolume = TRUE;
        set_password(&m->ProtectedHidVolPassword, hidPwd);
        m->ProtectedHidVolPkcs5Prf = 0;   /* 自动探测 */
        m->ProtectedHidVolPim      = 0;
        secure_wipe(hidPwd, sizeof(hidPwd));
    }

    to_driver_path(volume, m->wszVolume, VC_MAX_PATH);

    m->nDosDriveNo           = driveNo;
    m->VolumePim             = pim;
    m->pkcs5_prf             = 0;      /* 自动探测所有 PRF */
    m->bCache                = FALSE;  /* 不在驱动里缓存密码 */
    m->bCachePim             = FALSE;
    m->bMountReadOnly        = readOnly;
    m->bMountRemovable       = FALSE;
    m->bExclusiveAccess      = FALSE;
    m->bMountManager         = TRUE;   /* 通告挂载管理器 -> 出盘符 */
    m->bPreserveTimestamp    = TRUE;
    m->BytesPerSector        = 512;
    m->BytesPerPhysicalSector= 512;
    m->UseBackupHeader       = FALSE;
    m->RecoveryMode          = FALSE;

    h = vc_open();
    if (h == INVALID_HANDLE_VALUE) { free(m); return 1; }

    printf("正在挂载 %ls -> %c: ...\n", m->wszVolume, 'A' + driveNo);

    if (!DeviceIoControl(h, VC_IOCTL_MOUNT_VOLUME,
                         m, sizeof(*m), m, sizeof(*m), &ret, NULL)) {
        fprintf(stderr, "[X] IOCTL 调用失败, GetLastError=%lu\n", GetLastError());
        secure_wipe(m, sizeof(*m)); free(m); CloseHandle(h);
        return 1;
    }

    rc = m->nReturnCode;
    if (rc != 0) {
        fprintf(stderr, "[X] 挂载失败: nReturnCode=%d (%s)\n", rc, vc_err(rc));
        secure_wipe(m, sizeof(*m)); free(m); CloseHandle(h);
        return 1;
    }

    printf("[OK] 挂载成功 -> %c:\n", 'A' + driveNo);
    printf("     文件系统 NTFS      : %s\n", m->bIsNTFS ? "是" : "否");
    printf("     只读(拒绝访问导致) : %s\n", m->VolumeMountedReadOnlyAfterAccessDenied ? "是" : "否");
    printf("     只读(设备写保护)   : %s\n", m->VolumeMountedReadOnlyAfterDeviceWriteProtected ? "是" : "否");
    if (protectHidden)
        printf("     [Spike 2] 已按'仅挂外层 + 保护隐藏卷'模式挂载。\n"
               "               请向该盘写入大量数据，隐藏卷区域被触碰时应转为只读。\n");

    broadcast_arrival(driveNo);

    secure_wipe(m, sizeof(*m));
    free(m);
    CloseHandle(h);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 命令：卸载                                                          */
/* ------------------------------------------------------------------ */
static int cmd_unmount(int driveNo, BOOL all, BOOL force)
{
    HANDLE h = vc_open();
    VC_UNMOUNT u;
    DWORD ret = 0;

    if (h == INVALID_HANDLE_VALUE) return 1;

    memset(&u, 0, sizeof(u));
    u.nDosDriveNo     = all ? -1 : driveNo;
    u.ignoreOpenFiles = force;

    if (!DeviceIoControl(h, all ? VC_IOCTL_UNMOUNT_ALL_VOLUMES : VC_IOCTL_UNMOUNT_VOLUME,
                         &u, sizeof(u), &u, sizeof(u), &ret, NULL)) {
        fprintf(stderr, "[X] IOCTL 失败, GetLastError=%lu\n", GetLastError());
        CloseHandle(h);
        return 1;
    }

    if (u.nReturnCode != 0) {
        fprintf(stderr, "[X] 卸载失败: nReturnCode=%d (%s)\n",
                u.nReturnCode, vc_err(u.nReturnCode));
        CloseHandle(h);
        return 1;
    }

    printf("[OK] 卸载成功%s\n", all ? "（全部）" : "");
    if (u.HiddenVolumeProtectionTriggered)
        printf("     [!] 隐藏卷保护曾被触发 —— 说明有写操作试图覆盖隐藏卷区域。\n"
               "         这正是 Spike 2 期望看到的保护行为。\n");

    CloseHandle(h);
    return 0;
}

/* ------------------------------------------------------------------ */
static void usage(void)
{
    printf(
    "vcspike -- VeraCrypt 驱动 IOCTL 验证工具\n"
    "\n"
    "用法:\n"
    "  vcspike version                          查询驱动版本（连通性测试）\n"
    "  vcspike list                             列出已挂载的卷\n"
    "  vcspike mount <卷路径> <盘符> [选项]      挂载\n"
    "  vcspike mount-outer <卷路径> <盘符>       仅挂外层卷并保护隐藏卷 (Spike 2)\n"
    "  vcspike unmount <盘符> [-f]              卸载指定盘\n"
    "  vcspike unmount-all [-f]                 卸载全部\n"
    "\n"
    "mount 选项:\n"
    "  -pim <n>    指定 PIM（默认 0 = 标准迭代次数）\n"
    "  -ro         以只读方式挂载\n"
    "\n"
    "示例:\n"
    "  vcspike version\n"
    "  vcspike mount C:\\tmp\\vc-spike\\test.hc Z\n"
    "  vcspike mount-outer C:\\tmp\\vc-spike\\test.hc Y\n"
    "  vcspike unmount Z\n"
    "\n"
    "注意: 密码通过隐藏输入交互录入，不走命令行参数（避免出现在进程列表中）。\n"
    "      本程序需以管理员身份运行。\n");
}

int main(int argc, char **argv)
{
    int i;

    if (argc < 2) { usage(); return 1; }

    if (!is_elevated()) {
        fprintf(stderr, "[!] 警告: 当前不是管理员权限，打开驱动设备很可能失败。\n\n");
    }

    if (!strcmp(argv[1], "version"))  return cmd_version();
    if (!strcmp(argv[1], "list"))     return cmd_list();

    if (!strcmp(argv[1], "mount") || !strcmp(argv[1], "mount-outer")) {
        wchar_t wpath[VC_MAX_PATH];
        int driveNo, pim = 0;
        BOOL ro = FALSE;
        BOOL outer = !strcmp(argv[1], "mount-outer");

        if (argc < 4) { usage(); return 1; }

        MultiByteToWideChar(CP_ACP, 0, argv[2], -1, wpath, VC_MAX_PATH);
        driveNo = drive_letter_to_no(argv[3]);
        if (driveNo < 0) { fprintf(stderr, "[X] 盘符无效: %s\n", argv[3]); return 1; }

        for (i = 4; i < argc; i++) {
            if (!strcmp(argv[i], "-pim") && i + 1 < argc) pim = atoi(argv[++i]);
            else if (!strcmp(argv[i], "-ro")) ro = TRUE;
        }
        return cmd_mount(wpath, driveNo, pim, ro, outer);
    }

    if (!strcmp(argv[1], "unmount")) {
        int driveNo;
        BOOL force = FALSE;
        if (argc < 3) { usage(); return 1; }
        driveNo = drive_letter_to_no(argv[2]);
        if (driveNo < 0) { fprintf(stderr, "[X] 盘符无效: %s\n", argv[2]); return 1; }
        for (i = 3; i < argc; i++) if (!strcmp(argv[i], "-f")) force = TRUE;
        return cmd_unmount(driveNo, FALSE, force);
    }

    if (!strcmp(argv[1], "unmount-all")) {
        BOOL force = FALSE;
        for (i = 2; i < argc; i++) if (!strcmp(argv[i], "-f")) force = TRUE;
        return cmd_unmount(-1, TRUE, force);
    }

    usage();
    return 1;
}
