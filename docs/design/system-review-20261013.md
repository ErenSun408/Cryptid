# 全项目系统审查（2026-10-13）

本次按 8 个模块逐一审查正式产品代码（不含 `spike/`、`third_party/`），关注正确性缺陷、R-03 / 8.6 可否认性、提权与隔离边界以及可优化点。审查以静态阅读为主，辅以少量本机实测；未修改任何代码，未执行全套测试与实机验收。

结论：没有发现会导致卷数据损坏或密钥直接泄露的逻辑缺陷；最需要先处理的是提权组件的 DLL 劫持面（🔴）和测试包调试日志对可否认性的影响（🟠）。

## 优先级总表

| 级别 | 问题 | 位置 | 状态 |
|---|---|---|---|
| 🔴 | 提权 exe 位于用户可写目录，非 KnownDLL 的 `bcrypt.dll` 可从程序目录劫持 | `packaging/windows/usb-vault.nsi:26`、建卷助手、setup helper | 静态分析确认，未实机利用 |
| 🔴 | 正式卡记录操作运行在「联网开发」helper 边界 | `src/card/vikey/vikey_helper_client.cpp:1152-1153` | 已知遗留，发布前阻塞项 |
| 🟠 | 测试包（`package-release`）调试日志记录隐藏卷与登录口令类型 | `tools/package-p2-development.ps1:73`、`card_login_mount_session.cpp:2184/2757/2901` | 已确认 |
| 🟠 | ViKey PIN 无防耗尽保护，凭据配置错误时自动流程可锁卡 | `src/card/vikey/vikey_session.cpp:227` | 待确认 UserSN 是否过滤外来卡 |
| 🟠 | 锁页内存默认配额约 42 页，峰值余量偏小 | `src/security/secure_buffer.cpp:30` | 已实测配额 |
| 🟠 | `mount_manager = 1` 在 `HKLM\SYSTEM\MountedDevices` 留下的痕迹未按 8.6 评估 | `src/volume/mount_request.cpp:127` | 待实机对比 |
| 🟠 | WebView2 配置目录及 Crashpad 转储长期留存 | `tools/app/main.cpp:1056`、`webview_cleanup.cpp:300` | 待验证转储内容 |
| 🟠 | 管理员会话在应用生命周期内常驻 | `src/volume_creation/elevation_session.cpp` | N-08 设计取舍，需记为残余风险 |
| 🟡 | 其余低风险问题 | 见各模块 | — |
| 🟢 | 可维护性优化 | 见各模块 | — |

## 模块划分

| # | 模块 | 范围 |
|---|---|---|
| 1 | 安全基础 | `src/security` |
| 2 | 卡通信 | `src/card`、`src/card/vikey`、`tools/vikey_helper` |
| 3 | 卷层 | `src/volume` |
| 4 | 建卷与提权 | `src/volume_creation`、`tools/volume_creator` |
| 5 | 应用编排 | `src/application` |
| 6 | UI 宿主 | `src/ui`、`tools/app/main.cpp` 中的 WebView 部分 |
| 7 | 前端 | `ui/card_session.html`、`ui/card_session.js` |
| 8 | 入口、平台与安装 | `tools/app`、`src/platform/windows`、`tools/setup_helper`、NSIS 与打包脚本 |

## 1. 安全基础

**🟠 锁页内存配额。** 每个 `SecureBuffer` 单独 `VirtualAlloc` + `VirtualLock`，至少占 1 页；全仓库未调用 `SetProcessWorkingSetSize`。本机实测默认配额下同时锁定到第 42 页时 `VirtualLock` 返回 1453（ERROR_WORKING_SET_QUOTA）。登录峰值估算 15～25 页；helper 侧每个请求另持 3～4 个凭据缓冲，journal 加载再持 6 个。超额时会在任意位置抛出 `system_error`，表现为难以诊断的登录失败。

- 修复：主程序及使用 `SecureBuffer` 的 helper 启动时调高最小工作集（数 MB）；增加同时持有 64 个缓冲的压力测试；可选改为共享锁页池分配小块秘密。

**🟡 已知残余。** Argon2 512 MiB 工作区可换页（`argon2id_runtime.cpp:53`，头文件已说明）；崩溃转储策略见模块 8。

**已确认正常：** 所有出错路径清零；`ConfirmedPassword` 一次性取出；UTF-8 校验拒绝过长编码与代理码点；NFC 规范化有上限；KDF 有固定答案向量测试。

## 2. 卡通信

**🔴 联网开发边界承载正式卡操作。** 所有需要卡会话的操作被强制要求 `network_enabled_development` 边界（无 AppContainer、允许联网），零网络 AppContainer 只用于三个探测操作。开发日志 2026-08 已写明「生产材料进入前仍须重新通过可靠的出站隔离门」，目前尚未关闭，应列为 P6 出口条件。

**🟠 PIN 次数耗尽。** 每次 helper 操作都重新 `user_login`，仅在剩余次数为 0 时拒绝。本机凭据错误时，一次登录流程中的多次自动调用即可耗尽次数。修复：剩余次数小于最大值时停止自动登录并提示。需先确认 `set_user_sn` 是否已把非本产品的卡过滤掉。

**🟡 低：**
- 卡记录头长度字段固定写 216（`card_record_v1.cpp:167`），三槽记录 292 字节；AAD 不覆盖实际长度，截断为 216 会静默丢弃最旧口令槽。需写卡能力，修复需新格式版本，记录在案即可。
- 两槽/三槽切换在最初几个字节处中断时，残留不以 `UVCR` 开头，不会被识别清理（`card_record_journal_v1.cpp:404-406`），对可否认性有轻微影响。

**🟢 优化：** 解锁时每次解密都重新打开 HMAC/AES 提供程序，journal 每次 SHA-256 亦然，可改用 CNG 伪句柄；`vikey_helper_protocol.cpp` 中 9 组近似读写函数（约 400 行）可表驱动。

**已确认正常：** IPC 字段校验严格、nonce、尾随数据检查与超时；helper 挂起启动、核实安全属性后再恢复并置于 job；可执行文件校验后持有句柄阻止替换；逐一推演 journal 同长度提交与长度切换的各中断点，均能恢复一致；解锁不提前退出。

## 3. 卷层

**🟠 MountedDevices 痕迹。** 挂载固定 `mount_manager = 1`，Windows 通常会在 `HKLM\SYSTEM\MountedDevices` 留下卸载后不清除的映射。需求清单只规定「不删除映射」，未分析其能否推断挂载次数或时间。应实机对比挂载前后注册表后再按 8.6 定性。

**🟡 低：**
- FAT32 单文件 4 GiB 限制未预检，复制前只查剩余空间（`volume_file_operations.cpp:245`），大文件写满 4 GiB 后才失败回滚。
- 目录检查要求最终解析回同一盘符（`volume_containment.cpp:89-96`），导出到映射网络盘、`subst` 盘或文件夹挂载点时报错不易理解，UI 应给出明确提示。
- 保留设备名缺少 `COM0`、`LPT0`、上标数字变体与 `CONIN$`、`CONOUT$`（`volume_entry_path.cpp:20`）。

**🟢 优化：** 每次 IOCTL 都重新打开驱动并查询版本；`partition_write_policy.cpp` 与 `mount_partition` 多语句压行，涉及破坏性分区写入，宜用 clang-format 重排；测试构建日志记录容器路径与隐藏卷保护标志（`volume_provider.cpp:254`）。

**已确认正常：** 驱动请求位于清零后的锁页结构；PRF 固定 SHA-512；删除不进入 junction 目标；复制遇链接中止；自身包含关系判断正确；重名不覆盖且回滚不删除既有文件；盘符均匀随机；卸载重试有上限，强制卸载仅在明确选择后发生。

## 4. 建卷与提权

**🟠 常驻管理员会话。** 应用每次启动即请求 UAC 并保留提权中转进程至退出（`tools/app/main.cpp:2102-2109`），源于 N-08 的无条件提权要求。代价是：一次同意后，同用户恶意代码注入本进程即可在不再弹窗的情况下发起分区格式化或 BitLocker 解密。文件操作均以调用方身份执行，因此无法借此获得管理员级任意写文件。应在需求清单 8.6 / N-08 下记为已接受残余风险。

**🟡 低：**
- 无 `--owner` 的直接 UAC 分支接受第一个连接的同用户客户端（`tools/volume_creator/main.cpp:260-263`），管道名与 nonce 均在命令行中。产品内总先建立会话，该分支不可达，但应补齐父进程 PID 核对或删除。
- `scan_volume_tail_space` 以管理员身份按盘符打开 `\\.\X:`（`volume_space.cpp:87`），与第 542 行「原始设备一律走 VeraCrypt 设备命名空间」的原则不一致；若盘符解析到其他卷，隐藏卷尺寸可能与外层数据重叠。应改用 `\\?\GLOBALROOT\Device\VeraCryptVolumeX`。
- `elevation_session.cpp` 作为提权核心大量压行并使用全局状态，宜重排。

**已确认正常：** 格式化、导入、删除均 `ImpersonateNamedPipeClient` 后执行，目标以调用方身份 `CREATE_NEW` 预建；受保护容器核对卷序列号与文件索引，拒绝链接与数据流；请求头各长度有上限并检查嵌入 NUL；会话管道仅本用户与 SYSTEM、单实例、拒绝远程并双向核对 PID；helper 命令行固定。

## 5. 应用编排

**🟠 测试包日志与可否认性。** 打包脚本强制 `package-release`，该配置开启 `USB_VAULT_DEBUG_LOG`。日志会写入「以登录派生挂载文件安全盘」「开始创建含隐藏卷的文件安全盘」「验证诱饵口令：匹配」及容器路径，与 R-03 直接冲突。近期 `out/installers` 交付均为此类测试包。

- 修复：交付说明明确「测试包记录隐藏卷活动，不得用于真实敏感数据」；任何真实使用前改用 `package-production`。

**🟡 低：**
- 登录口令在整个会话期间保留在锁页内存（`state_.login_password`），用于后续建卷；拔卡、重置、退出时清除，但暴露时间长于必要。
- 设置文件每次写入重读约 10 次并整体覆写，无锁；并发写入会丢失一次更新。

**🟢 优化：** `mount_orchestrator.cpp` 为 P1 遗留路径，仅 `p1_7` 工具使用，可删除或标为遗留；`card_login_mount_session.cpp` 3.5k 行、46 处 `catch (...)`，宜按建卷、诱饵维护、文件管理、挂载拆分。

**已确认正常：** 容器索引固定大小、随机填充、无明文头、整文件原子替换且未改槽位逐字节保留；每槽 AES-GCM 随机 nonce；操作历史仅存内存定长数组；拔卡、重置、确认登录重置路径均清除会话秘密与卡根密钥；设置项符合 8.4。

## 6. UI 宿主

**🔴 DLL 劫持提权。** 安装目录为当前用户可写的 `%LOCALAPPDATA%\Programs\USB Vault`；其中 `requireAdministrator` 的 `usb_vault_volume_creator.exe` 每次启动都经 UAC 运行。dumpbin 显示其导入 `bcrypt.dll`，本机 KnownDLLs 不含该项，因此会优先从程序目录加载。同用户恶意程序放入伪造 `bcrypt.dll` 后，下一次例行 UAC 同意即以管理员身份执行恶意代码。主程序对具名文件的摘要校验与句柄锁定（`main.cpp:578-586`）挡不住新增 DLL。

- 修复：提权 exe 入口最先调用 `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)`，并将非 KnownDLL 导入改为延迟加载；`/DEPENDENTLOADFLAG:0x800` 对 exe 自身静态导入是否生效需实测；根本方案是提权组件安装到用户不可写位置。修复后用 Procmon 核实加载路径。

**🟠 WebView2 配置目录留存。** 环境未指定数据目录或额外参数，配置固定在 `安装目录\USB Vault.exe.WebView2\EBWebView`，仅卸载时清理。其中的 Crashpad 转储可能含页面输入的密码字符串，目录本身也留有使用时间痕迹。可选方案：InPrivate 模式、关闭崩溃上报与磁盘缓存、每次会话用临时目录并在退出时擦除。转储实际内容需实测。

**已确认正常：** 密码以字节数组进入锁页缓冲并擦除源数组，字符串参数同样擦除；DevTools、右键菜单、脚本对话框、WebMessage、自动填充与密码保存均关闭；导航仅允许精确页面 URI；HTML/JS 摘要校验并持有句柄。页面直接提供建卷与导出的绝对目录，前提是页面不可注入，见模块 7。

## 7. 前端

前端未发现 XSS 或存储痕迹问题。

**🟡 低：** JS 密码字符串无法擦除且副本偏多：强度提示每次按键执行 `passwordInput.value.normalize("NFC")`（`card_session.js:1003`）；初始化失败时保留 `retainedPassword` 回填（`:5434-5482`）。与模块 6 的崩溃转储叠加时有泄露面，可改为按长度估算或交由原生计算。

**🟢 优化：** `card_session.js` 单文件 7.4k 行，现行 CSP 允许拆分为 ES module。

**已确认正常：** CSP 为 `default-src 'none'; script-src 'self'`；全文件无 `innerHTML`、`insertAdjacentHTML`、`eval`、`new Function`，文本均经 `textContent`；不使用 localStorage、sessionStorage、IndexedDB 或 cookie；注册全局 `error` 与 `unhandledrejection`；`busy` 标志防重复提交；口令字节数组用后擦除并清空输入框。

## 8. 入口、平台与安装

**🔴 DLL 劫持（补充）。** NSIS 把 `usb_vault_setup_helper.exe` 解到用户 `%TEMP%` 下的 `$PLUGINSDIR`，再以 `--install-driver-elevated` 提权运行。`SetDllDirectoryW(L"")`（`tools/setup_helper/main.cpp:55`）只移除当前目录，程序目录仍在搜索路径中，且静态导入在 `wWinMain` 前已加载。所有提权 exe 应按模块 6 统一修复。

**🟡 低：**
- 所有进程未设置崩溃转储策略（无 `WerAddExcludedApplication`、`WerSetFlags`、`SetErrorMode`），持有密钥的进程崩溃时可能生成并排队上传转储；按用户范围排除不需要管理员权限。
- 主程序无单实例保护：两个实例会弹两次 UAC、建两个提权会话；卡操作已有全局 mutex 串行，但容器索引与设置文件的读改写跨进程未同步，可能使某容器从列表消失（容器文件本身不受影响）。建议 `Local\` 单实例 mutex 并激活已有窗口。
- `Global\USBVault.CardOperation.v1` 使用默认安全描述符（`card_operation_lease.h:12`），同机其他用户可抢先占用造成卡操作持续「忙」。
- 启动日志在所有构建中写入 `安装目录\logs`（`main.cpp:2049`），内容仅为步骤码，但保留每次启动时间，需按 8.6 确认。

**已确认正常：** 驱动安装校验摘要、UAC 期间持有文件、提权子进程再次校验、拒绝重解析点且不覆盖既有驱动；WebView2 安装程序摘要校验；`vikey_helper` 使用 `SetDefaultDllDirectories`。

## 后续处理顺序

1. 提权组件 DLL 劫持修复与 Procmon 验证。
2. 测试包交付说明补充日志警示；明确真实使用前切换 `package-production`。
3. PIN 防耗尽保护（先确认 UserSN 过滤行为）。
4. 调高锁页配额并补压力测试。
5. 实机验证 MountedDevices 痕迹与 WebView2 Crashpad 内容，再决定处置。
6. 将常驻管理员会话与联网开发边界写入需求清单残余风险，后者作为 P6 出口条件。
7. 其余 🟡、🟢 项按模块分批处理。
