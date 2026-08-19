# Spike 3a — 建卷工具裁剪可行性分析

> **日期**：2026-08-16
> **方法**：**纯源码静态分析，未编译、未运行**。所有结论附源码行号，可复核。
> **数据来源**：VeraCrypt master 分支源码（`src/Common/Format.c`、`src/Format/Tcformat.c`、
> `src/Common/Volumes.c`、`src/Format/Format.vcxproj`）+ 本地 `spike/ref/` 已有头文件
> **对应问题**：N-01（隐藏卷无法通过官方接口程序化创建）

---

## 结论

**裁剪可行，且比 N-01 中的预估简单得多。建议推进 Spike 3b（编译验证）。**

原以为要从一个上万行的图形向导里"抠"出建卷逻辑。实际情况是：
**建卷核心本身就完整支持隐藏卷，向导独有的逻辑只有 4 个函数、约 320 行，且连续排列在文件末尾。**

---

## 一、最重要的发现：核心层一直支持隐藏卷，只是没对外暴露

`FORMAT_VOL_PARAMETERS`（`Common/Format.h:27-52`）**本身就带隐藏卷字段**：

```c
BOOL  hiddenVol;                  // 是否创建隐藏卷
unsigned __int64 hiddenVolHostSize;  // 宿主（外层卷）大小
...
HWND  hwndDlg;                    // 可为 NULL
BOOL  bGuiMode;                   // 已有的无 GUI 开关
BOOL (__cdecl *progress_callback)(unsigned __int64, void*);  // 已有的无 GUI 进度回调
```

`TCFormatVolume()` 对隐藏卷是完整实现的，不是半成品：

| 行为 | 位置 |
|---|---|
| 校验宿主容量、计算隐藏卷数据区偏移 | `Format.c:125-131` |
| `dataOffset = hiddenVolHostSize - TC_VOLUME_HEADER_GROUP_SIZE - size` | `Format.c:131` |
| 设置 `cryptoInfo->hiddenVolumeOffset` | `Format.c:559-562` |
| 隐藏卷头写入位置 `hiddenVolHostSize - TC_HIDDEN_VOLUME_HEADER_OFFSET` | `Format.c:641` |
| 以 `OPEN_EXISTING` 打开宿主文件而非新建 | `Format.c:378-379` |
| 备份头处理 | `Format.c:723-737` |

**对 N-01 的修正**：N-01 说"隐藏卷无法通过任何官方接口程序化创建"——
这个结论**对外仍然成立**（SDK 的 `VeraCryptFormatOptions` 确实没有隐藏卷字段，
命令行也确实不支持，已由 Issue #1305 和本地实测确认）。
但准确的表述是：**不是核心不支持，而是官方对外发布的那两层接口没把它暴露出来。**
内部核心 `TCFormatVolume` 一直支持，只需填两个字段。

这把裁剪工作的性质从"重新实现"降级为"重新暴露"，难度差一个量级。

**另一处关键信号**：`bGuiMode`、`progress_callback`、以及全局 `Silent` 开关的存在，
说明 VeraCrypt 自己**已经为无 GUI 调用铺好了骨架**（推测是为 Format SDK 做的）。
我们不是第一个走这条路的人。

---

## 二、"VeraCrypt Format.exe" 的实际构成

依据 `src/Format/Format.vcxproj`：

| 来源 | 文件 |
|---|---|
| 本地（3） | `Tcformat.c`（向导，**10786 行**）、`InPlace.c`、`FormatCom.cpp` |
| `..\Common\`（37） | `Format.c`、`Volumes.c`、`Crypto.c`、`Pkcs5.c`、`Xts.c`、`Random.c`、`Fat.c`、`Dlgcode.c`、`Progress.c`、`Language.c`、`Keyfiles.c`、`SecurityToken.cpp`、`BootEncryption.cpp`、EMV/智能卡相关 12 个 …… |
| 工程引用 | `Crypto.vcxproj`（算法实现） |

---

## 三、向导独有逻辑：只有 4 个函数

隐藏卷创建是**两阶段**流程——先建外层卷，再挂载外层卷扫描其文件系统、
算出末尾连续空闲区有多大，才能确定隐藏卷的最大尺寸。
第二阶段是 `TCFormatVolume` **没有**覆盖的部分，也是向导里唯一有价值的代码：

| 函数 | 行号 | 约行数 | 作用 | 我方替代方案 |
|---|---|---|---|---|
| `MountHiddenVolHost` | 9950 | 27 | 只读挂载外层卷 | **直接用 Spike 1 已验证的 IOCTL 挂载替换** |
| `AnalyzeHiddenVolumeHost` | 9797 | 150 | 判定 FAT/NTFS/exFAT、取簇大小 | 保留逻辑，剥掉错误弹窗 |
| `ScanVolClusterBitmap` | 9984 | 85 | `FSCTL_GET_VOLUME_BITMAP` 从尾部反扫连续空闲簇 | 几乎零耦合，可近乎原样搬 |
| `DetermineMaxHiddenVolSize` | 9747 | 42 | 纯算术：空闲簇 × 簇大小 − 保留区 − 1/200 余量（上限 10MB） | 纯计算，无耦合 |

**合计约 320 行，位于 `Tcformat.c:9747-10068` 连续区间。**
`Tcformat.c` 其余约 10460 行全部是对话框页面流程、控件读写、本地化文案——**整块丢弃**。

其中 `ScanVolClusterBitmap` 的算法值得记一笔：它从簇位图**末尾往前扫**，
找出末端连续空闲区的长度。因为隐藏卷必须坐落在外层卷的尾部，
所以只有"末尾对齐的连续空闲块"才是可用空间——这也解释了为什么
VeraCrypt 官方建议外层卷用 FAT（NTFS 会在尾部写元数据）。

---

## 四、耦合面已量化

`Common/Format.c` 全文对 GUI/宿主环境的依赖 **仅 14 个函数、40 处调用点**：

| 函数 | 次数 | 性质 | 处理 |
|---|---|---|---|
| `GetString` | 9 | 本地化文案（Language.c） | 删除，改错误码 |
| `MessageBoxW` | 8 | 弹窗 | 删除，改返回值 |
| `Error` / `ErrorDirect` / `AskErrYesNo` | 7 | 弹窗（Dlgcode.c） | 删除，改错误码 |
| `UpdateProgressBar` | 4 | 进度条（Progress.c） | **已有 `progress_callback` 分支可走**（`Format.c:988-1057`） |
| `handleWin32Error` | 3 | 弹窗（Dlgcode.c） | 改为记录 `GetLastError()` |
| `RandgetBytes` | 2 | **随机数，必须保留**（Random.c） | 保留 |
| `UacFormatFs` / `UacFastFileCreation` | 2 | UAC 提权封装 | 删除（R-02 已证实无需提权） |
| `MountVolume` / `UnmountVolumeAfterFormatExCall` / `GetLastAvailableDrive` | 4 | NTFS 格式化需先挂载 | 用我方 IOCTL 实现替换 |
| `KillTimer` | 1 | 仅在 `bGuiMode` 为真时调用 | 天然不触发 |

**耦合是浅的、集中的、且以错误提示为主**——不是深层架构纠缠。
绝大多数处理方式是"删掉弹窗、改成返回错误码"。

---

## 五、裁剪方案

| 档位 | 内容 |
|---|---|
| **保留（核心）** | `Format.c`、`Volumes.c`、`Crypto.c`、`Pkcs5.c`、`Xts.c`、`GfMul.c`、`Random.c`、`Fat.c`、`Endian.c`、`Crc.c` + `Crypto.vcxproj` 全部 |
| **重写（约 320 行）** | 上述 4 个函数，剥离 GUI；外加一个 `main()` 做参数解析 |
| **新增（薄壳）** | 一个 shim，把 `Error`/`GetString`/`MessageBoxW`/`handleWin32Error` 替换成错误码累积 |
| **丢弃** | `Tcformat.c`（10786 行）、`InPlace.c`、`FormatCom.cpp`、`Dlgcode.c`、`Language.c`、`Progress.c`、`Keyfiles.c`、`BootEncryption.cpp`、`SecurityToken.cpp` 及 EMV/智能卡 12 个文件 |

**产物**：一个无界面命令行 exe，参数为容器路径、大小、算法、外层密码、隐藏卷大小、隐藏卷密码，
通过 stdout 报进度、退出码报结果。主程序 `CreateProcess` 调用它。

---

## 五点二、⚠️ 补充发现（2026-08-16 晚）：官方已有现成的无 GUI 建卷工程，方案应改用它

完整克隆源码后发现 `src/FormatDLL/` —— **这就是 Format SDK 那个 DLL 的源码**，
它在 VeraCrypt 主仓库里，不在 SDK 仓库里（SDK 仓库只发预编译产物，所以此前查不到）。

**这修正了本文第五节的裁剪方案与第七节第 2 项的风险判断。**

### 修正一：不要裁剪，官方的做法是"全链接 + 开关关界面"

`FormatDLL.vcxproj` 的源文件清单**包含** `..\Common\Dlgcode.c`、`..\Common\Language.c`、
`..\Common\Progress.c`，甚至**包含整个向导 `..\Format\Tcformat.c`**。
官方没有剥离任何 GUI 文件。

`VeraCryptFormatSDK.cpp:350-352` 的无 GUI 做法是：

```c
Silent   = TRUE;   // We don't want UI
bGuiMode = FALSE;  // Ensure GUI mode is off
InitApp (g_hDllInstance, L"");
```

外加 `volParams.hwndDlg = NULL` 与 `bForceOperation = TRUE`（`:300-302`）。

**即：官方的答案是"照常链接全部代码，用 4 个开关把界面关掉"，而不是把 GUI 代码删掉。**

**因此第七节第 2 项（`Dlgcode.c` 传递依赖深度未测）的答案已经揭晓**：
依赖确实深到官方也懒得剥，索性整体链接。**这不再是风险，而是既定做法**——
代价只是产物体积变大，而这对一个一次性调用的命令行工具毫无影响。

**本文第五节"丢弃 `Dlgcode.c` / `Language.c` / `Progress.c` / `Tcformat.c`"的判断过于乐观，作废。**

### 修正二：隐藏卷缺失只是一行硬编码

`VeraCryptFormatSDK.cpp:287`：

```c
volParams.hiddenVol = FALSE; // SDK does not support hidden volumes
```

**SDK 不支持隐藏卷，就只是这一行。** 底层 `TCFormatVolume` 一直支持（见第一节）。

### 修正后的 Spike 3b 方案（取代第五节）

不再"从向导裁剪"，改为**以 `FormatDLL` 为起点改造**——它是官方维护的、
已知可编译、已验证可无界面运行的工程，起点远比自行裁剪靠前：

1. 复制 `FormatDLL` 工程，改产物为 exe（或保留 DLL 由主程序的子进程加载）
2. 在公开结构体 `VeraCryptFormatOptions` 中增加 `hiddenVol` / `hiddenVolHostSize` 两个字段
3. 把 `:287` 那行硬编码改为从选项透传
4. 补上两阶段编排（建外层卷 → 挂载 → 扫簇位图 → 算隐藏卷尺寸 → 建隐藏卷）。
   **第三节那 4 个函数已经随 `Tcformat.c` 一起编译进来了，且均非 `static`，可直接调用**，
   连重写都可能省掉——只需处理它们内部的弹窗（`Silent` 已能压掉大部分）

工作量预期**低于**第五节原方案。风险主要转移到"那 4 个函数在 `Silent` 模式下的行为是否正常"。

### 附带的许可证机会（可选，不影响主线）

L-01 已确认 **SDK 仓库的预编译 DLL 是纯 Apache 2.0，链接使用不传染**。
而我方若从主仓库 `FormatDLL` 源码**重新编译**，产物含 TCL 3.0 代码，必须开源。

因此存在一个可选的优化：**普通卷用官方 Apache 2.0 预编译 DLL（可进程内调用、闭源），
仅隐藏卷走我方开源工具**。可减少开源面，但增加两套代码路径。
**建议 v1.0 不采用**——统一走一个开源工具更简单，反正该工具本就不含商业价值。

---

## 五点五、为什么建卷不能像挂载一样走 IOCTL

这是个必然会被反复问到的问题，在此定论。

**驱动没有建卷接口。** `Apidrvr.h` 中定义的 38 个 IOCTL 里**没有任何一个是创建/格式化卷**。
唯二带"写"语义的 `TC_IOCTL_BOOT_ENCRYPTION_SETUP(19)` 与
`TC_IOCTL_WRITE_BOOT_DRIVE_SECTOR(34)` 均为系统盘加密专用，与文件容器无关。

**根本原因是职责划分，不是接口缺失**：

| | 真正的工作在哪 | 用户态做什么 |
|---|---|---|
| **挂载** | **驱动内**：读卷头、派生密钥、建加密上下文、注册虚拟磁盘、实时加解密 | 递一个 `MOUNT_STRUCT` 过去而已 |
| **建卷** | **用户态**：生成主密钥 → PBKDF2 派生 → 构造并 XTS 加密 512 字节卷头 → 写文件 → 填充数据区 → 建文件系统 | 全部 |

建卷全程是纯用户态的密码学运算 + 文件 I/O，**没有一步需要内核**。
驱动的职责是"为已挂载的卷做实时加解密"，不是"造卷"。
把一次性、无需特权的建卷塞进常驻内核只会白白扩大攻击面——不会有人这么设计。

**佐证**：`Format.c` 中唯一用到驱动的地方，是 NTFS 格式化时先 `MountVolume` 把卷挂出来、
再调 Windows 的 `FormatEx` 往里写文件系统（`Format.c:833-850`）。
即便在建卷流程内，驱动的作用也仅是"把已造好头的卷挂出来"，未参与"造"本身。
`AnalyzeHiddenVolumeHost` 用 `TC_IOCTL_GET_VOLUME_PROPERTIES` 也只是查询。

### 这正是两者许可证处理不同的原因

| | 功能所在 | 我方行为 | 边界 | 结果 |
|---|---|---|---|---|
| **挂载** | 驱动（独立安装的进程外组件） | **调用**系统接口 | IOCTL——**天然存在** | 非衍生作品，**可闭源** |
| **建卷** | 用户态源码 | **包含**其代码 | **无天然边界** | 受 TCL 3.0 传染，**必须开源** |

挂载能闭源不是运气，而是该功能本就活在我方进程之外，我们只能"喊话"。
建卷没有这堵墙，代码必须编进我方二进制，传染无法回避。
**因此才需要人为造一个进程边界**（独立 exe + `CreateProcess`）——
把 IOCTL 天然给予的那堵墙，在建卷这侧手工砌一道。这就是裁剪方案的全部意义。

**附带结论**：建卷不触碰驱动，故**同样不需要管理员权限**，与 R-02 一致。
产品从建卷到挂载全程免 UAC。

---

## 六、许可证边界（与 L-01 一致）

- 该 exe 含 VeraCrypt 派生代码 → 受 **TrueCrypt License 3.0**，**必须开源**
- 开源的仅是 VeraCrypt 本就公开的建卷代码 + 我方约 320 行胶水，**不含任何商业价值**
- 主程序通过**进程边界**调用，不链接、不编译其源码 → **不构成衍生作品，可闭源可混淆**
- 与挂载走 IOCTL 是同一套隔离思路

⚠️ 与 L-01 相同的遗留事项：**此许可证边界仍需律师出具意见书。**

---

## 七、未验证事项（本次分析的边界）

诚实标注，避免被当成已完成的结论：

1. **完全没有编译过。** 以上全部基于读源码，不构成"能编译通过"的证据。
2. **`Dlgcode.c` 的传递依赖深度未测**——这是**最大不确定项**。
   Format.c 直接调用它只有 3 个函数，但 `Volumes.c`、`Random.c` 可能还有间接依赖。
   若传递依赖过深，可能被迫连 `Dlgcode.c` 一起保留（它很大且强绑 GUI），工作量会显著上升。
3. **工具链**：VeraCrypt Windows 端只支持 MSVC。本机目前**只有 MinGW，没有 `cl`/`msbuild`/`cmake`**，
   Spike 3b 需先装 VS Build Tools（3–5GB，需按 D 盘安装偏好处理）。
4. **NTFS 外层卷路径依赖挂载**：`FormatFs` 走的是"先挂载再调 `FormatEx`"，
   需要我方 IOCTL 挂载代码配合。FAT 路径（`FormatNoFs`/`FormatFat`）无此依赖。
   考虑到官方建议外层卷用 FAT，此项影响有限。
5. **未验证裁剪后建出的卷能否被官方驱动正常挂载**——这是 Spike 3b 的核心验收标准。

---

## 八、下一步建议

**Spike 3b 验收标准**（建议）：
1. 裁剪版 exe 能建出一个带隐藏卷的容器
2. 该容器能被 `vcspike mount` 用两个密码分别挂出外层卷与隐藏卷（复用 Spike 2 的验证方法）
3. `type` 字段分别为 2（外层）与 1（隐藏），容量符合预期
4. 全程无任何图形界面弹出

**排期影响**：P1 中"独立建卷工具"一项，按本次分析可下调预估
（原按"从上万行向导裁剪"估，实际是"320 行重写 + 一层错误码 shim"）。
但**在 Spike 3b 编译通过前不要调整排期**——第七节第 2 项是真实风险。
