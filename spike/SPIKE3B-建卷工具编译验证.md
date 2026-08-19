# Spike 3b — 建卷工具编译验证

> **日期**：2026-08-16 起，2026-08-19 完成里程碑 2
> **前置**：`SPIKE3A-建卷工具裁剪可行性.md`（源码分析结论，其第五点二节已确定以 `FormatDLL` 为起点）
> **当前状态**：✅ **里程碑 1、里程碑 2 均已达成** ——
> 改造版 FormatDLL 可在**无任何图形界面**下建出带隐藏卷的容器，
> 并被 `vcspike` 用两个密码分别挂出，`type` 与容量均符合预期。
>
> 🔴 **同时发现一项此前未识别的权限约束，已升级为主文档的 N-08**：
> 计算隐藏卷最大尺寸所需的 `FSCTL_GET_VOLUME_BITMAP` **要求管理员权限**，
> 即**建卷路径需要 UAC 提权**，R-02「无需管理员权限」的适用范围须收窄。

---

## 里程碑 1：基准构建 ✅

**目的**：在动任何代码之前，先证明"官方原版能编过"。
这样之后一旦报错，就一定来自我方改动，而不是环境或上游问题。

### 结果

| 项 | 值 |
|---|---|
| 产物 | `src\x64\Release\VeraCryptFormat.dll`（同时落到 `src\Release\SDK Files\x64\`） |
| 大小 | 1.23 MB |
| 架构 | x64（`machine (x64)`） |
| 导出 | `VeraCryptFormat`、`VeraCryptFormat_Initialize`、`VeraCryptFormat_Shutdown` |
| 错误 | 0 |

导出函数与 `VeraCryptFormatSDK.h` 的声明完全一致，确认这就是官方 Format SDK 那个 DLL。

---

## 🔴 上游构建顺序缺陷（会卡住任何人，务必记住）

**直接编 `FormatDLL` 必定失败**：

```
Format\FormatCom.h(20,10): error C1083: 无法打开包括文件: "FormatCom_h.h"
```

**原因**（非我方改动引入）：

- `FormatCom_h.h` 是 MIDL 从 `Format\FormatCom.idl` 生成的，**不在仓库里**
- 跑 MIDL 的是 **`Format` 工程**（`Format.vcxproj` 里有 `<Midl>` 配置）
- **`FormatDLL.vcxproj` 完全没有 MIDL 配置**（grep `Midl` 命中数为 0），
  它经由 `Tcformat.c` → `FormatCom.h` 间接依赖那个头文件，默认它已存在
- 但 `VeraCrypt.sln` 里 FormatDLL 声明的依赖**只有 Crypto 和 Zip，漏了 Format**

**结论：必须先编 `Format`，再编 `FormatDLL`。** 官方编译指南未提及此点。

---

## 可复现的构建步骤

### 环境（本机实测可用）

| 组件 | 版本 | 位置 |
|---|---|---|
| VS Build Tools | 2022 **17.14.37531.7** | `D:\BuildTools\VS2022` |
| MSVC cl | **19.44.35228**（工具集 14.44.35207） | 同上，Hostx64\x64 |
| Windows SDK | **10.0.22621.0** + 10.0.26100.0 | `C:\Program Files (x86)\Windows Kits`（无法重定向） |
| NASM | **2.16.03** | `D:\BuildTools\bin`（须在 PATH） |
| YASM | **1.3.0** | 同上 |
| 源码 | master 浅克隆 | `D:\Desktop\usb-vault\spike\_vcref` |

**安装的 VS 组件**（按 FormatDLL 实际需要裁剪，非官方指南全集）：
`Workload.VCTools`、`VC.Tools.x86.x64`、**`VC.Runtimes.x86.x64.Spectre`**、
**`VC.ATL`**、**`VC.ATL.Spectre`**、`Windows11SDK.22621`

- Spectre 库**必需**：`FormatDLL.vcxproj` 设了 `<SpectreMitigation>Spectre</SpectreMitigation>`
- ATL **必需**：源文件清单含 `BootEncryption.cpp`
- **已砍掉且确认不需要**：MFC（全仓库无 `afx` 引用）、ARM64、WDK、WiX、VC++ 1.52

### 命令

```bat
:: 汇编器必须在 PATH —— Crypto.vcxproj 以裸命令调用 nasm.exe / yasm.exe
set PATH=%PATH%;D:\BuildTools\bin
call "D:\BuildTools\VS2022\VC\Auxiliary\Build\vcvars64.bat"

:: 第一步：必须先编 Format，它跑 MIDL 生成 FormatCom_h.h
msbuild VeraCrypt.sln /t:Format    /p:Configuration=Release /p:Platform=x64 /m

:: 第二步：再编 FormatDLL
msbuild VeraCrypt.sln /t:FormatDLL /p:Configuration=Release /p:Platform=x64 /m
```

**附带产物**：第一步会同时编出 `Format\x64\Release\VeraCryptFormat.exe` ——
**这就是 N-01 中"唯一能创建隐藏卷的官方 GUI 向导"本体**。
它现在可从源码重建，等于给 3b 兜了底：即便 DLL 改造受阻，源码仍完全可控。

### 里程碑 2 起可用的一键脚本

`spike\build3b.bat` —— 重建 FormatDLL → 拷贝 DLL → 编译 harness。

> 🔴 **两个编码坑（会浪费时间，记下来）**：
> 1. **`.bat` 文件必须纯 ASCII**。cmd.exe 按 ANSI 代码页（本机 936）读批处理，
>    UTF-8 的中文注释会被拆成非法命令，报一堆 `'xxx' 不是内部或外部命令`。
> 2. **`.cpp` 含中文时 `cl` 必须加 `/source-charset:utf-8`**，
>    否则报 `C2065 未声明的标识符` + `C2001 常量中有换行符`。
>    **不要用 `/utf-8`** —— 那会把执行字符集也改成 UTF-8，
>    输出到 GBK 控制台反而全是乱码。

---

## 里程碑 2：透传隐藏卷参数 ✅

### 改动（仅 2 个文件，30 行）

`git diff` 可完整审阅，位于 `spike/_vcref`：

| 文件 | 改动 |
|---|---|
| `FormatDLL/VeraCryptFormatSDK.h` | `VeraCryptFormatOptions` 增加 `hiddenVol`、`hiddenVolHostSize` 两个字段 |
| `FormatDLL/VeraCryptFormatSDK.cpp` | ① 解除 `volParams.hiddenVol = FALSE` 硬编码，改为透传<br>② 隐藏卷模式下跳过"宿主可用空间"检查（容器已存在，`size` 指的是隐藏卷而非文件）<br>③ 补几何合法性校验，对齐 `TCFormatVolume()` 的守卫（`Format.c:127-129`）<br>④ `GetVolumeDataAreaSize()` 与进度条总量改用 `options->hiddenVol` |

**SPIKE3A 的判断得到证实**：核心本就完整支持隐藏卷，
改造性质确实是「重新暴露」而非「重新实现」。**未触碰 `Tcformat.c` 一行。**

### 编排 harness

`spike\spike3b_hidden.cpp` —— 实现 SPIKE3A 第五点二节的两阶段编排：

```
阶段 1   建外层卷（hiddenVol=FALSE）
阶段 2a  挂载外层卷 -> 写入诱饵
阶段 2b  扫簇位图求尾部连续空闲块 -> 算隐藏卷最大尺寸
阶段 2c  建隐藏卷（hiddenVol=TRUE）
阶段 3   挂回外层卷，校验诱饵是否被隐藏卷覆盖
```

挂载/卸载复用已验证的 `vcspike`（外部进程），harness 只负责编排与位图扫描。

### 实测结果（2026-08-19）

**建卷**：

| 阶段 | 结果 |
|---|---|
| 1 外层卷 200MB / FAT / AES / SHA-512 | ✅ `VCF_SUCCESS` |
| 2a 挂载 + 写入 3 × 10MB 诱饵 | ✅ |
| 2b 求隐藏卷最大尺寸 | ✅ **真实簇位图扫描**：簇 1024 B、总簇 202928、尾部连续空闲 172205 簇（168.17 MB）→ 减 0.5% 自留余量 → **167.17 MB** |
| 2c 建隐藏卷 167.17 MB | ✅ `VCF_SUCCESS` |
| 3 诱饵存活校验 | ✅ **3/3 内容校验一致** |
| 全程 GUI 弹窗 | ✅ **零** |

> **2b 的两版数据说明**：首轮（普通用户）该步走了 `GetDiskFreeSpace` 降级估算，
> 属**带瑕疵通过**。N-08 定位并解决后，以管理员权限重跑，
> **2b 改为真实位图扫描，里程碑 2 干净通过**。降级分支已从代码中删除。
>
> 巧合的是两条路径本次给出**完全相同**的 172205 簇 —— 因为外层卷刚格式化、
> 诱饵顺序写入，零碎片。该巧合已由碎片化测试排除"扫描器有 bug"的可能，见下节。

**验收标准 2 / 3 —— `vcspike` 三种方式挂载**（`spike\verify3b.bat`）：

| 挂载方式 | 对应模式 | 实测 type | 实测容量 | 预期 |
|---|---|---|---|---|
| 隐藏卷密码 | A 正常使用 | **`1`(隐藏卷)** | 167.04 MB | ✅ |
| 外层密码，不开保护 | B 胁迫交出 | **`0`(普通卷)** | 199.75 MB | ✅ |
| `mount-outer`，开保护 | C 维护诱饵 | **`2`(外层卷)** | 199.75 MB | ✅ |

**同时二次确认了 Spike 6 对 Spike 2 的订正**：模式 B 报告 `type=0` 而非 `type=2`，
在我方自建的容器上重现，结论稳固。

> **容量小注**：请求 167.17 MB，驱动报告 167.04 MB。
> 差值来自卷头与数据区的换算，属正常，非缺陷。

### 结论

**里程碑 2 的四条验收标准全部通过。**
N-01 的解决方案（以 FormatDLL 为起点改造 + 两阶段编排）**已由端到端实测证实可行**。
P1 的建卷工具不再有技术不确定性，可按此路线排期。

---

## 🔴 N-08：建卷需要管理员权限（本次新发现）

### 现象

阶段 2b 首轮（普通用户权限）中，`FSCTL_GET_VOLUME_BITMAP` 返回
`err=1 (ERROR_INVALID_FUNCTION)`，harness 降级为 `GetDiskFreeSpace` 的空闲簇总数估算。

**该降级路径不安全**：空闲簇总数**忽略碎片**，会高估尾部连续可用空间。
N-05 技术附注第 3 点已论证"外层占用 < 总量 − 隐藏卷"只是必要条件而非充分条件。
**碎片化测试实测其高估幅度可达 40.9%**，见下一节。

### 定位：探针实测（`spike\probe_bitmap.c`）

逐个访问掩码试，两种文件系统 × 两种权限，**共四组**：

| 打开方式 | 普通用户 | **管理员** |
|---|---|---|
| `GENERIC_READ`（官方用法） | CreateFile **err=5** | ✅ **OK**（FAT32 与 NTFS 均成功） |
| `FILE_READ_DATA \| SYNCHRONIZE` | CreateFile **err=5** | ✅ **OK** |
| `FILE_READ_ATTRIBUTES \| SYNCHRONIZE` | FSCTL **err=1** | ❌ **仍然 err=1** |
| `0` | FSCTL **err=1** | ❌ **仍然 err=1** |

管理员组的实测值：`Y:` FAT32 → `BitmapSize=202928`；
`C:` NTFS → `err=234 (MORE_DATA)`、`BitmapSize=52429055`（分块循环路径亦被覆盖）。

**三点结论**：

1. 🔴 **`err=1` 与文件系统无关，也与权限无关** —— 四种组合下，
   凡是不带读数据权限的掩码**一律** `err=1`。
   `spike3b_hidden.cpp` 原注释猜测的"FAT 不支持该 FSCTL"**是错的**，已订正。
   真正含义是：**该 FSCTL 要求句柄具备 `FILE_READ_DATA`。**
2. 🔴 **带读数据权限打开卷设备需要管理员权限**，普通用户 `err=5`。
3. 因果链因此确定且**无绕路余地**：

   ```
   FSCTL 要 FILE_READ_DATA  ->  带 FILE_READ_DATA 开卷要管理员  ->  必须提权
   ```

   两条可用掩码（`GENERIC_READ`、`FILE_READ_DATA`）都要提权。
   官方 `ScanVolClusterBitmap()`（`Format/Tcformat.c:9997`）用的正是 `GENERIC_READ`。

**即：计算隐藏卷最大尺寸这一步，必须提权。**

---

## ✅ 碎片化测试：扫描器正确性验证（`spike\frag_test.cpp`）

### 为什么需要这个测试

阶段 2b 以真实位图扫描重跑后，得到
**尾部连续空闲簇 == 空闲簇总数 == 172205（差 0.0%）**。
该结果有两种可能，**输出完全一样**：

- **(a)** 刚格式化 + 顺序写入 = 零碎片，两值本就相等（best case）
- **(b)** 扫描器有 bug，压根没在算"连续"，只是把空闲总数换个名字输出

**N-08 整条结论链都架在这个扫描器上**，必须排除 (b)。

### 做法与结果

建 100MB 卷 → 顺序写 5 × 10MB → **删掉中间的 f1、f3 制造空洞** → 三个时点各扫一次：

| 阶段 | 空闲段数 | 尾部连续 | 空闲总数 | 高估 |
|---|---|---|---|---|
| A 空卷 | 1 | 98.95 MB | 98.95 MB | 0.0% |
| B 顺序写满 | 1 | 48.95 MB | 48.95 MB | 0.0% |
| **C 删中间两个** | **3** | **48.95 MB** | 68.95 MB | 🔴 **20.00 MB / 40.9%** |

**三处互相印证，结论无歧义**：

1. 空闲段数 **1 → 1 → 3**，与制造的空洞数吻合
2. 差值 **20.00 MB 正好等于删掉的两个 10MB 文件**，不多不少
3. 尾部连续值在 B→C 之间**纹丝不动**（48.95 MB）—— 删的是中间，尾部本就没变

**✅ 扫描器正确。阶段 2b 的 0% 是零碎片的真实结果，里程碑 2 干净通过。**

### 🔴 由此得出的产品结论（比测试本身更重要）

**碎片风险在建卷时很低，在模式 C 时很高。**

- **建卷**：流程恰好是"新格式化的外层卷 + 用户顺序拷入诱饵" ——
  正是上表的 B 形态，零碎片。这解释了阶段 2b 为何是 0%
- **模式 C**：用户过几个月更新一次诱饵，加加删删，**碎片持续累积**，
  逐步向上表的 C 形态演变。而 8.5 的"显示还可添加 X MB"用的是同一套算法

**这把"模式 C 要不要提权"从两难变成了单选**：
不能用保守估算替代 —— **模式 C 恰恰是碎片最严重的场景，
即估算错得最多的时候，恰好是后果最严重的时候**（算多了就写坏隐藏卷）。

➡️ **建议直接定为"模式 C 照常提权"，不再留作待决项。**

### 与 R-02 的关系（须收窄，不是推翻）

R-02「无需管理员权限」的实测对象是 `version / mount / list / unmount` ——
**那部分结论完全成立，不受影响**，日常登录挂载确实不需要 UAC。

但**建卷路径是另一条链路**，R-02 未覆盖。修正后的表述：

| 路径 | 频率 | 管理员权限 |
|---|---|---|
| 挂载 / 卸载 / 查询（日常登录、拔卡卸载） | 每次登录 | ✅ **不需要**（R-02 成立） |
| 建**普通卷**（加密盘容器、档二的隐藏盘容器） | 一次性 | ✅ 不需要（无需扫位图） |
| 建**隐藏卷**（档三） | 一次性 | 🔴 **需要** |
| 模式 C"剩余可写空间"显示 | 低频但**非一次性** | 🔴 **需要**（同一算法） |

### 🔴 连带的否认性问题（重要）

按上表，**档二建卷不提权、档三建卷提权** ——
**UAC 提示本身就区分了两个档位**，且 Windows 会记录提权事件。
这直接违反 **8.4 的本地状态层不可区分要求**。

**缓解（低成本，必须做）**：**建卷流程无条件提权**，与档位无关。
两档表现完全一致，差异消失。代价仅是档一/档二多弹一次 UAC，属可接受。

**模式 C 的提权**则是另一回事：它是运行期功能，每次使用都要弹 UAC。
两个选项，**需决策**：

| 选项 | 评价 |
|---|---|
| 模式 C 照常提权 | 精确，但每次更新诱饵都弹 UAC，且多一条提权记录 |
| 模式 C 用保守估算代替精确值 | 不提权，但**会高估可写空间，可能导致用户写坏隐藏卷** —— 与该功能的目的直接冲突，不可取 |

**倾向选项 1**：模式 C 本就要求"独处时使用"，多一次 UAC 不构成额外暴露；
而算错空间的后果是数据永久损坏。

### ✅ 已完成的代码订正

补验通过后已落实：

- `spike3b_hidden.cpp` 的 `ScanTailFreeClusters()` 改为**只用 `GENERIC_READ`**，
  FSCTL 失败即报错退出，**不安全的降级分支已删除**
- 该 harness 开头加**提权自检**，未提权直接拒绝运行，避免跑到一半才失败
- 阶段 2b 增加"真实值 vs 朴素估算"的量化对比输出
- `probe_bitmap.c` 中 `err=1` 的错误提示已订正为"句柄缺少 `FILE_READ_DATA`"

### 复现命令（管理员 PowerShell）

```powershell
cd D:\Desktop\usb-vault\spike
$env:VCSPIKE_PASSWORD = "OuterPass123456"
.\vcspike.exe mount D:\Desktop\usb-vault\spike\spike3b.hc Y
.\probe_bitmap.exe Y
.\probe_bitmap.exe C
$env:VCSPIKE_PASSWORD = "x"
.\vcspike.exe unmount Y -f
```

> ⚠️ **注意是 PowerShell 语法**。cmd 的 `cd /d`、`set VAR=值`、裸 `xxx.exe`
> 在 PowerShell 里都不工作（分别要用 `cd`、`$env:VAR=`、`.\xxx.exe`）。

---

---

## ✅ 附加验证：`ChangePwd()` 无界面可用（2026-08-19）

主文档 **N-06** 的解法要求「档一升到档二/三时改写容器 A 的卷密码」
（`KDF(S, 用户输入)` → `KDF(S,"diskA")`）。该能力来自 `Common/Password.c:190`，
但它带 `hwndDlg` 参数，需确认传 `NULL` 不会弹界面。

### 实现：给 FormatDLL 再加一个导出

```c
VCF_API int __cdecl VeraCryptFormat_ChangePassword(
    const wchar_t* path,
    const char* oldPassword, int oldPim, const wchar_t* oldHash,
    const char* newPassword, int newPim, const wchar_t* newHash);
```

**保持无界面需要两件事，缺一不可**：

| # | 措施 | 依据 |
|---|---|---|
| 1 | `Silent = TRUE` | `VeraCryptFormat_Initialize()` 已设。`handleError()` 开头即 `if (Silent) return;`（`Dlgcode.c`），所有错误弹窗被吞掉 |
| 2 | 🔴 `SetRandomPoolEnrichedByUserStatus(TRUE)` | **`Silent` 覆盖不到这一条**。`ChangePwd` 会调 `UserEnrichRandomPool()`（`Dlgcode.c:7421`），后者用 `DialogBoxParamW` 弹出"晃鼠标收集熵"对话框，只看 `IsRandomPoolEnrichedByUser()`，**不看 `Silent`** |

**附带一条**：传入非零的 `newPrf` 会短路掉 `CheckPasswordLength()` 分支（又一条 GUI 路径）。
按 Spike 6 我们本来就要显式锁定 PRF，正好一举两得。

### 实测结果（`spike\spike3b_chpwd.cpp`）

| 检查项 | 结果 |
|---|---|
| 建普通卷 20MB，密码 `OldPass12345678` | ✅ |
| `VeraCryptFormat_ChangePassword` 返回 | ✅ `VCF_SUCCESS`，耗时 **2781 ms** |
| 改密码前后本进程可见窗口数 | **0 → 0，全程零弹窗** |
| 用**新**密码挂载 | ✅ 成功 |
| 用**旧**密码挂载 | ✅ 被正确拒绝 |

**结论：N-06 的升档改密码方案可行，无技术障碍。**

> **两点须记入产品实现**：
> 1. **耗时约 2.8 秒**。升档是一次性操作，可接受，但 UI 需要给进度反馈，不能看起来像卡死。
> 2. `SetRandomPoolEnrichedByUserStatus(TRUE)` 意味着**跳过用户鼠标熵采集**，
>    随机池仅由 `Randinit()` 的系统熵源播种。
>    这与官方 FormatDLL 建卷路径的行为一致（它压根不调用该采集），
>    但**是一个有意识的取舍，应在安全设计文档中留档**。

---

## 未完成项

| 项 | 说明 |
|---|---|
| NTFS 外层卷路径 | 本次外层卷用 FAT。`DetermineMaxHiddenVolSize()` 对 NTFS 走另一分支（`Tcformat.c:9899` 起）。**注意该分支同样调用 `ScanVolClusterBitmap()`**，故 N-08 的提权结论对 NTFS 同样成立（探针的 C: 对照组已佐证）。产品若允许 NTFS 外层卷，仅需另测尺寸计算的边界值 |

**Spike 3b 已全部完成，无阻塞项。**

---

## 本目录产物一览

| 文件 | 用途 |
|---|---|
| `build3b.bat` | 重建 FormatDLL + 编译 harness |
| `spike3b_hidden.cpp/.exe` | 两阶段建卷编排 harness |
| `verify3b.bat` | 三种挂载方式的验收脚本 |
| `probe_bitmap.c` | `FSCTL_GET_VOLUME_BITMAP` 访问掩码探针 |
| `probe.bat` | 编译并运行探针（含 NTFS 对照组） |
| `spike3b_chpwd.cpp/.exe` | `ChangePwd()` 无界面可用性验证（N-06） |
| `buildchpwd.bat` | 重建 FormatDLL + 编译 chpwd harness |
| `frag_test.cpp/.exe` | 碎片化测试，验证尾部连续扫描器正确性 |
| `buildfrag.bat` | 编译 frag_test |
| `spike3b.hc` | 产出的 200MB 双卷容器，密码见下 |
| `chpwd.hc` | 改密码验证用的 20MB 普通卷 |

**测试容器密码**：
- `spike3b.hc` 外层 = `OuterPass123456`，隐藏 = `HiddenPass123456`
- `chpwd.hc` 改密码后 = `NewPass87654321`（原 `OldPass12345678` 已失效）
- `frag.hc` = `FragPass12345678`

**对 `_vcref` 的累计改动**：仍只有 `FormatDLL/VeraCryptFormatSDK.{h,cpp}` 两个文件 ——
里程碑 2 的隐藏卷透传 + 本节的 `VeraCryptFormat_ChangePassword` 导出。
`git diff` 可完整审阅，`git checkout` 可回退。**`Tcformat.c`、`Password.c` 均未改动一行。**

> ⚠️ 与 `vcspike.c` 一样，本目录全部代码均为**一次性验证代码，不是产品代码**。
> 产出的是结论，代码用完可以扔。P1 写 `VolumeProvider` 与建卷工具时应照结论重写。
