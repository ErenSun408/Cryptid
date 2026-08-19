/*
 * Spike 3b 附加验证 —— ChangePwd() 无界面可用性
 *
 * 背景：主文档 N-06 的解法要求「档一升到档二/三时改写容器 A 的卷密码」
 *       （KDF(S, 用户输入) -> KDF(S,"diskA")）。
 *       该能力来自 Common/Password.c:190 的 ChangePwd()，但它带 hwndDlg 参数，
 *       需确认传 NULL 时不会弹出任何界面。
 *
 * 本程序验证：
 *   1. 建一个普通卷，密码 = OLD_PWD                （模拟档一的容器 A）
 *   2. 调 VeraCryptFormat_ChangePassword 改为 NEW_PWD（模拟升档改写）
 *   3. 用 NEW_PWD 挂载 -> 应成功
 *   4. 用 OLD_PWD 挂载 -> 应失败
 *   5. 全程不得有任何窗口弹出
 *
 * ⚠️ 一次性验证代码，非产品代码。
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string>

#include "VeraCryptFormatSDK.h"

static const wchar_t *CONTAINER = L"D:\\Desktop\\usb-vault\\spike\\chpwd.hc";
static const char    *CONTAINER_A = "D:\\Desktop\\usb-vault\\spike\\chpwd.hc";
static const char    *OLD_PWD   = "OldPass12345678";
static const char    *NEW_PWD   = "NewPass87654321";
static const wchar_t *PRF       = L"SHA-512";
static const uint64_t VOL_SIZE      = 20ULL * 1024 * 1024;
static const char     LETTER    = 'Y';

static const char *VCSPIKE = "D:\\Desktop\\usb-vault\\spike\\vcspike.exe";

/* 窗口计数：确认 ChangePwd 期间没有新顶层窗口出现 */
static int g_windowCount = 0;

static BOOL CALLBACK CountVisibleWindows(HWND h, LPARAM)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h)) g_windowCount++;
    return TRUE;
}

static int VisibleWindows(void)
{
    g_windowCount = 0;
    EnumWindows(CountVisibleWindows, 0);
    return g_windowCount;
}

static BOOL CALLBACK OnProgress(int pct, void *)
{
    static int last = -1;
    if (pct / 50 != last / 50) { printf("      format %d%%\n", pct); last = pct; }
    return TRUE;
}

static int TryMount(const char *pwd)
{
    char buf[1024];
    sprintf_s(buf, "set VCSPIKE_PASSWORD=%s&& \"%s\" mount \"%s\" %c >nul 2>&1",
              pwd, VCSPIKE, CONTAINER_A, LETTER);
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
    printf("=== Spike 3b 附加验证: ChangePwd 无界面可用性 ===\n\n");

    DeleteFileW(CONTAINER);

    int rc = VeraCryptFormat_Initialize();
    if (rc != VCF_SUCCESS) { printf("[X] SDK 初始化失败 %d\n", rc); return 1; }
    printf("[OK] SDK 已初始化 (可见窗口 %d)\n\n", VisibleWindows());

    /* ---- 1. 建普通卷 ---- */
    printf("[1] 建普通卷 20MB，密码 = %s\n", OLD_PWD);
    VeraCryptFormatOptions o = { 0 };
    o.path                = CONTAINER;
    o.isDevice            = FALSE;
    o.password            = OLD_PWD;
    o.size                = VOL_SIZE;
    o.encryptionAlgorithm = L"AES";
    o.hashAlgorithm       = PRF;
    o.filesystem          = L"FAT";
    o.quickFormat         = TRUE;
    o.progressCallback    = OnProgress;
    o.hiddenVol           = FALSE;

    rc = VeraCryptFormat(&o);
    if (rc != VCF_SUCCESS) { printf("[X] 建卷失败 %d\n", rc); VeraCryptFormat_Shutdown(); return 1; }
    printf("   [OK] 建卷成功\n\n");

    /* ---- 2. 改密码 ---- */
    printf("[2] 调 VeraCryptFormat_ChangePassword: %s -> %s\n", OLD_PWD, NEW_PWD);
    printf("    改密码前可见窗口 = %d\n", VisibleWindows());

    DWORD t0 = GetTickCount();
    rc = VeraCryptFormat_ChangePassword(CONTAINER,
                                        OLD_PWD, 0, PRF,
                                        NEW_PWD, 0, PRF);
    DWORD dt = GetTickCount() - t0;

    printf("    改密码后可见窗口 = %d\n", VisibleWindows());
    printf("    返回 %d %s  耗时 %lu ms\n", rc,
           rc == VCF_SUCCESS ? "(VCF_SUCCESS)" : "(失败)", dt);

    VeraCryptFormat_Shutdown();

    if (rc != VCF_SUCCESS) { printf("\n[X] ChangePwd 失败\n"); return 1; }
    printf("   [OK] 改密码返回成功\n\n");

    /* ---- 3. 用新密码挂载 ---- */
    printf("[3] 用【新】密码挂载 -> 预期成功\n");
    int mNew = TryMount(NEW_PWD);
    printf("   vcspike 返回 %d -> %s\n", mNew, mNew == 0 ? "[OK] 挂载成功" : "[X] 挂载失败");
    if (mNew == 0) Unmount();

    /* ---- 4. 用旧密码挂载 ---- */
    printf("\n[4] 用【旧】密码挂载 -> 预期失败\n");
    int mOld = TryMount(OLD_PWD);
    printf("   vcspike 返回 %d -> %s\n", mOld, mOld != 0 ? "[OK] 已正确拒绝" : "[X] 旧密码仍可挂载!");
    if (mOld == 0) Unmount();

    printf("\n=== 结论 ===\n");
    BOOL pass = (mNew == 0) && (mOld != 0);
    printf("新密码可挂载 : %s\n", mNew == 0 ? "是" : "否");
    printf("旧密码被拒绝 : %s\n", mOld != 0 ? "是" : "否");
    printf("全程弹窗     : %s\n", VisibleWindows() == 0 ? "无" : "**有**");
    printf("总判定       : %s\n", pass ? "通过" : "**不通过**");

    return pass ? 0 : 1;
}
