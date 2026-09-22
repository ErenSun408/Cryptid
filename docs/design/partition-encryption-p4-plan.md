# P4.1a — 分区加密源码审计与最小骨架

日期：2026-09-22。状态：只读源码／二进制导出审计完成，分区代码、建卷和暂停续作实测尚未实施。本片独立审阅，不把静态证据当作设备可行性验收。

## 本期目标

用户明确优先最小可行骨架。先贯通一个用户已准备的普通非系统数据分区：选择与校验 → 卡认证 → 空分区加密 → 手动挂载／卸载；再增加已有 NTFS 数据原地加密及正常暂停／正常退出／重开／原卡认证续作。保留原文件容器能力。

不做应用内分区创建／缩放／移动／删除，不做系统盘、整盘或分区隐藏卷。断电、强杀、突然掉线后的无损恢复暂不实施，不作 P4 出口，也不自动转入 P5。目标校验、错误停止、密码清理和卡认证是最小骨架的一部分。

## 固定输入与可复核证据

- 上游：VeraCrypt 1.26.29，提交 `b48e31f5b47da7d41025e3f0e02751675e15005a`；本地 `out/p326-veracrypt-upstream` 的 HEAD 匹配，`git status --short` 为空。
- 产品补丁：`third_party/veracrypt_format/veracrypt-1.26.29-usb-vault.patch`、`veracrypt-1.26.29-geometry.patch` 与 `UsbVaultGeometry.inl`；已审查其接口边界，未重新应用补丁或重建 DLL。
- 现有 `out/local-dependencies/VeraCryptFormat.dll` SHA-256：`416C9DABB1ABD83F4A0A36ACC1BAB233727B8E359A388507AD5E6DCF6363F184`，与 third_party README 的审定摘要一致。
- `dumpbin /nologo /exports` 实际列出 6 个导出：`VeraCryptFormat`、`VeraCryptFormatBinary`、`VeraCryptFormat_ChangePassword`、`VeraCryptFormat_Initialize`、`VeraCryptFormat_Shutdown`、`VeraCryptReadHiddenGeometry`。没有原地加密 Begin／Resume 导出。仅读取 PE，未加载 DLL 执行设备操作。

以下上游位置均相对固定源码根目录；产品位置相对本仓库。

| 审计项 | 源码事实与接入结论 |
|---|---|
| 空分区格式化 | `src/FormatDLL/VeraCryptFormatSDK.cpp:156` 的设备分支使用 `options->isDevice`、`GetDeviceSize` 与 `GetFormatSectorSize`，交给 `TCFormatVolume`。有可复用候选路径，未证明 USB Vault 设备建卷已可用。 |
| 设备几何 | `src/Format/Tcformat.c:341` 的 `GetFormatSectorSize` 查询设备几何，失败可终止进程；产品 `src/volume/mount_request.cpp` 仍填写逻辑／物理扇区 512。不能把文件容器假设直接套给 4Kn；最小布局范围需实验后确定。 |
| 创建入口 | 产品 `src/volume_creation/creator_helper_engine.cpp:178` 固定 `is_device=FALSE`，且按新建文件、删除失败容器的语义管理目标；`volume_creator.cpp:190` 只接受绝对本地文件路径。设备必须使用独立目标／动作，不能仅放宽原路径校验。 |
| 原地加密接口 | `src/Format/InPlace.h` 声明 `CheckRequirementsForNonSysInPlaceEnc`、`EncryptPartitionInPlaceBegin`、`EncryptPartitionInPlaceResume`。`FormatDLL.vcxproj` 未列入 `InPlace.c`；`Tcformat.c:357` 后向导代码受 `VCSDK_DLL` 条件排除。当前 DLL 不是现成的原地转换 SDK。 |
| 原地加密前置条件 | `src/Format/InPlace.c:120` 检查分区路径、NTFS、可缩文件系统、最小尺寸、卷头所需可用空间与扇区尺寸；其中管理员检测仅警告，产品不能把它当作权限门禁。 |
| 必要内部修改 | `InPlace.c:473` 起通过 `FSCTL_SHRINK_VOLUME` 收缩 NTFS 文件系统，必要时移动尾部簇，为加密头部预留空间。这不是产品提供“划出新分区”功能，不能因排除分区管理而错误禁掉原地加密自身的文件系统准备。 |
| 正常暂停与续作 | `InPlace.c:749` 的 Resume 从备份卷头恢复，循环检查 `bVolTransformThreadCancel`；循环结束更新头部，暂停返回 `ERR_USER_ABORT` 并置 PAUSED。产品适配需区分正常暂停、创建取消与失败，不能统一套现有建卷失败清理。 |
| 进度保存 | `InPlace.c:62` 的周期为 64 MiB；`:1826` 的 `FastVolumeHeaderUpdate` 更新加密区起点／长度，`:2245` 用密码读取备份卷头。正常暂停也会收尾更新；写入失败或清理中再次失败不能上报“已暂停”。 |
| 现有 GUI 与本地状态 | `InPlace.c:2145` 的 `SaveNonSysInPlaceEncSettings` 写入 AppData 配置计数／擦除算法；`Common/Dlgcode.h:79` 定义相关文件名。向导存在询问共享打开、锁卷与坏扇区清零的路径；新无界面适配必须明确替代策略，不能无条件照搬 GUI 行为。 |
| 现有测试范围 | `volume_provider_tests` 钉住 512 字节请求及路径参数，现有 creator/provider 集成夹具为文件容器；其通过不证明物理分区权限、几何和原地转换正确。 |

## 最小接入安排

1. **下一代码片：只读识别骨架。** 独立分区目标与可选性结果、Windows 枚举及只读测试；页面／调用方用会话内 ID 选择，禁止页面直接给提权进程任意设备路径。排除系统、启动、EFI、恢复及未知布局；提供打开磁盘管理、返回刷新。设备身份、偏移和长度必须在真正写入前重新核验。
2. **单个空分区闭环。** 在自建隔离测试盘验证格式化候选 API，再接入独立设备建卷动作、卡派生二进制密码、进度及手动挂载／卸载。文件容器的失败删除策略不用于设备；用户明确同意清空目标后才可进入写入。
3. **NTFS 原地转换闭环。** 为单独分发的创建 helper 提供受限 Begin／Resume／Pause 适配，不把上游源代码直接链接进闭源主程序。尽量复用上游算法与备份卷头进度，不先建设额外抗断电事务系统。
4. **正常暂停验收。** 多个进度点请求暂停，等待当前块及卷头成功保存并释放写句柄；正常退出后重新打开、使用原卡认证、复核同一目标，再读取卷头续作，最终文件摘要一致。已暂停卷不得当作全盘加密完成，不自动挂载到普通文件管理流程。
5. **后续补齐。** 在单分区链路成功后完成 P4 其余产品接线及回归；自动／批量挂载和扩大兼容范围不阻塞第一骨架。分区的逐字节密文导出目前不支持，相关入口应拒绝，明文导出可在完整挂载后复用并单独验收。

支持矩阵先以隔离测试盘上的普通基本数据分区为候选；GPT／MBR、512n／512e／4Kn 的支持必须逐项核实，不能从“VeraCrypt 有这个分支”直接宣称全支持。未知状态默认不可写，不在本片硬编码一个未经验证的产品支持承诺。

## 基本安全与明确不承诺的能力

- 正常暂停成功要有明确结果；若保存失败，报告失败／状态不确定，禁止静默从零开始、自动格式化或销毁分区。
- 拔卡撤销认证，停止后续工作并尽力收尾；新一次续作重新认证。设备断开或进程被杀不等于正常暂停成功，本期不保证其数据无损恢复。未实现的恢复不显示为已支持。
- 上游读错误可询问后将坏扇区清零；MVP 不自动同意该破坏性修复，遇到此类错误应停止并报告。不得用伪造“用户同意”来消除弹窗。
- 提权边界检查目标身份、范围、权限与独占条件，不能只信盘符或磁盘编号；无法确认目标时拒绝写入。
- 延续 R-03 与 8.6：密码只在原生锁页缓冲区驻留并及时清零，不写日志、命令行或进度文件；不引入档位、密码角色或隐藏卷存在标记。普通转换进度与秘密状态不能混放。
- 本片未改产品代码，不调用格式化／加密／挂载，不读写生产分区；断电无损保护的延期不改变已有数据先备份的要求。

## 本片验证与未完成项

执行固定源 HEAD／干净状态校验、DLL SHA-256 和 PE 导出检查，结果如上。现有 `volume_provider_tests`、`mount_orchestrator_tests`、`creator_protocol_tests`、`container_target_path_tests` 共 4/4 通过（0.77 秒）；只是既有行为基线。

未执行分区集成、UAC 设备权限、正常暂停续作、实卡或多机测试，未重建 DLL，也未执行断电故障注入。P4.1a 的输出为本审计与收窄后的规划，不代表 P4.1 整体完成；下一代码片从只读识别开始。本片已按用户授权提交为 `a17744e`，未推送。

## P4.1b — 原生只读分区发现骨架

2026-09-22：新增 `partition_catalog` 原生模块及测试，作为 P4.2 选择页面的数据来源前置。本片不接 WebView、提权 IPC 或任何写入动作；打开磁盘管理按钮、刷新 UI、会话内选择 ID 均留给下一接线片。核心 API 每次调用重新枚举，不使用持久缓存。

- 数据源为 Windows `FindFirstVolumeW`／`FindNextVolumeW` 返回的卷 GUID；这是一份**卷对象承载的分区清单**，不是完整物理分区表，未暴露为卷的 MSR／恢复等分区可能不出现。不接受外部设备路径或盘符作为查询输入。
- `CreateFileW` 以零 desired access、`OPEN_EXISTING` 打开枚举卷，只请求三个查询 IOCTL（分区信息、磁盘区段和几何），以及文件系统信息。无 `ReadFile`／`WriteFile`、锁卷、卸载、格式化、权限提升或注册表写入。
- 返回卷 GUID、盘号、分区号、起点／长度、逻辑扇区、候选状态与首个查询错误；仅驻留原生内存。清单枚举或当前 Windows 卷身份确认失败则整体抛错；单项查询失败保留不可选项，不能伪装为可用。
- 目前仅将 GPT 基本数据分区或已识别且非活动的 MBR IFS 分区、单磁盘区段、NTFS、512 字节逻辑扇区列为**候选**。GPT 只允许无属性或 NO_DRIVE_LETTER；平台必需、只读、影子副本、隐藏及未知属性拒绝。拒绝非正范围、加法溢出、未对齐与分区／区段不一致。
- 当前 Windows 卷优先排除；GPT EFI／MSR／恢复类型及 MBR 活动分区不可选。已测当前 Windows 卷排除，但不宣称已识别所有启动依赖、异地分页文件、BitLocker 或存储池。没有卷对象的分区不因缺席被推断为可用。
- **候选不是写入授权**：磁盘持久身份、设备替换／重新分区复核、BitLocker 状态、分页／启动依赖、容量支持、写保护与独占条件仍需在接入写入前补齐。当前盘号、偏移和卷路径的快照不能直接作为提权写目标；NTFS/512 筛选也不是加密支持矩阵的实测结论。
- 单元测试覆盖 40 个判定场景；没有把真假 GUID 或设备路径写入测试输出。`partition_catalog_tests --scan` 为显式只读实机检查，不注册到默认 CTest，不改变产品启动行为；连续两次刷新并验证 Windows 卷恰有一项被排除。

验证：新增模块 MSVC `/W4 /WX /sdl /analyze` 编译通过；Release 模块、测试与主应用构建通过；四组相关 CTest 4/4 通过（0.19 秒），40 个判定场景通过；当前机器只读枚举 5 卷、2 候选、3 不可选，Windows 卷排除 1。首次应用构建命令使用不存在的目标名而失败，修正为既有 `usb_vault_vikey_p2_development_app` 后通过。未执行全量测试、ASan、API 故障注入、设备建卷或卡认证实测。

安全审查：检查 API 失败默认拒绝、变长枚举上限 4096、IOCTL 返回长度、范围溢出、RAII 句柄释放与原生路径来源；没有扩大现有容器接口权限，枚举事实未持久化。热插拔可让快照过期，后续写入必须重新验证，本片无写入口。用户已授权提交本片并继续下一接线片。

API 依据：[CreateFile 查询访问](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilea)、[GPT 分区属性](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-partition_information_gpt)。
