# P0 Spike 1 & 2 — VeraCrypt 驱动 IOCTL 验证

## 目的

| Spike | 验证什么 | 不通过的后果 |
|---|---|---|
| **1** | 不经 `VeraCrypt.exe`，用户态程序可直接通过 IOCTL 完成挂载/卸载/查询 | 整条技术路线不成立，退回被 F-046 禁止的命令行方案 |
| **2** | 隐藏卷双模式：隐藏卷密码挂真实数据；外层密码挂外层卷并保护隐藏卷 | 胁迫否认性方案不成立，双 PIN 设计要推翻 |

---

## ✅ Spike 1：已通过（2026-08-15）

环境：Windows 11 Home 26200 / VeraCrypt 1.26.29 / 驱动版本 `0x126`

| 验证项 | 结果 |
|---|---|
| IOCTL 通道连通 | ✅ `GET_DRIVER_VERSION` 返回 `0x126` |
| 挂载 | ✅ `MOUNT_VOLUME` 成功，盘符正常出现 |
| 盘符可用性 | ✅ 读写、建子目录、嵌套文件全部正常 |
| 资源管理器可见 | ✅ `Win32_LogicalDisk` 显示 DriveType=3 (本地磁盘) FAT32 |
| 状态查询 | ✅ `GET_MOUNTED_VOLUMES` 正确返回位图与卷路径 |
| 卸载 | ✅ 盘符消失，`Win32_LogicalDisk` 与磁盘管理均无记录 |
| 数据持久性 | ✅ 卸载后重新挂载，文件与目录结构完好 |
| 错误密码拒绝 | ✅ `nReturnCode=3` (ERR_PASSWORD_WRONG) |
| **全程未启动 VeraCrypt.exe** | ✅ |

### 性能（对应需求 P-002：认证到挂载 ≤5 秒）

| 操作 | 耗时 |
|---|---|
| 挂载 | **1.42 ~ 1.47 秒**（三次测量） |
| 卸载 | **73 ~ 106 毫秒** |

余量充足。剩余约 3.5 秒可供提高 PIM 与读卡通信开销使用。

### 已验证的客户需求条目

- **第 2 条**（不加载时资源管理器和磁盘管理都看不到盘符）→ ✅ 卸载后两处均无记录
- **第 4 条**（扫描是随机数）→ ✅ 容器前 1KB 有 252/256 种不同字节值，高熵无特征
- **第 6 条**（盘内文件操作与常规一致）→ ✅ 新建/编辑/读取/子目录全部正常

---

## 五项待观测指标 —— 实测结论

### 1. `volumeType` 字段取值 ✅ 部分确认
普通卷 = **0**（实测确认）。隐藏卷/外层卷/外层卷写保护的取值待 Spike 2 确认。

### 2. 卷路径前缀 ⚠️ 原假设错误，已修正
**不能加 `\??\` 前缀，必须原样传路径。**

依据：官方客户端的 `CreateFullVolumePath()`（`Common/Dlgcode.c:1568`）只是原样复制路径，
仅用开头是否为 `\DEVICE` 判定裸设备还是文件容器：

```c
void CreateFullVolumePath (wchar_t *lpszDiskFile, size_t cbDiskFile,
                           const wchar_t *lpszFileName, BOOL * bDevice)
{
    UpperCaseCopy (lpszDiskFile, cbDiskFile, lpszFileName);
    *bDevice = FALSE;
    if (wmemcmp (lpszDiskFile, L"\\DEVICE", 7) == 0)
        *bDevice = TRUE;
    StringCbCopyW (lpszDiskFile, cbDiskFile, lpszFileName);   // 原样复制
}
```

错误加前缀会导致 `DeviceIoControl` 返回 `ERROR_PATH_NOT_FOUND(3)`。

**路径转换由驱动内部完成** —— `GET_MOUNTED_VOLUMES` 回读时显示的是
`\??\D:\Desktop\usb-vault\spike\test.hc`，即驱动自己补上了前缀。

### 3. `BytesPerSector = 512` ✅ 适用
文件容器场景下固定 512 工作正常，未出现 `ERR_SECTOR_SIZE_INCOMPATIBLE(20)`。
裸分区场景需另行验证（v2.0 分区形态时再测）。

### 4. 挂载耗时 ✅ 见上表

### 5. 资源管理器刷新 ✅
`bMountManager = TRUE` + `broadcast_arrival()` 组合下盘符立即出现，无需手动刷新。

---

## 🔑 重大发现：全程不需要管理员权限

`version` / `mount` / `list` / `unmount` **在普通用户权限下全部成功**。

原因：驱动的 IOCTL 声明为 `FILE_ANY_ACCESS`（`Apidrvr.h:27`）。

**对产品的影响（正面）：**
- 主程序日常运行**无需 UAC 提权**，用户体验大幅改善
- 符合技术约束中「运行时尽量降低权限」的要求
- 之前排期里预留的「设计提权时机」这项工作可以取消

**注意**：本结论仅覆盖文件容器。裸分区挂载涉及物理磁盘访问，很可能仍需管理员权限，
v2.0 做分区形态时必须重新验证。

---

## 编译

```bat
:: MinGW（已验证，产物 32 位；结构体 pack(1)，可与 64 位驱动通信）
gcc -Wall -O2 -D_WIN32_WINNT=0x0601 -o vcspike.exe vcspike.c -luser32

:: MSVC（产物 64 位）
cl /W4 /O2 vcspike.c user32.lib
```

四条编译期断言必须通过，否则拒绝编译：
`VC_PASSWORD=136` / `VC_MOUNT=982` / `VC_UNMOUNT=16` / `VC_MOUNT_LIST=17424`

这是防止把布局错误的结构体发给内核驱动（可能蓝屏或损坏数据）的第一道防线。

## 用法

```bat
vcspike version                        查询驱动版本
vcspike list                           列出已挂载卷
vcspike mount <卷路径> <盘符> [-pim N] [-ro]
vcspike mount-outer <卷路径> <盘符>     仅挂外层卷并保护隐藏卷 (Spike 2)
vcspike unmount <盘符> [-f]
vcspike unmount-all [-f]
```

密码输入优先级：环境变量 `VCSPIKE_PASSWORD` / `VCSPIKE_HIDDEN_PASSWORD`（仅供自动化测试），
否则走控制台隐藏输入。**两者都不经命令行参数**，避免密码出现在进程列表中。

---

## ✅ Spike 2：已通过（2026-08-15）—— 但结论要求修改设计

测试容器：`hidden.hc` 200MB，外层 198MB(FAT) / 隐藏卷 50MB(FAT)

| 验证项 | 结果 |
|---|---|
| 隐藏卷密码挂载 | ✅ `type=1(隐藏卷)`，容量 **49.88 MB** |
| 外层卷密码挂载 | ✅ `type=2(外层卷)`，容量 **199.75 MB** |
| 同容器两套数据 | ✅ 完全隔离 |
| 隐藏卷文件不泄露 | ✅ 外层视角下 `SECRET.txt` / `真实资料/` 均不可见 |
| 写满外层时保护生效 | ✅ 写入 190MB 后隐藏卷 **6/6 文件校验和完全一致** |
| 保护触发标志可读 | ✅ `HiddenVolumeProtectionTriggered = TRUE` |

`volumeType` 取值实测确认：**0=普通卷，1=隐藏卷，2=外层卷**。

### 🔴 关键发现：保护模式不能用于胁迫场景

**原设计（PIN-B → 挂外层卷 + 开启隐藏卷保护）是错误的，必须改。**

VeraCrypt 官方文档 [Protection of Hidden Volumes](https://veracrypt.io/en/Protection%20of%20Hidden%20Volumes.html) 明确警告：

> "When an adversary asks you to mount an outer volume, you of course must **not**
> mount it with the hidden volume protection enabled."
>
> 保护激活期间，对手 "**can** find out that a hidden volume exists within the outer
> volume (he/she will be able to find it out until the volume is unmounted and
> possibly even some time after the computer has been powered off)."

**实测佐证**：向开启保护的外层卷写入 190MB 后，
- 应用层写入全部"成功"（操作系统写缓存），但卸载后 `fill_*.bin` **全部消失**
- 可用空间仍显示 8.0MB/198.2MB —— **空间被占用却无文件引用，文件系统进入不一致状态**

这种"写进去的东西凭空消失、空间还不释放"的行为，任何稍懂技术的人都能看出异常，
等于直接告诉对方"这里有隐藏卷"。

### 修正后的三模式设计

| 模式 | 用途 | `bProtectHiddenVolume` | 说明 |
|---|---|---|---|
| **A. 正常使用** | 访问真实数据 | — | 用隐藏卷密码挂载 |
| **B. 胁迫交出** | 应对胁迫 | **FALSE** | 挂外层卷，**绝不开保护** |
| **C. 维护诱饵** | 用户自己更新诱饵文件 | **TRUE** | 仅在确认无人观察时使用 |

**模式 B 的代价必须让客户知情**：不开保护时，对手若向外层卷写入数据，
**隐藏卷会被静默覆盖，真实数据永久丢失**。这是否认性无法回避的固有代价——
要么保住数据但暴露隐藏卷，要么保住隐藏性但可能丢数据，二者不可兼得。

建议产品设计：
- 模式 B 触发后，在**隐藏卷内部**记录一条"曾进入胁迫模式"的时间戳（下次正常登录时告警）
- 外层卷预留足够大的诱饵数据，降低对手需要写入的可能性
- 文档中明确告知用户此风险

---

## ⏳ Spike 2 原始测试步骤（已执行，留档）

### 准备工作

用 VeraCrypt GUI 向导创建一个**隐藏卷**容器：
- 路径：`D:\Desktop\usb-vault\spike\hidden.hc`
- 外层卷密码：`OuterPass123456`，里面放几个无害诱饵文件
- 隐藏卷密码：`HiddenPass123456`
- 外层建议用 FAT（VeraCrypt 官方建议：NTFS 可能在尾部写数据导致隐藏卷空间不足）

### 测试步骤

```bat
:: 模式 A —— 隐藏卷密码挂载，应看到隐藏卷真实数据
set VCSPIKE_PASSWORD=HiddenPass123456
vcspike mount D:\Desktop\usb-vault\spike\hidden.hc Z
vcspike list                          :: 记录 type 字段取值
vcspike unmount Z

:: 模式 B —— 仅挂外层卷 + 保护隐藏卷（胁迫场景逃生路径）
set VCSPIKE_PASSWORD=OuterPass123456
set VCSPIKE_HIDDEN_PASSWORD=HiddenPass123456
vcspike mount-outer D:\Desktop\usb-vault\spike\hidden.hc Y
vcspike list                          :: type 应变为"外层卷写保护"

:: 关键验证：向 Y 盘持续写入直到触碰隐藏卷区域
::   期望：写入失败或卷转为只读，隐藏卷数据未被破坏
vcspike unmount Y                     :: 应输出"隐藏卷保护曾被触发"

:: 最终确认：再用隐藏卷密码挂载，数据应完好
set VCSPIKE_PASSWORD=HiddenPass123456
vcspike mount D:\Desktop\usb-vault\spike\hidden.hc Z
```

### 通过标准
- 同一容器，两个密码分别挂出两套不同数据
- 模式 B 下写满外层卷不破坏隐藏卷
- 卸载时能读到 `HiddenVolumeProtectionTriggered` 标志

---

## 参考头文件

`ref/` 下是从 VeraCrypt 官方仓库下载的头文件与源码，**仅供比对，不参与编译**。
正式代码中不得 `#include` 这些文件，以保持 clean-room 边界
（`vcspike.c` 中所有结构体均为独立重新声明）。
