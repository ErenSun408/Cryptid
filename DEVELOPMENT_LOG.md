# 开发日志

> 更新：2026-09-19。主日志压缩版；保留阶段脉络、关键决策、验证、安全边界和提交记录。
> 技术结论以[需求梳理清单 v2](需求梳理清单_v2.md)为准，阶段范围见[里程碑](MILESTONES.md)。
> [完整历史原文](docs/archive/development/DEVELOPMENT_LOG_2026-09-19_full.md)保留本次压缩前的全部内容；下文原文行号指向该快照。

## 阅读与状态说明

- 各阶段条目是当时的记录，“未提交”“尚未实现”等不代表今日状态；后续决策覆盖较早记录。
- 本轮前产品基线为 `82fed0f`（主程序更名为 `USB Vault.exe`）；交付说明已提交为 `969bacb`，独立依赖说明已提交为 `419ed62`。资料归档与日志整理获准提交，随本条记录落库；未推送。
- 2026-09-08 之前的 P3 指现在的 P4（加固与交付）；之后 P3 指数据管理。
- 主日志删去逐文件清单、重复审计过程与多轮调试细节；需要完整测试参数、失败原因和审查上下文时查原文。
- 归档材料中的旧根目录设计稿/原始需求路径按 `docs/README.md` 映射；历史路径不批量改写。

## 必须继续跟踪的边界

- R-03：秘密锁页、禁止隐式复制、用后清零；WebView2、转储和特权读取的能力边界不能表述为“内存中绝无明文”。
- 8.6：不得持久化档位或可推断档位的标识；中性设置与加密索引分离，日志内容和批次结果不作为持久记录。
- ViKey 开发兼容模式没有出站隔离保证，不等于 SDK 一定联网；生产材料启用前仍须通过独立的网络隔离安全门。
- 索引 v2 排序写在密文内；旧版应用不能继续修改同一索引。多实例并发写入不保证合并。
- 明文导出两道确认屏未做，与 Q-04 验收条目冲突，书面确认仍需跟踪；历史记录中的图标自绘、跨容器移动/迁移异步与取消缺口未在本次清理中关闭。
- 自动挂载在登录后后台串行执行，批次可超过五秒；批量暂停、中断、回退取消暂缓。强制卸载可能丢失未写入数据。
- 配置页三项供应商凭据明文显示，失败保留输入；用户密码无条件预填 `11111111` 是已接受取舍，实际卡 PIN 不同须手动修改。
- 安装器未签名；第三方再分发/法律审查、干净机安装卸载、缺依赖/UAC/重启、多机杀软及最新真卡流程仍需实机验收。
- P2.22～P2.26 的独立日志条目在原文中已标为缺失；本次不凭推测补造记录。
- `out/local-dependencies/`、交付文件与保留容器不是普通缓存；本次只压缩文档，不重新验证业务或关闭既有风险。

## 当前进展速览（截至 2026-09-19）

- P1 核心链路已完成，历史真卡验收含两种口令挂载与拔卡清理；这些结果不替代最新版本的实机回归。
- P2 已实现卡初始化/认证、容器创建/导入/挂载及加密路径索引；P3 已扩展文件管理、导出迁移、后台批次与偏好保存。
- P3.23：中性设置持久化、密文内排序、新容器置顶、登录后后台自动挂载已提交。
- P3.24：统一安装器负责依赖与许可；凭据配置已并入同一主窗口，替代较早的独立原生修复界面。缺依赖时需重跑安装器或手动补齐。
- 主程序现为 `USB Vault.exe`，内部目标及协议中的 development 命名部分保留；产品名称变化不表示已完成量产安全验收。
- 最新交付为 `out/deliveries/USB-Vault-20260919.zip`；SHA-256 为 `AA803000BC76E550AEB952FC5DEEB1047DBBC2B7EA924D0C5719209CFBDCC705`。该包未在本机完成安装验收。
- 清理前两片累计净减少约 8.23 GiB；正式构建统一引用 `out/local-dependencies/VeraCryptFormat.dll`，第三方输入和容器保留。
- 第三片已归档两版设计稿与三份原始需求资料、删除五个过时本地脚本；仍被构建/测试引用的开发工具保留。
- 最近清理验证：开发/发布构建通过、5 项定向测试通过，资料归档摘要/链接和页面契约检查通过；这不是最新全量或硬件验收。

## 精简记录

### 2026-08-21 — P1.0 安全基础设施

- 原文：第 22～106 行。
- **状态：** 已完成并获准提交
- 按 P1.0 实施时的阶段编号，P1.0 至 P1.3 必须能在 `ICardStore` 桩实现下完成测试；真实 USB 硬件不是 P1.0 的依赖。该编号已于 2026-08-22 重排， 新边界见后文「P1 子阶段重排」。
- `asan`：优化且带插桩的构建通过，CTest 2/2 通过，AddressSanitizer 无异常。 构建命令包含 `/fsanitize=address`，测试程序依赖 `clang_rt.asan_dynamic-x86_64.dll`。
- 审计期间已修复：最初的 `VirtualLock` 失败路径会重复清零一块刚分配且本来就为零的内存。对于超大请求，这可能无意义地触碰大量内存并放大拒绝服务影响。现已移除该冗余清零；构造函数返回之前不可能已有秘密数据写入。
- 提交：`e0909a2 [P1.0]add secure memory foundation and build validation`

### 2026-08-22 — P1.1 密钥派生

- 原文：第 107～197 行。
- **状态：** 已完成并获准提交
- 官方 `phc-winner-argon2` tag `20190702` ZIP 由离线方式提供；标签对应提交 `62358ba2123abd17fccf2a108a301d4b52c01a7c`，归档 SHA-256 为 `506A80B90AC3CA8407636F3B26B7FED87A55F2F1275CDE850CD6698903C4F008`。
- 新增固定 KDF v1 接口：Argon2id v19、m=524288 KiB、t=2、p=1、32 字节 card secret、16 字节 salt、64 字节原始输出，associated data 固定为 `usb-vault:volume-password:v1`，调用方不能覆盖或调低参数。
- 独立生产参数向量由 Python `cryptography 50.0.0` Argon2id 生成，正式 P-H-C reference 实现的 64 字节结果逐字节一致： `AB9C82025115EBFEA6BAE133D9FC7348AE11CAA6042ABDCD7ED353A8F698167ED89C0F13F5B49AF00CCB8CA7020DD961342D4E65DA1F39386135E374B1127472`。
- KDF context 显式固定 v19、Argon2id、成本、secret 与 associated data； 未启用会清空调用方缓冲区的 flags，秘密生命周期仍由 `SecureBuffer` 所有者控制。

### 2026-08-22 — P1 子阶段重排

- 原文：第 198～224 行。
- **状态：** 已完成并提交计划文档
- 原 P1.0~P1.4 总范围合理，但原 P1.1 同时覆盖第三方密码学、KDF、安全内存、 驱动 ABI、挂载、卸载和性能闭环；原 P1.4 同时覆盖卡通信与插拔安全响应； 最小 UI 没有独立出口条件。
- 已同步 `MILESTONES.md` 与技术主文档中的编号、出口条件和硬件边界。
- 计划修改未触碰运行时代码；编号一致性和 `git diff --check` 均通过。
- 提交：`f691745 [docs]split P1 into auditable development stages`

### 2026-08-22 — P1.2 单容器驱动边界

- 原文：第 225～315 行。
- **状态：** 已完成安全审计并获准按拆分边界提交
- `VolumeProvider::unmount()` 保持单次驱动边界语义；上层 `unmount_with_retry()` 在两秒窗口内每 100 ms 只重试精确匹配的 busy 错误，不自动升级为强制卸载。
- 拆分后的 P1.2 快照已重新配置、构建并验证：`dev` 4/4、`asan` 4/4、 `analyze` 4/4、Debug `integration` 5/5、Release `integration-release` 5/5 全部通过；AddressSanitizer 无异常，MSVC 代码分析构建无发现。
- Windows Shell 或第三方进程短暂持有卷句柄时，普通卸载可能在两秒窗口后仍失败； 当前策略会明确报告而不会强制卸载，后续 UI 必须把该状态呈现给用户。
- 已于 2026-08-22 批准继续按重排后的边界拆分提交。

### 2026-08-22 — P1.3 双容器应用编排

- 原文：第 316～417 行。
- **状态：** 实现、测试与安全审计完成，等待提交决定
- N-06 要求 B 存在时先以 `KDF(S, 用户输入)` 挂 B；B 失败立即认证失败， 不得派生或挂载 A；B 成功且 A 存在时，A 才使用 `KDF(S, "diskA")`。
- P1.1 `derive_volume_password_v1()` 接受锁页 `SecureBuffer` 的用户输入和 32 字节卡材料，按每容器 salt 返回 64 字节锁页卷密码；适合在每次 mount 返回后立即清空派生输出。
- 生产文件探测只把 `FILE_NOT_FOUND`/`PATH_NOT_FOUND` 视为不存在；目录、最终路径重解析点和其他访问错误全部失败关闭，避免错误地降级到另一派生分支。
- 最终验证结果：`dev` 5/5、`asan` 5/5、`analyze` 5/5、Debug `integration` 7/7、Release `integration-release` 7/7 全部通过；`/W4 /WX`、AddressSanitizer 与 MSVC 代码分析均无发现，`git diff --check` 无空白错误。
- 安全审查未发现未解决的越界访问、敏感值持久化、二次派生探测、错误分支越权挂载或正常卸载隐式强制问题。剩余边界是驱动 ABI 只接收路径而非已打开句柄， 因而文件探测与驱动打开之间无法彻底消除本地文件系统竞态；攻击者需已具备修改容器目录的权限，认证门和事务回滚把其影响限制为拒绝服务。硬链接同一文件也不能仅靠词法比较识别；P1.4 必须继续保证 A/B 由产品创建为两个独立文件。

### 2026-08-23 — P1.4 独立建卷工具

- 原文：第 418～523 行。
- **状态：** 已完成安全审查并获准按拆分边界提交
- IPC 是版本化的固定宽度二进制协议：严格校验 magic/version/header size、操作、长度、 保留字段和进度 stage；路径上限 32767 个 UTF-16 字符，密码上限 128 字节。 密码不进入命令行、环境变量或普通堆序列化缓冲，而是从调用方锁页 `SecureBuffer` 直接写入本地命名管道，在 helper 中直接读入新的 `SecureBuffer`。
- 每次调用使用 BCrypt 生成的 128 位随机管道名/nonce；helper 以 first-instance、 reject-remote 和仅 SYSTEM/对象所有者可访问的 DACL 创建单实例管道，并要求 nonce 与命令行随机值一致，未知协议版本、操作、stage 或错误码全部失败关闭。
- 最终自动化结果：`dev` 6/6、`asan` 6/6、`analyze` 6/6；Debug `integration` （排除需前台 UAC 的新建卷验收）8/8，Release `integration-release` 同范围 8/8。 AddressSanitizer 无异常，MSVC 静态分析修正一次 `wWinMain` SAL 签名后无发现， `/W4 /WX` 与 `git diff --check` 通过。
- 2026-08-23 在前台接受 UAC 后，Release `volume_creator_integration_tests` 退出码为 0：真实创建并验证了普通容器 A、容器 B 隐藏卷、未保护外层卷和受保护外层卷，对应 `type=0/1/0/2`，满足阶段要求的 `type=1/0/2` 全路径出口。
- 提交：`a1bde93 [P1.4]add volume creation client protocol`、 `0b964d0 [P1.4]add elevated volume creation helper`、 `d951ba8 [P1.4]add volume creation verification`

### 2026-08-23 — P1.5 最小 UI 与应用闭环

- 原文：第 524～549 行。
- **状态：** ✅ 已完成；小切片 1~15 均已按独立可审查边界提交
- `MILESTONES.md` 将 P1.5 的出口限制为：在桩卡下完成“单一密码输入 → 挂载现有容器 → 主动卸载”的最小闭环。P2 的注册、忘记密码、口令强度、风险确认、建卷向导、 三模式策略、备份与全量 UI 均不进入本阶段。
- `usb-vault_ui-disign.html` 只能作为视觉参考。其用户名、注册/忘记密码页、“密码存储于 USB 密钥”、按隐藏卷分类展示和包含隐藏卷事件的历史日志与最新需求冲突，P1.5 不得直接复用这些交互和文案。
- 仓库和本机 Build Tools 中尚未发现 WebView2 SDK 头文件。该依赖只影响后续 UI 外壳切片，不阻塞本次 UI 无关的卡存储合约。
- R-03 对 WebView2 口令桥接形成后续审计重点：浏览器输入和 COM/消息桥可能产生不可锁页的临时副本。后续切片必须避免 JSON/普通 C++ 字符串承载口令，复制进 `SecureBuffer` 后立即清空 DOM 与桥接缓冲，并明确记录浏览器进程内存无法完全清零的剩余边界，不能把 WebView2 便利性当成豁免。

### 小切片 1：`ICardStore` 单 blob 合约

- 原文：第 550～568 行。
- 新增 `ICardStore`，只暴露 `get_blob`、`put_blob` 与 `capabilities`。blob 通过 `SecureBuffer` 返回或借用，不允许接口层引入 `std::string`、`std::vector` 等会复制、 重分配秘密的所有权容器。
- 本切片不加入生产固定密钥。契约测试中的桩按调用即时生成 32 字节固定 blob，含首字节 NUL，用后由 `SecureBuffer` 清零；真卡实现、写入策略和硬件错误映射仍属于 P1.6。
- 独立 MSVC Debug 构建在 `/W4 /WX` 下通过；测试 7/7 通过，其中新增契约测试覆盖二进制 NUL 保留、get/put 所有权语义及 capability 不改变单 blob 形态。
- 审查未发现普通堆容器持有 blob、日志/异常输出秘密、档位/模式字段或本地持久化行为。 接口故意不规定 blob 长度；P1.5 应用会话在交给固定参数 KDF 前仍须强制验证 32 字节， P1.6 真卡实现也应在设备边界校验自身容量与返回长度。
- 提交：`e0fd8e8 [P1.5]define single-blob card store contract`

### 小切片 2：UI 无关的应用会话控制器

- 原文：第 569～599 行。
- 新增 `ApplicationSession`，单次挂载调用按值接收不可复制的 `SecureBuffer` 口令，从 `ICardStore` 读取一次 blob，先强制校验为 KDF 所需的 32 字节，再调用既有 `mount_session`。成功、普通失败、非法卡 blob 和残留挂载异常路径都会立即显式清空口令与卡 blob；对象成员不保存两者或派生结果。
- 全新 MSVC Debug `/W4 /WX` 构建通过，CTest 8/8；新增状态机测试覆盖成功挂载、非法卡 blob、普通认证失败、P1.3 回滚残留、重复挂载拒绝、B→A 卸载顺序，以及 B/A 分别失败后的精确重试状态。
- MSVC `/analyze` 构建无发现。Visual Studio 多配置生成器会让 `link.exe` 忽略直接传入的 `/fsanitize=address`，因此该次运行未计为 ASan 证据；随后在仓库正式 Ninja `asan` 预设下重新插桩构建，`application_session_tests` 1/1 通过且无 ASan 发现。
- 尚未实现 WebView2 宿主、口令桥或真实 UI 闭环。
- 提交：`4e4a1ef [P1.5]add retryable application session lifecycle`

### 小切片 3：可擦除的 UI 口令 UTF-8 入口

- 原文：第 600～626 行。
- 新增 `consume_password_utf8`，只接收调用方拥有的可变字节跨度，不接收 `std::string`、UTF-16 `BSTR` 或 JSON 文本。入口把原始 UTF-8 字节复制到锁页 `SecureBuffer` 后立即用 `SecureZeroMemory` 擦除来源，再以 `MultiByteToWideChar(..., MB_ERR_INVALID_CHARS, ...)` 严格校验编码。
- MSVC Debug `/W4 /WX` 完整构建通过，CTest 9/9；新增测试覆盖 ASCII 成功复制、中文与内嵌 NUL 保留、过长编码/截断编码/代理项编码拒绝、空输入拒绝，以及各可观察来源缓冲的清零。
- MSVC `/analyze` 的 `password_ingress_tests` 目标构建无发现；正式 Ninja `asan` 预设重新插桩构建，新增测试 1/1 通过且无 AddressSanitizer 发现。
- 剩余风险位于尚未实现的 WebView2 边界：密码在 DOM、JavaScript 字符串、`TextEncoder` 内部及 COM 跨进程封送中可能有不可锁页副本。下一切片必须最小化存活时间、避免 JSON/字符串桥并及时清空可控缓冲，但浏览器进程内存无法作绝对清零保证。
- 提交：`8e68661 [P1.5]add wipeable UI password ingress`

### 小切片 4：UI 会话适配器

- 原文：第 627～649 行。
- 抽出只包含 `authenticate_and_mount`、`unmount_active` 与统一 `mounted()` 的 `IApplicationSessionActions`，既有 `ApplicationSession` 实现该接口；接口不暴露盘符、 容器数量、卷类型、档位或任何敏感材料。
- MSVC Debug `/W4 /WX` 完整构建与 CTest 10/10 通过；新增测试覆盖二进制安全的口令转交及来源擦除、挂载失败仍擦除、统一 mounted 状态，以及卸载失败不得隐藏活动状态。
- 首次链接新增测试时遇到一次 `LNK1201` 写 PDB 失败；未修改代码原样重试即成功，确认为瞬时工具链/文件占用问题，不计为产品缺陷。
- MSVC `/analyze` 目标构建无发现；正式 Ninja `asan` 预设下新增测试 1/1 通过且无 AddressSanitizer 发现；`git diff --check` 通过。
- 提交：`9c09fd5 [P1.5]add UI session action boundary`

### 小切片 5：WebView2 本地页面宿主骨架

- 原文：第 650～679 行。
- 依据微软原生 Win32 集成方式，引入固定版本 `Microsoft.Web.WebView2 1.0.4129.50`； CMake `FetchContent` 固定官方 NuGet URL 与 SHA-256 `D3934F482D484B89FB4825DF720C710664E1143A1E90F7B3A60794EF33F473D2`，静态链接 x64 Loader。SDK 包只缓存于已忽略的 `out/`，不提交第三方二进制包。
- 本机已有 WebView2 Runtime `151.0.4129.101`。Debug `/W4 /WX` 宿主构建成功，隐藏启动后主进程保持运行并产生 6 个新的 `msedgewebview2.exe` 进程，测试结束后已清理；证明 environment/controller 能在真实 Runtime 上建立。
- 首次运行因错误地以空输出探测 `UrlCreateFromPathW` 缓冲长度而停在初始化错误路径；改为有界输出缓冲后真实运行通过。MSVC `/analyze` 又发现 192KB 栈缓冲 C6262 及 `wWinMain` SAL C28251，分别改为普通堆路径缓冲并补齐入口注解后分析无发现。
- Debug 既有 CTest 10/10 通过；正式 Ninja `asan` 预设下宿主完成插桩编译和链接。GUI ASan 二进制未自动启动，因此该结果只证明宿主可插桩构建，不作为运行时内存检查证据。
- 提交：`47cb861 [P1.5]add local-only WebView2 host`

### 小切片 6：单密码框页面契约

- 原文：第 680～701 行。
- 将初始化占位页扩展为唯一一个 `type=password` 的会话输入控件、一个中性动作按钮与统一 `aria-live` 状态区；没有用户名、注册/忘记密码、模式选择、卷分类、日志入口或任何可推断档位的页面字段及文案。
- MSVC Debug `/W4 /WX` 完整构建与 CTest 11/11 通过；MSVC `/analyze` 的页面契约测试目标构建无发现；正式 Ninja `asan` 预设下该测试 1/1 通过且无 AddressSanitizer 发现。
- 人工复核未发现提交、网络、脚本、浏览器存储、自动填充或页面分类状态；本切片没有 DOM 到原生的口令复制路径，禁用控件也不会产生用户输入的秘密副本。
- 视觉样式只提供中性登录卡片，不复用设计稿中与技术主文档冲突的用户名、密码存卡、 隐藏卷分类和历史日志内容。完整视觉与无障碍验收仍属于后续 UI 工作。
- 提交：`482bd5d [P1.5]define inert single-password UI`

### 小切片 7：WebView2 字节数组适配器

- 原文：第 702～724 行。
- 新增 `submit_webview_password_array`，边界只接受一维 `VT_UI1` SAFEARRAY；拒绝字符串、JSON、其他元素类型及多维数组，避免把密码转换成普通所有权字符串。
- 边界计算支持非零下界，并使用有符号宽运算避免上下界相减时溢出。本切片仅实现可独立测试的 COM 数据适配器，尚未注册 `IDispatch` host object，也未启用页面控件。
- MSVC Debug `/W4 /WX` 完整构建与 CTest 12/12 通过；专项测试覆盖非零下界、非 ASCII 与内嵌 NUL 的逐字节转交、成功及会话异常后的来源清零，以及错误 VARIANT 类型拒绝。
- 人工复核未发现字符串/JSON 密码副本、日志、持久化或分类状态。错误类型在无法证明其所有权和字节布局时只拒绝、不擅自改写；浏览器 DOM、JavaScript 引擎及 COM 跨进程封送仍可能保留不可控副本，后续桥接只能缩短可控副本寿命，不能声称绝对清零。
- 提交：`29a00cf [P1.5]add WebView password array adapter`

### 小切片 8：窄化原生 WebView host object

- 原文：第 725～746 行。
- 新增无类型库的最小 `IDispatch` 实现，只解析一个区分大小写的 `submitPasswordBytes` 方法名；未知名称、未知成员、属性读取、错误参数数量和非字节数组参数均拒绝。对象不提供 mounted、盘符、容器、卷类型、日志、档位或其他状态查询。
- MSVC Debug `/W4 /WX` 完整构建与 CTest 12/12 通过；新增测试覆盖唯一方法名、无类型信息、 未知状态名拒绝、属性访问拒绝、有效数组成功转交、应用失败映射，以及两条路径的来源清零。
- MSVC `/analyze` 的正式代码和专项测试目标构建无发现；正式 Ninja `asan` 预设下专项测试 1/1 通过且无 AddressSanitizer 发现；`git diff --check` 待提交前复核。
- 人工复核未发现 host object 暴露分类状态、记录秘密、把口令变成字符串，或在异常文本中回显输入。其生命周期依赖后续宿主确保控制器活得更久；真正注册到页面后仍须限制仅本地源可访问，并验证 typed array 以 SAFEARRAY 而非字符串/JSON 方式封送。
- 提交：`4d5be60 [P1.5]add narrow WebView host object`

### 小切片 9：受限本地 WebView2 的 host object 注册

- 原文：第 747～770 行。
- WebView2 controller 建立后、首次本地导航前创建窄化 `IDispatch`，以中性名称 `nativeBridge` 调用 `AddHostObjectToScript` 注册；任何创建或注册失败都会中止导航， 不会降级为没有安全桥的可输入页面。
- MSVC Debug `/W4 /WX` 完整构建与 CTest 13/13 通过；`usb_vault_app` 的 MSVC `/analyze` 构建无发现；正式 Ninja `asan` 预设下宿主完成插桩编译和链接。
- 在已安装的 WebView2 Runtime `151.0.4129.101` 上隐藏启动真实 Debug 宿主 3 秒：主进程保持运行并产生 6 个新的 `msedgewebview2.exe` 进程；测试结束后只清理本次进程。 这证明本地导航前的 host object 注册与真实 Runtime 生命周期可建立。
- 人工复核未发现新增外部导航、脚本、持久化、日志或分类状态。临时后端即使被意外调用也只清空并拒绝口令；但 host object 一旦注册就会对该 WebView 的文档可见，因此现有严格导航白名单仍是安全边界，后续不得放宽为任意本地或网络页面。
- 提交：`23c2a7c [P1.5]register local WebView host object`

### 小切片 10：固定 blob 桩卡与正式应用会话组合

- 原文：第 771～800 行。
- 新增 P1.5 专用 `FixedBlobCardStore`：每次读取直接在锁页 `SecureBuffer` 中生成固定的 32 字节测试 blob，不把 blob 保存为成员或普通堆对象；桩明确只读，写入会拒绝，也不冒充任何真实硬件能力。
- MSVC Debug `/W4 /WX` 完整构建与 CTest 14/14 通过；新增测试覆盖固定 blob 的独立锁页分配、确定性内容、无虚假能力和只读拒绝。MSVC `/analyze` 的桩、测试和宿主无发现。
- 针对轻量模型质量方面的担忧，在全新的 `out/build/review-clean` 目录从零配置并编译全部 53 个构建步骤，随后 CTest 14/14 再次通过；证明结果不依赖原有增量对象缓存。
- 安全复核未发现 blob 常驻普通内存、秘密日志、持久化状态或档位实体。固定 blob 仅为 P1.5 桩，不能进入 P1.6 真卡实现；固定容器文件名和盐不是秘密，但发行配置与建卷流程后续仍需形成明确契约。页面启用前还必须补齐关闭主动卸载，并依据微软 WebView2 契约设置 `shouldPassTypedArraysAsArrays=true` 后做真实封送验证；两项均未被当前禁用页面触发， 记录为后续边界而非本切片阻塞。
- 提交：`16f8037 [P1.5]compose fixed-card application session`

### 小切片 11：窗口关闭主动卸载守卫

- 原文：第 801～824 行。
- 新增 `prepare_ui_session_for_close`：无挂载时直接允许关闭；存在活动挂载时先调用既有主动卸载，只有统一 mounted 状态确实清空后才允许销毁窗口。该函数为 `noexcept`，避免 C++ 异常穿越 Win32 窗口过程。
- MSVC Debug `/W4 /WX` 完整构建与 CTest 15/15 通过；专项测试覆盖无挂载不调用卸载、 活动挂载成功后允许关闭、失败阻止关闭并保留状态，以及随后重试成功。
- MSVC `/analyze` 的守卫、测试和宿主无发现；正式 Ninja `asan` 预设下专项测试 1/1 通过且无 AddressSanitizer 发现；`git diff --check` 待提交前复核。
- 真实 Runtime 首次以隐藏窗口启动时 `MainWindowHandle=0`，无法由测试工具发送 `WM_CLOSE`，因此不计产品失败；清理后改为普通窗口启动，获得有效句柄并成功发送正常关闭，主进程在 5 秒内自行退出。500ms 后尚有 2 个本次 WebView2 子进程处于退出尾声， 测试随后按 PID 清理，未触碰既有 WebView2 进程。
- 提交：`a7d328f [P1.5]guard window close with unmount`

### 小切片 12：WebView2 typed array 真实 Runtime 验证

- 原文：第 825～852 行。
- 依据微软 WebView2 host objects 契约，测试页面在调用前显式设置 `chrome.webview.hostObjects.options.shouldPassTypedArraysAsArrays = true`，再把固定的 `Uint8Array` 直接传给既有 `submitPasswordBytes`；没有改动产品页面，密码框和按钮仍禁用。
- 首次专项链接因测试目标仍是 console subsystem、源码入口为 `wWinMain` 而失败；把目标明确声明为 `WIN32` 后恢复，该问题只涉及测试目标类型，与 typed array 封送行为无关。
- MSVC Debug `/W4 /WX` 完整构建与 CTest 15/15 通过；专项目标在 Debug、`/analyze` 和 AddressSanitizer 配置下均构建通过，`/analyze` 无发现。Debug 与 ASan 专项可执行文件均在本机 WebView2 Runtime `151.0.4129.101` 上退出 0，证明当前 Runtime 会把该 `Uint8Array` 投影为既有适配器要求的一维 `VT_UI1` SAFEARRAY，并保持字节值不变；ASan 无发现。
- JavaScript 引擎、WebView2 和 COM 内部仍可能制造测试无法逐字节追踪或主动擦除的短期副本； 本切片只验证可控 typed array 与原生消费链路，不把它表述为浏览器进程全内存清零保证。 `git diff --check` 通过。
- 提交：`7276726 [P1.5]verify WebView typed array bridge`

### 小切片 13：WebView2 主动卸载方法

- 原文：第 853～876 行。
- P1.5 出口需要用户可主动卸载，而既有 host object 只有密码提交方法；本切片只新增无参数 `unmount`，把调用转交既有 `UiSessionController::unmount()`，仍不启用产品页面。
- MSVC Debug `/W4 /WX` 完整构建与 CTest 15/15 通过；专项单元测试覆盖卸载成功、卸载失败、 带参数调用拒绝、属性访问拒绝，以及 `mounted` 仍不可发现。
- MSVC `/analyze` 专项目标构建无发现；正式 Ninja `asan` 预设下专项测试 1/1 通过且无 AddressSanitizer 发现。
- 真实 Runtime 验证页面扩展为先提交固定非秘密 `Uint8Array`、再调用零参数 `unmount`； Debug 与 ASan 可执行文件均在本机 WebView2 Runtime `151.0.4129.101` 上退出 0，原生探针确认会话先挂载后卸载，且既有 JavaScript 数组清零断言仍通过。
- 提交：`7f8070f [P1.5]expose active-session unmount`

### 小切片 14：WebView2 统一活动会话查询

- 原文：第 877～902 行。
- 页面启用前复核发现：挂载失败回滚可能留下仍需处理的活动挂载，WebView 渲染进程也可能重载；若页面只有 `submitPasswordBytes` 与 `unmount`，重载后无法安全恢复“打开/卸载” 按钮状态，回滚残留也可能暂时失去可见恢复入口。
- MSVC Debug `/W4 /WX` 完整构建与 CTest 15/15 通过；专项单元测试覆盖活动/非活动布尔值、 `VT_BOOL` 返回类型、带参数拒绝、属性访问拒绝，以及原始 `mounted` 名称保持隐藏。
- 真实 Runtime 验证在固定非秘密 typed array 提交后调用查询并得到 JavaScript `true`， 主动卸载后再次查询得到 `false`；Debug 与 ASan 可执行文件均在本机 WebView2 Runtime `151.0.4129.101` 上退出 0，既有数组清零断言继续通过。
- 该布尔值会向严格本地页面表明当前应用会话是否仍有活动挂载，这是实现状态反馈和恢复所需的最小可观察面；真实与胁迫路径、一个与两个容器均只得到同一个 `true`，不能据此分类。未发现口令副本、日志或持久化行为。产品页面仍无脚本且控件禁用。 `git diff --check` 通过。
- 提交：`c37db3d [P1.5]expose unified session activity`

### 小切片 15（2026-08-24）：产品页面最小会话状态机

- 原文：第 903～956 行。
- 复核 `MILESTONES.md` 的 P1.5 出口、`需求梳理清单_v2.md` 的 R-03 与 8.6，以及已提交的 `UiSessionController`、窄化 host object、本地页面、导航白名单和关闭守卫后，本切片只启用页面侧“输入密码—打开—统一状态反馈—主动卸载”状态机，不扩大到真卡、插拔监听或新模式。
- 当前 Runtime `151.0.4129.101` 的逐层诊断确认：即使启用 `shouldPassTypedArraysAsArrays`，同一个 `Uint8Array` 也可能投影为一维 `SAFEARRAY(VARIANT)`，每项为 `VT_I4`，而不是先前观察到的 `SAFEARRAY(UI1)`。适配器现仅接受这两种窄格式；variant 路径逐项要求 `VT_I4` 且值域为 0~255，使用锁页 `SecureBuffer` 归一化，并在转交既有密码入口前清零 COM 整数槽。混合类型、越界整数、直接 `SAFEARRAY(I4)`、非数组和会话失败均有拒绝/清零测试，不接受字符串或任意自动类型转换。
- MSVC Debug `/W4 /WX` 完整构建与 CTest 15/15 通过；真实 Runtime 闭环连续三次退出 0： 含内嵌 NUL 和多字节 UTF-8 的固定非秘密输入逐字节到达原生探针，DOM 输入已清空，页面进入统一活动状态，随后主动卸载并恢复 ready。加固后的正式宿主经 UAC 隐藏启动 4 秒保持运行并创建 6 个 WebView2 子进程，随后只停止本次主进程；未点击输入、未访问容器、未产生挂载， 子进程已退出。
- 安全复核未发现页面日志、持久化、用户名、模式/档位实体或分类反馈。产品禁用 WebMessage， 测试页的 WebMessage 仅存在于不打包、不可日常运行的专项测试宿主，测试 CSP 不允许内联脚本。 WebView2、JavaScript 引擎和 COM 内部仍可能产生应用无法主动擦除的短期秘密副本；当前实现会立即清空 DOM，在异步编组结算后清空 typed array，并清空已识别的 COM/原生受控缓冲区， 但不能承诺浏览器全内存清零。异步代理挂起期间 typed array 必须保留原值，否则会破坏提交； 本地 `session.js` 的替换防护仍依赖后续安装包与签名完整性阶段。
- 提交：`072f79b [P1.5]accept WebView variant byte arrays`、 `42000bf [P1.5]activate secure session controls`

### 阶段收口（2026-08-24）

- 原文：第 957～967 行。
- P1.5 出口条件已满足：固定 blob `ICardStore` 桩下能够完成“输入单一密码 → 挂载两个盘 → 统一状态反馈 → 主动卸载”，且窗口关闭会先尝试卸载并在失败时保留可重试入口。
- 最终验证为 MSVC Debug CTest 15/15、Debug 真实 WebView2 Runtime 连续三次退出 0、正式 ASan 相关测试与真实 Runtime 闭环退出 0、MSVC `/analyze` 全量通过；安全复核未发现秘密日志、本地持久化、模式/档位实体或分类反馈。浏览器与 COM 内部不可控短期副本风险按本阶段已知边界保留，后续由安装包完整性和签名阶段继续收口。
- USB 安全卡已经确认到货。P1.6 可开始真卡型号、接口、驱动/SDK、单 blob 读写及可选持久计数器能力的实机审计；到货不等同于能力验收通过，结论须在 P1.6 记录。

### 需求补充：明文导出与密文换机迁移（2026-08-24）

- 原文：第 968～985 行。
- 已确认保留既有明文导出，并补充密文导出供个人用户换机迁移；其余需求不变。
- 为避免把“密文导出”误实现成新的密钥体系，技术口径定为逐字节复制既有 VeraCrypt 容器： 不解密、不重新加密、不修改容器，不导出卡内根密钥 `S`、口令、PIN、派生结果、档位或恢复材料。换机后仍须使用原安全卡与原口令，卡丢失/损坏仍不可恢复，也不提供自动迁移、 新卡换卡或恢复码。
- 明文导出的原有风险确认、隐藏盘二次确认和零本地痕迹要求全部保留；密文迁移导出同样不得持久化目标路径、历史或校验值。复制失败、空间不足、取消和目标冲突不得留下被误认成成功结果的不完整副本。
- 验证：`git diff --check` 通过；既有 MSVC Debug CTest 15/15 通过。本切片只改文档，未触碰生产代码或安全卡。安全复核确认未新增根密钥导出、恢复码、新卡迁移或档位元数据；P2 实现时仍须专项防范目标覆盖、部分副本、重解析点/路径替换、Shell 最近记录和持久化复制校验值。
- 提交：`f48cca9 [docs]define encrypted migration export boundary`

### P1.6 真卡存储（2026-08-24 开始）

### 计划、需求与既有实现审计

- 原文：第 988～998 行。
- `MILESTONES.md` 的 P1.6 出口为：真实 `ICardStore` 完成单 blob 读写、以能力位报告硬件差异， 卡材料只进入 `SecureBuffer`，且不得在本地持久化秘密；P1.7 的插拔监听和拔卡卸载不提前混入本阶段。
- `需求梳理清单_v2.md` 要求卡至少保存 32 字节密钥材料；N-07 还要求审计可写持久计数器， 但不支持时允许降级为仅保留完整性校验。N-02 的单 blob 和本地不可区分边界继续成立。
- 已检查 `ICardStore`、`FixedBlobCardStore` 及其契约测试：现有应用边界只有 `get_blob`、 `put_blob` 和能力位，读结果为 `SecureBuffer`；P1.5 桩明确只读且不保存可变秘密。正式适配器尚不存在，现阶段没有需要迁移的真卡代码。

### 真卡首次接入只读审计

- 原文：第 999～1017 行。
- 插卡后未出现格式化、初始化或厂商软件提示。Windows 将新到达设备识别为 `LiViKey ViKeyPRO`，硬件 ID 为 `VID_1987&PID_0331`、设备版本 `0x0500`；首次安装和本次到达时间均为 2026-08-24 04:52:12（系统记录时间）。设备状态为 Started、问题码为 0， 使用微软内置 `HidUsb` / `input.inf`，不需要专用内核驱动。
- 设备只公开一个厂商自定义 HID 集合：Usage Page `0xFF00`、Usage `0x0001`；Input/Output Report 长度均为 0，Feature Report 长度为 257 字节。USB 字符串没有公开序列号。探测仅调用 PnP 属性、HID attributes/string/preparsed-data/caps 读取接口，没有发送 Feature Report、 PIN、初始化或写入命令。
- 厂商产品页称 ViKeyPRO 提供 4KB 数据区、用户/管理员 PIN 权限与 64 个计数器模块，纸面能力覆盖 32 字节 blob 和 N-07 计数器需求；但厂商 SDK 当前仅通过客服渠道提供。本次尚未取得官方头文件、库、示例、二进制校验信息和协议说明，故这些能力尚未完成实机验收。
- 网络上可找到非官方 API 转录，显示数据写入和计数器初始化需要管理员权限、运行期计数器为递减模型；该资料只作为索引线索，不作为生产 ABI 或安全结论。取得与本卡匹配的官方 SDK 并审计其签名、位数、调用约定、缓冲区长度、错误码、PIN 重试/锁卡语义前，不进行登录或任何写卡测试。
- 提交：无（仅文档审计）

### 官方 SDK 静态审计（小切片 1）

- 原文：第 1018～1050 行。
- SDK 已从仓库移到仓库外的 `D:\Desktop\usb-vault-local-deps`；Git 当前只看到本次既有的三份文档修改，没有 SDK 文件进入工作区。收到的压缩包版本为 `ViKeySDK_V4.1.2.37-7.4.7-7`，大小 498,898,534 字节，SHA-256 为 `AA50C5BF2AB071D90E965DD9B984B36E5F76B854307E41FAB27FA4D638D93555`；解压后共 1,051 个文件、1,156,017,632 字节。厂商未随包提供独立校验签名或可核对的发布哈希。
- 已完整提取并渲染复核官方《产品说明书》和《API 开发手册》的相关页，交叉检查头文件、x64 导出表及 .NET/C# 示例。主头文件 SHA-256 为 `739A3527E25942BA29F643BAFACDA37B33915820341896A1089DD1164FE93953`；正式 x64 `ViKey.dll` 为 4.1.2.37，SHA-256 为 `735A2347F66F10C649C9B38ED0232B839812277C8E178F0C5084C676CF1DE48E`。SDK 内 47 份同名 DLL 只有三个不同哈希，除当前 x86/x64 版本外只有一份旧 x64 4.1.0.16，全部未签名， 不存在可替换的已签名版本。
- 头文件使用 `extern "C"` + `__stdcall`，x64 DLL 的未修饰导出与所需的发现、登录、容量查询、 数据读写、次数模块和清理函数一致。头文件和各语言示例均把数据地址/长度声明为 32 位 `DWORD`/`uint`，但 PDF 的单函数页把两者写成 16 位 `WORD`；权限汇总表又与 `VikeyGetType` 单函数页冲突。生产适配器须以头文件、导出和示例的交集建立显式 ABI 断言， 并要求供应商书面确认这两处文档冲突，不能直接照抄 PDF 原型。
- 7.4.6 及以后硬件必须先调用 `VikeySetUserSN` 才能发现设备；7.4.5 及以后还须在登录前设置 API Key。文档要求从加密管理工具“帮助中心”复制 UserSN，并用管理工具根据 API Key 种子生成 API Key。SDK 示例内含一组通用演示常量，不能据此认定为本采购批次凭据。当前尚未取得本卡的 UserSN、API Key/种子及其供应链交付说明，因此不能进行有意义的匿名 API 探测。
- 当前手册称卡数据区最大可达 16KB，产品页对 ViKeyPRO 的描述为 4KB，实际型号容量须通过 `VikeyGetUserDataSize` / `VikeyGetAdminDataSize` 实机查询，不能预设。数据区分为用户区与管理员区；当前权限表显示用户登录可读写用户区、只读管理员区，足以承载一个 32 字节 blob。 计数器为 16 位递减模块，提供初始化、读取、递减与校验 API，纸面上覆盖 N-07，但持久性、 断电行为、原子性、溢出/耗尽和权限仍须真卡验证。

### SDK 供应链与集成安全审查

- 原文：第 1051～1082 行。
- x64 `ViKey.dll` 未签名，PE 头没有 ASLR、NX_COMPAT、CFG 或高熵地址标志，包含 `.boot`、 `.vikey` 等保护/加壳节；导入依赖含 WinHTTP、Winsock、Crypt32、DbgHelp 等。静态导入只能证明它具备相应能力，不能证明会联网，但也无法从受保护二进制审计其真实控制流。约 188MiB 的官方管理工具同样未签名、加壳且缺少 ASLR/NX，并声明 WinINet/WinHTTP 网络依赖。本次没有运行 DLL、管理工具、示例或远程授权工具。
- SDK PDF 内的《软件开发协议》允许为保护应用而合并/链接接口软件，并允许合理数量的归档副本， 同时禁止逆向、反汇编、反编译和修改；包内没有独立 `LICENSE`。协议没有清楚写明可把 `ViKey.dll` 随消费级产品再分发，故发行前必须取得供应商对 DLL 再分发、更新与漏洞响应的书面授权。自行逆向 HID Feature Report 协议既违反当前协议边界，也会把未经验证的私有协议变成生产 ABI，不采用。
- 在当前证据下，不允许把该 DLL 直接加载进持有根密钥、用户口令和派生结果的主进程。下一代码切片应先建立可替换的 SDK 边界、严格错误映射和失败关闭测试；若供应商不能提供签名且带现代缓解标志的库，还需把供应商组件放进受限、禁网的独立 helper，并把 DLL 哈希列为显式部署白名单。即使隔离，DLL 仍会看到其返回的卡内 blob，因此隔离只能缩小主进程攻击面，不能把未知供应链风险描述成已消除。
- P1.6 还暴露一个需求契约缺口：产品登录界面只有一个任意长度的用户口令，但设备另需一个固定 8 字符用户 PIN；同时还需要批次 UserSN 和 API Key。实现前必须明确配卡/初始化流程、三类凭据的来源与生命周期、是否允许相同或默认 PIN、blob 地址/长度/版本及首次写入的失败恢复规则。 任何方案都不得把卡 PIN、根密钥或可推导档位的信息持久化到本地；未明确前不能把默认 PIN 硬编码进产品，也不能把用户口令静默截断成 8 字符充当卡 PIN。
- 提交：无（仅文档审计）

### UserSN 获取与匿名真卡探测（小切片 1 补验）

- 原文：第 1083～1101 行。
- 已通过官方管理工具的“帮助中心”取得当前账户 UserSN。完整 UserSN 不写入仓库、日志或产品配置；本次仅经标准输入交给一次性独立探测进程。探测前再次核对 x64 DLL SHA-256 为 `735A2347F66F10C649C9B38ED0232B839812277C8E178F0C5084C676CF1DE48E`，只调用 `VikeyGetLibraryVersion`、`VikeySetUserSN`、`VikeyFindEx`、硬件 ID/类型/固件/容量/权限/PIN 次数查询及 `VikeyUninitialization`；没有设置 API Key、激活、登录、尝试 PIN、读写数据区或调用计数器。一次性探测脚本执行后已删除，未进入 Git 工作区。
- 匿名调用成功发现 1 张实体卡：接口库 4.1.2.37、硬件类型值 3（ViKeyPRO）、固件 7.4.7， 硬件 ID 与管理工具界面一致。用户数据区为 8,192 字节，管理员数据区为 8,192 字节，总容量 16KB；这解决了产品页 4KB 与新版 API 手册“最高 16KB”的冲突，并远超 32 字节单 blob 需求。
- `VikeyGetLevel`、用户 PIN 次数与管理员 PIN 次数查询均返回 `0x80000024` （`VIKEY_ERROR_NOT_ACTIVE`），与管理工具显示“未激活”一致。这些查询不执行登录，未消耗 PIN 尝试次数。SDK 的公开头文件和示例没有激活 API，激活只暴露在管理工具中，当前尚不清楚它会绑定哪些账户/批次信息、是否联网、是否可逆及是否同时初始化密码/数据区。
- 因此下一安全边界从“取得 UserSN”推进为“确认并执行首次激活”：在供应商说明激活副作用、 API Key 交付方式、默认/初始化 PIN、恢复出厂影响和离线/联网要求前，不点击“激活”。卡仍未写入项目根密钥，P1.6 单 blob 写入/读回与计数器实测尚未开始。

### 首次激活与激活后匿名复核

- 原文：第 1102～1114 行。
- 已明确决定激活这张新卡，并在官方管理工具内完成操作。界面确认卡已绑定当前 UserSN， 出厂日期为 2026-08-13；软件 ID 仍是厂商默认 `1234ABCD`，设备名为空。激活前后均未向数据区写入项目根密钥，也未设置产品设备名或修改软件 ID。
- 使用同一经哈希核对的 x64 DLL 再次执行独立匿名只读探测：`VikeyFindEx` 发现 1 张卡， `VikeyGetLevel` 成功返回 0（匿名权限）；用户 PIN 与管理员 PIN 的当前/最大尝试次数均为 200/200，说明激活未消耗 PIN 次数。`VikeyUninitialization` 成功，未执行登录、PIN 尝试、 数据读写或计数器操作。一次性复核脚本随后删除，完整 UserSN 仍未写入仓库。
- 激活阻塞已解除。下一步转为只读确认管理工具“密钥设置”页的 API Key 种子/生成流程，再制定正式配卡值；在该流程确认前不使用 SDK 示例 API Key，不尝试默认用户/管理员 PIN，也不覆盖卡的密钥设置。

### 开发 API Key 生成与匹配验证

- 原文：第 1115～1131 行。
- SDK 批量配置注释确认 API Key 种子最长 32 字符且可自定义；示例生成结果为 128 个十六进制字符。管理工具页面同时包含 PIN 修改、尝试次数、API Key 生成和“外壳独立密钥”写卡四类操作；本次只使用“API 密钥种子 → 生成 API 密钥”，没有触碰其他三类状态。
- 使用系统 CSPRNG 生成 16 字节开发专用随机种子并编码为 32 个十六进制字符。种子 SHA-256 短指纹为 `7532E145A07A`；生成的 128 字符 API Key 经格式校验，SHA-256 短指纹为 `ED580A9B5298`。两个明文均未进入聊天、仓库或开发日志，只保存在主机 Windows 凭据管理器的 `usb-vault-vikey-dev-api-seed` 和 `usb-vault-vikey-dev-api-key` 通用凭据中；剪贴板已清空。
- Codex 文件/进程沙箱与主机凭据库隔离，沙箱内 `cmdkey /list` 不会显示上述主机凭据；在同一主机用户上下文中读取开发 API Key 后执行独立验证，`VikeySetUserSN`、`VikeyFindEx`、 `VikeySetApiKey`、`VikeyGetLevel` 和 `VikeyUninitialization` 均返回成功。权限级别仍为 0， 证明 API Key 匹配且该验证没有登录或消耗 PIN 次数。一次性验证脚本随后删除。
- 该凭据仅用于当前开发卡和 P1.6；生产种子/API Key 必须另行生成并进入正式构建秘密管理， 不能复用开发值。厂商资料仍未说明同一 UserSN + 种子生成的 API Key 是账户/批次通用还是单卡独立，量产流程定案前须向供应商确认或用第二张卡交叉实测。

### 硬件交付边界确认

- 原文：第 1132～1147 行。
- 客户决定经转述确认：量产阶段由客户硬件方自行烧录安全卡，当前 ViKeyPRO 只用于开发阶段跑通， 最终卡型/供应商可能替换。因此 P1.6 不实现量产烧录工具、生产卡初始化后台或生产凭据托管， 也不把 ViKey 选型写死为产品架构。
- 现有 `ICardStore` 单 blob 合约正是正式交付边界：业务层只接触 `get_blob`、`put_blob` 和能力位， 卡材料只以 `SecureBuffer` 进入/离开。ViKey 的 DLL、ABI、UserSN、API Key、8 字符设备 PIN、 数据区偏移和错误码必须封闭在厂商适配实现及部署注入层，不得泄漏到 KDF、挂载编排、UI 或本地持久化格式。
- 当前 P1.6 仍要用这张卡完成真实 32 字节 blob 写入/读回与持久计数器验证，以证明抽象可实现； 但验收结论只证明 ViKey 开发适配可用，不构成量产硬件锁定。以后换卡时只替换适配器并重跑 `ICardStore` 契约、秘密生命周期、错误映射、插拔和能力降级测试，不修改上层安全设计。
- 下一代码切片建议先实现“厂商无关函数表/错误域 + ViKey 动态加载边界”的单元测试骨架， 不立即把未签名 DLL 加载进主进程；该边界评审并提交后，再接真实登录、blob 和计数器调用。
- 提交：`d665955 [P1.6]record ViKey audit and replaceable hardware boundary`

### 小切片 2：通用错误域与 ViKey 动态加载边界

- 原文：第 1148～1202 行。
- 重新核对 `MILESTONES.md` 的 P1.6 出口、`需求梳理清单_v2.md` 的 R-03、N-02 和 8.6， 并检查既有 `ICardStore`、固定 blob 桩、错误类别实现、CMake 目标和卡契约测试。结论是 `ICardStore` 已经是正式的厂商无关边界，不应再公开一套照搬 ViKey 登录/地址/计数器语义的 “通用”函数表；供应商切换只需替换适配实现。
- MSVC Debug 全量 CTest **16/16** 通过；新增测试覆盖相对路径和不完整装载接口拒绝、DLL 装载失败、缺少导出时卸载、完整 15 项函数表的单一生命周期，以及已知/未知 ViKey 状态到 `CardError` 的失败关闭映射。
- 对仓库外 x64 DLL 重新计算 SHA-256，仍为 `735A2347F66F10C649C9B38ED0232B839812277C8E178F0C5084C676CF1DE48E`；使用 `dumpbin /exports` 静态确认 15 个必需导出全部存在。该操作只读取 PE，不加载 DLL，也不访问安全卡。安全扫描同时确认运行期 ABI 中没有管理员登录、计数器初始化或档位/模式实体，代码与测试中未加入 UserSN、API Key、PIN 或根密钥明文。
- 真卡 `ICardStore`、8 字符设备 PIN 的独立凭据注入、32 字节 blob 地址/版本/首次写入恢复、 持久计数器实测和拔卡处理均未实现。本切片没有改变卡片状态。
- 提交：`00b3319 [P1.6]add isolated ViKey SDK loading boundary`

### 小切片 3：ViKey DLL 固定身份与加载期防替换门

- 原文：第 1203～1257 行。
- 重新核对 P1.6 的真卡出口、R-03 与 8.6，并复查小切片 2 的 `VikeyModule`、构建隔离及测试。 本切片仍不实现 helper、IPC、卡登录或 `ICardStore`，只完成任何供应商代码执行之前都必须经过的本地二进制身份门；产品主程序继续不链接 ViKey 目标。
- 依据 Microsoft `CreateFileW` 文档确认：不授予 `FILE_SHARE_WRITE` 可在句柄存续期拒绝后续写入， 不授予 `FILE_SHARE_DELETE` 可拒绝后续删除/改名；依据 `GetFileInformationByHandleEx` / `FileAttributeTagInfo` 检查目录、普通文件与重解析点；SHA-256 使用 Windows CNG 的 `BCryptCreateHash` → `BCryptHashData` → `BCryptFinishHash` 生命周期。
- 新增 `vikey_artifact_tests`，覆盖固定目录/文件名构造、相对目录拒绝、正确摘要、摘要不匹配、 空文件、超过 16 MiB、失败后句柄释放，以及令牌存续期间文件写入/删除和部署目录/上一层测试目录改名被拒绝。模块测试进一步证明锁句柄随模块生命周期持有，并为两支测试增加顶层异常输出，避免 Debug 失败被 Windows 错误报告对话框掩盖。
- 该门只证明“准备加载的文件与显式摘要一致”，不能证明未签名供应商 DLL 本身安全。独立 helper、最小 IPC、禁网/低权限进程令牌、显式继承句柄清单和进程缓解策略仍未实现，所以真实 DLL 仍不得加载进产品主进程；下一小切片先建立 helper 进程与 IPC 外壳，并由 ViKey 适配构建注入不可由运行时用户配置改写的当前版本摘要。
- 提交：`357b751 [P1.6]require verified ViKey DLL identity`

### 小切片 4：受限 ViKey helper 与最小 IPC 空外壳

- 原文：第 1258～1323 行。
- 重新核对 P1.6 出口、R-03、N-02、8.6 以及前三个小切片的 `ICardStore`、ViKey 私有 ABI、 动态加载和 DLL 固定身份门。结论是供应商代码必须停留在独立非提权进程中，主程序和公共卡接口均不能链接 helper 客户端或 ViKey SDK；本切片只验证进程/IPC 边界，不加载 SDK、不登录、 不读取 UserSN/API Key/PIN、不读写卡片、计数器或根密钥。
- 依据 Microsoft 官方文档复核 `CreateRestrictedToken`、`CreateProcessAsUserW`、 `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`、进程缓解策略和 job object：显式句柄清单要求目标句柄可继承且 `bInheritHandles=TRUE`；受限主令牌可直接用于 `CreateProcessAsUserW`；`KILL_ON_JOB_CLOSE` 可收口异常/超时路径，`ACTIVE_PROCESS=1` 与子进程策略共同阻止 helper 扩张进程树。
- PE 复核确认 helper 为 Windows GUI，启用 high-entropy ASLR、动态基址和 NX，嵌入清单为 `asInvoker`/`uiAccess=false`；helper 仅导入 Windows `ADVAPI32.dll` 与 `KERNEL32.dll`，不导入 `ViKey.dll`。`usb_vault_app.exe` 导入表同样不含 ViKey。敏感信息扫描和 `git diff --check` 在本切片最终复核中执行；本测试过程没有加载厂商 DLL、访问安全卡或改变卡片状态。
- 真卡会话 RAII、独立 8 字符设备 PIN 注入、32 字节 blob get/put、首次写入恢复、持久计数器和 P1.7 拔卡处理仍未实现。
- 提交：`9603b38 [P1.6]add restricted ViKey helper boundary`

### 小切片 5：ViKey helper 固定身份与启动期防替换门

- 原文：第 1324～1376 行。
- 重新核对 P1.6 出口、R-03、8.6、小切片 3 的 DLL 身份令牌和小切片 4 的受限 helper 启动路径。本切片只解决 helper 可执行文件自身的身份与启动期替换风险；不实现网络隔离、SDK 装载、卡登录、PIN/API Key/UserSN 注入、blob/计数器操作，也不接触根密钥或安全卡。
- 新增移动独占的 `VerifiedLocalArtifact` 与 `verify_local_artifact`：只接受固定本地盘的绝对普通文件，显式拒绝相对路径、空文件名、NTFS 备用数据流、末级重解析点和最终路径重定向； 同时锁住包含目录与文件，文件只共享读、两者都不共享删除，令牌销毁前不能写入、删除或替换已验证制品。文件大小由调用方给出上限，当前 DLL/helper 均限制为 1 字节至 16 MiB，并通过持有的同一文件句柄分块计算 SHA-256。
- 新增/扩展测试覆盖通用令牌不可复制、正确摘要、摘要不匹配、相对路径、备用数据流、空/超限文件、文件写入/删除锁、目录及上一层改名锁、helper 正确摘要启动、错误摘要启动前拒绝和 150 ms 卡死终止。MSVC Debug 与 AddressSanitizer 全量 CTest 均为 **18 项通过、1 项因父令牌已受限而明确跳过**；两套进程测试在普通桌面令牌下分别单独通过。
- PE 复核确认产品 `usb_vault_app.exe` 不导入 ViKey 或网络库；helper 仍为 Windows GUI、 `asInvoker`/`uiAccess=false`，启用 high-entropy ASLR、动态基址和 NX，且只依赖 `ADVAPI32.dll` 与 `KERNEL32.dll`。仓库扫描未发现当前卡的 UserSN 或硬件 ID；既有文档中的厂商默认软件 ID 不是秘密。本切片没有加载 SDK、访问安全卡或改变卡片状态。
- 提交：`db7bef1 [P1.6]verify ViKey helper identity before launch`

### 小切片 6：ViKey helper 可验证网络隔离门

- 原文：第 1377～1447 行。
- 重新核对 P1.6 出口、R-03、N-02、8.6 以及小切片 4~5 的受限进程、IPC 和 helper 身份门。 本切片只建立供应商代码接触卡内 blob 前的独立禁网门，不装载 `ViKey.dll`，不调用 SDK， 不访问安全卡，也不读取或注入设备 PIN、API Key、UserSN、根密钥或其他凭据。
- 对照 Windows 官方 AppContainer 启动与 capability 文档后，选择不声明任何 capability 的 AppContainer：网络能力默认拒绝，且可与现有受限主令牌、显式句柄清单、单进程 job 和进程缓解同时使用。未选择 Windows 防火墙/WFP 作为运行时证明，因为其规则创建通常需要管理员、 修改全局状态并依赖部署配置，不能替代每次启动前可回读的子进程令牌属性。
- 最终 MSVC Debug、AddressSanitizer 与 `/analyze` 三套全量 CTest 均为 **18 项通过、1 项因 Codex 沙箱父令牌已经受限而明确跳过**。普通桌面令牌下 Debug 与独立 Release 优化版进程测试均通过；ASan 插桩父测试进程启动未插桩 helper/fixture 也通过，未报告内存错误。MSVC `/analyze` 全量构建零项目诊断；仅对 Windows SDK `WS2tcpip.h` 自身的 C6101 误报作头文件范围内的窄抑制，项目代码仍保持 `/WX`。
- 发布时仍须把 helper/DLL 固定 SHA-256 注入不可由普通用户修改的配置，并由安装器落实受保护目录 ACL。本切片未改变该部署门，也未完成真卡会话 RAII、独立设备 PIN、32 字节 blob、首次写入恢复、持久计数器或 P1.7 拔卡处理。
- 提交：`7ab0237 [P1.6]add verifiable ViKey helper network isolation`

### 小切片 7：ViKey helper 供应商运行库与零能力 HID 兼容性门

- 原文：第 1448～1537 行。
- 重新核对 P1.6 出口、R-03、8.6、小切片 3 的供应商 DLL 身份门，以及小切片 4~6 的受限 helper、启动期防替换和零 capability AppContainer 禁网门。本切片只验证供应商运行库和 ViKeyPRO HID 能否在既有边界内工作；不注入 UserSN、API Key 或设备 PIN，不建立 SDK 卡会话， 不发送 HID input/output/feature report，也不读取或写入卡内 blob。
- 对照 Windows 官方 AppContainer、HID device capability、custom capability 和设备安装安全文档。普通 HID 的受管应用访问要求 `humanInterfaceDevice` 设备 capability；Windows 10 1809 起清单中的 VID/PID/Function 限制会被忽略。更精确的 custom capability 需要 capability 所有者、 应用包/SCCD，并由驱动 INF 或驱动代码把 capability 绑定到设备接口；设备 ACL 同样属于驱动/ 部署安全描述符，不是无管理员、每次随机 profile 的运行时开关。
- 无害 `ViKey.dll` 夹具在普通桌面令牌下通过完整进程测试：来源/暂存双重摘要、启动期文件锁、 固定导出调用、完整安全属性、禁网回归、错误摘要启动前拒绝、超时和 profile 清理均成立。 官方 x64 `ViKey.dll` 的 SHA-256 复核为 `735A2347F66F10C649C9B38ED0232B839812277C8E178F0C5084C676CF1DE48E`，在相同零网络 AppContainer 内加载并完成版本查询；未关闭 Defender，也未调用任何卡 API。
- 开发兼容模式仍必须保留 helper 独立进程、非提权受限令牌、固定 SHA-256、启动期防替换、净化环境、显式继承句柄清单、单进程 job、现有进程缓解、版本化 nonce IPC、截止时间和失败关闭。 该模式只使用当前开发卡、开发凭据和非生产 blob；不得承载量产根材料、生产 API Key/PIN 或被记作生产安全验收结果。
- 因当前小切片 7 尚未提交，以上决策先并入同一审阅边界；下一代码切片须在本切片提交决定后开始，不把历史零网络启动函数静默改义，应为开发兼容启动提供显式、不可误认的入口与测试。
- 提交：`da88f0f [P1.6]verify ViKey helper runtime compatibility`

### 小切片 8：ViKey helper 明确联网的开发兼容启动边界

- 原文：第 1538～1614 行。
- 重新核对 P1.6 出口、R-03、N-02、8.6，以及需求清单 2026-08-25 新增的开发/生产运行边界。本切片决策为：开发阶段不让零 capability AppContainer 的 HID 拒绝继续阻塞真实 SDK 适配，允许 helper 使用系统默认网络与开发卡；生产材料进入前仍须重新通过可靠的出站隔离门。
- 新增三个显式开发入口，分别用于 helper 空握手、供应商运行库版本查询和 HID 打开测试；原 `probe_vikey_helper`、`probe_vikey_vendor_runtime`、`probe_vikey_hid_access` 继续强制进入零 capability AppContainer，既有禁网合约没有改义。IPC 安全位为精确集合：开发模式只删除 `app_container`、`no_capabilities`、`low_integrity` 三位，其余边界位不得缺失或多报。
- 仓库外官方 x64 `ViKey.dll` 的 SHA-256 仍为 `735A2347F66F10C649C9B38ED0232B839812277C8E178F0C5084C676CF1DE48E`；普通桌面下通过完整开发边界加载并只调用 `VikeyGetLibraryVersion`。真实 ViKeyPRO 测试先确认零 capability AppContainer 仍返回 `ERROR_ACCESS_DENIED`，再由开发模式成功打开/关闭同一设备的读写句柄； 未发送任何 HID report，未调用卡 API，未读取凭据或 blob，卡状态未改变。
- 随机暂存目录与摘要/锁能阻止制品静默替换，但它仍位于当前用户可写的 LocalAppData，尚未由安装器落实受保护 ACL。当前无秘密的版本/HID 兼容测试可接受这一开发风险；真实卡会话接入时仍须确认只使用测试材料，并把安装目录 ACL、生产摘要注入和出站隔离保留为交付门。
- 提交：`905cb64 [P1.6]add network-enabled ViKey development boundary`

### 小切片 9：ViKey 开发卡会话 RAII 与错误映射核心

- 原文：第 1615～1675 行。
- 重新核对 P1.6 出口、R-03、`ICardStore` 可替换适配边界及现有开发 helper 约束。本切片只实现可由 helper 后续调用的会话核心与伪 SDK 测试，不修改 IPC，不加载真实 `ViKey.dll`，不访问安全卡，也不读取或注入实际 UserSN、API Key、设备 PIN、blob 或计数器。
- 官方头文件与示例确认调用顺序为 `VikeySetUserSN` → `VikeyFindEx` → `VikeySetApiKey` → `VikeyUserLogin`，退出时执行 `VikeyLogoff` 与 `VikeyUninitialization`；用户 PIN 明确为 8 个字母或数字。官方示例一致使用 32 个十六进制字符的 UserSN 与 128 个十六进制字符的 API Key， 但头文件没有单独声明这两个长度，因此把精确长度保留为 ViKey 私有适配约束，不上移到 `ICardStore`。
- MSVC Debug 与 `/analyze` 构建最终均为 **21/21 通过**：受限沙箱内先得到 19 项通过、2 项因父令牌已受限而明确跳过，随后在普通桌面令牌下串行补跑同一构建的两项 helper 跨进程测试并全部通过。`/analyze` 零项目诊断。AddressSanitizer 构建为 19 项普通测试通过、2 项按既有强进程运行时约束跳过；新增 `vikey_session_tests` 与 `vikey_module_tests` 均在 ASan 下通过， 未报告内存错误。
- 当前只完成进程内会话核心，尚未把凭据安全送入开发 helper，也未在 helper 内装载正式函数表或建立真实会话；因此不能记作真卡认证通过。下一小切片应设计固定长度、版本化、单次消费的凭据 IPC，在开发 helper 内落入 `SecureBuffer` 后立即清除传输缓冲，再接入本会话核心。
- 提交：`7de1701 [P1.6]add ViKey development session lifecycle`

### 小切片 10：开发凭据受控 IPC 与 helper 会话接线

- 原文：第 1676～1740 行。
- 重新核对 P1.6 真卡出口、R-03、`ICardStore` 可替换边界、2026-08-25 开发/生产运行决策， 并复查版本化 nonce IPC、开发 helper 进程边界、DLL 双重摘要/防替换门以及小切片 9 的 `VikeySession`。本切片只打通开发凭据传输与 helper 内一次真实函数表会话调用，不实现 `ICardStore`、blob 读写或计数器，不引入档位状态。
- helper 只在明确识别为联网开发环境时接受开发会话操作，并将三项凭据直接读入三块固定长度的锁页 `SecureBuffer`。随后重新验证工作目录内固定名 `ViKey.dll` 的摘要并持有文件锁，使用 `VikeyModule` 解析完整 15 项函数表，调用 `VikeySession::open`，显式注销与反初始化；请求作用域在响应写回前结束并擦除全部 helper 侧凭据。每个 helper 进程仍只处理一个请求并退出，失败后不复用供应商全局状态。
- 协议测试覆盖版本 2、操作/载荷精确匹配、200 字节请求体往返、错误尺寸拒绝及调用方凭据保持； 进程测试覆盖开发会话的完整 SDK 顺序、精确开发安全属性、调用方锁页缓冲保持、错误凭据的通用错误映射、错误尺寸的暂存前拒绝，以及成功/失败后的暂存目录清理。扩展后的无害 DLL fixture 强制验证 `SetUserSN → FindEx → SetApiKey → 尝试预算 → UserLogin → GetLevel → Logoff → Uninitialization` 顺序。
- 尚未使用实际开发凭据调用官方 SDK 或实体卡。本切片只证明受控 IPC、真实装载/函数解析代码和会话生命周期可由无害 fixture 贯通，不能记作实体卡认证通过。下一小切片需在明确使用测试 PIN 的前提下建立一次实体开发卡会话，并在登录前后受控复核 PIN 尝试首值确为剩余预算；任何错误尝试都必须停止，不能自动重试。
- 提交：`ac41e8e [P1.6]wire development credentials into ViKey helper session`

### 小切片 11：实体开发卡会话与 PIN 尝试预算复核

- 原文：第 1741～1794 行。
- 重新核对 P1.6 真卡出口、R-03、开发联网兼容边界、小切片 9 的会话生命周期和小切片 10 的固定长度凭据 IPC。本切片只建立并关闭一次实体开发卡用户会话，读取登录前后 PIN 尝试预算； 不实现或调用 blob/计数器接口，不注入生产材料，不改变档位状态。
- 官方管理工具和 SDK 资料确认当前开发卡 UserSN 为 32 个十六进制字符、用户 PIN 为 8 个字母或数字。开发阶段决定把 UserSN、API Key 和用户 PIN 保存在当前主机用户的 Windows 凭据管理器；项目仓库、Git 历史、开发日志、helper 环境和运行探针命令行均不保存凭据值。 对应目标为 `usb-vault-vikey-dev-user-sn`、`usb-vault-vikey-dev-api-key` 和 `usb-vault-vikey-dev-user-pin`。
- MSVC Debug 全量构建通过；受限沙箱内 CTest 为 19 项执行通过、2 项跨进程测试按既有父令牌约束跳过，普通桌面令牌下三项 helper 测试为 **3/3 通过**。MSVC `/analyze` 聚焦构建完成且会话/协议测试 **2/2 通过**；AddressSanitizer 下同两项测试 **2/2 通过**，未报告内存错误。
- 官方 x64 `ViKey.dll` SHA-256 再次核对为 `735A2347F66F10C649C9B38ED0232B839812277C8E178F0C5084C676CF1DE48E`。在普通用户、非提权、 允许联网的开发 helper 中使用凭据管理器内开发凭据建立实体 ViKeyPRO 用户会话成功；登录前和登录后均返回 **200/200**，确认首值表示剩余尝试预算，成功登录不消耗预算，随后注销和反初始化成功。测试未读取或写入用户区、管理员区、blob 或计数器。
- 提交：`4774f7c [P1.6]verify real ViKey development session`

### 小切片 12：ViKey 固定 32 字节用户区原语

- 原文：第 1795～1844 行。
- 重新核对 P1.6 真卡出口、R-03、N-02 的 `ICardStore` 单 blob get/put 边界及开发联网兼容决策。本切片只在已认证的 ViKey 私有会话内实现固定用户区读写原语，不修改公共 `ICardStore`，不增加档位状态、计数器或真实卡事务。
- `VikeySession` 新增固定 32 字节、地址 0 的 `read_user_blob` 与 `write_user_blob`。每次操作先要求会话仍处于认证状态，再查询用户区容量；容量不足时在读写前返回 `invalid_configuration`，SDK 查询和数据操作错误继续映射到通用 `CardError`。
- MSVC Debug 全量构建通过。受限沙箱内 CTest 为 19 项执行通过、2 项按既有父令牌约束跳过； 普通桌面令牌下补跑三项 helper 测试为 **3/3 通过**。MSVC `/analyze` 聚焦构建与 `vikey_session_tests` 通过且无新增项目诊断；AddressSanitizer 下同一测试通过，未报告内存错误。
- 当前原语只存在于认证会话内部，尚未接入 helper 单次事务或公共 `ICardStore`，因此不能记作 P1.6 的实体 blob 写入/读回出口完成。下一小切片应先以无害 fixture 实现并验证 helper 事务： 备份原 32 字节、写入非生产测试值、读回比对、恢复原值、再次读回确认恢复。
- 提交：`aea344d [P1.6]add fixed ViKey user blob operations`

### 小切片 13：helper 可恢复 blob 往返事务

- 原文：第 1845～1905 行。
- 重新核对 P1.6 真卡出口、R-03、N-02 单 blob 边界、小切片 12 的固定用户区原语及开发联网约束。本切片只建立开发 helper 内的单次可恢复事务和无害夹具验证，不调用正式 SDK、不读取 Windows 凭据管理器、不连接实体卡，也不修改公共 `ICardStore` 或档位状态。
- 新增开发专用 `verify_development_blob_round_trip`：先把原 32 字节读入锁页 `SecureBuffer`，再写入测试值并读回验证，随后无条件写回原值并再次读回验证。测试失败在恢复成功后重新抛出原通用错误；恢复无法验证时提升为 `device_failure`。三次读回、测试写入参数和原值写回参数均由锁页缓冲承载并在作用域退出时清零，内容比较遍历完整固定缓冲。
- MSVC Debug 全量构建通过；受限沙箱内 CTest 为 19 项执行通过、2 项按既有父令牌约束跳过， 普通桌面令牌下三项 helper 测试为 **3/3 通过**。MSVC `/analyze` 聚焦构建完成且会话/协议测试 **2/2 通过**，无新增项目诊断；AddressSanitizer 下同两项测试 **2/2 通过**，未报告内存错误。
- 当前只证明伪 SDK 失败路径和无害 DLL 跨进程链，尚未调用官方 DLL 执行实体用户区往返，不能记作 P1.6 真卡 blob 出口完成。下一小切片应新增默认构建不包含的手工验收入口，从凭据管理器读取开发凭据、在锁页缓冲生成一次非生产测试值，并复用本事务；入口实现与测试完成后再单独决定是否对实体开发卡执行。
- 提交：`27b7abe [P1.6]add recoverable ViKey blob round trip`

### 小切片 14：实体 blob 往返手工验收入口

- 原文：第 1906～1956 行。
- 重新核对 P1.6 真卡出口、R-03、N-02 单 blob 边界、开发/生产运行决策及小切片 13 的可恢复 helper 事务。按既有评审结论，本切片只建立默认构建不包含的手工验收入口，不执行实体卡写入；真卡执行继续作为下一独立决策点。
- 复查既有开发会话 probe、Windows 凭据管理器读取、helper client v4 blob 操作及跨进程事务测试。三项开发凭据仍只从固定凭据目标读取，helper/DLL 路径和 SHA-256 仍由非秘密命令行参数提供；本切片不修改 IPC、helper、SDK 会话、公共 `ICardStore` 或卡上地址布局。
- MSVC Debug `/W4 /WX` 全量构建通过，CTest **22/22**：20 项执行通过，2 项 helper 跨进程测试因当前父令牌已受限按既有规则跳过。新增测试两次直接生成锁页随机 blob，验证固定 32 字节形状、有效分配及独立调用结果不相同；不读取凭据、不启动 helper、不访问设备。
- 手工入口已经具备执行条件，但尚未用官方 DLL、开发凭据和实体开发卡运行，因此 P1.6 的真卡 blob 写入/读回出口仍未完成。评审并提交本切片后，应单独决定是否在供电稳定、卡片固定且接受中断窗口风险的前提下执行一次实体往返。
- 提交：`866a6e4 [P1.6]add ViKey blob acceptance probe`

### 小切片 15：实体开发卡 blob 往返验收

- 原文：第 1957～2000 行。
- 小切片 14 已按批准创建本地提交 `866a6e4 [P1.6]add ViKey blob acceptance probe`。 本切片不修改产品代码，只按独立风险决策执行一次实体开发卡验收并记录结果。
- 执行前重新核对 P1.6 真卡出口、开发/生产运行边界、小切片 13 的恢复判据及小切片 14 的防误触入口。已明确理解并接受测试写入至恢复之间发生断电、拔卡或强制终止时可能无法恢复卡内原 32 字节的残余风险；本次目标仅为开发卡和非生产用户区，不承载真实根材料。
- 在普通用户、非提权、允许联网的开发 helper 边界中，从固定 Windows 凭据目标读取开发 UserSN、API Key 和测试 PIN；命令行只携带非秘密路径、摘要及显式风险确认参数。执行一次且仅一次实体 blob 往返，程序返回：测试写入和读回成功，原值写回成功，恢复后 32 字节逐字节验证成功；进程退出码为 0。
- P1.6 总出口尚未完成：公共 `ICardStore` 仍未接入 ViKey 适配，持久计数器尚未实测，生产出站隔离/安装 ACL/发布摘要/生产凭据管理仍是交付门。下一最小代码切片应先审计并设计正式 `ICardStore` get/put 与开发 helper IPC 的边界，避免把 ViKey 私有凭据、地址或错误域上移到公共接口；计数器继续作为之后的独立切片。
- 提交：`cac2d45 [P1.6]record real ViKey blob acceptance`

### 小切片 16：正式 ViKey `ICardStore` 适配边界

- 原文：第 2001～2047 行。
- 小切片 15 已按批准创建本地提交 `cac2d45 [P1.6]record real ViKey blob acceptance`。本切片重新核对 P1.6、 R-03、N-02、公共 `ICardStore`、P1.5 固定桩、产品装配及 helper IPC v4；范围仅为建立正式 ViKey 卡存储适配边界，不同时升级 IPC、读取凭据、加载官方 DLL、访问实体卡或替换产品桩。
- 新增伪后端单元测试，覆盖二进制数据原样返回、写入不改变调用方缓冲、能力位为空、31/33 字节写入在后端调用前拒绝、31/33 字节后端结果失败关闭，以及通用读取/写入错误不被适配器改写。测试伪后端不保存传入 blob。
- MSVC Debug `/W4 /WX` 全量测试 **23/23**：21 项执行通过，2 项 helper 跨进程测试按既有受限父进程令牌约束跳过。新测试在 MSVC `/analyze` 与 AddressSanitizer 下分别 **1/1** 通过，无新增项目诊断或内存错误。初次目标构建暴露库的公共头依赖未传播；将核心接口依赖改为 `PUBLIC` 后修复，随后三套配置全部通过。
- `git diff --check` 通过。装配复核确认 `tools/app/main.cpp` 仍实例化 `FixedBlobCardStore`，新库仅由新单测链接；现有产品可执行文件导入表没有 ViKey 项。本切片未读取 Windows 凭据管理器、 未加载官方 SDK、未启动 helper、未连接或改写实体卡。
- 提交：`bfc15e2 [P1.6]add formal ViKey card-store adapter`

### 小切片 17：helper IPC v5 正式 blob 后端

- 原文：第 2048～2103 行。
- 在 `bfc15e2` 基础上重新核对 P1.6、R-03、N-02、IPC v4、正式 `VikeyCardStore` 适配边界、helper 单请求退出模型和现有开发凭据读取代码。本切片只把固定 32 字节 get/put 接到可注入的正式后端；不替换产品固定桩、不新增生产凭据实现、不加载官方 SDK、不读取凭据，也不连接或改写实体卡。
- helper IPC 由版本 4 升至版本 5，新增独立的开发 blob read/write 操作。请求继续携带精确 DLL 摘要和固定长度锁页凭据；write 另携带精确 32 字节锁页 blob。响应头新增显式 `payload_bytes`，只有 read 成功允许声明并紧随 32 字节 blob，错误响应和其他成功响应必须为零载荷；协议层拒绝声明载荷却走普通无载荷响应函数的情况。
- 协议测试覆盖 v5 操作与精确载荷、read/write 请求往返、调用方缓冲保持、锁页 read 响应、 缺失/错误长度/错误状态携带载荷的失败关闭。跨进程 fixture 覆盖正式 `VikeyCardStore -> VikeyHelperBlobBackend -> helper` 的 get、put、模拟已部分写入后报错、每次重新加载凭据、凭据源失败不重试且不暂存，以及暂存目录清理。
- MSVC Debug 全量 CTest **23/23**：21 项执行通过，2 项按受限沙箱父令牌约束跳过；普通桌面 helper 测试 **3/3 通过**。首次普通桌面补跑时，网络夹具在完整响应前以诊断码退出；增强非秘密分阶段诊断并显式重建两种网络夹具后，同一测试和随后完整三项均通过，未能复现。
- 提交：`07a1e28 [P1.6]add helper-backed ViKey blob operations`

### 小切片 18：开发凭据提供者与正式后端只读入口

- 原文：第 2104～2157 行。
- 在 `07a1e28` 基础上重新核对 P1.6、R-03、N-02、开发/生产运行边界、既有 Windows 凭据读取代码及三个手工 probe。本切片只提供开发专用凭据来源和 IPC v5 正式后端的实体卡只读入口；不替换产品固定桩、不定义生产凭据部署方案，也不调用 `put_blob`。
- 新增默认构建排除的 `usb_vault_vikey_development_credentials` 组件及 `WindowsCredentialProvider`，只读取当前用户下三个固定开发目标。每次加载建立新的 `VikeyCredentialSet`；`CredReadW` 返回的原始 blob 在复制到锁页 `SecureBuffer` 后先用 `SecureZeroMemory` 擦除，再交给 `CredFree` 释放。异常发生在任意一项时，已构造的锁页凭据随栈展开自动清零。
- Debug 全量 CTest **24/24**：22 项执行通过，2 项按受限父令牌约束跳过；普通桌面 helper 测试 **3/3 通过**。MSVC `/analyze` 与 AddressSanitizer 下三个聚焦测试分别 **3/3 通过**， 无新增项目诊断或内存错误。只读 probe 无参数运行在读取路径、摘要、凭据或设备前以用法错误退出，退出码为 1。
- 官方 x64 DLL 仍匹配既有 SHA-256 白名单 `735A2347F66F10C649C9B38ED0232B839812277C8E178F0C5084C676CF1DE48E`；当前 Debug v5 helper SHA-256 为 `C270D3E25CF8D62974D6832D4889F7A9065A26584570AE251749CE2CAA0C2E25`。安全审批首次拒绝在隐含授权下读取三项开发凭据；随后明确授权把这些凭据交给本机具普通网络权限的开发 helper 和官方 SDK，用于一次实体开发卡只读 `get_blob`，未扩大为写入或生产材料授权。
- 提交：`714e371 [P1.6]add development ViKey credential provider`

### 小切片 19：显式启用的真卡开发版应用装配

- 原文：第 2158～2210 行。
- 在 `714e371` 基础上重新核对 P1.6、R-03、8.6 及开发/生产运行边界。默认产品入口仍硬编码 `FixedBlobCardStore`，此前的正式 ViKey 适配器、helper 后端和开发凭据提供者均尚未进入产品应用；本切片只增加一个可单独构建、显式确认联网风险的开发版 EXE，不替换默认产品，也不把 Windows 凭据管理器确定为正式产品凭据方案。
- 新增默认构建排除的 `usb_vault_vikey_development_app`。它复用正式 UI 和 `ApplicationSession`，但把 `ICardStore` 装配为 `VikeyCardStore -> VikeyHelperBlobBackend -> WindowsCredentialProvider`；默认 `usb_vault_app` 继续装配 `FixedBlobCardStore`，没有改变既有演示/离线路径。
- Ninja 默认 `all` 目标不包含开发应用。二进制导入表复核确认默认 EXE 不导入 `CredReadW` 或 `ViKey.dll`；开发 EXE 按设计导入 `CredReadW`，但不静态导入 `ViKey.dll`，官方 SDK 仍只在独立 helper 内经摘要验证后动态加载。代码与测试未发现凭据或 blob 进入参数、日志、异常文本、 普通可重分配字节容器或持久文件。
- 真实应用闭环尚未验收：开发 EXE 尚未在连接的开发卡上完成“启动界面 -> 输入密码 -> 读取卡 blob -> 挂载/失败提示 -> 正常退出”的手工流程，WebView2、建卷工具、容器状态和卡内现有 blob 的兼容性仍可能暴露集成问题。下一最小切片应先生成或记录当前制品摘要，按用户对凭据和实体卡的明确授权启动一次开发 EXE，执行只读应用级验收；不得在该验收中初始化或覆写卡 blob。
- 提交：`9195800 [P1.6]add opt-in ViKey development app`

### 小切片 20：真卡开发版应用只读界面冒烟

- 原文：第 2211～2261 行。
- 在 `9195800` 后重新复核开发应用调用顺序：用户提交非空输入后， `ApplicationSession` 先调用 `ICardStore::get_blob`，随后才检查并挂载容器；该路径没有 `put_blob`、初始化卡或其他卡写入调用。开发 EXE 目录当前只有页面资源，没有 `container_a.hc` 或 `container_b.hc`，因此即使读卡成功也预期在后续“无可用容器”处失败。
- 当前开发 EXE SHA-256 为 `C210DB44BBBD628ED0914D47A2FE5437EC7AD5B620E86F322C6AB19FC6F4D735`，helper SHA-256 为 `EFC38B2C5D44466C0C5EC170BC0E7726BA212FC5F3EA8C598AC5D4E7AC64E034`；仓库外官方 x64 `ViKey.dll` 仍匹配白名单 `735A2347F66F10C649C9B38ED0232B839812277C8E178F0C5084C676CF1DE48E`。
- P1.6 的正式后端实体 `get_blob` 已由小切片 18 的独立 probe 证明，实体 blob 写入/读回/恢复也已由小切片 15 证明，因此本次 UI 可观察性缺口不推翻既有硬件证据。若要把应用级读卡也做成独立验收，应另加仅开发版可用、不会泄露 blob/凭据的阶段结果；完整容器挂载仍属于后续带匹配测试容器的集成验收/P1.7，不应通过临时复制不匹配容器冒充成功。
- 复核后明确指出最终应用必须允许首次使用者建立容器。当前开发 EXE 只复用了 P1.5 的 “挂载已有容器”最小 UI，不能从零初始化产品；P1.4 虽已有独立建卷工具，但主界面尚未接入 “无容器 -> 建卷向导 -> 分别创建 A/B -> 返回登录”的 P2 流程。因此它只能称为真卡装配开发预览，不能称为可供新用户完整试用的 EXE。该范围限定不改变本次只读冒烟结论，也不得以手工复制容器代替正式建卷向导验收。
- 提交：`f2480df [P1.6]record ViKey development app smoke test`

### 小切片 21：P1.6 完结与计数器暂缓决策

- 原文：第 2262～2296 行。
- 重新核对 `需求梳理清单_v2.md` 的单 blob 合约、P1.6 开发/生产边界、N-07 和 `MILESTONES.md` 的 P1 子阶段出口。P1.6 的必需出口已有独立证据：正式 `ICardStore` 适配只暴露 get/put/能力位；IPC v5 和 helper 后端以锁页缓冲传递精确 32 字节 blob 与开发凭据；真实开发卡完成 get、受控测试写入、读回、原值恢复及恢复后逐字节验证；ViKey 私有类型、凭据、 地址和错误域没有越过适配边界；默认产品构建仍不包含联网开发实现。
- 开发版应用已完成制品参数、真实 WebView2 和提交/清理冒烟，但它只挂载已有容器，不包含 P2 的首次建卷向导；该限制不属于 P1.6 真卡存储出口，也不得把开发预览描述为完整用户试用版。
- 复核 N-07 机制 2 后决定暂缓卡上单调计数器，不继续进行 ABI 实现、实体初始化或消耗测试。 该计数器只能证明中间发生过其他挂载，无法区分用户自行测试胁迫密码、维护模式或真实胁迫， 因而告警解释力有限；对手绕过本软件直接使用卡材料时又不会触发计数。ViKey 的有限递减计数器还会引入初始化、耗尽、回绕比较、断电同步和不可恢复实体变化风险。
- N-07 机制 1“隐藏卷完整性校验”继续保留在 P2，可独立判断数据是否真正受损。计数器只作为未来可重新评估的增强项，不计入当前工期，不再是 P1.6、P1.7、P2 或量产卡选型的阻塞门。
- 提交：`124933a [docs]close P1.6 real card storage`

### P1.7 插拔监听与真机闭环（2026-08-27 开始）

### 小切片 1：厂商无关的卡存在状态与移除卸载核心

- 原文：第 2299～2342 行。
- 重新核对 `需求梳理清单_v2.md`、`MILESTONES.md` 和 R-03/8.6。P1.7 的必需范围是插卡发现、 拔卡触发卸载、忙碌/异常反馈和真卡最小闭环；已取消的自动登录不得因插卡监听重新引入， 也不得在此阶段顺带加入 P2 建卷向导或暂缓的计数器。
- 代码检索确认正式应用尚无 `WM_DEVICECHANGE`、`RegisterDeviceNotification` 或 `DBT_*` 处理。 现有 `ApplicationSession::unmount_active()` 已按 B 后 A 的顺序卸载，并会在失败时保留仍挂载状态以供重试；`prepare_ui_session_for_close()` 也已复用同一卸载边界。因此本切片先固定不依赖 Win32 或 ViKey VID/PID 的状态/卸载语义，Windows 设备通知装配留给下一独立切片。
- 新增测试覆盖：到卡不认证、重复到卡幂等、无会话离卡、有会话离卡只卸载一次、卸载异常保持可重试会话、非抛异常但仍挂载时失败关闭。MSVC Debug `/W4 /WX` 聚焦测试通过；Debug 全量 CTest **26/26**：24 项执行通过，2 项按既有受限父进程令牌约束跳过。
- MSVC `/analyze` 与 AddressSanitizer 下聚焦测试分别 **1/1 通过**，无新增项目诊断或内存错误； `git diff --check` 通过。首次直接构建未加载 Visual Studio 开发环境，因标准库路径缺失而失败； 经既有 `VsDevCmd.bat` 环境入口重跑后正常通过，确认不是源码错误。
- 提交：`90a1c45 [P1.7]add card removal session controller`

### 小切片 2：Windows HID 设备通知适配与开发版接线

- 原文：第 2343～2398 行。
- 在 `90a1c45` 基础上重新核对 P1.7、基本需求第 3 条、R-03/8.6 和既有窗口生命周期。正式应用尚未处理 `WM_DEVICECHANGE`，但 P1.6 的 HID 兼容性证据已确认当前开发卡是 HID 类接口，硬件身份为 `VID_1987/PID_0331`，打开时还可核验 Usage `FF00/0001`。移除完成后设备已不可打开， 因此离卡通知只能在 HID 类 GUID 之外按设备接口路径中的精确 VID/PID 令牌筛选。
- 新增测试覆盖大小写无关的精确路径、错误 VID/PID、嵌入子串、超长 PID token、目标到达不认证、 目标移除卸载，以及错误 class GUID、过小/奇数/超限/无 NUL 结构和无关事件全部忽略。首次测试编译因测试代码使用未由当前头文件集声明的 `GUID_NULL` 失败，改为显式零 `GUID{}` 后在严格 `/W4 /WX` 下通过，不涉及生产逻辑降级。
- Debug 全量 CTest **27/27**：25 项执行通过，2 项按既有受限父进程令牌约束跳过；开发版 EXE 在普通 Debug、MSVC `/analyze` 和 AddressSanitizer 三套 UI 构建中均成功链接。聚焦通知测试在 Debug、`/analyze` 和 AddressSanitizer 下分别 **1/1 通过**，无新增项目诊断或内存错误。
- 当前真实 Windows 注册、启动枚举和物理插拔事件尚未做实体界面验收；合成消息只能证明解析和路由语义。设备移除成功后底层会话已关闭，但 WebView 页面仍会保持上一次按钮/文案，直到用户下一次操作触发状态查询；忙碌失败已有原生提示，但页面状态同步仍是下一切片的必需工作。
- 提交：`968cddc [P1.7]wire Windows HID card notifications`

### 小切片 3：拔卡后 WebView 会话状态刷新

- 原文：第 2399～2448 行。
- 在 `968cddc` 基础上重新核对 P1.7、R-03/8.6、页面契约和 WebView2 安全设置。 拔卡控制器已能关闭底层会话，但页面只在初始化、提交密码或手工卸载后查询 `hasActiveSession()`，因此外部移除后会留下过期按钮和文案。本切片仅修复这个窗口到页面的状态同步边界，不引入自动登录、设备详情、模式分类或 P2 建卷。
- MSVC Debug 全量 CTest **27/27**：25 项执行通过，2 项按既有受限父进程令牌约束跳过。 Debug 真实 WebView2 Runtime 闭环退出 0，覆盖“会话仍在”和“外部卸载后已关闭” 两条路径。首次在受限沙箱内直接启动隐藏 GUI 时 15 秒超时；使用允许桌面组件的普通本机环境复跑后通过，确认为 GUI/WebView2 沙箱约束，不是产品逻辑回归。
- 开发应用、页面契约和真实 WebView2 闭环目标在 MSVC `/analyze` 和 AddressSanitizer 下均成功构建，无新增项目诊断。页面契约在两种配置下均 **1/1 通过**；ASan 插桩的真实 WebView2 闭环退出 0，未报告内存错误。`git diff --check` 通过。
- 代码层闭环已完成，但尚未使用真实开发卡做“应用启动时卡已插入 / 运行中插卡 / 活动挂载中拔卡”验收。真实卸载闭环需要与当前卡 blob 匹配的测试容器；在匹配容器就绪前， 只能证明设备通知和无活动会话路径，不能把“密码后挂载失败”冒充为拔卡卸载验收。
- 提交：`528fd63 [P1.7]refresh UI after external unmount`

### 小切片 4：P1.7 真卡验收夹具（可丢弃匹配容器）

- 原文：第 2449～2520 行。
- 在 `528fd63` 基础上复核 P1.7 出口条件与阻塞项。代码层的插卡发现、拔卡卸载和页面刷新已齐备，但真卡闭环缺少可挂载对象：开发版 EXE 只认自己目录旁的 `container_a.hc` 与 `container_b.hc`，两者当前都不存在；仓库内 P1.3 遗留容器由合成卡材料建成，卷密码与真卡 blob 的派生结果不一致，不得复制冒充。
- 破坏性保证同样落在库内而不是可执行文件内：`require_absent` 对既有文件、目录以及无法分类的目标一律拒绝，绝不打开或覆盖既有容器；`require_regular_file` 校验三个必需制品； `FixtureCleanup` 在**调用建卷之前**登记暂存路径所有权，因此建卷写到一半再抛异常时暂存文件仍会被删除。发布采用「先建暂存名、两只都成功后再改名为正式名」，`publish_b` 后若 `publish_a` 失败，已发布的容器 B 会被回滚，不留半发布布局。
- 变异验证确认新测试确有鉴别力：删除「建卷前登记所有权」后报 `a partially written container survived the failure`；删除 blob 尺寸门后报 `fixture changed card blob size`；删除发布回滚的所有权转移后报 `a half-published layout survived the failure`；三者恢复后全部通过。
- MSVC Debug `/W4 /WX` 全量 CTest **28/28 通过，0 失败**（本次环境下此前受限父令牌跳过的 2 项也已执行通过）。`/analyze` 与 AddressSanitizer 两套配置下，夹具库、夹具 EXE、默认应用与开发版应用均构建成功且无新增诊断，夹具测试在两套配置下分别 **1/1 通过**。 首次测试编译因自定义 `exists` 与 `std::filesystem::exists` 经 ADL 产生重载歧义而失败，改名为 `path_exists` 后通过，不涉及产品逻辑降级。
- 提交：`3deed4c [P1.7]add disposable real-card container fixture`

### 小切片 5：真卡验收前的 UAC 创建器管道访问修复

- 原文：第 2521～2565 行。
- 在启动真卡验收前复核 P1.4 创建器权限边界、P1.7 夹具调用链及昨日失败现场。普通权限的夹具通过 `runas` 启动提权创建器，再连接由创建器建立的单实例、拒绝远程命名管道；问题发生在 UAC 之后：提权令牌的默认对象属主是 Administrators，而原 DACL 使用 `OWNER RIGHTS`，因此获得访问权的是提权对象属主，不是发起提权的普通权限 Windows 用户，客户端会在连接管道时被拒绝。
- MSVC `/analyze` 下创建器协议测试、提权创建器和 P1.7 夹具均构建成功，无新增项目诊断， 聚焦测试通过；AddressSanitizer 下相同三个目标构建成功，聚焦测试通过且未报告内存错误。
- `git diff --check` 通过。安全复核确认没有新增可被低权限其他用户访问的主体，没有改变密码传递、客户端模拟、目标文件操作或卡访问边界；故障仍为失败关闭。
- 下一步在本修复获准提交后运行已提交的 P1.7 夹具，按既有授权读取开发凭据并对开发卡执行一次只读 `get_blob`，接受两次 UAC 后生成约 120MB 可丢弃容器。随后分别输入准确测试口令 `p1.7-real-test` 与 `p1.7-duress-test`，验证两者都挂出两个盘，并配合物理拔卡验证自动卸载、 页面刷新及盘符/驱动无残留。
- 提交：`8039f09 [P1.7]fix elevated creator pipe access`

### 小切片 6：P1.7 真卡双路径挂载验收与 WebView2 数组封送修复

- 原文：第 2566～2615 行。
- 用户在普通 PowerShell 中运行一次性容器夹具，两次 UAC 创建流程均成功，生成 20 MiB 的 `container_a.hc` 与 100 MiB 的 `container_b.hc`；这证明小切片 5 的显式用户 SID 管道 DACL 已在真实「普通权限客户端—提权创建器」边界生效。
- 页面仍先用 `TextEncoder` 生成 UTF-8 typed array，但立即复制到普通可变数字数组；随后清空 DOM 并擦除 typed array，再把普通数组交给异步 host object。调用结算后立即擦除普通数组，外层 `finally` 为两个数组保留兜底清零。原生侧继续只接受既有窄化的一维 `SAFEARRAY(UI1)` / `SAFEARRAY(VARIANT/I4)`，不接受字符串或 JSON。
- 真实 Runtime 集成覆盖扩展为两次提交：先验证与真卡验收相同的 14 字节公开测试口令，再验证原有含 NUL 和多字节 UTF-8 的 4 字节负载；两次都必须逐字节匹配、进入统一活动状态并完成主动/外部卸载闭环。
- 安全复核确认新增普通 JavaScript 数组是异步 COM 封送所需的短期可控副本，typed array 在复制后立即清零，普通数组在调用结算后立即清零；没有字符串化、日志、持久化、档位实体或分类反馈。浏览器/WebView2 内部仍可能产生应用无法主动擦除的短期副本，此风险与既有宿主对象边界相同。此次真卡流程只读 `get_blob`，未执行 `put_blob`，也未改写卡内容。
- 提交：`7c486b1 [P1.7]verify real-card mount and fix password array transport`

### 小切片 7：拔卡强制卸载与 MSVC/Ninja 头文件依赖修复

- 原文：第 2616～2708 行。
- 重新核对 P1.7 的出口条件、基本需求第 3 条、R-03/8.6、既有普通卸载重试、窗口关闭守卫和手工安全卸载路径。物理拔卡后卡材料已不可再信任，若卷因资源管理器等进程占用而拒绝普通卸载，应优先消除仍公开可见的解密盘符；但用户主动卸载与关窗仍须保持非强制，避免把普通 busy 状态转换成可能丢失未写入数据的破坏性操作。
- 审阅未提交实现时发现构建证据污染：`dev`、`analyze`、`asan`、`integration`、 `review-clean`、`asan-debug-diagnostic`、`release-diagnostic`、 `release-helper-diagnostic`、`vikey-release-diagnostic` 共 9 个 Ninja 目录的 `msvc_deps_prefix` 被写成乱码，`application_session.cpp.obj` 的 Ninja 依赖数为 0；正常的 `p1_7_acceptance` 同一目标有 128 条依赖。
- `p1_7_acceptance` 已在 UTF-8 配置保护下重新生成，核心库、挂载诊断及 CTest **28/28** 均已刷新通过；但旧的 `usb_vault_vikey_development_app.exe` 仍被既有运行进程占用，链接器无法覆盖该单一制品。本轮未擅自终止用户进程；相同开发应用目标已在 Analyze 与 ASan 目录从零构建成功。随后关闭旧窗口，验收目录中的开发版应用已重新链接成功，现可执行真卡测试。
- 软件无法保证强制卸载时尚未写入磁盘的数据不丢失；这是物理拔卡后消除持续暴露盘符与数据完整性之间的明确取舍，因此 UI 必须保留当前警告，正常卸载路径不得复用此策略。
- 提交：`b0198d7 [build]fix MSVC Ninja header dependencies`； `d852f87 [P1.7]force unmount busy volumes after card removal`。

### P2：功能全量

### 小切片 0：取消注册后的卡初始化与双信封认证需求基线

- 原文：第 2711～2767 行。
- 2026-08-28 明确 P2 新入口：不再需要注册；产品未初始化卡首次插入时要求设置日常登录密码，已初始化卡直接进入登录；只有创建带隐藏卷的隐藏盘时才产生诱饵密码，此后两个密码均可从同一入口登录；首次建卷向导暂缓，后续单独补齐。
- 检查现有 `ICardStore`、`FixedBlobCardStore`、ViKey 会话、`kdf_v1`、`ApplicationSession`、 `mount_orchestrator`、`UiSessionController` 和 WebView 页面。当前实现把固定 32 字节 blob 直接当 S，页面只有登录/卸载两态；没有产品初始化标记、记录版本、密码认证信封、已认证无卷态或损坏记录分类。现有 `mount_orchestrator` 仍按“B 存在则 B 用输入、A 用固定域；只有 A 则 A 用输入”分支，不能直接承载新协议。
- `需求梳理清单_v2.md` 将 2026-08-28 决策声明为优先于旧冲突表述：取消注册，区分厂商设备激活与产品初始化，允许初始化后无卷登录，首次建卷向导不自动启动且暂缓实现。
- `git diff --check` 通过；Markdown 表格、标题与新增代码块由文本检查确认无尾随空白。本切片不运行 C++ 测试：没有产品代码变化，现有测试也尚无双信封协议可执行；下一实现切片必须先以纯内存 codec/state-machine 单元测试覆盖，再接 `ICardStore` 与 UI。
- 安全复核确认新基线不在本地或卡上保存档位/模式/诱饵启用位，主/第二信封对只持诱饵密码者保持“有效主记录 + 不可判定随机第二记录”的否认性；认证先于挂载，错误密码不会短暂挂出 A； 损坏记录失败关闭，避免误初始化覆盖。双信封切换的原子提交仍未设计，因此明确禁止在首次建卷向导暂缓期间先落地半套切换。

### 小切片 1：Card Record v1 固定格式与纯内存双信封 codec

- 原文：第 2768～2833 行。
- P2.0 需求基线已提交为 `2efe7cb [docs]rebaseline P2 card initialization and login`。本切片遵守用户“首次建卷向导暂缓”的顺序，只锁定持久格式与纯内存密码操作，不接 UI、`ICardStore`、 ViKey helper、实体卡写入或建卷事务。
- 新增公开 `card_record_v1` 纯内存 API，记录固定 **216 字节**：64 字节 little-endian 公开头， 后接两个各 76 字节的信封。头含 magic/version/length、16 字节 Argon2 盐、16 字节 record id、 固定 m=512MiB/t=2/p=1 与 Argon2 v1.3 参数；信封均为 12 字节 nonce、48 字节 ciphertext、 16 字节 GCM tag，布局不存在档位差异。
- Debug 全量构建通过，CTest **29/29**：27 项通过，2 项按受限父令牌约束跳过；新测试约 44.5s， 因为真实执行多条 512MiB KDF 路径。原 `kdf_v1_tests` 约 3.6s 并保持全部向量通过，证明共享运行器重构没有改变卷密码派生。
- 安全复核确认：S、Argon master、HMAC key、AES key object 与解密明文均在 `SecureBuffer` 中； CNG/Argon 失败路径由 RAII 清理；头与槽位均认证；主/第二信封等长且均呈随机外观；没有明文密码、命中槽位、诱饵启用、档位或模式持久字段。密码长度差异不是秘密；相同密码比较不提前接触卡记录。代码与测试均未读取 Windows 凭据、调用 ViKey SDK、写实体卡或修改容器。
- 提交：`300e3be [P2.1]add deniable card record codec`。

### 小切片 2：Card Record Journal v1 可恢复提交协议核心

- 原文：第 2834～2901 行。
- 重新核对 N-02/N-06、Card Record v1 固定 216 字节格式、R-03/8.6 与 P2.1 遗留边界。卡记录更新既用于首次初始化，也用于以后双信封改封；两者必须走完全相同的物理协议，不能额外写入诱饵启用、档位、模式或操作类型。
- 描述符含固定 magic/version/length、随机 16 字节 commit id、随机 16 字节 previous id、槽号、 记录长度、记录 SHA-256 与描述符 SHA-256。读取固定获取两描述符和两槽；稳定态要求两描述符相同，中断态只接受新 `previous id` 唯一指向旧 `commit id` 的有向关系。描述符及摘要只承担事务恢复，不包含密码语义，也不替代 Card Record v1 的 GCM 认证。
- 新增伪随机访问存储测试，覆盖空白物理区失败关闭、受控 provisioning、连续两次槽切换、暂存部分写后报错、完整写后报错并由回读确认、暂存回读不一致、提交描述符部分写、完整描述符写后报错、旧槽清理撕裂、镜像描述符撕裂、随机初始槽、稳定态旧 marker 消除、随机提交 ID 链、 单描述符损坏、活动槽损坏禁止回滚、双描述符损坏、容量不足和 malformed 输入写前拒绝。新测试在 Debug 下独立构建并通过。
- 最终随机提交链版本的 Debug 全量构建和 CTest **30/30**：28 项通过，2 项按受限父令牌约束跳过； 新日志测试约 0.05 秒。MSVC `/analyze` 全量构建无项目诊断，CTest **30/30**（28 通过、2 跳过）。 ASan 全量构建和 CTest **30/30**（28 通过、2 跳过），未报告内存错误。
- 提交：`035d8cb [P2.2]add deniable recoverable card record journal`。

### 小切片 3a：闲置槽可验证清理与 ViKey 会话随机访问接入

- 原文：第 2902～2950 行。
- 进入 ViKey 适配前重新审计 P2.2 的每个断电点，确认“暂存新记录后、发布新描述符前”断电时， 旧描述符和旧活动记录确实仍然有效，原子性没有问题；但原描述符只摘要活动槽，重新插卡无法知道闲置槽已由随机填充变为部分或完整的新记录，因而可能留下可识别的 `UVCRD2` 头和更新历史痕迹。 这不是数据恢复缺陷，而是 R-03/8.6 下的可否认性与历史清理缺口。
- 审计确认 `VikeySession::read_user_blob/write_user_blob`、P1 开发往返探针和 helper IPC v5 仍固定 32 字节。为保持既有实体卡恢复证据，本切片不改这些路径；新增独立的 `vikey_card_record_io` 适配层，由私有 `VikeyCardRecordStorage` 实现 `ICardRecordJournalStorage`，通过会话封装的厂商 `GetUserDataSize/ReadData/WriteData` 执行 736 字节范围内的随机访问。这样 helper 继续独立编译旧会话代码，不被尚未迁移的 IPC 提前绑定到 Card Record 核心。
- 新增独立伪 SDK 测试支持任意合法地址/长度，并在真实日志布局上受控 provisioning。针对性测试已通过： ViKey 会话读取 marker、提交并读取完整 Card Record v1、厂商修改写参数不影响调用方，以及容量只有 735 字节时在任何厂商读取前拒绝。日志故障测试新增断言，证明部分暂存写失败后重新读取返回旧记录，同时再次随机覆盖暂存槽并镜像含新闲置摘要的描述符。
- 安全复核确认：正常读取不写卡；仅在描述符不一致或闲置摘要不匹配时归一化；活动记录摘要不符仍失败关闭而不回滚；日志与 ViKey 适配层不持久化密码种类、命中槽、档位、模式或更新计数。 SHA-256 仍只用于撕裂/偶发损坏检测，不声称抵抗可任意改写并重算摘要的攻击者。实体卡迁移和受控重新配卡仍未授权、未执行。
- 提交：`e89ca26 [P2.2]bind inactive journal slot integrity`、 `ceff863 [P2.3]add ViKey card record random access`。

### 小切片 4a：helper IPC v6 固定 216 字节卡记录消息

- 原文：第 2951～2981 行。
- 审计 `ICardStore`、`VikeyCardStore`、helper backend/client/main、IPC v5 和 P1 开发工具后确认， 现有 `development_blob_round_trip/read/write` 均明确绑定 32 字节，并承担实体开发卡原值恢复证据。 直接把它们改成 216 字节会破坏 P1 探针；此时直接把 `VikeyCardStore` 切为 216 字节，又会让尚未迁移的 P1 `ApplicationSession` 把 Card Record 错当成 32 字节 KDF 输入。
- Debug 针对性构建、helper 链接和 `vikey_helper_protocol_tests` 已通过。尚待全量 Debug、MSVC `/analyze`、ASan、CTest 和最终安全复核；未启动 helper、未加载厂商 DLL、未读取凭据或写卡。
- 最终 Debug、MSVC `/analyze`、ASan 均完成全量构建，CTest **31/31**：29 项通过、2 项按受限父令牌约束跳过；静态分析无项目诊断，ASan 无内存错误。`git diff --check` 通过（仅既有 LF→CRLF 提示）。
- 安全复核确认 helper main 与客户端尚未把新操作列入会话分派：helper 收到合法 v6 新操作时只会通过既有默认分支返回 `invalid_operation`，不会读取凭据体、加载厂商 DLL 或触卡；客户端也没有对外启动 API。旧开发操作仍严格要求 32 字节，新卡记录操作严格要求 216 字节，成功读响应尺寸不能互相冒充，失败响应仍禁止携带载荷或安全属性。
- 提交：`59b8399 [P2.4]add fixed card record IPC messages`。

### 小切片 4b：helper 卡记录执行与联网开发客户端

- 原文：第 2982～3026 行。
- 在 P2.4a 提交后继续审计 helper 的独立构建、客户端启动边界、ViKey 日志适配和跨进程伪 SDK。 helper 使用静态 CRT 并直接编译一组最小源文件，不能把整个 `usb_vault_core` 链入厂商进程；否则会同时引入 Argon2、512MiB KDF 工作区和产品密码解封代码，扩大供应商 DLL 所在进程的秘密面。
- 扩展伪 `ViKey.dll`，在不改变旧 32 字节严格状态机的前提下支持有效镜像日志、任意合法日志范围读写和 SHA-256 描述符；跨进程测试新增 marker 读取、完整初始化头写入、调用方记录不被厂商写参数修改、215 字节输入在暂存前拒绝，以及临时目录清理断言。正式 `ICardStore`、产品装配、UI、 Windows 开发凭据和实体卡均未切换或访问。
- 跨进程测试可执行文件及扩展伪 SDK 已在三套配置下以 `/W4 /WX` 编译链接。尝试在 Codex 工具的沙箱外执行时，先在既有“development vendor runtime staging”用例被路径身份门拒绝：Codex 包将 `LocalAppData` 重定向到 `LocalCache`，与此前开发日志记录一致；未放宽最终路径校验。 随后从普通、非管理员 PowerShell 直接运行 Debug `vikey_helper_process_tests`，完整输出 `vikey_helper_process_tests passed`，证明既有隔离/暂存/清理路径及新增 marker 读取、216 字节日志提交和短输入拒绝用例均在正常桌面令牌下通过。
- 当前提交状态：未提交，待审阅。下一独立切片才设计并切换正式 216 字节 `ICardStore` 后端；联网开发 helper 仍不得承载生产材料，实体开发卡迁移或重新配卡仍未授权、未执行。
- 提交：`35130b5 [P2.4]execute journaled card record IPC`。

### 小切片 5：正式 216 字节 ICardStore 适配与 helper 后端

- 原文：第 3027～3071 行。
- 重新审计 N-02 单一不透明 blob 合约、N-06 的 216 字节逻辑记录、现有 `ICardStore`、P1 `VikeyCardStore/VikeyHelperBlobBackend` 和 `ApplicationSession`。公共接口无需增加记录类型、状态或厂商概念；但旧 ViKey 适配器静态绑定 32 字节并被 P1 开发应用用于裸根秘密，不能原地改成 216 字节，否则当前应用会把完整 Card Record 当作 KDF 输入并破坏既有真卡恢复证据。
- 新增 `vikey_card_record_store_tests`，覆盖 216 字节二进制透传、调用方所有权、215/217 字节写前拒绝、215/217 字节后端读失败关闭、公共错误域保持和 capability 不虚报。旧 `vikey_card_store_tests`、开发配置测试及新适配器测试均针对性构建并通过。
- Debug、MSVC `/analyze`、ASan 三套全量构建均通过；三套 CTest 均为 **32/32**：30 项执行通过、 2 项按既有受限父进程令牌约束跳过。静态分析无项目诊断，ASan 未报告内存错误； `git diff --check` 通过（仅既有 LF→CRLF 提示）。跨进程测试在三套配置中均已重新编译，但因 Codex 父令牌受限而按设计跳过。随后从普通、非管理员 PowerShell 运行最新 Debug `vikey_helper_process_tests`，完整输出 `vikey_helper_process_tests passed`，证明正式 216 字节 `ICardStore` 组合链、旧 32 字节链、凭据生命周期、helper 隔离和暂存清理均在桌面令牌下通过。
- 当前产品 `usb_vault_app` 仍只链接旧 P1 32 字节桩/开发装配；本切片不改变登录、挂载或 UI，因而不会把 216 字节记录误传给 `derive_volume_password_v1`。下一切片应在产品层新增卡记录状态机： 先分类 provisioning/initialized/legacy/损坏，再实现初始化和登录，不能直接替换现有 `ApplicationSession` 的 32 字节读取。
- 提交：`8755a4a [P2.5]add helper-backed card record store`。未读取 Windows 开发凭据、未加载官方 SDK、未连接或改写实体卡；联网开发 helper 仍不得承载生产材料。

### 小切片 6：产品层卡初始化与统一认证状态机

- 原文：第 3072～3113 行。
- 重新审计 N-02、N-06、Q-07、R-03 和 8.6，并检查 Card Record v1 codec、正式 216 字节 `ICardStore` 与仍使用 P1 32 字节开发链的 `ApplicationSession`。本切片只建立可复用的产品层生命周期服务，不提前切换 UI、挂载或旧开发装配；口令强度、二次确认与风险确认属于后续 UI 接入切片，首次建卷向导继续暂缓。
- 初始化在慢 KDF 前后各读取并验证一次精确 provisioning 状态，避免用户计算密码期间卡记录已被另一流程改变仍遭覆盖；随后只调用一次 `put_blob`，无自动重试。无论写调用报告成功还是失败， 都重新读取逻辑记录并与预期 216 字节常量时间比较：只有完全一致才返回 S；写报错但复读完全一致视为已提交，无法复读或成功写后内容不一致统一按 `device_failure` 失败关闭；若写报错且旧记录仍在，则保留原卡错误供上层呈现。
- 新增真实 Argon2 参数的 `card_record_service_tests`，覆盖全部公开状态只读分类、初始化后直接得到与再次登录相同的 S、非 provisioning 状态零写入、未初始化状态禁止登录、写后报错但精确复读成功、KDF 期间状态变化不覆盖、提交前写失败保留旧 marker 与原错误，以及写后损坏/无法复读的失败关闭。codec 测试同时验证初始化返回的 S 与日常密码解封结果一致。
- Debug、MSVC `/analyze`、ASan 三套全量构建均通过；三套 CTest 均为 **33/33**：31 项执行通过、 2 项按既有受限父进程令牌约束跳过。静态分析无项目诊断，ASan 未报告内存错误。用户已在上一切片从普通 PowerShell 运行最新 Debug `vikey_helper_process_tests` 并得到 passed；本切片未改 helper 或跨进程路径，无需重复实体桌面令牌测试。
- 提交：`95c3b16 [P2.6]add card initialization and authentication service`。

### 小切片 7a：主 App 自动接纳新卡的需求与安全基线

- 原文：第 3114～3140 行。
- 决定不提供独立开发配卡 EXE。主 App 可在当前硬件适配器确认目标设备类型、厂商会话可用、 稳定实体身份且完整读取介质后，对没有 USB Vault 产品记录、P1/外来数据或损坏痕迹的卡直接显示日常密码设置页，不增加单独接纳确认页。插卡检测严格只读；密码提交前不得写 marker 或日志。
- 审计现有日志代码发现两个后续实现点：首次日志建立尚未暴露只读介质足迹分类，且现有 `provision_card_record_journal_v1` 可由调用方直接覆盖任意 736 字节介质；后续须增加“无产品足迹/可恢复记录/bootstrap 中断/损坏或外来”边界并限制写入口。初始化页面停留期间 helper 会话不保持，因此 ViKey 适配还须把 `VikeyGetHID` 的硬件 ID 纳入操作前后复验，不能只依赖 VID/PID 或“恰好只有一张卡”。
- 首次 bootstrap 仍有物理事实限制：任意非原子介质的第一笔写都可能在产品意图完整落盘前撕裂。 协议必须尽早写入可识别意图、对完整 marker/有效描述符中断自动恢复，并对无法证明来源的部分痕迹失败关闭；不得用本地进度文件补洞。该边界与日志内已有提交恢复分开测试。
- 当前切片先锁定技术真相与小切片边界，未修改代码、未运行厂商 DLL、未读取凭据或访问实体卡。 下一实现切片先做厂商无关的介质足迹检查和可恢复 bootstrap，再接 ViKey 硬件 ID/helper IPC， 最后接主 App 初始化 UI。
- 提交：`e8186a8 [docs]rebaseline automatic card enrollment`。

### 小切片 7b：厂商无关介质足迹与可恢复 bootstrap

- 原文：第 3141～3175 行。
- 在 736 字节日志私有边界新增只读 `CardRecordJournalMediaState`：`no_product_evidence` 只表示固定位置没有已知产品前缀，不自行授权覆盖；`provisioning_interrupted` 只接受槽内完整精确 marker 且尚无有效描述符；有效描述符、当前/未来记录前缀、撕裂描述符前缀以及不完整 marker 均归入 `product_evidence`。检查固定读取两份描述符和两个槽，不执行日志归一化或任何写入。
- 扩展日志故障测试，覆盖：空白介质只读无足迹、撕裂描述符/部分 marker 作为产品证据、完整 marker 中断可恢复、部分 marker 重试零写入失败、描述符中断后 bootstrap 恢复、有效 marker 幂等零写入、 直接重复 provisioning 拒绝、initialized/损坏 initialized/未来版本零覆盖，以及人工构造摘要完整的未来逻辑记录可原样上送 `unsupported_version`。
- Debug、MSVC `/analyze`、ASan 三套全量构建均通过；三套 CTest 均为 **33/33**：31 项执行通过、 2 项按既有受限父令牌约束跳过。静态分析无项目诊断，ASan 未报告内存错误；`git diff --check` 通过（仅既有 LF→CRLF 提示）。本切片没有读取开发凭据、加载厂商 DLL、启动 helper 或访问实体卡。
- 安全复核确认介质无产品足迹状态不包含厂商、设备、档位、模式、信封命中或诱饵启用信息，也不持久化到本地；它只是后续硬件适配器判定待接纳卡的一个输入。当前仍未解决检查与写入之间的换卡窗口，也未识别 ViKey P1/外来非空数据；这两项明确留给下一独立切片的稳定硬件 ID 和 ViKey 私有介质策略，主 App 尚不能据此自动接纳实体卡。
- 提交：`47e5d2e [P2.7]add recoverable card media bootstrap`。

### 小切片 8a：ViKey 会话稳定硬件身份绑定

- 原文：第 3176～3209 行。
- 重新审计自动接纳基线、ViKey SDK ABI、动态符号加载、会话生命周期及随机访问路径。ABI 与模块层已经声明并强制加载 `VikeyGetHID`，但会话此前只保存 SDK 索引 0；拔出原卡后插入另一张卡时， 同一 helper 进程内的后续用户区操作仍可能落到新卡，不能满足 P2.6 初始化防换卡要求。
- `VikeySession` 现将 `VikeyGetHID` 设为必需函数。确认恰好一张设备后、设置 API Key 前读取一次硬件 ID，登录并确认用户权限后再次读取；任一调用失败、返回零或两次值不一致均失败关闭，并按是否已经登录执行 `logoff + uninitialization` 或仅 `uninitialization`。
- Debug、MSVC `/analyze`、ASan 三套全量构建与 CTest 均通过；每套 **33/33**：31 项执行通过、 2 项按既有受限父令牌约束跳过。静态分析无项目诊断，ASan 未报告内存错误。由于本切片改变了 helper 内部会话调用序列，Codex 受限父令牌下直接执行按设计返回 skip；随后从普通、非管理员 PowerShell 运行最新 Debug `vikey_helper_process_tests`，完整输出 `vikey_helper_process_tests passed`，确认新增 `VikeyGetHID` 调用未破坏 helper 隔离、伪 SDK 会话、216 字节记录链及暂存清理。该测试使用伪 `ViKey.dll`，未访问实体卡。
- 仍未封闭“主 App 一次检查后，下一次 helper 进程打开的是另一张卡”的跨进程窗口。下一独立切片必须把期望硬件 ID 纳入 helper IPC 请求并由新会话比对，再实现 ViKey 私有的无产品痕迹/P1/外来介质分类；在此之前主 App 仍不得自动 bootstrap 实体卡。
- 提交：`9e50961 [P2.8]bind ViKey sessions to hardware identity`。

### 小切片 8b：helper IPC 跨进程硬件身份绑定

- 原文：第 3210～3252 行。
- 审计确认 P2.8a 只能封闭单个 `VikeySession` 内的换卡窗口：正式 backend 每次 get/put 都启动新 helper，新进程此前只知道“当前索引 0”，无法证明它仍是主进程先前检查的实体卡。仅给读写请求增加 ID 也不完整，因为主进程还缺少一个不触碰用户区的可信身份建立入口。
- `card_record_read/write` 的固定载荷各增加 4 字节非零期望硬件 ID。协议 writer、reader 和客户端启动前验证均拒绝零值；helper 登录后先与会话 ID 比对，错配映射为 `device_failure`，并在任何日志容量查询或随机访问前关闭。会话本身仍会在每个物理用户区操作前再次调用 `VikeyGetHID`。
- 协议测试覆盖 v7 身份操作、固定载荷尺寸、身份请求凭据往返、卡记录读写 ID 往返、零期望 ID 发送前拒绝，以及成功身份响应必须为非零。Debug 定向构建 helper、协议测试和跨进程测试目标通过，`vikey_helper_protocol_tests` 已执行通过。
- 跨进程测试扩展为“身份探针得到伪 SDK ID 1 → 绑定 ID 的正式 216 字节读写成功”，并覆盖错配 ID 返回 `device_failure`、零 ID backend 构造失败、凭据不重试和暂存目录无残留。Codex 父进程令牌受限，当前直接执行仍按设计 skip。用户首次从普通桌面令牌补跑时，测试在正式卡记录阶段返回 `invalid_operation`；定位确认新增身份探针在认证并读取硬件 ID 后直接登出，而伪 SDK 的严格状态机遗漏了这条不访问用户区的合法关闭路径。生产代码调用序列无误，测试替身现允许从 `level_read` 登出，避免为迎合替身而给生产路径加入无意义的额外 SDK 调用；修复后仍待桌面令牌复跑确认。
- 提交：`56bc3e9 [P2.8]bind helper requests to card identity`。

### 小切片 9a：ViKey 会话内只读介质分类

- 原文：第 3253～3289 行。
- 审计自动接纳基线、厂商无关日志足迹、ViKey 随机访问和 P1 固定 blob 后确认：P1 只在地址 0 保存无 magic 的 32 字节随机根秘密，因此“前 32 字节非零、其余空白”无法与同形外来数据可靠区分。两者都必须阻止自动覆盖，故适配器公开诚实的合并分类 `legacy_p1_or_foreign_prefix`，不向 UI 或业务层声称已经证明来源。
- 新增 ViKey 私有 `VikeyCardMediaState` 和严格只读 `inspect_vikey_card_media`。它先复用 736 字节日志证据检查：现有/损坏产品痕迹归 `product_evidence`；精确 provisioning 中断仅在日志区外仍全零时归 `provisioning_interrupted`，否则归 `conflicting_evidence` 并阻止恢复。
- `vikey_card_record_io_tests` 的伪用户区扩为与当前实体卡一致的 8192 字节，覆盖完整全零、P1 同形前缀、末字节外来数据、精确 provisioning 中断、日志外冲突数据、有效产品日志以及不足 736 字节容量；断言检查路径不执行任何 SDK 写调用，容量不足也不进入 SDK 数据读取。
- 安全复核确认扫描缓冲为固定最大 4 KiB 的 `SecureBuffer`，不会按供应商声明容量一次性分配巨量内存；所有读取均保持会话身份复验。产品证据优先于空白启发式，部分产品 magic 不会因其余字节为零而降级成新卡。全零外来介质与真正出厂空白在物理上不可区分，但两者均没有可保存的非零用户数据，符合用户已经锁定的“可访问 ViKey 且完全无产品痕迹时直接初始化”口径。
- 下一切片应把该枚举作为绑定期望硬件 ID 的只读 helper IPC 结果，并让正式 backend 建立一次插卡上下文；仍不得在探测操作里写 marker。之后再单独实现提交密码时的“复验同一 ID + 复验介质状态 + bootstrap”事务。当前提交状态：已确认提交；本地提交将在本记录后创建。
- 提交：`3f5853e [P2.9]classify ViKey user media read-only`。

### 小切片 9b：helper IPC 原子建立介质上下文

- 原文：第 3290～3328 行。
- 审计 P2.8b 身份探针与 P2.9a 只读分类后确认，若主 App 分别启动两个 helper 获取硬件 ID 和介质状态，两次进程之间仍可换卡，无法证明分类属于随后绑定的实体。P2.9b 因此只合并这两个只读动作，不提前实现密码提交、marker 写入或 bootstrap。
- helper IPC 升级至 v8，新增固定凭据载荷的 `card_media_probe`。helper 在同一完整认证会话中保存登录前后已复验的非零硬件 ID，执行完整用户区分类，显式关闭会话后一次返回 ID 和严格枚举状态。成功响应新增独立 `result` 字段，避免截断 ID 或混用 `detail`；协议拒绝 v7 降级、零 ID、 零值/越界状态、其他操作携带状态，以及失败响应残留状态。
- Debug、MSVC `/analyze`、ASan 三套全量构建和 CTest 均通过；每套 **33/33**：31 项实际通过、 2 项按既有受限父令牌约束跳过。`/analyze` 无项目诊断，ASan 未报告内存错误。由于 IPC 版本和 helper 内部调用序列发生变化，`vikey_helper_process_tests` 仍须由用户在普通、非管理员桌面 PowerShell 对最新 Debug 产物补跑；随后运行最新 Debug 产物并得到完整输出 `vikey_helper_process_tests passed`，确认原子介质探针、绑定 ID 的正式卡记录链、受限进程边界和暂存清理均通过。该测试使用伪 `ViKey.dll`，不访问实体卡。
- 下一独立切片是用户提交日常密码后的初始化事务：携带本次上下文的期望 ID，在写卡前重新检查同一实体及允许的 `blank`/可恢复状态，再执行可恢复 bootstrap 和既有 marker 初始化服务。 在该事务完成前，主 App 仍不得仅凭探测结果写入实体卡。当前提交状态：全部验证通过，用户已批准提交；本地提交将在本记录后创建。
- 提交：`0e99624 [P2.9]probe ViKey card media atomically`。

### 小切片 10a：ViKey 会话内安全 bootstrap 原语

- 原文：第 3329～3366 行。
- 审计 v8 插卡上下文、厂商无关 bootstrap 和 ViKey 分类后，将写事务继续拆成可独立验证的适配器原语与后续 IPC 两层。本切片不改变协议或 helper 分派，只实现同一已认证稳定会话中的写前状态复验与可恢复 marker 日志建立。
- 原语复用同一个带容量缓存的 ViKey 随机访问存储，完整重做介质分类并要求实际状态与期望状态逐值一致；不一致映射为 `device_failure`，且不进入任何供应商写调用。准入后调用既有可恢复 journal bootstrap，后者写后逐字节复读逻辑 provisioning marker；原语再扫描 736 字节日志区之外的完整用户区，发现并发出现的非零尾部时失败关闭。
- 安全复核确认写权限只来自适配器允许状态与精确复验的交集，不能由调用者把受保护状态降级成空白。所有范围读写仍在每次供应商调用前复验会话硬件 ID，写入仅限既有 736 字节日志保留区； marker 非秘密，不含密码、根秘密、信封、模式、档位或诱饵信息。供应商 SDK 不提供跨调用硬件事务，完整分类与首个写调用之间仍存在无法彻底消除的物理窗口；写后全尾区复验可发现但无法回滚外部并发写入，因此后续 helper 仍必须先绑定期望硬件 ID，且任何异常都不得自动重试或改绑。
- 下一切片 P2.10b 应把期望硬件 ID 与精确期望介质状态加入固定 IPC 请求，在同一 helper 会话先比对 ID，再调用本原语并显式关闭，客户端只暴露一次性开发 bootstrap 操作。当前提交状态：实现、 测试和安全复核完成，已确认提交；本地提交将在本记录后创建。
- 提交：`d504575 [P2.10]guard ViKey media bootstrap`。

### 小切片 10b：IPC v9 同卡/状态绑定 bootstrap

- 原文：第 3367～3405 行。
- 审计 P2.10a 原语、v8 插卡上下文和正式 216 字节 backend 后确认，bootstrap 请求必须同时携带探针得到的硬件 ID 与当时的精确介质状态；只携带 ID 会允许 UI 停留期间介质状态变化后仍写卡， 只携带状态则无法封闭跨 helper 换卡窗口。
- helper IPC 升级至 v9，新增固定 208 字节 `card_media_bootstrap` 请求：厂商摘要、非零期望 ID、 4 字节严格状态和三个固定长度厂商凭据。writer、reader 和客户端启动前验证均只接受 `blank` 或 `provisioning_interrupted`；请求不含日常登录密码、根秘密、marker 副本或最终 Card Record。
- Debug、MSVC `/analyze`、ASan 三套全量构建和 CTest 均通过；每套 **33/33**：31 项实际通过、 2 项按既有受限父令牌约束跳过。`/analyze` 无项目诊断，ASan 未报告内存错误。协议测试在三套配置中实际通过；由于 IPC 版本、helper 分派和伪 SDK 调用序列均变化，仍待在普通、非管理员 PowerShell 运行最新 Debug `vikey_helper_process_tests`。随后补跑并得到完整输出 `vikey_helper_process_tests passed`，确认 v9 bootstrap 成功、错 ID/错状态拒绝、既有 helper 隔离与卡记录链以及暂存清理均通过；该测试只使用伪 `ViKey.dll`，未访问实体卡。
- 下一切片应在产品层编排“v8 探测上下文 → 用户确认/强度合格的日常密码 → v9 bootstrap → 绑定同一 ID 的正式 `ICardStore` → 既有 Card Record 初始化服务”，并定义中途失败后的恢复入口。 当前提交状态：全部验证通过，已确认提交；本地提交将在本记录后创建。
- 提交：`5a959ab [P2.10]bind helper media bootstrap context`。

### 小切片 11a：正式 ViKey 初始化适配边界

- 原文：第 3406～3443 行。
- 审计 P2.6 厂商无关初始化服务、P2.9b 易失插卡上下文、P2.10b 同卡/状态绑定 bootstrap、正式 216 字节 helper backend 与开发凭据生命周期后确认：产品层下一步需要的是可替换硬件适配器， 不应让通用 `CardRecordService`、密码入口或 UI 直接调用 ViKey 私有 helper API。本切片只建立该边界，不提前接入日常密码、生成根秘密、写最终 Card Record 或改变 UI。
- bootstrap 入口在加载凭据、验证制品、建立暂存目录或启动 helper 前拒绝零硬件 ID，以及 `legacy_p1_or_foreign_prefix`、`foreign_data`、`conflicting_evidence`、`product_evidence` 四类不可写状态；只把 `blank` 或 `provisioning_interrupted` 连同原始硬件 ID 原样交给 v9。 helper 仍负责重新认证、比对实际 ID、完整重分类和可恢复 marker 写入，因此适配器不会把旧探针结论当作当前写权限。
- 进程测试新增正式初始化适配链：独立测试 PIN 的空白伪卡经适配器探测得到 ID 1/`blank`，再通过适配器 bootstrap；断言两次操作分别加载一套新凭据。零 ID 与外来状态均在凭据加载前返回 `invalid_configuration`，凭据提供者失败只调用一次、归一化为 `backend_unavailable`，并检查所有路径无 helper 暂存目录残留。
- Debug、MSVC `/analyze`、ASan 三套全量构建和 CTest 均通过；每套 **33/33**：31 项实际通过、 2 项按既有受限父令牌约束跳过。`/analyze` 无项目诊断，ASan 未报告内存错误。新增正式适配成功链位于受限环境会跳过的 `vikey_helper_process_tests`；随后在普通、非管理员桌面 PowerShell 对最新 Debug 产物补跑并得到 `vikey_helper_process_tests passed`。测试仅使用伪 `ViKey.dll`，不会访问实体卡。
- 提交：`7dbab36 [P2.11]add formal ViKey enrollment adapter`。

### 小切片 11b：厂商无关新卡初始化事务

- 原文：第 3444～3487 行。
- 审计 P2.6 `CardRecordService`、P2.11a ViKey 私有探测/bootstrap 适配器、正式 216 字节 store、 密码入口和强度需求后，将本切片锁定为可独立审查的产品写事务：它只接收已由后续 UI 提交层完成二次确认和强度策略验收的单个日常密码，不在没有明确产品口径时擅自发明强度算法，也不提前改登录、插拔路由或首次建卷向导。
- 新增厂商无关 `ICardInitializationTarget` 和 `CardInitializationService`。目标必须把“确保精确 provisioning marker”和随后返回的正式 `ICardStore` 绑定到同一物理插卡上下文；服务固定先调用前者，成功后才把密码移交既有 `CardRecordService::initialize_and_authenticate`，从而复用精确 marker 前置检查、KDF 后二次状态检查、可恢复写、逐字节回读和成功直接返回根秘密 S。
- helper 进程测试将正式初始化适配链改为新 ViKey 目标，覆盖探测和 bootstrap 分别加载新凭据、 已验证成功的重复 `ensure` 不重启 helper、正式 store 能力边界，以及精确状态错配第一次失败后同一目标被永久作废、第二次调用不重新加载凭据。受限 Codex 父令牌下该进程测试仍按设计跳过， 随后在普通、非管理员桌面 PowerShell 补跑最新 Debug 产物并得到 `vikey_helper_process_tests passed`；测试只使用伪 `ViKey.dll`，未访问实体卡。
- 最终代码在 Debug、MSVC `/analyze`、ASan 三套全量构建和 CTest 中均为 **34/34**：32 项实际通过、2 项按受限父令牌约束跳过。`/analyze` 无项目诊断，ASan 未报告内存错误；新增通用事务测试在三套配置中均实际执行通过。
- 提交：`98eb75b [P2.11]orchestrate vendor-neutral card initialization`。

### 小切片 12a：不可伪造的密码确认与策略边界

- 原文：第 3488～3532 行。
- 审计 Q-06、P2 入口表、现有 `password_ingress`、WebView 数组传输和 P2.11b 初始化事务后确认： 主需求只锁定“设置并确认”“最低强度约束”“强度反馈”和遗忘风险确认，没有锁定最小长度、字符类别、词典、评分算法或阈值。旧设计稿中的注册页已经被 2026-08-28 决策废弃，当前 P1 页面只有登录单密码框，不能拿旧稿隐含规则充当产品口径。
- `ConfirmedPassword` 不可默认构造、不可拷贝，只能由 `confirm_password` 的私有构造路径产生； 它携带已接受的非秘密强度反馈，原始密码只能通过消费整个证明对象取出。P2.11b `CardInitializationService` 已收紧为只接受该证明，不再能用裸 `SecureBuffer` 绕过确认与策略。 bootstrap 失败时证明内密码在重抛前销毁；成功时原样移动给既有 Card Record 服务消费。
- `card_initialization_service_tests` 改为必须先经测试策略和两份相同输入创建证明，再进入 P2.11b 事务；既有 bootstrap 顺序、精确 marker、KDF 前失败关闭及最终 initialized v1 覆盖保持通过。
- Debug、MSVC `/analyze`、ASan 三套全量构建与 CTest 均为 **35/35**：33 项实际通过、2 项按受限父令牌约束跳过。`confirmed_password_tests` 与初始化事务测试在三套配置中都实际执行通过； `/analyze` 无项目诊断，ASan 未报告内存错误。本切片没有修改 helper、IPC、ViKey 调用或进程测试语义，P2.11b 已由用户桌面令牌验证，故无需重复 PowerShell 补跑。
- 提交：`3abc689 [P2.12]require confirmed password proofs`。

### 小切片 12b：离线卡密码长度基线

- 原文：第 3533～3570 行。
- 复核 Q-06 后确认 Card Record 可随卡被取得并复制，日常密码是对离线猜测的唯一记忆秘密；卡在场和 512 MiB Argon2id 会提高成本，但不能把该密码按普通在线 MFA 的 8 字符下限处理。对照最终版 NIST SP 800-63B-4 的口令指南，本切片采用长度优先、允许 Unicode/空格且不强制大小写、数字、 符号组合的基线。客户随后考虑隐藏盘用户需要同时记忆日常／诱饵两套密码，决定把最低长度从 NIST 单因素建议的 15 字符调整为 12 字符，并暂缓弱密码检测；这是明确的可用性取舍和已知剩余风险。18 字符“较强”仅为鼓励更长密码的反馈线，不把长度反馈宣称为熵证明。
- 新增 `offline_card_password_policy_tests`，覆盖 11/12、17/18、128/129 的全部阈值边界；确认中文 BMP 字符和四字节 emoji 均按码点而非 UTF-8 字节计数，空格及单一字符类别不触发组合规则；并覆盖过长编码、截断、代理项及大于 U+10FFFF 的四类畸形 UTF-8 失败关闭。
- Debug、MSVC `/analyze`、ASan 三套全量构建和 CTest 均为 **36/36**：34 项实际通过、2 项按受限父令牌约束跳过；新策略测试在三套配置中均实际通过。`/analyze` 没有项目诊断，ASan 未报告内存错误。本切片不修改 helper、IPC 或 ViKey 行为，因此无需用户额外补跑桌面 PowerShell 测试。
- 常见／已泄露口令及重复模式拦截已由客户决定暂缓。由此保留的风险是 12 字符仅约束长度，不能阻止常见短语、连续数字或其他低猜测成本输入；Argon2id 提高每次猜测成本但不会把弱密码变强。 后续仍需统一 NFC 归一化，再把策略接入初始化与未来诱饵密码设置。遗忘风险确认文案和双输入 WebView 安全传输仍未实现。本切片已完成实现、测试和安全复核，已确认提交；本地提交将在本记录后创建。
- 提交：`758668e [P2.12]add offline card password baseline`。

### 小切片 12c：共享 Unicode NFC 密码入口

- 原文：第 3571～3610 行。
- 审计现有 `password_ingress`、WebView 两类 SAFEARRAY、P2.12a 确认证明和 P2.12b Unicode 码点策略后确认：若只在初始化或只在登录一侧规范化，视觉相同的预组字符／组合字符会得到不同 KDF 输入并永久锁死数据。规范化因此必须位于两条产品路径将来都会复用的 UI 密码入口，并发生在双输入比较、强度计数和任何 KDF 之前；具体初始化页面尚未接入，但不得另建第二套入口。
- 对照 Microsoft `NormalizeString` 的显式长度和缓冲区估算语义，新增消费式 `normalize_password_utf8_nfc`。它拒绝空输入、畸形 UTF-8 和超过 4096 字节的原始桥接值，通过 `MultiByteToWideChar` → `NormalizeString(NormalizationC)` → `WideCharToMultiByte` 得到唯一 NFC UTF-8 字节。所有调用都传显式长度，不依赖 NUL 结尾，因而嵌入 U+0000 不会截断。
- 新增 `password_normalization_tests`，覆盖 ASCII 幂等、`e + U+0301` 与预组 `é` 收敛为相同字节、 韩文 Jamo 合成为 NFC 音节、嵌入 NUL 后的 emoji 保留、过长编码／截断／代理项失败关闭、空输入拒绝和 4097 字节前置拒绝。`password_ingress_tests` 另验证桥接源在 NFC 成功、畸形和超限路径均清零；WebView 测试验证 4097 元素的 `VT_UI1` 与 `VT_VARIANT` 数组均拒绝、擦除且不触达会话。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **37/37**：35 项实际通过、2 项按受限父令牌约束跳过。新增规范化测试、密码入口、UI 控制器和两类 WebView 数组测试在三套配置中均实际通过；`/analyze` 无项目诊断，ASan 未报告内存错误。本切片不修改 helper、IPC、ViKey 或跨进程调用，故无需用户额外补跑桌面 PowerShell 测试。
- 兼容边界是 NFC 现在改变产品 UI 登录的 KDF 输入语义：ASCII 和已经是 NFC 的密码字节不变； 尚未发布的非 NFC P2 测试记录以及人为用组合形式创建的旧开发记录不承诺可继续登录。正式 P2 产品记录尚未部署，因此本切片在初始化 UI 接入前锁定规范化协议，不增加迁移或双重尝试旁路。 仍待实现遗忘风险确认、双输入 WebView 提交和未初始化页面路由；弱密码检测按客户决定暂缓。 本切片已完成实现、测试和安全复核，已确认提交；本地提交将在本记录后创建。

### 小切片 13a：厂商无关的只读卡入口路由

- 原文：第 3611～3644 行。
- 审计 P2 入口表、P1 `CardPresenceController`、ViKey v8 介质探针、可恢复 Card Record 日志及当前 WebView 单登录页后确认：P1 控制器只负责物理在场与拔卡卸载，不应混入产品生命周期判断；入口路由应作为独立的厂商无关边界。注册、首次建卷向导和旧 P1 卡迁移均不进入本切片，唯一实体开发卡未被读取或写入。
- 新增 `CardEntryRouter` 及固定三态结果。只有适配器已经完整授权的 `initializable_media`，或产品日志中的精确 `provisioned_uninitialized`，才进入初始化页；只有 `initialized_v1` 进入登录页。 blocked、P1、未来版本、malformed、未知枚举和任意检查异常统一进入 `unavailable`，不提供自动重试、宽松降级或“看起来空白”旁路。适配器明确报告 blocked 或新介质时不会多做产品记录检查。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **38/38**：36 项实际通过、2 项按受限父令牌约束跳过；新增测试三套均实际通过，`/analyze` 无项目诊断，ASan 未报告内存错误。改动不涉及 helper、IPC、实体卡或跨进程行为，无需补跑桌面 PowerShell 测试。
- 安全复核确认路由器没有写接口、密码、卡内原文、硬件 ID、信封命中、档位或模式状态；公开结果只有初始化／登录／不可用三态。异常和未知值均收敛到不可用，不能把损坏或未来记录转成可覆盖的新卡。剩余风险是严格只读合约尚无 ViKey 实现，且上下文所有权和拔卡清理尚未接入主 App；因此当前结果不能宣称 UI 已能识别 P2 卡。本切片已完成实现、测试和安全复核，已确认提交；本地提交随本记录创建。
- 提交：`bd3a70e [P2.13]route card entry read-only`。

### 小切片 13b：Card Record 日志只读逻辑状态检查

- 原文：第 3645～3680 行。
- 审计 P2.13a 只读入口合约、`read_card_record_journal_v1` 与 `load_journal` 后确认：正常 store 读取会在描述符未镜像、闲置槽摘要不符或提交清理中断时随机覆盖闲置槽并重写描述符，不能用于用户尚未提交密码时的页面选择；但内部 `load_journal` 本身只执行容量检查、四次精确读取、SHA-256 摘要校验和 commit/previous 有向关系选择，没有写调用，可作为只读原语的唯一事实来源。
- 扩展 `card_record_journal_v1_tests`：验证稳定 provisioning、initialized、future 和物理提交有效但 framing malformed 的分类；验证部分暂存写入后仍选旧 marker、描述符发布后清理撕裂仍选新记录； 同时覆盖空白日志与活动槽损坏失败。所有成功／失败路径都断言写调用为零，稳定和可恢复场景还断言恰好执行四次区域读取。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **38/38**：36 项实际通过、2 项按受限父令牌约束跳过；扩展后的日志测试三套均实际通过，`/analyze` 无项目诊断，ASan 未报告内存错误。 改动不涉及 helper、IPC、跨进程行为或实体卡，无需补跑桌面 PowerShell 测试。
- 安全复核确认函数只新增一个只读公开状态出口，不暴露记录字节、摘要、槽位、commit id、是否发生中断或是否需要恢复；这些物理细节不会成为 UI 或隐藏盘侧信道。未来版本只被识别为不可登录状态，物理损坏不被降级为可覆盖状态。剩余边界是 ViKey 尚未在一次原子探针中携带该状态，主 App 也尚未持有并清理对应插卡上下文。本切片已完成实现、测试和安全复核，已确认提交；本地提交随本记录创建。
- 提交：`2563918 [P2.13]inspect journal state without writes`。

### 小切片 13c：ViKey helper v10 原子入口状态探针

- 原文：第 3681～3721 行。
- 审计 helper v9 `card_media_probe`、客户端响应校验、P2.13b 日志只读原语和正式 enrollment 适配器后确认：若主 App 在收到 `product_evidence` 后再通过 `ICardStore` 或第二个独立 helper 调用判断记录状态，会分别引入恢复写入或跨会话换卡／状态漂移窗口。逻辑记录状态必须作为既有原子介质探针的受限结果，在同一认证 ViKey session 和同一响应中返回。
- helper IPC 升级为 v10，响应头由 44 字节增至 48 字节并新增显式 wire `CardLogicalState`。它不直接序列化 C++ 枚举，只允许 provisioning、initialized v1、unsupported 和 malformed 四值。协议固定关系为：仅 `product_evidence` 必须携带一个合法逻辑状态；blank、P1 同形前缀、foreign、 provisioning interrupted、conflicting 均必须为零；所有失败响应和其他操作也必须为零。
- 协议测试覆盖 v10 响应往返及全部关系约束：产品证据缺少逻辑状态、非产品介质夹带逻辑状态、 未知逻辑枚举、失败响应残留逻辑状态均拒绝。ViKey IO 测试验证产品 marker 的只读状态为 provisioning 且供应商写计数不变。进程测试新增 provisioned、独立 initialized 夹具记录的原子返回，以及 blank 探针不携带逻辑状态三条端到端断言。第一次普通桌面补跑暴露伪 DLL 的日志只存于单个 helper 进程全局内存，不能在下一 helper 中复读前一进程写入；原断言错误地把夹具当成真实持久卡。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **38/38**：36 项实际通过、2 项因 Codex 父令牌已受限而按规则跳过；协议与 ViKey IO 测试三套均实际通过，`/analyze` 无项目诊断，ASan 未报告内存错误。随后在普通非管理员桌面 PowerShell 补跑修正后的最新 Debug `vikey_helper_process_tests` 并得到 `passed`；该测试只使用伪 DLL，未访问实体卡。
- 安全复核确认 v10 版本、48 字节头长、nonce、安全属性、payload 长度、非零 ID、介质枚举、逻辑枚举及二者关系都在客户端接受响应前校验；降级 helper、尾随 IPC 数据及错误响应残留字段继续失败关闭。逻辑状态只描述公开产品生命周期，未增加密码命中、信封槽位、隐藏盘形态、恢复动作或写卡入口。验证项已全部闭环，本切片完成实现、测试和安全复核，已确认提交；本地提交随本记录创建。

### 小切片 13d：ViKey 不可变一次插卡上下文

- 原文：第 3722～3762 行。
- 审计 P2.13a 通用入口路由、P2.13c v10 原子探针、正式 enrollment 生命周期和初始化目标后确认： helper 已能在同一会话返回硬件 ID、介质证据及可选逻辑状态，但原始 `probe()` 仍由 enrollment 公开，主 App 若分别查询页面路由和初始化目标，可能重复启动 helper 并制造跨会话状态漂移。 本切片须把一次原子结果封装成明确所有权对象；主 App/WebView 接线及拔卡销毁留给后一切片。
- 新增 ViKey 私有 `VikeyCardInsertionContext`，独占持有不可变 `VikeyCardMediaProbe` 并实现 `IReadOnlyCardEntryInspection`。blank／provisioning interrupted 映射为可初始化，产品证据映射为产品记录，P1 同形前缀、外来数据和冲突证据统一映射为 blocked；产品记录的 provisioning、 initialized、unsupported、malformed 状态直接读取同一缓存，不调用 helper、store 或日志恢复读。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **39/39**：37 项实际通过、2 项因 Codex 父令牌已受限而按规则跳过；新增上下文测试三套均实际通过，`/analyze` 无项目诊断，ASan 未报告内存错误。随后在普通非管理员桌面 PowerShell 补跑最新 Debug `vikey_helper_process_tests` 并得到 `passed`；该测试只使用伪 `ViKey.dll`，未访问实体卡。
- 安全复核确认路由对象只持有非秘密硬件 ID、安全属性、厂商介质枚举及公开生命周期状态，不含卡内原文、密码、摘要、槽位、commit id、信封命中或模式信息；路由只读 const 快照，不能写卡、恢复日志或静默改绑。`unique_ptr` 与不可拷贝基类避免上下文被无意复制，删除对象即可清除一次插入绑定。 剩余风险是主 App 尚未实际持有并在拔卡时销毁对象；当前结果不能宣称页面已经识别 P2 卡。 本切片已完成实现、测试和安全复核，已确认提交；本地提交随本记录创建。
- 提交：`6846fc8 [P2.13]bind immutable card insertion context`。

### 小切片 13e：厂商无关插卡上下文所有者

- 原文：第 3763～3802 行。
- 审计 P2.13a 路由、P2.13d ViKey 不可变上下文、P1.7 `CardPresenceController`、HID 通知和当前 WebView 主程序后确认：现有开发 App 仍在启动时固定构造 P1 32 字节 blob backend，卡到达只更新 presence，拔卡只负责卸载；若此时直接替换成 P2 路由，唯一旧 P1 开发卡会被正确阻断，但初始化页面和正式 216 字节登录尚未接好，反而会破坏仍需保留的 P1 真卡回归入口。因此先独立完成通用易失所有权边界，不修改现有 P1 EXE 的可见行为，实际主 App/WebView 接线继续作为下一切片。
- 新增 `card_entry_context_controller_tests`，覆盖无卡初态、单次来源调用和 route 固化、有效上下文保留、新到达在来源调用前销毁旧绑定、blocked 上下文保留至拔卡、拔卡幂等清理、控制器析构清理， 以及换卡来源异常／空结果统一失败关闭。测试明确验证换卡探针失败后旧上下文已销毁，不能继续使用旧卡的初始化或登录 route。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **40/40**：38 项实际通过、2 项因 Codex 父令牌已受限而按规则跳过；新增生命周期测试三套均实际通过，`/analyze` 无项目诊断，ASan 未报告内存错误。本切片未修改 helper、IPC、伪 DLL 进程测试或实体卡路径，无需补跑普通桌面 PowerShell 测试。
- 安全复核确认新控制器不接触密码、卡记录原文、摘要、硬件 ID、模式、档位或信封状态；异常路径只保留公开 unavailable，不泄漏失败细节。旧上下文在新来源调用前释放，避免换卡期间同时存在两份实体绑定；来源失败没有旧状态回退，拔卡/退出也不会留下可继续使用的 route。剩余边界是 `ICardEntryInspectionSource` 的 ViKey 组合、HID 到达/拔卡调用顺序以及 WebView route 查询尚未接入； 当前开发 EXE 仍是 P1 兼容路径，不能宣称页面已经识别或初始化 P2 卡。本切片已完成实现、测试和安全复核，已确认提交；本地提交随本记录创建。
- 提交：`180a446 [P2.13]own volatile card entry context`。

### 小切片 13f：ViKey 来源与 HID 生命周期组合

- 原文：第 3803～3846 行。
- 审计 P2.13e 上下文所有者、正式 ViKey enrollment、P1.7 presence／强制卸载和 Windows HID 广播过滤后确认：P2 不能另写一套设备路径解析，也不能在拔卡卸载失败时恢复已经失效的卡上下文；同时重复 HID arrival 不应被当作失败后的自动重试。现有 P1 重载仍须保留，以免在 P2 页面尚未接入时破坏唯一旧开发卡的回归程序。
- 新增 `CardDeviceLifecycleController`，组合 `CardPresenceController` 与 `CardEntryContextController`。首次到达先标记物理 presence，再执行一次只读来源并发布固定 route； 重复到达返回原 route 且不再次调用来源。拔卡固定先销毁入口上下文和 route，再调用原 P1.7 `force_unmount_active()`；即使强制卸载抛异常且会话仍残留，也绝不恢复已移除卡的上下文。
- 新增 `card_device_lifecycle_controller_tests`，覆盖首次到达顺序、重复到达不重试、来源失败仍保留物理 presence 但 route 为 unavailable、拔卡先销毁上下文再普通／强制卸载、重复拔卡幂等，以及强制卸载抛异常后会话虽残留但上下文不恢复。扩展 HID 测试验证 P2 重载仅接受同一严格目标，外来 HID 不改变既有 route；扩展伪 DLL 进程测试，使正式 enrollment 经通用来源接口返回同一个具体 ViKey 上下文并继续绑定初始化目标。
- 安全复核确认拔卡时入口上下文的销毁先于任何可能失败的卸载调用，防止物理卡已离开后 UI 仍持有可登录／初始化 route；重复 arrival 不会把 unavailable 状态静默重试成可用。共享 HID 过滤没有放宽边界，来源异常继续由 P2.13e 收敛为 unavailable。剩余风险是同步 helper 调用未来接入窗口线程时最多会阻塞到既有超时，且主 App/WebView 尚未实际使用该组合。本切片已完成实现、测试和安全复核，已确认提交；本地提交随本记录创建。
- 提交：`64325da [P2.13]coordinate card device lifecycle`。

### 小切片 13g：精确 provisioning marker 恢复目标

- 原文：第 3847～3881 行。
- 审计 WebView host、P2.13e 上下文所有权、P2.11 初始化目标和需求中“重新探测发现精确 provisioning 日志后直接继续、不得再次 bootstrap”后发现：现有 `VikeyHelperCardInitializationTarget` 只接受 blank／provisioning interrupted。若初始化在 marker 已提交后中断，下一次严格只读探针会正确进入 initialization route，但动作层无法构造目标，页面接线会形成死路。因此先补该恢复边界，再做通用所有者到初始化动作的交接。
- blank／provisioning interrupted 保持既有 pending → v9 bootstrap → verified 语义；零 ID、bare 介质夹带逻辑状态、产品缺逻辑状态、initialized／unsupported／malformed、P1 同形前缀、外来／ 冲突及未知枚举继续在凭据加载和任何外部副作用前按 invalid configuration 拒绝。
- 新增 `vikey_card_initialization_target_tests`：使用会在任何凭据请求时抛错的 provider，验证精确 marker 连续两次 ensure 均零凭据加载、正式 store 仍可取得；验证两种 bare 上下文仍可构造；并覆盖零 ID、矛盾组合、其余产品状态、P1／外来／冲突和未知枚举全部拒绝且零凭据加载。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **42/42**：40 项实际通过、2 项因 Codex 父令牌已受限而按规则跳过；新增目标测试三套均实际通过，`/analyze` 无项目诊断，ASan 未报告内存错误。本切片没有修改 helper／IPC／进程测试或实体卡路径，无需补跑普通 PowerShell。
- 安全复核确认 marker 组合必须精确匹配，不会把 initialized、未来、损坏或非产品介质降级为可覆盖状态；skip-bootstrap 只避免重复 marker 写，最终初始化仍通过硬件 ID 绑定 store 和产品服务的 marker 复读，探针与提交之间换卡会失败。目标不缓存凭据或卡记录，新增状态仅在进程内易失。 剩余边界是通用上下文所有者尚不能请求该目标，主 App/WebView 也尚未接线。本切片已完成实现、 测试和安全复核，已确认提交；本地提交随本记录创建。

### 小切片 13h：一次性同步初始化动作

- 原文：第 3882～3920 行。
- 审计 P2.13e～P2.13g、`ConfirmedPassword`、厂商无关初始化服务与需求中“不确定失败必须作废上下文”后确认：不能把可写初始化目标返回给 UI 长期保存，也不能在目标构造或写卡失败后继续使用原 route。否则拔卡事件与 UI 持有对象之间会形成悬空写授权，并可能绕过新一次只读探测。
- 动作把不可复制的 `ConfirmedPassword` 直接移动给既有 `CardInitializationService`，固定沿用 bootstrap → marker 复读 → KDF／根秘密生成 → 最终记录写入与复读验证。成功返回根秘密给未来已认证状态；工厂异常、空目标、bootstrap／记录失败和成功都不会恢复 route 或上下文，重复调用只能得到 invalid operation，必须物理拔卡后重新插入并完成新探测。
- 新增 `card_entry_initialization_controller_tests`，通过正式 Card Record 初始化事务覆盖成功路径， 并验证控制器在目标工厂运行前已经失效、上下文持续存活到事务结束、目标先于上下文销毁、最终记录为 initialized v1；另覆盖无卡、登录／不可用 route、工厂异常、空目标、bootstrap 失败和重复调用，确认不合格 route 不消耗只读上下文，而已开始的初始化在任何结果下都不可重试。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **43/43**：41 项实际通过、2 项因 Codex 父令牌已受限而按规则跳过；新增测试三套均实际通过，`/analyze` 无项目诊断，ASan 未报告内存错误。本切片没有修改 helper／IPC／伪 DLL 进程测试或实体卡路径，无需补跑普通 PowerShell。
- 安全复核确认写目标不会逃逸到 UI 或事件循环，控制器在任何外部初始化动作前不可逆撤销授权； 密码证明只向正式服务移动一次，目标构造不接触凭据，正式服务继续在 bootstrap 失败时清除密码。 未发现本地状态、硬件 ID、密码、信封、模式或档位信息新增落盘。剩余边界是主 App/WebView 尚未提供初始化双输入页面，也尚未消费成功返回的根秘密进入已认证无卷状态。本切片已完成实现、测试和安全复核，已确认提交；本地提交随本记录创建。

### 小切片 14a：易失认证会话与拔卡清理

- 原文：第 3921～3957 行。
- 审计既有 `ApplicationSession`、P2.13h 初始化动作、Card Record 服务和“已认证但可无卷”需求后确认：P1 会话固定读取 32 字节裸 blob 并立即挂载，且明确不保留卡材料，不能承接 P2 解封后的根秘密，也无法表达合法无卷状态。为保持 P1 真卡回归路径，未修改或替换该兼容会话。
- `CardDeviceLifecycleController` 现编译期强制接收 `IVolatileCardAuthenticationSession`，不保留可漏接清理的二参数构造器；P1 继续直接使用原 `CardPresenceController`。首次有效到达按 presence → 清陈旧认证 → 只读探测执行，重复到达不清当前会话、不重探测。
- 新增 `card_authentication_session_tests`，覆盖无认证初态、移动建立、三种错误长度、活跃会话拒绝替换、清理幂等及清理后可建立新会话。扩展入口初始化测试，验证成功直接建立认证态，活跃认证在工厂前拒绝且不消耗 route/context；既有工厂／bootstrap 失败仍不会建立认证。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **44/44**：42 项实际通过、2 项因 Codex 父令牌已受限而按规则跳过；新增和扩展测试三套均实际通过，`/analyze` 无项目诊断，ASan 未报告内存错误。本切片没有修改 helper／IPC／ViKey 进程协议或实体卡路径，无需补跑 PowerShell。
- 安全复核确认根秘密只有一个锁页所有者，错误输入、拒绝替换、显式清理和析构均复用 `SecureBuffer` 的清零释放；UI 没有字节访问器，生命周期对象无法省略根秘密清理依赖。拔卡在任何可能失败的卸载前清秘密，避免残留卷导致旧认证材料继续驻留。剩余边界是登录 route 尚未有同类一次性认证动作，认证会话也尚未以受限作用域向 P2 挂载／建卷派生提供根秘密。本切片已完成实现、 测试和安全复核，已确认提交；本地提交随本记录创建。

### 小切片 14b：同卡只读登录目标

- 原文：第 3958～3996 行。
- 审计 P2.13 登录 route、P2.14a 易失认证会话、Card Record 认证结果和后续卷派生后确认：登录失败不能一律消费上下文。精确的错误密码结果 `CardCredentialRejected` 应允许同一次插卡重试；设备读取失败、硬件 ID 不匹配、记录状态变化或日志歧义等非凭据错误必须作废上下文。同时，成功登录输入仍要用于 `KDF(S, user input)` 派生卷密码，不能先只建立根秘密会话再无条件销毁输入。因此本切片只建立同卡只读目标，不提前实现会丢失登录输入或错误消费上下文的半成品动作。
- 新增 `vikey_card_login_target_tests`：验证精确 initialized v1 上下文及 enrollment 工厂构造过程均零凭据加载；外来上下文、零硬件 ID、缺失逻辑状态、provisioning／未来／损坏记录、裸卡、P1 同形前缀、外来／冲突介质和未知枚举全部返回 invalid configuration，且拒绝路径不加载凭据。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **45/45**：43 项实际通过、2 项因 Codex 父令牌已受限而按规则跳过；新增测试三套均实际通过，`/analyze` 无项目诊断，ASan 未报告内存错误。本切片未修改 helper／IPC／伪 DLL 进程测试或实体卡路径，无需补跑 PowerShell。
- 安全复核确认上层接口没有写能力，目标只接受同源精确上下文，所有状态校验都先于凭据和 I/O； 后续读取仍由正式后端绑定原硬件 ID，探针与读取之间换卡会失败。目标不持有用户密码、根秘密或卡记录原文，读取结果由调用方以锁页缓冲接收。剩余风险集中在尚未实现的登录动作：必须区分可重试的凭据拒绝与应撤销上下文的其他失败，并把登录输入和根秘密以受限作用域交给挂载派生，禁止增加 UI 可读秘密访问器。本切片已完成实现、测试和安全复核，已确认提交；本地提交随本记录创建。
- 提交：`e2a3a51 [P2.14]bind read-only card login target`。

### 小切片 14c：可重试登录动作与一次性成功交接

- 原文：第 3997～4041 行。
- 审计 P2.14b 只读登录目标、Card Record 解封、P2.14a 易失认证会话、P1 `ApplicationSession` 与现行派生规则后确认：P1 会再次读取 32 字节裸卡 blob，且仅有 A 容器时仍用用户输入派生，不能复用于 216 字节 P2 记录和固定 `KDF(S,"diskA")`。本切片因此只完成厂商无关登录动作与同步成功交接，具体 P2 挂载处理器留作下一独立边界。
- 新增 `ICardLoginSuccessConsumer`：它是同步、非 UI、不可复制的所有权交接接口，一次接收锁页根秘密 S 与原始 NFC 登录输入。未来实现可把 S 移入易失认证会话并立即完成卷密码派生；登录输入不得在调用返回或抛出后保留。
- 新增 `card_entry_login_controller_tests`，使用正式 Card Record KDF 验证同一次插卡先输错、上下文保留、再输入正确密码成功；成功处理器收到与初始化相同的 S 和原登录输入，且调用前 route/context 已撤销；成功尝试还必须执行第二次读取。另覆盖 KDF 期间记录变化、factory 异常、空目标、首次读卡失败和公开记录状态变化全部失败关闭，以及初始化／不可用 route 在目标构造前拒绝且不消费上下文。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **46/46**：44 项实际通过、2 项因 Codex 父令牌已受限而按规则跳过；新增登录动作测试三套均实际通过，`/analyze` 无项目诊断，ASan 未报告内存错误。本切片没有修改 helper／IPC／伪 DLL 进程测试或实体卡路径，无需补跑 PowerShell。
- 安全复核确认错误凭据路径销毁两份密码和本次记录／目标但保留非秘密上下文；其他错误清秘密后撤销上下文；成功必须通过同卡同记录复读，并在秘密离开动作前销毁目标、清除两份记录及撤销授权。 成功处理器不向 UI 暴露 getter，且输入和 S 只通过锁页移动所有权传递。剩余风险是具体处理器尚未实现，当前主 App 仍不能执行 P2 登录或挂载； 后续必须确保处理器把 S 交给 `CardAuthenticationSession`、只在同步派生期间借用秘密，并在失败路径清除输入且不意外回退 P1 的裸 blob 读取。本切片已完成实现、测试和安全复核，已确认提交；本地提交随本记录创建。

### 小切片 14d：独立 P2 登录挂载会话

- 原文：第 4042～4085 行。
- 审计 8.6、P2.14a～P2.14c、既有 `mount_session()`、P1 `ApplicationSession` 和拔卡生命周期后确认：底层 B→A、失败回滚及 Shell 通知事务可以共享，但 P1 会话会再次读取 32 字节裸卡材料， 且旧 A-only 兼容分支使用登录输入，不能成为 P2 成功处理器。P2 还必须把“卡认证成功”与“卷操作成功”分离，合法表达已认证无卷和认证后挂载失败。
- 新增 `CardLoginMountSession`，同时实现一次性 `ICardLoginSuccessConsumer` 与最小活动卷生命周期， 但编译期明确不实现 P1 `IApplicationSessionActions`。它不接收 `ICardStore`、不重新读卡，也没有 `authenticate_and_mount()` 入口；成功交接先把精确 32 字节 S 移入 `CardAuthenticationSession`， 再在同一同步调用栈内借用根秘密与原 NFC 登录输入执行卷操作。
- 扩展 `mount_orchestrator_tests`，锁定 P2 A-only 使用固定 `diskA`、无卷成功且不执行派生／挂载； 既有 P1 单容器、B gate、回滚和布局校验测试继续通过。生命周期／HID 测试改为直接伪造最小活动卷接口，证明插拔路径不再依赖 P1 登录能力。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **47/47**：45 项实际通过、2 项因 Codex 父令牌已受限而按既有规则跳过；`/analyze` 无项目诊断，ASan 未报告内存错误。`git diff --check` 通过。本切片不改 helper／IPC／伪 DLL 进程路径，无需用户手动补跑 PowerShell 测试。
- 安全复核确认 P2 类型不持有卡 store 或厂商后端，没有 P1 裸材料回退；根秘密只有既有锁页会话长期持有且仅受限核心借用，登录输入只存在于同步栈上的锁页所有者；无卷及卷失败不会泄露密码角色、 模式、档位或容器形态到持久状态。剩余风险是主 App/WebView 尚未组合 P2 入口、处理器和恒定单密码框，实体驱动挂载仍沿用 P1 已验证的底层事务证据。本切片已完成实现、测试和安全复核，已确认提交；本地提交随本记录创建。

### 小切片 15a：厂商无关的产品 UI 原生会话边界

- 原文：第 4086～4129 行。
- 审计 Q-07、8.6、P2.12c 共享 NFC 入口、P2.13 插卡上下文、P2.14 登录／认证／挂载链，以及现有 P1 `UiSessionController`、COM host、WebView 页面和主 App 后确认：现有 UI 只有 P1 单次提交， 不能直接复用；本切片先建立不含 ViKey、COM 或页面实现的产品原生控制器，后续接线不得越过它直接触达卡记录、硬件身份或 P1 32 字节裸卡入口。
- 新增 `card_product_ui_session_controller_tests`，覆盖五态投影、认证优先级、遗留挂载阻断与清理、 登录 NFC／来源擦除／可重试凭据拒绝、认证后挂载失败、初始化双输入独立 NFC、匹配和强度拒绝、 首输入 UTF-8 失败时第二输入仍擦除、错误 route、重叠输入拒绝，以及统一卸载转发。
- 首次完整 Debug 增量测试中，`card_device_lifecycle_controller_tests.exe` 因测试对象仍按新增虚接口前的旧 `CardEntryContextController` 布局编译，而核心库已使用新布局，触发无效内存读取并留下系统错误弹窗。执行全量 clean rebuild 后同一测试立即通过；这是增量构建旧对象失配，不是产品卡流程缺陷。后续涉及公开多态布局的切片必须至少执行一次干净重建。受限测试进程的遗留弹窗需由用户点击“确定”关闭，当前会话无权强制结束该进程。
- Debug、MSVC `/analyze`、ASan 三套干净构建与完整 CTest 均为 **48/48**：46 项实际通过、2 项因 Codex 父令牌受限按既有规则跳过；`/analyze` 无项目诊断，ASan 未报告内存错误。新增测试三套均实际通过，`git diff --check` 通过；不涉及 helper／伪 DLL 或实体卡，无需用户手动补跑 PowerShell。
- 安全复核确认 UI 边界不暴露硬件 ID、密码命中的信封、模式、档位、容器形态或卷类型，不记录或持久化输入及状态；两个初始化输入不能别名，所有拒绝和异常路径统一清零，残留挂载也不能与新插卡授权叠加。剩余边界是 WebView/COM 复制面、页面强度反馈和正式 App 组合尚未实现；本切片已完成实现、测试和复核，已确认提交；本地提交随本记录创建。

### 小切片 15b：P2 WebView 安全自动化桥接

- 原文：第 4130～4171 行。
- P2.15a 提交为 `cb8f95d [P2.15]add product UI session boundary`，未推送。继续审计 Q-07、8.6、 R-03、P1 WebView 数组适配器／host object、P2.15a 控制器、产品页和 `tools/app/main.cpp` 后确认： 现有页面及主程序仍只组合 P1 32 字节裸卡链，不能直接改名复用；本切片只建立独立 P2 COM 边界， 不改变开发 EXE 的可见行为。
- 新增 `CardProductWebViewHostObject`，固定只暴露 `submitLoginPasswordBytes`、 `submitInitializationPasswordBytes`、`unmount`、`hasActiveSession` 和 `getSessionState` 五个方法。 五态只编码为 0～4 的中性整数；旧 P1 `submitPasswordBytes` 及硬件 ID、密码角色、模式、档位、 容器形态、卷类型和根秘密均不存在于可查询成员表。
- 新增 `card_product_webview_host_object_tests`，覆盖五态整数投影、禁止成员表、单输入 NFC／擦除／ 重试与成功、双输入 COM 顺序／独立数组／NFC 相等、确认失败、两个参数各自的类型错误及索引、 错误参数数量清理、统一卸载、活动会话查询和 property 调用拒绝；P1 数组与 host object 回归测试继续通过。
- 定向构建首次发现 `SafeArrayGetDim` 的 Windows 头文件签名不接受 `const SAFEARRAY*`，将内部计数辅助函数改为非 const 引用后通过。这是编译期限定符问题，没有生成或运行失配二进制，也未复现 P2.15a 陈旧对象布局导致的系统错误弹窗。
- 安全复核确认桥接不会向页面返回秘密或可区分形态；双输入在首个错误发生前已同时受擦除守卫， 锁页副本由 RAII 清除，产品控制器仍是唯一 NFC 与策略入口。无法主动擦除的剩余面仍是 WebView2、 JavaScript 引擎和 COM 内部可能产生的短期副本；不支持的字符串等非数组参数只拒绝而不解释或修改。页面尚未调用本桥，正式 App 也仍是 P1，因此当前还不能进行真实 P2 UI 全流程验收。本切片已完成实现、测试和安全复核，已确认提交；本地提交随本记录创建。

### 小切片 15c：五态产品页面与假后端 WebView2 闭环

- 原文：第 4172～4214 行。
- P2.15b 已提交为 `fab54a4 [P2.15]add product WebView bridge`，未推送。审计 Q-07、8.6、R-03、 P1 产品页／页面契约／真实 Runtime 测试、P2.15a 控制器、P2.15b 自动化桥、当前 CMake 打包和 `tools/app/main.cpp` 后确认：现有 `index.html` 与 `session.js` 必须继续服务尚未迁移的 P1 开发 EXE；本切片新增独立 P2 页面资产，避免页面先切换而主程序仍注册旧方法的不可运行半接线状态。
- 登录与初始化均在调用桥前清空 DOM 字段；`TextEncoder` typed array 和传给 COM 的普通数值数组在成功、拒绝、编码异常和桥异常路径重复兜底清零。卡状态变化事件会立即清空尚未提交的两个 DOM 输入并合并异步刷新；原生五态与活动卷两次查询若发生竞争，任何“仍有卷但得到动作 route”的组合在页面侧也固定降为不可用，最终原生控制器仍会再次校验 route。
- 新增 `card_product_ui_page_contract_tests`，锁定两个 route-specific 密码控件、五态完整性、禁止字段、严格 CSP、P2 方法名、DOM／typed array／普通数组擦除、NFC 长度反馈阈值、异步串行、插拔清空与刷新合并，以及活动卷竞争时 fail-closed。既有 P1 页面及 Runtime 资产保持不变。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **50/50**：48 项实际通过、2 项因 Codex 父令牌受限按既有规则跳过；`/analyze` 无项目诊断，ASan 未报告内存错误。P2 真实 WebView2 假后端闭环在三套构建的普通本机权限下均退出 0；受限沙箱内新旧两套 Runtime 测试都退出 1， 对照确认是 WebView2 启动环境限制而非 P2 页面断言失败。不涉及 helper／伪 DLL 或实体卡，无需用户手动补跑 PowerShell。
- 安全复核确认页面不持久化或记录输入、状态和强度，不展示可区分密码命中结果；错误密码与初始化拒绝均使用通用反馈，残留挂载优先提供清理而不允许新授权。页面无法主动擦除 JavaScript 字符串、 DOM/WebView2/COM 内部副本的既有 R-03 剩余风险仍在；可控字节数组均已清零，插拔也会立即删除可见 DOM 值。正式 App 尚未组合本页面，因此当前“带 UI 全流程”只完成假后端验收，不代表实体卡或真实容器已通过。本切片已完成实现、测试和安全复核，已确认提交；本地提交随本记录创建。

### 小切片 15d：P2 ViKey 开发主程序组合

- 原文：第 4215～4254 行。
- P2.15c 已提交为 `c281211 [P2.15]add five-state product UI`，未推送。审计 Q-07、8.6、R-03、 P2.11～P2.15 全链、现有 P1／ViKey 开发应用 composition root、HID 回调、窗口关闭守卫和 CMake 依赖后确认：正式组合必须另建默认排除的 P2 开发目标，不能把尚需保留的 P1 开发入口原地替换， 也不能从旧 `ApplicationSession` 或 32 字节 blob store 借用登录能力。
- 新增 `usb_vault_vikey_p2_development_app`，标题明确标记“P2 ViKey 联网开发模式”，继续要求既有网络开发确认开关、helper／供应商目录绝对路径及两个 SHA-256。它只组合 `VikeyHelperCardEnrollment -> CardEntryContextController -> CardAuthenticationSession -> CardLoginMountSession`，再接入 `CardProductUiSessionController`、P2 host object、五态页面、 `CardDeviceLifecycleController` 和严格 VID/PID HID 通知。
- 扩展 `ui_close_guard_tests` 覆盖 P2 无卷直接关闭、卸载失败阻止关闭并保留状态、重试成功后允许关闭。开发配置／凭据聚焦测试、默认应用、P1 开发应用和 P2 开发应用的定向构建均通过。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **50/50**：48 项实际通过、2 项因 Codex 父令牌受限按既有规则跳过；三个应用目标在三套配置均构建成功，`/analyze` 无项目诊断，ASan 未报告内存错误。P2 链接图只含 Card Record backend、helper client／protocol、正式 card store、 HID 和 WebView2 等预期依赖；导入表含开发凭据所需 `CredReadW` 及 HID／SetupAPI，不含 `ViKey.dll`。不涉及伪 DLL 进程变更，无需用户手动补跑 PowerShell。
- 安全复核确认 P2 composition root 不构造 P1 裸卡读取链；凭据仍由每次 helper 操作新鲜加载到锁页所有者，命令行只承载非秘密路径、摘要和显式开发确认。联网 helper、Windows Credential Manager 开发部署和仓库外未签名厂商 DLL 仍不满足生产隔离要求。实体卡 UI 全流程、当前容器状态及新卡出厂全零口径尚未验证；下一切片应先用现有非空白开发卡做只读入口／不可用页冒烟，再由用户选择是否准备专用空白卡进行有写入的初始化验收。本切片已完成实现、测试和安全复核，并已确认提交为 `ce7390b`。

### 小切片 15e：一次性 P1 开发卡重置工具

- 原文：第 4255～4306 行。
- P2.15d 已提交为 `ce7390b [P2.15]add development product application`，未推送。确认当前唯一 P1 卡没有用户数据或正式容器，客户卡也将直接从当前 P2 版本开始，因此不实现根秘密迁移，只需一个能把该开发卡恢复为 P2 可接纳空白状态的最简工具。
- 审计 N-06、完整用户区分类、ViKey 会话随机访问、helper IPC／客户端和既有 P1 写入探针后确认： 普通 P2 bootstrap 必须继续拒绝 `legacy_p1_or_foreign_prefix`；重置也不能只把地址 0 的 32 字节写零而跳过硬件身份与完整区域复验。该模糊状态无法证明一定来自 P1，因此仅由开发工具的显式永久擦除确认承担人工授权，不扩散到产品 UI 或自动接纳路径。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **50/50**：48 项实际通过、2 项因 Codex 父令牌受限按既有规则跳过；重置 CLI 在三套配置均单独构建成功，`/analyze` 无项目诊断，ASan 未报告内存错误。Debug 无参数启动在任何凭据或卡访问前输出用法并退出 1；`git diff --check` 通过。
- 安全复核确认破坏能力留在默认排除的开发 EXE 和联网开发 helper 操作中，需两个精确确认参数， 并以探测 ID、helper 内同卡复验和最终同 ID blank 复验约束目标。剩余风险是该 32 字节形状本质上仍可能是同形外来数据；已明确授权当前无数据开发卡永久擦除，正式产品仍拒绝自动覆盖。 本切片实现和三套验证已完成，已确认提交，并另行明确授权在提交成功后运行工具重置当前实体开发卡；本地提交已创建为 `27faf01`。
- 运行器、结果文件及两组临时 helper／供应商 DLL 副本已删除。仓库只留下本条实体结果记录， `27faf01` 之后没有新的实现改动；本记录尚未提交。

### 小切片 15f：初始化密码强度与常驻显隐控件

- 原文：第 4307～4361 行。
- 实体 P2 初始化页验看时要求把纯文字长度反馈改成“密码强度＋三个格子”，空密码不显示， 不再显示还差几个字符；少于 12 字符时初始化按钮必须灰色且不可点击，常态提示改为“请输入并确认日常密码，至少12字符”。审计 P2.12c 原生 NFC／12～128 码点策略和 P2.15c 页面后决定只细化前端投影，不增加组合字符规则、在线泄露检查或新的强度裁决。
- 用页面自有按钮替换 WebView2 只在首次输入／聚焦时出现的瞬态密码揭示控件，并屏蔽原生 `::-ms-reveal`。日常密码和确认密码各有独立、可访问的 `aria-controls`／`aria-pressed` 状态； 点击外部不会移除图标或复位显隐。提交、插拔、路由变化和统一 DOM 清理都会把两个输入恢复为 `type=password`，避免把明文显示状态带入下一次输入。
- Debug、MSVC `/analyze`、ASan 三套默认构建和完整 CTest 均为 **50/50**：48 项实际通过、2 项因 Codex 父令牌受限按既有规则跳过；`/analyze` 无项目诊断，ASan 未报告内存错误。三套默认排除的真实 WebView2 假后端目标均单独构建并退出 0，Debug 页面契约定向测试也退出 0。Debug P2 ViKey 开发应用已在旧窗口关闭后重新链接，并复制新版 HTML／JavaScript；未访问实体卡或真实凭据。
- 安全复核确认灰色按钮只是提前拒绝，不能替代原生 NFC、长度和双输入确认；脚本仍不记录或持久化密码。用户主动显形后，密码会按预期以文本形式存在于 DOM／WebView2 内部，延续 R-03 已知不可擦除副本风险；本切片只让该状态跨失焦保持，不跨提交、插拔或路由清理。可控 typed array、普通数组和 DOM 值的既有提交前清除与异常兜底未削弱。实现、三套验证和安全复核已完成，待实体初始化页目测后决定是否提交；`27faf01` 后的实体执行记录也将随本切片一并纳入候选提交。
- 提交：`896e3f0 [P2.15]refine initialization password feedback`。

### 小切片 16a：认证后仪表盘外壳

- 原文：第 4362～4414 行。
- P2.15f 已提交为 `896e3f0 [P2.15]refine initialization password feedback`，当前切片从干净工作区开始。用户指定以仓库根目录 `usb-vault_ui-disign.html` 为视觉来源：登录成功直接进入现有风格的仪表盘，删除全部 Emoji 和账号管理，在加密盘／隐藏盘概览右侧增加新建入口； 文件管理、备份、历史和设置不隐藏，但必须不可点击，鼠标悬停显示“正在开发中”。
- Debug、MSVC `/analyze`、ASan 三套默认构建和完整 CTest 均为 **50/50**：48 项实际通过、2 项因 Codex 父令牌受限按既有规则跳过；`/analyze` 无项目诊断，ASan 未报告内存错误。三套默认排除的 WebView2 集成目标均构建成功，Debug P2 ViKey 开发应用已重新打包新版 HTML／JavaScript；页面契约定向测试退出 0，`git diff --check` 通过。
- 本轮用显式进程等待复核真实 WebView2 退出码时，产品集成测试和**未修改的** typed-array WebView2 基线均在当前宿主下于页面驱动消息前退出 1；这属于当前 Codex／WebView2 Runtime 环境限制，不能继续把 PowerShell 启动 GUI 后遗留的旧 `LASTEXITCODE=0` 视为成功。浏览器安全策略同时拒绝直接打开本地 `file://` 页面，未采用 localhost 或其他方式绕过。因此本切片仍需用户在 P2 开发应用中目测认证后页面；本记录不宣称真实 Runtime 自动化通过。
- 安全复核确认仪表盘完全由原生 `authenticated` 状态驱动，未扩大 COM 方法面，未读取或持久化 A/B 形态、密码、根秘密、硬件身份或账号数据；原型的模拟内容没有进入产品资源。开发中导航在所有认证情形下保持一致，不形成档二／档三差异。剩余风险是弹层尚未实现焦点圈闭，且真实 A/B 建卷、容量状态和实体卡视觉验收均未进入本切片。
- 提交：`dc0fb3a [P2.16]add authenticated dashboard shell`。

### 小切片 16b：认证会话内的加密盘 A 建卷事务

- 原文：第 4415～4464 行。
- 审计 N-05、N-08、8.6、现有 `CardLoginMountSession`、挂载编排、P1.4 建卷 helper 和 P1.7 一次性测试夹具后确认：A 与 B 必须独立创建；A 的卷密码固定为 `KDF(S,"diskA")`；普通 A 建卷也必须经过带 `requireAdministrator` manifest 的同一个独立 helper，不能因形态不同省略 UAC； 代码和本地产物不得新增档位、模式或容器组合状态。P1.7 夹具属于 disposable evidence，本切片只重新实现已验证的暂存、发布和清理语义，没有把夹具链接进产品。
- 扩展 `card_login_mount_session_tests`，覆盖未认证、非法容量、最终／暂存目标已存在、固定 `diskA` 与盐、创建失败、发布失败及清理、清理二次失败、发布后挂载失败保留最终容器、通知失败保留真实挂载状态，以及成功后的重复创建拒绝。其他四个会话假实现只补齐尚未由 UI 调用的新接口， 既有 UI／WebView／关闭守卫语义不变。
- Debug、MSVC `/analyze`、ASan 三套默认构建和完整 CTest 均为 **50/50**：48 项实际通过、2 项因 Codex 父令牌受限按既有规则跳过；`/analyze` 无项目诊断，ASan 未报告内存错误。Debug P2 ViKey 开发应用和 UAC 建卷 helper 已成功链接，`git diff --check` 通过。本切片未从页面发起真实 UAC， 也未在实体卡上执行建卷或挂载，因此不宣称真机建卷闭环已验收。
- 安全复核确认没有新增根秘密 getter、用户口令输入、档位字段、日志、注册表或配置写入；所有路径采用固定 A 名称，派生与挂载显式锁定既有算法参数。剩余风险是进程在 helper 成功与发布之间被强制终止时可能留下固定 `.preparing` 文件，后续尝试会失败关闭而不会自动删除；发布后挂载失败会留下有效 `container_a.hc`，下一次登录负责恢复挂载。页面仍只有禁用的预览按钮，下一切片才接入容量输入、原生调用和忙碌／成功／失败反馈。
- 提交：`5f35775 [P2.16]add authenticated container A creation transaction`。

### 小切片 16c：加密盘 A 容量与页面建卷闭环

- 原文：第 4465～4530 行。
- P2.16b 已提交为 `5f35775`，本切片从干净工作区开始。审计现有五态 UI 控制器、受限 `IDispatch` host object、认证后仪表盘弹层、WebView2 真实 Runtime 夹具及 P1.4 已验证容量后确认： 页面只需提供 A 容量，不应接触 S、卷密码、路径、盐或挂载参数；隐藏盘 B 仍须保持独立预览， 不能因共用弹层误调用 A 建卷。
- 加密盘弹层新增容量输入、范围说明和 `aria-live` 状态行。20 MiB 以下、超过 1 TiB、非十进制整数均保持“创建加密盘”禁用；有效提交先进入全局 busy，禁用导航、弹层关闭和重复提交，显示正在请求管理员权限。成功刷新原生活动卷状态并在弹层显示“创建成功”，返回按钮改为“完成”；失败重新读取原生状态，认证仍有效时保留容量供重试，并提示若已发布但未挂载可重新插卡恢复。
- 安全复核确认新桥只增加公开容量，不增加秘密输入、根秘密 getter、档位／模式／容器组合字段或本地持久化；页面和原生两层独立验证容量，后端仍使用不覆盖发布与固定派生。当前最小实现的已知风险是 COM 建卷调用同步等待 KDF、UAC helper 和格式化，操作期间主窗口可能暂时不响应，HID 拔卡事件在该真实耗时窗口内的及时处理尚未专项验收；用于当前演示时必须保持安全卡插入，正式交付前应把长任务迁出 UI 线程并补拔卡竞态验收。统一活动卷布尔值也无法提前判断 A 是否已存在，重复创建由后端失败关闭并显示通用错误。
- 实体 ViKey 联调随后确认修复后的 P2 应用已成功创建并挂载 A；用户由成功界面继续提出容量与盘符展示需求。提交：`792bc06 [P2.16]connect container A creation UI`。
- 提交：`792bc06 [P2.16]connect container A creation UI`。

### 小切片 16d：加密盘 A 实时信息投影

- 原文：第 4531～4576 行。
- P2.16c 已提交为 `792bc06`。审计 N-05、8.4、8.6、现有 `CardLoginMountSessionState`、固定 `ContainerLayout` 和 `VolumeProvider::mounted_volumes()` 后确认： A 是明确的独立普通卷，可在认证后显示其存在、容量和盘符；但不得为展示新增档位、B 的卷类型、 最近盘符或历史记录。会话已经可信记账 A/B 各自挂载标志和固定盘符，容量则须从固定 A 文件的当前属性读取，不能只缓存本次建卷输入，否则应用重启后信息会丢失。
- 会话测试覆盖 A 不存在、存在未挂载、挂载后容量／盘符以及背文件矛盾失败；控制器和 COM 测试覆盖未认证门控、MiB 精确换算、三态及 `VT_I4/VT_UI4/VT_BSTR` 类型。页面契约锁定两个 A 信息区、 三个只读桥调用、移除旧 A 占位和不查询 B 类型；契约测试主函数同时补异常捕获，使断言失败正常输出并退出，不再形成 Windows 未处理异常弹窗。
- Debug 定向 5/5、两个 JavaScript `node --check` 和隐藏运行的真实 WebView2 集成均已通过；集成流程验证初始化后 A 未创建、首次创建失败不变、第二次成功后显示 64 MiB／`X:` 并隐藏新建入口， 以及重登后恢复同一信息。Debug、MSVC `/analyze`、ASan 三套默认构建和完整 CTest 均为 **50/50**：48 项实际通过、2 项按既有环境限制跳过；三套 P2 开发应用和真实 WebView2 集成目标均成功链接，三套 GUI 夹具隐藏运行退出 0，`/analyze` 无新增项目诊断，ASan 无内存错误， `git diff --check` 通过。
- 安全复核确认状态只存在于进程内并仅在认证路由返回；没有新增配置、注册表、日志、路径、盘符历史、档位、B 形态或卷类型输出。三次 COM 只读查询可能被异步插拔事件穿插，页面会严格校验状态／容量／盘符组合并转入既有失败关闭路径，外部变化仍由合并刷新恢复，不会显示拼接出的错误盘符。剩余功能限制是安全卸载后的当前会话没有“重新挂载”动作，界面会明确要求重新插卡登录； 本切片也尚未在实体应用中目测新版信息区，自动 WebView2 只使用假后端的固定 `X:`。
- 提交：`12c6b82 [P2.16]show live container A status`。

### 小切片 16e：登录挂载策略化与手动挂载默认

- 原文：第 4577～4616 行。
- 本切片起于 2026-09-01 关于多容器形态的讨论，结论已写入《需求梳理清单 v2》**8.7**。 8.7.5 定：**默认不自动挂载**，"登录后自动挂载"是用户可开的单一布尔设置；该布尔对模式与档位完全不敏感（同 N-07 挂载计数器的论证），不构成 8.4 意义上的档位痕迹。
- 审计 `CardLoginMountSessionCore::consume_authenticated_login` 后确认：挂载此前写死在登录成功路径里，`mount_authenticated` 与 `MountRollbackError` 残留记账都只服务于该路径。多容器下 "登录挂载全部容器"本就不成立，且 Spike 6 的五秒预算在三个以上卷时必然超线，而并行挂载已于 2026-08-17 四增补实测作废。故先把登录与挂载解耦，不在本切片引入容器集合。
- 会话测试原有 28 处构造全部改为显式 `mount_on_login`，既有断言无一放宽；新增两个 `manual` 用例：一个断言登录后运行时事件为空、`mounted()` 为假且已存在的容器投影为"存在未挂载"， 一个断言 `manual` 登录后仍拒绝二次交接、且建卷路径照常发布并挂载。
- Debug **49/50**、MSVC `/analyze` **50/50**（无项目诊断）、ASan **48/50**。三处失败均为 `vikey_helper_process_tests` 与 `vikey_helper_hid_compatibility_tests` 的既有环境限制 （网络隔离；ASan 下为 `0xC0000135` DLL not found），位于 ViKey helper 进程链，与本切片改动的 `card_login_mount_session` 与 `tools/app/main.cpp` 无关。`git diff --check` 通过。 未访问实体卡或真实凭据。
- 提交：`4547fd9 [P2.16]make login mounting opt-in`。

### 小切片 16f：会话内容器集合与按容器挂载

- 原文：第 4617～4666 行。
- 承接 8.7.1。用户已在 2026-09-01 拍板：**多个 A 类容器全部共用固定 salt**，即继续使用 `kContainerASalt` 与固定 `"diskA"` 输入。该决定使卷密码派生不再需要任何 per-container 材料， 因此**容器清单不必持久化**，8.7.4 的分层列表在本切片可以完全绕开：会话内记住本次接管的容器， 重启后由用户重新选择文件。这同时让 8.4 本地状态层不出现任何容器清单。
- `IAuthenticatedCardVolumeSession` 新增 `create_container(path, size)`、`mount_container(path)`、 `unmount_container(id)` 与 `container_statuses()`。新增 `ContainerStatus` 只投影 id、**文件名**、挂载标志、容量与盘符 —— 完整路径、salt、卷密码、S、档位与卷类型仍不跨边界； 入站路径由用户在文件对话框中选定，出站只给 id 与文件名。
- 会话测试的假运行时改为路径无关：存在性、容量、建卷、发布、清理、挂载与卸载都能应答任意路径， 同时保留全部既有 A 断言（事件序列仅按实现顺序插入 `allocate`）。新增五个用例覆盖：两个容器取得不同盘符且按接管顺序投影文件名与容量、同一路径只接管一次且已挂载不可重复挂载、卸载后条目保留并可重新挂载且分配新盘符、未认证与文件缺失时拒绝挂载且不接管、`unmount_active` 释放全部已接管容器但不丢弃条目。
- 另有三个只用到 A 表面的假实现（关闭守卫、UI 控制器、COM 宿主）补齐四个新纯虚方法， 均以 `logic_error` 明示本切片不使用，避免沉默的默认实现。
- 提交：`527492b [P2.16]adopt containers per session`。

### 小切片 16g：UI 设计基线与产品页换肤

- 原文：第 4667～4710 行。
- 本轮提出当前界面观感不足，要求先出一份"高端、简洁"的全流程设计预览，再把已实现的内容套过去。 审计旧稿 `usb-vault_ui-disign.html` 后确认它只能作为视觉参考而不能作为改造起点：其用户名／注册／ 忘记密码三页签与 N-06（认证锚点是卡上双信封、取消注册）和 Q-02（忘记密码不实现）冲突； "加密盘／隐藏盘"分栏导航与 8.7.5「仪表盘不按卷类型分类展示」冲突；应用内文件管理器已按 8.7.5 降级为 v1.1。故重画而非改色。
- 新增 `usb-vault_ui-design.html`：26 屏全流程预览，分设计基线、会话入口、主界面、创建容器、 维护与数据四段，含设计语言页与流程地图。每屏标注实现状态（已实现／部分实现／未实现）、 设计说明与需求依据编号，屏内窗口按宿主实际尺寸等比缩放。
- 以脚本逐条镜像 `card_product_ui_page_contract_tests` 中全部针对页面的断言并全部通过：两个 route-specific 密码控件与 confirmation 字面量、严格 CSP、单一 `<script>`、三段强度条、 2×`data-create-kind`、4×`aria-disabled` + `data-tooltip`、两处 A 信息字段、容量控件字面量、 `.strength-slot` 与 `#card-session-error` 样式字面量、状态行先于错误行、禁用词与 emoji 清单。
- 🟡 **本次未运行 CTest**：本机 shell 无 cmake／MSVC 环境变量，正式 `ctest -R card_product_ui_page_contract_tests` 待下一次带环境的构建补跑。已在 VS 开发者环境下成功重建 `usb_vault_vikey_p2_development_app`，POST_BUILD 拷贝生效，部署页面与源文件逐字节一致。
- 提交：`cbf3f01 [ui]adopt graphite design baseline`。

### 小切片 16h：多个加密盘容器接入 UI

- 原文：第 4711～4778 行。
- 16f 已提交为 `527492b`，会话侧的 `create_container` / `mount_container` / `unmount_container` / `container_statuses` 均已可用，但 UI 仍只显示固定容器 A（走适配器）。 审计 8.7.1（容器数量不设上限、A 类卷密码固定从卡派生、多容器无密码隔离必须明示）、 8.7.2（创建入口恒显）、8.7.4（容器路径不得明文落盘）、8.7.5（只读状态展示、不按卷类型分类） 与 8.4（本地状态层不得留下容器清单）后确定本切片边界：只把已实现的会话能力接到界面上， 不新增核心方法、不动 B、不引入任何持久化。
- 控制器测试新增四例：未认证不查询也不暴露集合、选中路径与容量原样到达会话、越界容量在打开对话框前即被拒、取消不产生容器、卸载后条目保留且不再带盘符、按 id 重新挂载不再询问文件、未接管 id 拒绝、第二个容器经对话框接管、未挂载却带盘符时失败关闭。
- 真实 WebView2 假后端夹具已按新界面改写并**实跑通过（退出 0）**：夹具页镜像新的容器列表与 「打开已有」入口，仍加载真实 `ui/card_session.js`，走完无卡 → 初始化 → 已认证无容器 → 切到加密盘管理 → 建卷（注入失败 → 用户取消 → 成功）→ 校验行内文件名／容量／盘符且创建入口仍可见 → 重新登录后信息恢复 → 残留卷 → 卸载 → 回到无卡。假选择器固定在第二次调用返回空串，用来覆盖"取消不是失败"这条新路径。
- 🟡 遗留风险与待办： 1. **通用文件对话框的本地痕迹仍需 P3 逐字节验收**。已用 `FOS_DONTADDTORECENT` 且不设 client GUID，但 ComDlg32 自身的 MRU 行为必须实测确认为零写入，这正是 8.4 最容易漏掉的一类。 2. 容器清单不持久化，重启后需重新选择文件；8.7.4 要求的「加密分层清单存于容器 A 内」尚未实现。 3. 全部 A 类容器共用固定 salt（8.7.1 待定项），同一密码下各容器卷密码相同 —— 16f 已记录，本切片未改。 4. 四个 A 专用 COM 方法与控制器的 `create_container_a` / `container_a_status` 现已无页面调用， 属可清理的死面，留待单独一刀切除。
- 提交：`a8aa281 [ui]add the multi-container page and bridge`。

### 小切片 16i：切除单容器 A 专用界面面

- 原文：第 4779～4809 行。
- 16h 之后，页面已全部改走容器集合，四个 A 专用 COM 方法与控制器的 `create_container_a` / `container_a_status` 再无调用方，属自动化边界上的死面。本轮明确要求一刀切除，本切片只做删除与更名，不改任何行为。
- **未动会话层**：`IAuthenticatedCardVolumeSession::create_container_a` 与 `container_a_status` 仍在（`create_container_a` 只是固定路径版的 `create_container`），目前只剩测试调用。 它们与登录挂载策略共用固定 `container_a` 布局，是否一并折叠留待单独评审。
- 删除三个控制器测试与三个 COM 宿主测试（连同四个 A 专用 invoke 辅助），但把其中仍然成立的两处覆盖迁到新入口上：创建失败后仍保持已认证路由、以及非整数容量（`VT_R8` 64.5）必须被 `exact_uint32` 拒绝。
- 完整 CTest **49/50**（唯一失败仍是实体卡不在位的 `vikey_helper_process_tests`）； 真实 WebView2 假后端夹具重跑退出 0；两个 `node --check` 通过。
- 提交：`7495836 [ui]drop the single-container bridge`。

### 小切片 16j：移除 P1 应用链

- 原文：第 4810～4846 行。
- 决定后续只在 P2 现有基础上改动，P1 阶段的内容可以清理。审计依赖关系后确认 P1 应用链的唯一生产消费方是 `tools/app/main.cpp` 的非 P2 分支，边界干净；ViKey 的 P1 blob／helper 链 （32 字节卡存储、blob backend、helper blob 操作、p1_7 夹具与探针）耦合到 helper IPC， 单独作为下一个边界，本切片不动。
- `webview_password_array_tests` **改打到共享入口上**而不是删掉：七个用例改为直接调用 `consume_webview_password_array` 并校验消费结果与源数组擦除，只删掉两个纯粹验证"转发给 P1 会话"的用例（等价的失败擦除覆盖已在 P2 宿主测试里）。`ui_close_guard_tests` 删除三个 P1 用例， 保留产品用例；`card_login_mount_session_tests` 删除"未派生自 `IApplicationSessionActions`"的静态断言（该接口已不存在）。
- 完整 CTest 从 50 项降到 45 项，**44/45 通过**：唯一失败仍是实体卡不在位的 `vikey_helper_process_tests`。真实 WebView2 假后端夹具重跑退出 0。
- 🟡 下一个边界（未做）：ViKey P1 blob 链 —— `vikey_card_store.cpp`（32 字节）、 `vikey_helper_blob_backend`、helper 的 blob IPC 操作、`p1_7` 夹具与诊断、P1 卡重置工具、 blob 往返探针，以及依附其上的 `vikey_card_store_tests` 与 `vikey_helper_process_tests`。 这条链会动到 helper 协议，须单独评审后再切。
- 提交：`880960c [cleanup]remove the P1 application chain`。

### 小切片 16k：隐藏盘容器 B 的会话层建卷

- 原文：第 4847～4891 行。
- 本轮要求先做 B 类容器的最少建卷流程，当前不追求安全性细节，测试始终在稳定插卡状态下进行。
- 三条为"最少"服务的默认取舍，均已记录： 1. **B 固定使用 `ContainerLayout::container_b` 的路径，全机只允许一个**。这样 8.7.6 「首个 B 的卡记录改写时机」这个待定项根本不会出现，也不需要在 UI 里选路径。 2. **建 B 必须让用户再输一次当前登录口令**：manual 策略在登录成功后立刻清掉了口令明文， 而隐藏卷密码 `KDF(S, P_real)` 需要明文，会话不可能凭 S 反推。 3. **顺序为先建容器、再改写卡记录**（16l 接线）；中途失败时容器在、诱饵口令未生效，可重试。
- 会话测试新增三例：派生出的两个卷密码必须不同且分别落到外层/隐藏、事件序列为 `exists → derive×2 → create:hidden → publish → allocate → mount → notify`、接管后可在 `container_statuses()` 中看到；未认证、尺寸不可用（0／非 512 对齐／隐藏卷 ≥ 外层）、空口令一律在触碰运行时之前拒绝；格式化失败后不留会话状态。
- 夹具补强两处：测试布局给容器 B 一个与 A 不同的 salt（原先两者都是全零，派生分支无法区分）， `require_events` 失败时改为打印实际事件序列，便于定位。
- 提交：`187ac1c [P2.16]create the hidden volume container`。

### 小切片 16l：卡记录的第二口令

- 原文：第 4892～4934 行。
- 16k 已提交为 `187ac1c`。本切片补齐 B 流程的第二块：让卡记录能同时被"当前登录口令"与 "新设的第二口令"打开，否则用第二口令根本登录不进来，B 的外层卷也就永远挂不出来。
- `CardRecordService::set_second_password(existing, second)`：只接受 initialized v1 记录， 空口令直接拒绝，随后 `rewrap_card_record_v1` 把根重新封进两个信封，写入后**读回比对**， 写报错但记录确实落盘的情况按成功处理（沿用初始化那套可恢复提交语义）。 记录原语需要调用方口令两次（一次开旧记录、一次当作替换信封），`SecureBuffer` 故意不可拷贝， 因此复制是显式的、锁页的，并与原件一起被消费。
- `card_record_service_tests` 新增三例：改写后两个口令解出同一个根且记录不与改写前逐字节相同、 非 initialized 状态与空口令一律拒绝且不写卡、`write_then_throw` 按读回结果接受而 `throw_without_write` 如实报错且记录不变。
- 安全复核：口令只在调用内存在，显式副本与原件同时被消费；卡层不落任何"哪个是诱饵"的标记； 写卡仍绑定原始硬件 ID，且每次操作重新加载凭据。⚠️ 未做实体卡验证 —— 本切片全部为假存储测试。
- 提交：`00b6746 [P2.16]commit a second card record password`。

### 小切片 16m：隐藏盘建卷的 UI 全链

- 原文：第 4935～4975 行。
- 16k／16l 已分别提交为 `187ac1c`、`00b6746`。本切片把两半接到页面上，构成 B 的完整建卷路径。
- `CardProductUiSessionController::create_container_b(outer_mib, hidden_mib, 当前口令, 诱饵, 诱饵确认)`：三个输入缓冲在所有路径上擦除、互相重叠即拒；路由与尺寸（外层在 20–1048576 MiB、内层非零且小于外层）先校验；诱饵经 `confirm_password` 走**与日常口令同一套强度策略**（12–128 码点），随后先调 `set_second_card_password`、再调 `create_container_b`。 两个秘密各显式复制一份给消费型的卡接口，原件留给只读的建卷接口，用完即清。
- 🔴 **真实 WebView2 假后端夹具已跑通完整 B 流程（退出 0）**：真实页面填两容量 + 三口令 → 一次五参 COM 调用 → 假后端逐一比对四份口令载荷（卡记录的既有/新设、卷的内层/外层）与两个容量 → 容器列表变为两项 → 重新登录后信息恢复。这条覆盖正是五参逆序投递唯一能被证伪的地方。
- ⚠️ 已知取舍与遗留：诱饵输入框**没有显隐切换**（最少流程未做），12 位以上口令要盲打两遍； 当前口令输错会导致卡记录改写失败并退役插入，需要重新插卡；实体卡与真实建卷仍未验证。
- 提交：`f87a12a [P2.16]create the hidden container from the page`。

### 小切片 16n：隐藏盘并入统一容器集合

- 原文：第 4976～5008 行。
- 16m 已提交为 `f87a12a`。B 的最后一块：此前 B 的挂载状态由 `CardLoginMountSessionState` 里一个**独立布尔量** `container_b_mounted` 记账，根本不进 `containers`，因此登录挂出的 B 在容器列表里**完全看不见**，只有 16k 建卷当场那一次会被接管。
- 改为一套集合：`SessionContainer` 增加内部标志 `release_first`（仅决定卸载顺序，不跨应用边界）， 登录挂出 B、残留卷记账、以及 16k 的建卷全部走同一个 `adopt()`。删除 `container_b_mounted`， `mounted()`、盘符占用校验、`unmount_active`、`force_unmount_active` 全部只读这一个集合。
- 既有的 B-before-A 顺序、残留卷、强制卸载与 `mounted()` 用例全部原样通过，说明这次是纯重构。
- 🟡 **B 流程到此闭合，但有一处产品级缺口须另行定夺**：当前 `LoginMountPolicy` 默认 `manual`， 登录不挂任何卷，而 B 的两个卷都必须用**用户口令**派生 —— 会话在登录后已清掉该口令。 因此**重新登录后 B 无法被挂载**，除非二选一： 1. 打开"登录后自动挂载"（把 P2 应用切到 `mount_on_login`，代价见 8.7.5 的五秒预算）； 2. 新增一个"挂载该容器"时再输一次口令的动作（又一刀 UI）。 16k 建卷当场挂出的 B 在本次会话内可正常使用与卸载，不受此限制。
- 提交：`2c1c925 [P2.16]adopt the hidden container into the collection`。

### 小切片 16o：登录后免输入挂载任意已有容器

- 原文：第 5009～5059 行。
- 16n 已提交为 `2c1c925`。目标状态：**登录后不自动挂载，但用户可以自由地免输入挂载任何已有的 B 类容器，挂出外层还是隐藏卷由登录时用的口令决定。**
- **每容器一个 salt**（8.7.1 第 3 案）→ 无法预派生，每次挂载都要 P 在手。 🔴 **因此该目标状态等价于选定 8.7.1 的第 1 案（全部 B 共用一个 salt）**，代价是同口令下各 B 的卷密码相同 —— 该性质 A 类容器现已存在（共用固定 `kContainerASalt`）。 ✅ 2026-09-01 定案选第 1 案，8.7.1 的待定项已改写为定案并记录理由与代价； 当前代码形态（每类一个编译期常量 salt）与该决定一致，无需改动。
- 会话测试新增三例：手动登录只为 B 派生一次、两个容器都以未挂载状态接管； 按 id 挂载时 B 用保留的派生密码、A 用卡派生（逐字节比对实际传给挂载的密码）， 两者落在不同盘符；重复挂载空操作、未知 id 拒绝；强制卸载后 B 无法再挂（密码已丢弃）， 而主动卸载后仍可挂回。
- 🟡 遗留：只有布局里固定的两个容器能在登录后被列出，用户自选路径的容器仍不持久化 （8.7.4 的加密分层清单未做）；实体卡与真实建卷仍未验证。
- 提交：`a23ef1d [P2.16]mount adopted containers without retyping`。

### 小切片 17a：开放系统设置页

- 原文：第 5060～5153 行。
- 16o 已提交为 `a23ef1d`。侧栏「系统设置」此前是禁用占位项。设计稿 （`usb-vault_ui-design.html:1155`）给这一页定了 3 个开关 + 关于块 + 2 条说明， 且自带一条原则：「设置项刻意保持极少。每多一个开关，就多一处可能泄露使用方式的本地记录」。 因此本切片的做法是**从设计稿的既定条目里落地**，而不是新增设置项。
- **登录后自动挂载全部容器** —— 8.7.5 明确授权落盘，`LoginMountPolicy` 也已就位 （`card_login_mount_session.h:24`），但它必须在**登录之前**被读到，因此不能存进容器 A， 会成为产品的**第一个本地持久化产物**并直接进入 P3 的逐字节比对验收。 🔴 另有 8.7.5 的红字约束：开启后必须对自动挂载的卷数设上限或重谈 P-002 五秒预算。
- **记住容器位置** —— 依赖 8.7.4 的加密分层容器清单（存于容器 A 内），该清单未实现 （见 16o 遗留）。
- **决定（2026-09-02）**：先按设计稿把两个开关的 UI 正常做出来，不做未实现的后端， 也不做禁用或「正在开发中」标注。已说明代价：**两个开关目前只活在当前页面， 刷新即回默认值**，「记住容器位置」显示为开启并不代表路径真的被保存。 后端分别由 17b（8.7.5 落盘产物 + 卷数上限）与 8.7.4 的容器清单切片补齐。
- **三条负向验证**（临时改文件后重跑，确认守卫真的会咬）：版本改成 `9.9.9` → `FAIL: about version has drifted from the CMake product version`；插入 `<input type="checkbox">` → `FAIL: settings page offers a control this slice cannot persist` （该守卫在本次改为开关后已按上述新形态重写）；在处理器里加一行 `localStorage.setItem` → `FAIL: the product page persists state in browser storage`。三次均已还原。
- 提交：`5cdec95 [P2.17]open the settings page`。

### 小切片 17b：容器视图统一，不再按卷类型分栏

- 原文：第 5154～5216 行。
- 17a 已提交为 `5cdec95`。本轮要求：隐藏盘既然能建了，「隐藏盘概览」不该还挂着 「建卷能力接入后……」的过时文案；并要求把「打开已有」改称「导入」。
- 跨边界的 `ContainerUiStatus` 只有 `id / name / mounted / size_mib / drive_letter`， **故意不带卷类型**。要给隐藏盘单独建列表就得新增卷类型字段跨边界，那是直接的 8.4 风险。
- 因 8.7.1 定案选案 1（**全部 B 共用一个 salt**，`container_layout_v1.h:19` 的 `kContainerBSalt` 是编译期常量），B 的卷密码为 `Argon2id(登录口令, kContainerBSalt, S)`， **同卡同登录口令下该密码可开任意 B 容器**；会话已在登录时保留它 （`card_login_mount_session.cpp:393`）。因此导入 B 技术上可行。
- 页面契约测试：`test_authenticated_dashboard_is_real_state_only` 改为断言 `dashboard-section-containers` 存在、`dashboard-section-encrypted` 与 `dashboard-section-hidden` **均已不存在**、`data-container-list` 恰为 2 处， 两个建卷入口仍各 2 处。
- 提交：`ef02638 [ui]unify the container view`。

### 小切片 17c：新建容器改为单一入口＋容器类型选择

- 原文：第 5217～5275 行。
- 17b 已提交为 `ef02638`。本轮要求把新建容器流程也做成设计稿的样子： **一个「新建容器」按钮，点击后先弹出容器类型选择**，并给出该屏的四处文案改写。
- 用户指定的四处文案（设计稿 `create · choose type` 一屏）： 1. 引导语改为「容器是一个加密文件，挂载后显示为一个普通磁盘。请选择要创建的容器类型：」； 2. 标准容器改为「一个标准结构的容器，含一个加密卷，使用日常密码进行解密与挂载。」； 3. 隐藏卷容器改为「一个双层结构的容器，含一个外层卷与一个隐藏卷，使用诱饵密码或日常密码进行解密与挂载。」； 4. 「两种结构在磁盘上字节不可区分。选择哪一种，只有你自己知道。」整块删除。
- 🔴 第 4 条同时让设计稿注记「'字节不可区分'是本产品的卖点，也是唯一允许出现在这一屏的安全性说明」失去所指，一并从 `usb-vault_ui-design.html` 的 notes 里删掉， 避免设计稿与实现再次不同步。新文案未引入「档位／模式／胁迫」等 8.6 禁用词，仍只描述结构本身。
- WebView2 集成驱动新增两段：打开后必须停在类型步（标题「新建容器」、按钮「下一步／取消」、 标准容器预选、隐藏卷未选）；在参数步改动容量后按「上一步」，必须回到类型步且容量已复位为 64。 夹具新增 `fail-creation-type-step` / `fail-creation-back-step` 两个失败码。 隐藏盘分支改为「新建容器 → 点隐藏卷卡片 → 下一步」，并校验两张卡片的 `aria-checked` 互斥翻转。
- 提交：`26d305a [ui]create containers from one entry`。

### 小切片 17d：参数步迁到「容器」口径，日常密码不再重输

- 原文：第 5276～5413 行。
- 17c 已提交为 `26d305a`。本轮要求把参数步也做成设计稿的样子，并给出七条具体改动。
- 用户追问：会话保留日常密码后，诱饵密码登录的人能不能拿到它？**不能。** 会话保留的是「本次登录用的那一个」——`consume_authenticated_login` 收到什么就存什么， 诱饵登录留下的就是诱饵密码。日常密码在卡上只以 `Argon2id(日常密码)` 为密钥的 AES-256-GCM 信封形式存在，解开它需要日常密码本身；**持有 S 也没用，S 是信封里的载荷不是钥匙**。 同一段代码、同一个缓冲、同一个生命周期，因此保留在两种登录下逐字对称，8.7.2 不受损。
- WebView2 集成驱动：新增单位切换往返（2048 MiB 合法 → 切 GiB 后 2048 GiB 越界并红字提示 → 1 GiB 合法）、内层 97/128 触发「内层容量过载。」、取消勾选后创建按钮禁用三段； 夹具新增 `fail-creation-unit` / `fail-hidden-capacity-rule` / `fail-hidden-acknowledgement`。
- ⚠️ 未做（不在七条之内，需原生支持）：设计稿参数屏还有「名称」与「保存位置」两个字段， 当前标准容器仍由原生选择器选路径、隐藏容器仍用固定路径。
- 提交：`bb625b5` 与 `5006513` 两笔，见下节补记。
- 原生会话保留登录输入、四参数隐藏容器桥及本会话隐藏容器重挂载修复已提交为 `bb625b5 [P2.17]create the hidden container without retyping the login`。

### 小切片 17e：当前认证会话操作日志

- 原文：第 5414～5465 行。
- 复核 `需求梳理清单_v2.md` 8.4「本地状态层不可区分」与 8.6「档位不是运行时实体」，并检查当前会话的创建、挂载、卸载真实提交点、UI 控制器、COM host object、产品页和 WebView2 集成夹具。
- 产品侧栏的「操作日志」现为可进入页面，最新事件在前，以纯 `textContent` 构造四列表格；每次 native refresh 都重建内存投影，不使用 `innerHTML`、`localStorage`、`sessionStorage`、IndexedDB 或 cookie。 页面再次验证 128 上限、整数时间、固定枚举、文件名无路径和事件字段组合，支持清空当前会话日志。
- `usb-vault_ui-design.html` 的对应屏同步为当前会话语义，删除 30 天、标准容器持久化、卡标识、容量、 强制卸载详情等尚未实现或不应记录的内容。
- Debug、MSVC `/analyze`、ASan 三套完整 CTest 均为 **45/45**：43 项实际通过、2 项按受限父令牌规则跳过；`/analyze` 无项目诊断，ASan 未报告内存错误。最后加入的拔卡清除与共享 128 上限后，三套均重新构建成功，相关 5 项测试各 **5/5** 通过。`node --check` 两个脚本通过。
- 提交：`6988595 [P2.17]add current-session operation history`，未推送。

### 小切片 17f：去掉重复的卡状态指示，登录给出进行中反馈

- 原文：第 5466～5511 行。
- 17e 已提交为 `6988595`。本轮三条界面改动：去掉主页绿色的「安全卡已认证」、 开发窗口标题改名、登录输密码后要有加载动画（原话「不然用户会以为卡死」）。
- 会话动作按钮内嵌不确定型转圈，并在忙时把文案换成进行时： 登录「正在验证安全卡」、初始化「正在初始化安全卡」、卸载「正在安全卸载」。 解卡要跑一次 Argon2id 加卡 I/O，是数秒静默，缺少动效会被读成卡死。 🔴 **不做进度条**：设计稿明确「进度按真实阶段拆分，不用假进度条」， 而登录链路拿不到真实阶段，能给的诚实指示只有不确定型转圈。 转圈沿用设计稿 `.spin` 参数（`.8s linear infinite`），禁用态自动切金色描边， 并加 `prefers-reduced-motion` 降速。
- 集成驱动里 `action.textContent !== "尝试安全卸载"` 会被按钮新增子元素带来的空白打翻， 改为读 `card-session-action-label`。这与 17c「上一步」按钮是同一个坑， 凡是给按钮加图标就会再遇到一次。
- ⚠️ 构建期一次 `LNK1168`：开发 EXE 正在运行导致无法写入，关闭后重建通过，与改动无关。
- 提交：`a2008f2 [ui]drop the duplicate card badge and show login progress`。

### 小切片 17g：设置页重新初始化安全卡

- 原文：第 5512～5575 行。
- 17f 已提交为 `a2008f2`。本轮要求在设置页增加把一张 `initialized_v1` 卡破坏性退回 provisioning 状态的正式入口，并给出了另一位 agent 的流程方案（设置页执行、成功后自动关闭应用、下次启动由现有入口路由进初始化页）。
- **实现前逐条核实该方案的三个前提，均成立**： 1. `provision_uninitialized_card_record_v1()`（`card_record_v1.h`）已存在，产出的正是受控出厂流程写的那个精确 marker； 2. `commit_card_record_journal_v1` 调用的 `validate_logical_record` （`card_record_journal_v1.cpp:366`）**同时接受** `provisioned_uninitialized` 与 `initialized_v1`，不存在阻挡 initialized → provisioning 的状态机守卫，因此不需要新增 helper IPC； 3. `card_entry_router.cpp:20` 已把 provisioning marker 路由到初始化页。
- ⚠️ 一处不可用：`provision_card_record_journal_v1` 在检测到 product evidence 时会显式拒绝， 那是出厂路径。重置必须走普通 `commit` 路径。
- `card_record_service_tests` 新增三项：重置后卡上是逐字节精确的 marker、原两个密码都只得到状态拒绝（不是凭据答案）、卡可再次初始化；非 initialized 状态拒绝且 `put_calls == 0`； 🔴 写入未落卡时必须报错且记录不变——否则用户会以为密钥已销毁而其实还在。
- 提交：`ca59fdc [P2.17]reset the security card from settings`。

### 小切片 17h：保持一次性插卡上下文并恢复重置可达性

- 原文：第 5576～5608 行。
- 17g 已提交为 `ca59fdc`。提交后的真实组合审计发现：P2.14c 在初始化或登录成功前会按设计撤销一次性插卡上下文，因此已认证页面虽然显示重置入口，原实现仍要求 `login_required` route 与旧 context，生产组合中会得到 `invalid_operation`。同一问题也使初始化后立即设置第二密码不可达。
- 一份未提交的候选改动尝试在初始化和登录成功后重新绑定 context。该方案未采用：它会破坏 P2.14c 已验证的边界——成功处理器收到 S 与登录输入前必须先撤销插卡上下文，不能为了后续写卡复活或长期保留旧硬件／介质观察。
- 单元测试覆盖：初始化后旧上下文已销毁但第二密码写入可通过第二次探测完成；登录成功同样不保留 context；未认证重置零适配器访问；认证重置只探测／写入一次并留下精确 provisioning marker； UI、COM 和 WebView fake 均验证认证授权参数。
- Debug 全量 CTest **45/45**：43 项实际通过，2 项按环境限制跳过；受影响 6 项在 Debug、 MSVC `/analyze`、ASan 三套均通过。三套 WebView2 集成程序均退出 0；`/analyze` 无项目诊断， ASan 未报告内存错误；两个 JS 文件 `node --check` 通过；`git diff --check` 通过。
- 提交：已确认；本节随 `5aed730 [P2.17]reinspect the card before record writes` 提交，未推送。

### 小切片 18a：ViKey 开发凭据配置程序

- 原文：第 5609～5650 行。
- 本轮要求把手工打开 Windows 凭据管理器的流程收敛成一个最小配置程序，供开发测试包录入 UserSN、API Key 与用户 PIN。本切片只配置**当前 Windows 用户、当前机器**上的开发凭据， 不访问安全卡、不启动 helper、不加载 `ViKey.dll`，也不把它扩展为生产凭据部署方案。
- 复核既有 `WindowsCredentialProvider`、`VikeySession` 和小切片 11 的实体卡结论后，沿用三个固定 Generic Credential 目标：`usb-vault-vikey-dev-user-sn`、`usb-vault-vikey-dev-api-key`、 `usb-vault-vikey-dev-user-pin`。输入规则与真实会话保持一致：UserSN 为 32 个十六进制 ASCII， API Key 为 128 个十六进制 ASCII，用户 PIN 为 8 个字母或数字。
- 扩展 `vikey_development_credentials_tests`，覆盖 UserSN/API Key/PIN 的合法字符集、错误长度、 非法字符与未知字段。Debug、MSVC `/analyze`、ASan 三套目标均构建成功且该测试均通过； `/analyze` 无项目诊断，ASan 未报告内存错误。
- Debug GUI 做了两次“空输入、隐藏启动、确认消息循环存活、关闭指定新进程”的烟测，均通过且没有调用 `CredWriteW`。依赖表只有 `ADVAPI32`、`KERNEL32`、`USER32`、`GDI32` 与 MSVC Debug 运行库， 没有 ViKey、网络、Shell 或提权依赖；`git diff --check` 通过。
- 提交：`10ce729 [P2.18]add the ViKey credential configurator`。

### 小切片 18b：P2 内部开发测试包

- 原文：第 5651～5725 行。
- 18a 已提交为 `10ce729`。随后要求制作可供其他人下载测试的包。本切片先交付 **Windows 11 x64 内部开发测试包**，不是公开发布或生产安装程序：主程序仍是联网开发模式， 没有代码签名，VeraCrypt 驱动与 WebView2 Runtime 仍是机器前置条件。
- 新增 `package-p2-development.ps1`：只接受普通非 reparse 文件／目录；强制 package-release cache 形态；固定拒绝非白名单 ViKey x64 DLL 与非审计 FormatDLL；要求部署 HTML/JS 与源码摘要一致，并验证 VeraCrypt 源码基线为 `b48e31f5...` 后复制上游 License/NOTICE、项目补丁和说明。 后续纳入的 VeraCrypt 安装程序同时固定 SHA-256、产品版本／原文件名及 IDRIX Authenticode 签名证书指纹，暂存时统一命名为 `prerequisites\VeraCrypt Setup 1.26.29.exe`，不会自动执行。
- 首次含中文便条的 `preview3` 在解压校验时暴露了清单编码缺陷：文件本身存在，但 ASCII `SHA256SUMS.txt` 把文件名写成 `?????.txt`，逐项验证按预期失败。构建身份与全文件清单均改用 UTF-8 后生成 `preview4`；新目录解压后的 19 个清单项全部复核通过，中文便条路径保持完整，且未发现 PDB/ILK/LIB/EXP/OBJ。部署 HTML/JS 与 `ui/` 源文件逐字一致；失败的 `preview3` 目录、ZIP 和验证展开目录随后均已删除，避免误交付。
- 风险仍为：USB Vault 自身的 CMD/HTML/EXE 均尚未签名，摘要不能建立整包发布者身份；附带上游安装程序扩大了下载体积与版本过期面，且 VeraCrypt 聚合分发／source-offer 和 ViKey DLL 再分发授权仍待正式法律复核。因此只允许受控内部测试，不能发布为客户正式安装包。
- ⚠️ 当前 preview5 的 `BUILD-IDENTITY.txt` 诚实记录为 `fd6078d + dirty working tree`（该哈希已随提交信息重写失效，现对应 `10ce729`），因为打包链本身尚未提交。该切片提交后，应从 clean tree 重新生成最终预览名与摘要，再交付下载； 现阶段不把 preview5 描述为最终客户包。
- 提交：`4c49834 [P2.18]add the internal development test package`，未推送。

### 小切片 18c：提交信息格式统一

- 原文：第 5726～5752 行。
- 发现 `ce7390ba` 正文里的 `\n\n` 是字面量而非换行（PowerShell 下 `git commit -m` 不转义）， 并以 `fab54a4` 为范本定下提交信息规范：正文每段一个物理长行、段落之间真空行、 50–80 个英文单词、固定两段（一段说改了什么，一段说验证结果）。
- 审计 `022fcc4..HEAD` 共 20 笔：2 笔已完全合规、1 笔仅字面量 `\n` 需修、 10 笔正文不足 50 词需扩写（`022fcc4` 完全没有正文）、6 笔超 80 词需压缩， 另 1 笔为本轮新提交的 18b。
- 扩写素材全部取自本文件各切片记录，不从 diff 反推；压缩时保留有安全含义的事实 （不泄露登录身份、写后回读、挂载时拒绝重置、先写卡再建容器的顺序理由）， 砍掉的是设计论证与逐条测试清单。
- 本文件 24 行共 25 处失效短哈希按 old→new 映射重写，9 处 `待定`／`待审阅决定` 占位按规则 6 补齐为 `- 提交：`<hash> <subject>``。
- 提交：`895bc27 [docs]unify the commit message format`。

### 小切片 19a：B 类容器改为用户自选路径且不限个数

- 原文：第 5753～5811 行。
- 本轮要求把 B 类容器从「固定 `container_b.hc`、全机只能建一个」改成与 A 类一致： 用户自选路径、创建个数不设上限。
- 会话状态里的 `container_b_password` 改名为 `hidden_container_password`， 语义从「那一个 B 的密码」变成「所有隐藏卷容器共用的卷密码」，并改为**首次使用时才派生**： 容器可以在任意路径，登录时无从判断是否存在，不应为可能用不到的派生付出一次 Argon2id。 会话本来就为改写卡记录保留着登录口令明文，惰性派生不新增任何驻留秘密。
- 会话层新增：多容器（两个不同路径、各自盘符、第二次只派生诱饵那一侧、同路径仍拒绝重复接管）、 导入回退到登录派生并记住卷类别、两种派生都打不开时如实报错； 原「导入拒绝固定 B 路径」一例被上述两例取代。登录不再预派生，相关事件序列同步更新。
- 🔴 **遗留风险（8.7.6，本切片未解决且因多容器而放大）**：卡记录恒定两个信封，每建一个 B 都会用当次输入的诱饵密码改写主信封。放开个数后，若用户第二次输入了**不同的**诱饵密码， 先前所有 B 的外层卷将不可再打开，且按 8.7.3 产品不得校验、检测不出来。 现阶段只由页面上那句固定文案「若您曾设置过诱饵密码，此处需与曾经的诱饵密码保持一致」承担， 8.7.6 的两个候选方案仍待客户拍板。
- 提交：`5f7f7bf`（`[P2.19]create hidden containers at user-chosen paths`）。

### 小切片 19b：容量列改为报告已挂载的卷容量

- 原文：第 5812～5856 行。
- 实机验证 19a 时发现：总容量 128 MiB、隐藏卷 96 MiB 的 B 类容器，无论挂的是隐藏卷还是外层卷，app 的「容量」都显示 128 MiB，而资源管理器对隐藏卷显示 96 MiB。
- 会话运行时接口新增 `mounted_volume_size(wchar_t)`：按盘符在驱动的 `MountedVolumes` 里取 `disk_length`，驱动没有该盘符时返回空。文件回答不了这个问题——隐藏卷小于宿主容器， 标准卷也比文件小一个头部组。
- 会话层新增：128/96 的隐藏卷容器与 64 的标准容器同时在挂，断言分别报 96 与 64 （卷容量而非文件大小）；卸载后报 0；驱动丢掉盘符时 `container_statuses()` 抛 `std::runtime_error`。测试替身据此改为按盘符维护一张卷容量表，挂载写入、卸载抹除。
- 已挂载时真实密码下显示 96、诱饵密码下显示 128，这一差异只对已经通过认证的那一方可见， 且与资源管理器早已显示的口径一致，未引入新的可观测差异；两类容器的显示规则完全相同， 观察者无法从这一列区分 A 与 B。
- 提交：`11b0780 [P2.19]report the capacity of the attached volume`。

### 小切片 20：建卷位置改由页面填写名称与路径

- 原文：第 5857～5923 行。
- 本轮要求把建卷时的原生保存对话框换成 `usb-vault_ui-design.html` 里的表单形态： 在应用内填「名称」与「路径」，右侧「浏览」按钮打开原生选择框只负责填充字段。
- 新增 `include/usb_vault/ui/container_target_path.h` 与 `src/ui/container_target_path.cpp`： 纯字符串的 `resolve_container_target_path(directory, name)`，把保存对话框原先在结构上给出的保证逐条写成显式检查——只收盘符绝对路径（拒 UNC、`\\?\`、`\\.\` 与一切相对形式）， 拒 `.`／`..` 层级、空层级、以空格或点号收尾的层级、保留设备名（含带扩展名形式）、 控制字符、名称中的分隔符与通配符，整条路径限 259 个 UTF-16 码元（下游走无 `\\?\` 前缀的 Win32 调用）。扩展名 `.hc` 由本单元追加，页面无权决定文件如何被识别。
- 新增 `container_target_path_tests`：接受形态（含中文名、已带 `.hc`、其它扩展名、 `console`／`COM0` 这类仅形似设备名者）与拒绝形态（UNC、`\\?\`、`\\.\`、相对路径、 `..` 穿越、空层级、空格点号收尾、CON/NUL/COM1/LPT9、控制字符、超长路径）逐条覆盖。
- **零痕迹义务未随路径转移**：浏览框仍不带 client GUID、仍带 `FOS_DONTADDTORECENT`； 四个新输入框全部 `autocomplete="off"`；名称与路径不写本地／会话存储； 跨边界 BSTR 就地清零；页面契约测试把「关闭浏览器辅助」的计数从 4 提到 8 并断言。
- 提交：`cdeb4b3 [P2.20]name and place new containers on the form`。

### 小切片 21：建卷内层容量下限在会话层收口

- 原文：第 5924～5959 行。
- 本切片来自 2026-09-06 关于容器路径记忆的讨论。讨论中曾把这条判成「唯一的下限检查只在页面侧、一个 0 会一路走到 FormatDLL」，**核对后不成立**：控制器层 `card_product_ui_session_controller.cpp:227` 已经拒绝 `hidden_size_mib == 0`， 页面那条路到不了 FormatDLL，P2.20 立的「原生侧是唯一裁决」没有被违反。
- `card_login_mount_session_tests` 的无效尺寸对表补 `{1024, 0}`。原有四对覆盖不到它： `{0, 0}` 与 `{513, 0}` 都先被 outer 的非零／对齐检查拦下，`{1024, 513}` 与 `{1024, 1024}` 的 hidden 非零。改动前该用例会失败（不抛异常），是真回归用例。
- 按本轮定的测试纪律**只构建并运行这一个目标**，实跑通过、退出码 0。 全套留到 P2.27，本切片**未跑**其余目标、未构建 dev app、未实机验证。
- 无可观察行为变化：被新拒绝的输入在控制器层早已被拒，产品经由页面到达不了这个分支。
- 提交：`0b16dd8 [P2.21]refuse a zero-length hidden volume in the session layer`。

### 小切片 27：路径记忆收尾 —— 记住容器位置开关、全套测试、主文档回写

- 原文：第 5960～6091 行。
- P2.27 在 `MILESTONES.md` 里的定义是收尾三件事：全套测试、实机、主文档回写四项 （8.7.4 改为逐条密封／8.7.6 选案／Q-04 澄清／新增「A 类容器之间无隔离」告知项）。
- `tests/card_product_webview_integration.html` 是集成测试自己的极简夹具页（176 行，只放产品脚本要查的那些 id），**不是产品页的拷贝**。P2.26 往产品页加了「从列表移除」弹窗， 夹具页没跟着加，于是 `card_session.js` 启动时在 `forgetCloseButton.addEventListener` 上对 null 取属性抛异常 —— 页面再也没报出过状态，驱动第一步 `waitForState("no-card")` 超时。 已按夹具页自己的极简风格补上那六个 id。
- **全套（本计划中唯一一次）**：Debug **50/50**、MSVC `/analyze` **50/50**（无项目诊断）、 ASan **48/50**。ASan 的两处失败是 `vikey_helper_process_tests` 与 `vikey_helper_hid_compatibility_tests` 下 helper 子进程 `0xC0000135`（DLL not found）的既有环境问题，与本片无关，Debug 与 `/analyze` 下这两项都过。
- 🔴 **`/analyze` 首次覆盖 P2.22～P2.26 的新测试文件，查出两处并已修** —— 这正是「每片只跑最小目标、全套留到收尾」这条纪律要在收尾这一次付的账： 1. `container_index_v1_tests.cpp` 的格式常量对比触发 C6326（两个常量相比）。改为 `static_assert`：它们本来就是编译期常量，钉在编译期比钉在用例里更强，漂了就不该链出一个能跑的用例。 2. `card_login_mount_session_tests.cpp` 的多登录用例触发 C6262（单帧 19452 字节）。 根因是一个会话核心把整份操作历史按值带在身上，两个并排就超了 `/analyze` 的 16 KiB。
- **未做：实机**。`usb_vault_vikey_p2_development_app` 已点名重建（`out\build\dev`， 与 UI 资产同批刷新），真卡走查待实机执行。
- 提交：`0427d77 [tests]pin the container index format at compile time`、 `4c1c15c [P2.26]restore the elements the integration fixture page lost`、 `145b63b [P2.27]remember whether to remember where containers are`、 `2a9881c [ui]say where a dropped container file stays`； `4833456 [docs]write the container path memory decisions back`。

### 小切片 28：导入不再自动挂载，改由设置页的开关决定

- 原文：第 6092～6170 行。
- 需求来自用户：「导入容器默认不自动挂载，做成一个按钮开关在设置页『自动尝试挂载导入的容器』」。
- 新增 `adopt_container(path)`：查存在 → 以未挂载身份进列表（`derivation_known=false`）→ 记路径。**不派生、不占盘符、不挂载**，也**不写会话日志** —— 没有卷被碰过，列表里多出的那一行就是事件本身，与登录时从索引读回的条目完全同路。
- `card_product_ui_page_contract_tests`：开关计数从 3 改 4、`data-setting` 从 2 改 3， 新开关必须 `aria-checked="false"` 出厂且两个方法都到桥；原「记住容器位置」那条契约随页面改为表驱动而改钉 `refreshStoredSwitches` 与 `stored.write`。
- **默认值方向是收紧而非放宽**：改动后产品在用户没有明确要求时**更少**打开卷， 与 8.7.5 的默认、以及 8.7.4 "读回的条目不自动挂"一致。
- 提交：`9a5d197 [P2.28]let an import stop at the list`。

### 决策记录：8.4 本地状态层的边界（2026-09-07）

- 原文：第 6171～6209 行。
- 本条不是代码切片，是一次范围决定的留档。起因是讨论"下一步做导出还是做应用内文件管理器"时， 此前引用 8.7.5 的原话说文件管理器"会新增『最近打开』一类本地状态泄露面"，随后被追问具体机制。
- **决定：①②明示接受**。据此在 8.4 增设「本层的边界」：本层只约束**本产品自己写下的** 产物；系统与第三方程序因用户使用已挂载卷而写下的痕迹不计入，**包括由我方界面触发、 但由 Windows 写下的情形**（这正是文件管理器能用 shell 组件的前提）。
- `需求梳理清单_v2.md` 8.4：新增「本层的边界」小节（三类豁免、触发方与写入方的区分、 对我方产物不放宽的红线、以及与 Q-04 同型的书面确认留档提醒）。
- 该决定目前只是项目内决定，**未取得客户书面确认**。8.4 新小节里已按 Q-04 的先例写下留档提醒。
- 提交：`b061989 [docs]bound the local state layer to what this product writes`。

### 决策记录：新设 P3「数据管理」，原 P3 顺推为 P4（2026-09-08）

- 原文：第 6210～6278 行。
- 本轮要求开工侧栏那个「文件管理」入口。它在页面上一直是 `nav-item-disabled` 占位 （`ui/card_session.html:604`），而 8.7.5 把应用内文件增删改移降级为 v1.1，v1 只留只读状态展示。 把它做出来是范围变更，已明示并拍板提前。
- 1. **P2 的出口条件里写着「备份导出全部可用」，而它一行代码都没有** —— `src/` 下搜不到任何导出或迁移实现，页面侧栏的「备份中心」和「文件管理」一样是禁用占位。P2 欠着这笔账。 2. **把文件拖出加密卷就是明文导出**，Q-04 的三条可验收项当场生效（不可跳过的风险确认屏、 隐藏盘导出单独二次确认、导出动作零本地痕迹）。而 Q-04 明写这套确认屏"可与 Q-02 使用同一确认组件"。文件管理器与备份中心共用同一个受 Q-04 约束的边界。
- `MILESTONES.md`：新增第五节 P3「数据管理」（出口条件、合并理由、四项已定事实、P3.1～P3.10 切片表、P3.10 四项回写清单、P3.11 起的导出与迁移）；原第五节 P4 及其后各节顺推编号；总览表新增 P3 行并收窄 P2 行；P2 出口条件删去"备份导出"，两行导出任务移入 P3；第七节范围演进史新增 2026-09-08 一行；第八节工期风险条注明本次提前直接加在其上。
- P3.11 起的导出与迁移尚未拆片。
- 提交：`7d89a50 [docs]open P3 for the data-management work`。 ---

### 小切片 P3.1：卷内路径解析

- 原文：第 6279～6338 行。
- P3 的第一片，纯算法层，不碰文件系统。新增 `usb_vault::volume::resolve_volume_entry_path` 与 `resolve_volume_child_path`，把「会话认定的盘符 + 页面递来的相对路径」变成一条绝对路径， 并拒绝页面能说出口的一切越界写法。它是 `resolve_container_target_path` 的对应物：那一个管建卷表单，这一个管卷内容。
- 第一次变异把 `.` / `..` 的显式拒绝改坏，**测试照样通过** —— 因为「组件不得以点结尾」那条规则先把 `..` 和 `.` 都兜住了。换成拿掉禁用字符表里的冒号后测试立刻报 4 条失败，并确认失败时进程退出码为 1（不是 0，否则 CTest 会把它当通过、整套测试形同虚设）。
- 显式的越界检查**保留**：它给出更准的错误消息，且「以点结尾」那条规则将来若改动，越界不能跟着一起失守。但由此记下一条本套测试的已知边界：`require_rejects` 只断言抛了 `std::invalid_argument`，**不区分「因正确的理由被拒」和「被另一条规则顺手拒掉」**。这是沿用 `container_target_path_tests` 的既有写法，不是本片新引入的。
- 新增 `tests/volume_entry_path_tests.cpp`（6 组：join 与规范化、越界与 rooted、组件不是名字、 子名必须恰好一个组件、盘符校验、Win32 长度上限）。
- 提交：`2855467 [P3.1]refuse every page path that could leave the volume`。 ---

### 小切片 P3.2：目录枚举，并在这里真正堵住卷外逃逸

- 原文：第 6339～6405 行。
- 新增 `usb_vault::volume::list_volume_directory`：给一条驱动器绝对路径，回一份 `VolumeDirectoryListing`（名称／是否目录／是否重解析点／大小／修改时间，外加一个 `truncated`）。 `.` 与 `..` 丢掉，顺序是「目录在前，然后按序数不分大小写」，条目上限 20000。
- 第一次把越界比较改坏，测试**照样通过**，一度以为 subst 用例是假的。查下来是**变异方法坏了**： 构建命令写了 `>nul 2>&1`，而那个变异触发了 `/WX` 下的告警导致构建失败，跑的还是旧 exe。
- 用 `DefineDosDeviceW` 造一个指向临时目录的盘符（subst 的机制），它与卷内 junction 触发的是 **同一个判据** —— 最终路径落在别的盘 —— 但不需要第二块物理卷、不需要任何权限，在任何机器上都确定可跑。另外用 `FSCTL_SET_REPARSE_POINT` 手搓了一个卷内 junction（结构体 `ntifs.h` 里才有， 用户态 SDK 没有，故自行声明），用来验证 `is_reparse_point` 如实上报。两个用例本次实机都真的执行了，没有走 skip 分支。
- 新增 `include/usb_vault/volume/volume_directory_listing.h`、 `src/volume/volume_directory_listing.cpp`。
- 提交：`ded4a7b [P3.2]ask the file system which volume it actually opened`。 ---

### 小切片 P3.3：删除、移动、新建目录，且递归删绝不钻进链接

- 原文：第 6406～6466 行。
- 新增 `usb_vault::volume` 三个入口：`delete_volume_entry`、`move_volume_entry`、 `create_volume_directory`。同时把 P3.2 里的越界检查抽成内部共用件 `src/volume/volume_containment.{h,cpp}`，因为列表和这三个操作问的是同一个问题。
- **只读文件照删**，先清 `FILE_ATTRIBUTE_READONLY`。Windows 上只读是属性不是权限，且调用方已经决定了。属性只清被删的那一个条目，不波及别处。
- 新增 `include/usb_vault/volume/volume_file_operations.h`、 `src/volume/volume_file_operations.cpp`。
- 按 P3 测试纪律只跑本片最小目标：`review-clean`（Ninja + Debug + `/W4 /WX`）编译无警告， `volume_file_operations_tests` 通过；连带三个 volume 目标共 4/4。另完成上述 junction 变异测试。 全套留到 P3.10。
- 提交：`6d2105b [P3.3]unlink a junction instead of walking into it`。

### 小切片 P3.4：跨容器复制与移动，且不留半个文件

- 原文：第 6467～6592 行。
- 新增 `usb_vault::volume::copy_volume_entry` 与 `move_volume_entry_by_copying`。前者把一个条目 （文件或整棵目录）复制到另一处，同卷跨卷都行；后者复制完再删源，就是跨容器的「移动」。两者都收一个 `TransferCancellation`（`std::function<bool()>`），页面将来要用它做取消。
- **中途失败**：`ExclusiveFile` 以不共享（share mode 0）占住树里的一个文件，`CopyFileEx` 打开它时必然 `ERROR_SHARING_VIOLATION`。不需要满盘、不需要拔盘、不需要权限。注意预走查用的是 `FindFirstFile`（不开文件），所以失败精确地发生在复制阶段 —— 正是回滚要管的那一段。
- **空间不足**：`try_make_sparse_file` 造一个**声称 8 TiB、实占约 0** 的稀疏文件。先试了 1 PB， `SetEndOfFile` 失败（超出 NTFS 单文件上限），测试走了 skip 分支；改成 8 TiB 后本次真的执行了。 📝 再记一条：**skip 分支必须打印**，否则一条从没跑过的用例看起来和通过一模一样。
- `include/usb_vault/volume/volume_file_operations.h`：新增 `TransferCancellation`、 `copy_volume_entry`、`move_volume_entry_by_copying`。
- 按 P3 测试纪律只跑本片最小目标：`review-clean`（Ninja + Debug + `/W4 /WX`）编译无警告， `volume_file_operations_tests` 通过；连带四个 volume 目标 4/4。**本次运行无任何 skip 分支** （junction、subst 盘、稀疏文件三个夹具都真的执行了）。另完成上表四次变异测试。全套留到 P3.10。
- 提交：`54691b1 [P3.4]copy across containers without leaving half a file`。

### 小切片 P3.5：接到会话，按容器 id 而非盘符寻址

- 原文：第 6593～6691 行。
- 把 P3.1～P3.4 的存储层接到 `IAuthenticatedCardVolumeSession`，新增六个入口： `list_container_directory`、`delete_container_entry`、`rename_container_entry`、 `create_container_directory`、`move_container_entry`、`copy_container_entry`。存储层本身仍走 `CardLoginMountSessionRuntime`（新增六个虚函数），所以会话这一层能在没有真实卷的情况下被测。
- `src/application/card_login_mount_session_internal.h`：运行时新增六个虚函数，核心新增六个方法与私有的 `mounted_drive_letter`。
- 按 P3 测试纪律只跑本片最小目标：`review-clean`（Ninja + Debug + `/W4 /WX`）编译无警告， `card_login_mount_session_tests` 通过；受接口变更波及的四个 UI 目标一并构建，注册为测试的三个连同本片共 4/4 通过（`card_product_webview_integration_tests` 是 `EXCLUDE_FROM_ALL` 的手动夹具，只构建）。另完成下表四次变异测试，每次都不吞构建输出且确认退出码为 1。
- 变异；测试报出
- 提交：`bb7cc9d [P3.5]let a page reach a container by its identifier`。

### 小切片 P3.6：控制器投影 + WebView 桥

- 原文：第 6692～6777 行。
- 把 P3.5 的六个会话入口镜像到 `CardProductUiSessionController`，再镜像到 WebView 宿主对象 （`listContainerDirectory`／`deleteContainerEntry`／`renameContainerEntry`／ `createContainerDirectory`／`moveContainerEntry`／`copyContainerEntry`，DISPID 36～41）。 两者是机械镜像，所以合成一片。
- 这里是**拒整份列表**而不是跳过那一条。跳过会让页面画出一个与卷里实际内容不同的目录，而这正是这类攻击想要的效果；拒绝则会说出来。代价是一个坏名字挡住整个目录 —— 在一个安全产品里这个取舍是对的，且已写进注释。
- `src/ui/card_product_webview_host_object.cpp`：六个 DISPID 与名字、`pack_directory_listing`、 `DispatchTextWiper`、四个 invoke 处理（删除与新建目录共用一个，移动与复制共用一个）。
- 按 P3 测试纪律只跑本片最小目标：`review-clean`（Ninja + Debug + `/W4 /WX`）编译无警告， `card_product_ui_session_controller_tests` 与 `card_product_webview_host_object_tests` 均通过； 受核心改动波及的 `ui_close_guard_tests` 一并构建并通过，`card_product_webview_integration_tests` （`EXCLUDE_FROM_ALL` 的手动夹具）只构建。另完成上表四次变异测试，每次都不吞构建输出且确认退出码为 1。全套留到 P3.10。
- 提交：`9a109c1 [P3.6]bring a whole directory across the bridge in one call`。

### 小切片 P3.7：启用侧栏入口，做出浏览主视图

- 原文：第 6778～6869 行。
- 侧栏「文件管理」从 `nav-item-disabled` 占位改成真入口（`data-section="files"`），新增 `dashboard-section-files`：容器选择器、面包屑、列表、以及三个态 —— 没有容器／容器未挂载／ 文件夹为空。「备份中心」仍是占位，那是 P3.11 起的事。
- `listingRecordSeparator = "{B}u001e"`、`listingFieldSeparator = "{B}u001f"`，与 P3.6 的桥一一对应。 `parseDirectoryListing` 校验：头记录只能是 `"0"`／`"1"`，每条记录恰好 5 个字段，名字非空， 两个布尔位只能是 `0`／`1`，大小与时间必须是安全整数。**不合规就抛**，不猜、不容错 —— 一份读错的目录列表会让页面显示出与卷里实际内容不同的东西。
- 页面契约测试是**文本断言**：它能钉住「这段代码在」，钉不住「这段代码跑起来是对的」。所以本片额外做了一件事：用 node 把 `parseDirectoryListing` 从已发布的 `card_session.js` 里原样切出来求值， 喂给它**P3.6 宿主对象测试里钉死的那个打包串**，验证解析结果逐字段正确，并确认它拒绝空串、非法头、字段数不对、布尔位非法、空名字五种畸形输入。
- `ui/card_session.js`：四个元素查找、`parseDirectoryListing`、容器选择／面包屑／列表三个渲染、 `refreshFileManager`、三个监听器；`showDashboardSection` 记录当前分区并在进入 files 时取一次列表；`renderContainers` 顺带同步选择器。
- 提交：`785e25d [P3.7]open the file manager on a container the page cannot name a drive for`。

### 小切片 P3.8：删除与重命名

- 原文：第 6870～6946 行。
- 列表每行加「重命名」「删除」两个按钮，工具栏加「新建文件夹」，重命名与新建共用一个取名对话框 （`file-name-dialog`）。
- 但**页面必须把这件事说出来**，所以分区里加了一条常驻提示：「删除是永久删除，不进回收站，点下去就没有第二次询问 —— 与系统文件管理器同标准。」常驻提示不是二次确认，它不拦任何东西；它只是让按钮旁边站着一句实话。契约测试同时钉住两头：**这句话必须在**，以及**页面上不许出现 `file-delete-dialog`** —— 后者是为了让「将来有人悄悄加回确认屏」这件事必须先改测试、也就必须先回头看那条决策。
- 变异；测试报出
- 按 P3 测试纪律只跑本片最小目标：`card_product_ui_page_contract_tests` 通过。另完成上表三次变异测试、`node --check` 语法检查，并重跑了 P3.7 那个跨层解析验证（格式未变，仍然对得上）。 全套留到 P3.10。
- 提交：`8116f0d [P3.8]delete once and rename in place`。

### 小切片 P3.9：移动与拖放，含跨容器，且拖出去时指路

- 原文：第 6947～7023 行。
- 行可拖，落点有三种：**本容器的子目录**（行本身）、**面包屑上的任一祖先**（往上移）、 **另一个已挂载容器**（列表上方那条虚线拖放条）。按住 Ctrl 拖为复制。
- 🔴 **异步与取消仍是未解项**，已在 `MILESTONES.md` 记为 P3.6／P3.9 的遗留，现随本片一并顺延给 P3.10 复核后决定放 P4 还是补做。
- `ui/card_session.js`：`transferFileEntry`／`renderFileDropTargets`／`reportFileDropRefused` 与高亮托管，行加 `draggable` 与两个 dataset，三组 `dragover`／`drop` 监听 （列表／面包屑／拖放条）、`dragstart`／`dragend`，`runFileAction` 加一次让出。
- 按 P3 测试纪律只跑本片最小目标：`card_product_ui_page_contract_tests` 通过。另完成上表三次变异测试与 `node --check`。**拖放本身是纯交互，契约测试只能钉住代码在不在、数据给不给，给不出「拖起来是对的」—— 这一条必须留到 P3.10 实机。** 全套留到 P3.10。
- 提交：`922c284 [P3.9]let a drag cross containers and never leave one`。

### 小切片 P3.10：全套测试与主文档回写（实机未做）

- 原文：第 7024～7089 行。
- P3 文件管理器的收尾。**三件事里做完两件**，第三件当前做不了，如实记在这里。
- `review-clean`（Ninja + Debug + `/W4 /WX`）全量构建无警告，`ctest` 53 个目标全过，耗时 216.9 秒。 这是本计划中唯一一次全套（P3.1～P3.9 每片只跑自己的最小目标，沿用 P2.21～P2.27 的做法）。
- 1. **8.7.5 的降级条撤销** —— 原文那段"建议降级为 v1.1"的论证不再留在正文，改写为已实现的定稿： 实现范围、只管容器内部的边界、四项已定事实的**逐条落地情况**，以及未解项。原论证连同它是怎么被推翻的（③ 泄露面那条 2026-09-07 自行推翻）整段移到条目末尾留档，工期风险那句照旧保留。 2. **第五节条目表**那行（`需求梳理清单_v2.md:1625`）同步为「P3.1～P3.9 已实现并提交， 8.7.5 正文已回写」。
- `MILESTONES.md`：P3 阶段状态、P3.10 行、四项回写清单标完成，新增「图标自绘未做」一条。
- 提交：`0be84e1 [P3.10]run the whole suite and write the file manager back into the docs`。

### 计划：P3.11 起「导出与迁移」拆片（2026-09-09）

- 原文：第 7090～7145 行。
- 文件管理器落地后，按 `MILESTONES.md` 原本写的「待文件管理器落地后再排」把这一段拆成 P3.11～P3.17。 **本条只是计划，没有任何代码改动。**
- **提议：每一次明文导出都无条件走两道确认。** 与 8.7.3「创建 B 时新设的第二个密码不得验证」 是同一种思路 —— **宁可无条件，也不要有条件，因为条件本身即档位**。它比 Q-04 的字面要求更严格 （加密盘导出也要两道），代价是标准容器导出多点一次。
- **这一条改的是产品交互，已标 🟡 待拍板，本轮不替产品侧定。** 若按字面实现，那就必须接受 「弹屏次数即档位」这个泄露面，并写进 8.4 的已接受清单 —— 但那与 8.7.2 的既有口径直接冲突， 因此不该那样做。
- 其中 **P3.13 单独成片**是有意的：Q-04 第 3 条明说「导出动作不得留任何本地痕迹」**不属于 "用户自担风险"的范畴，是我方实现义务**。把它混进别的片里，很容易变成"顺手看一眼"。 单独一片，且要求逐条确认：不进 `OperationEvent`（封闭枚举里本就没有 export —— 这一片要确认它没被偷偷加上）、不写索引、不写设置、不留目标路径。
- 提交：`bd7540b [docs]slice the export work and name the leak in asking twice`。

### 🔴 事故：`review-clean` 构建树的头文件依赖一直是失效的（2026-09-09 查出并修复）

- 原文：第 7146～7203 行。
- 做 P3.11 时两个 UI 测试目标突然失败，症状离奇：控制器调 `container_statuses()` 却执行到了本片新加的 `export_container_entry` 桩上。**这是 vtable 错位** —— 两个翻译单元看到的接口不是同一个。
- 1. **删掉重配**：`chcp 65001` + vcvars 后重新配置 `review-clean`，前缀重新探测。 2. **新增 `review-clean` 预设**（configure／build／test 三处），于是它能且只能走 `tools\configure-preset.cmd review-clean` 这条带闸的路配出来。原先它无法用预设配置， 这正是当初被手工配坏的原因。 3. **`MILESTONES.md` 写明**该目录必须用那个工具配。 4. **写了一个依赖探针**：只改头文件的注释，看包含它的目标文件会不会被重编。 修复前会答 `DEPS BROKEN`，修复后答 `DEPS OK`。
- **全部重跑了一遍**：从零配置、从零构建、全套 `ctest` —— **53/53 通过，零警告，耗时 213 秒**， 其中包含刚才那两个失败的 UI 目标。所以：
- P3.10 记的「全套 53/53 通过」**成立**，本次干净重跑复现了它；
- 提交：`0a1b0c3 [build]give the clean test tree a preset so its deps cannot rot`。

### 小切片 P3.11：会话层的明文导出入口

- 原文：第 7204～7291 行。
- 新增 `export_container_entry(id, 相对目录, 名字, 绝对目标目录)`：把已挂载容器里的一个条目复制到本机上的普通位置。**这就是 Q-04 说的明文导出**，写出去的东西不再受本产品任何机制保护。
- `record_operation` 一次都没调，索引没写，设置没写，目标路径没留。Q-04 第 3 条明说这**不属于 "用户自担风险"的范畴，是我方实现义务** —— 用户接受的是"备份可能被人找到"，**不是**"软件替他记下了他备份过隐藏盘"。测试直接断言导出前后 `operation_history()` 的长度和索引写次数都不变。
- 第三次变异复验时，`volume_entry_path_tests` 显示 exit=0 却打印了失败 —— 那是把 `./exe | tail` 的退出码当成了 exe 的退出码。改成分开跑后确认是 1。**管道会吃掉退出码**， 与"变异测试不能吞构建输出"是同一类错误的两个面。
- `include/usb_vault/application/card_login_mount_session.h`、 `src/application/card_login_mount_session_internal.h`、`.cpp`：新增导出入口与 `upper_drive_letter`。
- 提交：`9a092b1 [P3.11]give the plaintext export its own name`。

### 小切片 P3.12：密文迁移导出

- 原文：第 7292～7359 行。
- 新增 `export_container_file(id, 绝对目标目录)`：把一个已收录容器**自己的文件**逐字节复制到用户选定的位置。这是 Q-04 2026-08-24 补充的那条路 —— 用户带去另一台机器，用**原卡 + 原口令**继续挂载。
- ⚠️ 顺带一提：即便不拒，VeraCrypt 多半也持有该文件句柄，`CopyFileEx` 会撞共享冲突。**但靠对方碰巧失败不是保证**，把它写成一条明说的规则才是。
- `include/usb_vault/application/card_login_mount_session.h`、内部头、`.cpp`：新增迁移入口。
- 按 P3 测试纪律只跑本片最小目标：`card_login_mount_session_tests` 通过。受接口变更波及的四个 UI 目标一并构建并运行，全过 —— 这也顺带确认了上一条修好的头文件依赖真的在工作：加一个纯虚函数， ninja 这次自己把控制器重编了。另完成上表三次变异测试。全套留到 P3.17。
- 提交：`cf2e1aa [P3.12]copy a container out without opening it`。

### 小切片 P3.13：零痕迹核查

- 原文：第 7360～7433 行。
- Q-04 可验收项第 3 条：**导出动作不得留任何本地痕迹**。它单独成片，是因为 Q-04 正文明写这条 **不属于"用户自担风险"的范畴，是我方实现义务** —— 用户接受的是"备份可能被人找到"， **不是**"软件替他记下了他备份过隐藏盘"。混进别的片里，它会退化成"顺手看一眼"。
- 变异；结果
- `tests/card_login_mount_session_tests.cpp`：新增零痕迹一组用例、`fail_copy` 夹具开关、 四个枚举值的 `static_assert` 与封闭性 switch（含 C4062 局部提错）。
- 按 P3 测试纪律只跑本片最小目标：`card_login_mount_session_tests` 通过。受前两片接口变更波及的四个目标一并构建运行，全过。另完成上表三次变异测试。全套留到 P3.17。
- 提交：`9b7b004 [P3.13]ask every export whether it wrote anything down`。

### 小切片 P3.14：导出的控制器投影 + WebView 桥

- 原文：第 7434～7485 行。
- 把 P3.11／P3.12 的两个导出入口镜像到控制器，再镜像到宿主对象： `exportContainerEntry`（4 参，DISPID 42）与 `exportContainerFile`（2 参，DISPID 43）。 与 P3.6 一样是机械镜像，合成一片。
- 变异；测试报出
- `tests/card_product_ui_session_controller_tests.cpp`、 `tests/card_product_webview_host_object_tests.cpp`：假会话由抛桩改为记录，各新增 2 组用例。
- 按 P3 测试纪律只跑本片最小目标：两个目标均通过（`/W4 /WX` 编译无警告）。受接口变更波及的 `card_product_ui_page_contract_tests`、`ui_close_guard_tests` 与集成夹具一并构建，前两个运行通过。 另完成上表三次变异测试。全套留到 P3.17。
- 提交：`7ffd22c [P3.14]mirror both exports onto the controller and the bridge`。

### 小切片 P3.15：备份中心，以及在那里发生的明文导出

- 原文：第 7486～7553 行。
- 侧栏「备份中心」从占位改成真入口 —— **至此侧栏没有任何占位了**。新增 `dashboard-section-backup`：一条常驻风险提示，加一个明文导出面板（要导出什么／导出到哪／ 选择位置／导出）。
- Q-04 可验收项 1 是 🔴「**不可跳过的**明文导出风险确认」，项 2 是隐藏盘的二次确认。 **2026-09-09 决定：不弹确认。** 决定前已把冲突讲清楚（去掉的是两条可验收项， 其中项 1 明标不可跳过；而项 3「不得留任何痕迹」意味着不能记日志，所以确认屏本身就是 Q-04 说的那个"举证依据"），重申继续。决定与冲突已记入 `MILESTONES.md`，本条再记一次。
- `ui/card_session.js`：六个元素查找、行末「导出」按钮、`offerEntryForExport`／ `renderBackupCentre`／`runExport`、两个监听器，`showDashboardSection` 进入 backup 时重绘。
- 按 P3 测试纪律只跑本片最小目标：`card_product_ui_page_contract_tests` 通过。另完成上表三次变异测试、`node --check`，并重跑了跨层解析验证。全套留到 P3.17。
- 提交：`21924b6 [P3.15]put the plaintext export where it can be said out loud`。

### 小切片 P3.16：页面的密文迁移导出

- 原文：第 7554～7615 行。
- 备份中心加第二个面板：选容器、选位置、导出 —— 把整个容器文件逐字节复制出去。
- 换机后仍须**原安全卡 + 原口令**；
- 这已经是本轮第三次了（P3.5 的 `invalid_argument` 派生自 `logic_error`、P3.13 的 C4062 默认关闭、 本片的位置断言太松）。**规律很清楚：变异测试在这个项目里主要不是在查代码，是在查断言写得够不够锋利。** 三次全都是"护栏看着在，其实没装上"。
- `tests/card_product_ui_page_contract_tests.cpp`：新增一组迁移契约；"忙态先上屏"那条计数 2 改 3，并把消息改成说清这个数字的含义（现在有三处会在绘制线程上搬字节）。
- 提交：`7805eec [P3.16]carry a whole container out, still encrypted`。

### 小切片 P3.17：导出与迁移的收尾（实机未做）

- 原文：第 7616～7681 行。
- 与 P3.10 同型：**三件事做完两件，第三件当前做不了。**
- 1. **不可跳过的明文导出风险确认 —— 🔴 未落地。** 2026-09-09 决定不弹确认屏。 决定前已明确告知：去掉的是第 1、2 两条，且第 1 条明标"不可跳过"；而第 3 条"不得留任何本地痕迹"意味着不能记日志，**所以这个确认屏本身就是 Q-04 说的那个"出事时的举证依据"**。 重申继续。须明示的那段话以备份中心的常驻提示保留。**建议走书面件**。 2. **隐藏盘的二次确认 —— 🔴 未落地**，随第 1 条一并去掉。另把拆片时发现的那个技术矛盾记在了条目里：按字面实现会**泄露档位**（弹几道屏就说出了容器是不是隐藏盘）， 将来若恢复确认屏**不要按字面做**，走"无条件两道"那条。
- P3.10 和 P3.17 都卡在实机上，而"实机验证"四个字不是可执行的指令。所以在 `MILESTONES.md` 里汇总了一份 **14 步清单**，两阶段共用，其中标红的四条是自动化测试**结构上**到不了的：
- `MILESTONES.md`：P3.17 行、P3.11 起小节标题、第五节阶段状态、阶段总表那一行、 自 P2 移入的两行补状态列，新增实机验证清单。
- 提交：`56b2db5 [P3.17]write the export back into the requirement it came from`。

### 四项同型决策的沿革汇总（2026-09-10）

- 原文：第 7682～7741 行。
- Q-04 与其后三项决定同属"用户自担风险／范围变更"型，此前散在三个文件里、互不相邻， 从未并排看过。本轮把它们的**原决策 → 现决策**汇总一次，便于回头核对改了什么。
- 不改任何决定，也不改各项的既有记录 —— 完整论述仍以各自正文为准，本条只是索引。
- **现**：仅明文导出，用户自担风险。F-037 与之直接矛盾且不可调和，删除。 2026-08-24 增设密文迁移导出 —— 原样逐字节复制既有容器，不解密不重加密， 不新增密钥备份或恢复通道，换机后仍须原卡原口令。
- **现**：收窄为只约束**本产品自己写下的**产物。Windows 与第三方程序因为用户使用已挂载卷而写下的痕迹明示接受，不计入差异 —— thumbcache、shell 最近使用项与跳转列表、 第三方程序自己的 MRU。**红线不放宽**：我方自己的最近打开列表、上次所在目录、排序方式仍一律禁止，复用 shell 组件时其默认 MRU 行为须逐个显式关闭。
- 提交：`f45eee1 [docs]put the four decision changes side by side`。

### UI：导出与迁移界面改动提交审查（2026-09-10）

- 原文：第 7742～7779 行。
- 决定提交当前工作区改动，使用 `[ui]` 前缀；本轮未追加产品代码改动。
- 本次作为一个界面功能边界提交：导出与迁移页面、配套契约测试与 WebView 测试页面、设计稿提示同步。
- 已检查 `tests/card_product_ui_page_contract_tests.cpp`、`tests/card_product_webview_integration.html` 与 `usb-vault_ui-design.html` 的配套差异。
- `cmake --build --preset review-clean --target card_product_ui_page_contract_tests`：通过，构建树已是最新状态。
- 原页面关于“可能击穿隐藏盘否认性”和“SSD 上覆写不保证有效”的提示已移除，当前确认弹窗只说明写出的是解密后的原始文件。Q-04 的完整风险提示要求仍有缺口，本次提交不代表消除该差异。

### UI：合并容器管理入口与仪表盘共用列表（2026-09-10）

- 原文：第 7780～7864 行。
- 本轮要求以 `usb-vault_ui-design.html` 为 UI 基准：删除独立的旧容器页，将文件管理改为容器管理，并补上仪表盘容器行的左侧拖拽与右侧详情。
- 已检查设计稿的两级容器管理、仪表盘概览与排序说明；对照 `MILESTONES.md` 的 P3 文件管理范围以及主需求 R-03、8.4、8.6、8.7.5。旧文档中的两个侧栏入口由本次指令合并，文件操作仍限于容器内部。
- 新增名称与详情值全部通过 `textContent` 渲染；选择器中的容器 ID 已由原生状态读取边界验证为正整数。未增加动态 HTML、持久化排序、目录记忆、导出记录或档位判断。
- 提交状态：未提交、未推送。
- 按要求构建 `out/p2_run_dev_app.ps1` 对应的程序：加载 MSVC 环境后，执行 `cmake --build --preset dev --target usb_vault_vikey_p2_development_app`，构建成功。

### UI：容器管理文件视图与右键重命名（2026-09-10）

- 原文：第 7865～7917 行。
- 本轮要求只调整容器管理页：移除列表中的移除与展开详情入口，单击容器进入文件视图；顶部保留原字号标题，右侧以较小字号显示可点击的逐级路径；文件表格以附图为准，仅留导出和红色删除图标，重命名改为右键菜单。
- 顶部统一为“容器管理 > 容器名 > 目录…”；标题保留 1.25rem，路径为 .875rem。包括当前目录在内的每一级都可点击，标题返回容器列表。路径只显示容器内相对层级，不暴露容器文件的绝对路径。
- JavaScript 语法及 `git diff --check` 通过；按要求不跑全套测试。P2 开发版重链接被正在运行的 EXE 占用阻止（LNK1168），本轮不能记为构建成功。
- 浏览器视觉与真实指针命中尚未验收，仍受此前本地预览策略限制；DOM 测试不代表像素级验收。
- 提交状态：未提交、未推送。

### UI：导出与迁移布局和选择交互（2026-09-10）

- 原文：第 7918～7929 行。
- 需求审计：核对 `MILESTONES.md` 的 P3 导出迁移范围及 `需求梳理清单_v2.md` Q-04、8.6 约束；本次按要求调整页面交互，不改变明文导出、密文复制、挂载限制或密钥边界，无新增需求冲突。
- 实现前检查 `ui/card_session.html` 的导出行与目标位置结构、`ui/card_session.js` 的选择状态及浏览入口，以及现有 DOM 模拟和页面契约测试。确认折叠时勾选显示与切换判断不一致导致无法取消全选。
- 盘符和挂载操作靠右排列，最右勾选框继续与文件行对齐；密文迁移改用双向箭头，列表容器改用立体盒子图标。
- 最小验证：DOM 回归 46 项通过，页面契约 1/1 通过，JavaScript 语法与差异检查通过；未跑全套。新增覆盖折叠选择/取消、跨容器隔离、展开取消、两个位置框和取消选择器。
- 提交确认：已确认提交，本轮以 `[ui]refine export and migration controls` 创建一个本地提交，包含 UI、回归测试与开发记录；沿用上述最小验证结果，不推送。

### UI：默认窗口尺寸与设置页尺寸选择（2026-09-10）

- 原文：第 7930～7949 行。
- 需求审计：检查 `MILESTONES.md`、`需求梳理清单_v2.md` 的 UI 范围与 R-03、8.6 约束，本次是新增显示偏好，无原生密钥、容器或持久化需求变化。按当前指令采用即时调整，不跨启动记忆。
- 实现前检查 `tools/app/main.cpp` 启动窗口、DPI 处理、WebView 安全设置，原生 COM 桥及其测试，以及页面侧栏 760px 折叠规则和设置页结构。原窗口以 900×640 外框创建。
- 最小验证：DOM 49 项通过；原生桥测试与页面契约各 1/1 通过；JavaScript 语法和差异检查通过。初次测试暴露测试数字类型不匹配及集成页面缺控件，修正后通过。未运行全套。
- 窗口尺寸选择框显式采用深色配色，选项使用深色背景与浅色文字，修复原生下拉出现白底的问题。
- 提交确认：已确认先提交窗口尺寸功能，以 `[ui]add window size presets and spacious startup` 创建本地提交，沿用本节最小验证，不推送；操作日志开关作为后续独立切片。

### UI：会话设置操作日志开关（2026-09-10）

- 原文：第 7950～7962 行。
- 前置提交已完成：`dc11fec`，窗口尺寸功能独立提交，未推送。
- 需求审计：复核需求清单与里程碑的日志分离、会话内日志和否认性边界；新增“开启操作日志”设置仅作用于用户可见操作日志，不能关闭安全卡恢复日志或拔卡卸载。无新增持久化要求，采用当前应用运行期间的易失偏好。
- 实现前检查会话核心 `record_operation`、日志环形缓冲区、UI 控制器、COM 布尔设置入口、设置页状态刷新和相关测试。
- 最小验证：DOM 55 项通过，会话核心、原生桥、页面契约各 1/1 通过；JavaScript 语法和差异检查通过。覆盖默认开启、关闭后操作仍执行但日志不增、重新开启记录恢复、入口隐藏/恢复、失败回退、认证和布尔类型限制。页面契约最初仍限定三个可编辑开关，更新为四个后通过；未运行全套。
- 提交确认：确认本轮操作日志开关和设置排序一并提交，采用 `[ui]add operation logging toggle and reorder session settings`，沿用上述最小验证结果，仅本地提交，不推送。

### UI：含隐藏卷创建前诱饵密码确认（2026-09-10）

- 原文：第 7963～7975 行。
- 需求审计：核对需求清单 N-06、8.7.2 及末尾重复建 B 的密码覆盖风险讨论，沿用里程碑当前隐藏卷创建范围。风险是旧密码解锁信息被替换、此前外层数据可能永久无法访问，并非直接覆盖旧容器文件；按指令改为创建前独立确认，不改变原生改写机制。
- 实现前检查隐藏卷容量校验、创建按钮门控、密码字节转换与清理、会话失效时关闭创建流程，以及 DOM、页面契约、WebView 集成测试。
- 参数页点击创建只打开重要提醒弹窗，固定提醒此前外层数据可能永久无法访问且无法通过本应用恢复，不按登录密码角色分支。展示当前诱饵密码，默认遮挡，眼睛切换显隐；用户勾选永久性后果确认后，才由弹窗按钮调用正式创建。
- 最小验证：DOM 61 项、页面契约 1/1、JavaScript 语法和差异检查通过；新增覆盖预确认无原生调用、默认遮挡、显隐、返回清理、重新确认及模拟原生失败后密码清理。DOM 测试补入 TextEncoder，契约更新为新增只读密码预览及第三个显隐按钮；WebView 集成脚本同步新确认步骤但未执行实机集成。未跑全套。
- 提交前风险修正：以原诱饵密码登录时，新密码会挤掉日常密码。复核 `CardRecordService::set_second_password` 与 `rewrap_card_record_v1`，确认改写只保留当前登录密码和新密码；前述仅涉及外层数据的表述不完整。最终弹窗统一改为“诱饵密码不一致可能使本卡原有密码失效，导致此前容器中的数据永久无法访问。请务必确认后再继续。”，不按角色分支，也不声称直接覆盖容器数据。

### UI：创建容器动态执行反馈（2026-09-10）

- 原文：第 7976～7993 行。
- 需求与设计审计：查看 `usb-vault_ui-design.html` 第 19 页“创建执行中”，核对里程碑的同步调用及取消缺口，以及两种容器显示一致、不得暴露结构的边界。
- 实现前检查页面两种创建调用、COM 同步创建入口、原生 VolumeCreator 的进度回调和 helper 等待流程。底层存在格式化阶段回调，但页面链未接入，且原始阶段包含内外层信息；当前切片不直接暴露这些阶段、不编造百分比，不增加无法兑现的取消按钮。
- 最小验证：DOM 66 项、页面契约 1/1、JavaScript 语法和差异检查通过。模拟延迟原生调用验证两种创建执行视图、等待时耗时更新，以及成功/失败后清理。未运行全套或真实大容量创建。
- HTML/JS 已同步开发目录并核对哈希，无需重链接 EXE。本切片是渲染器动态反馈，不是原生后台线程改造；真实 helper 同步执行时的窗口响应、动画和耗时更新仍需实机验证，准确进度及可取消创建仍未接通。本轮未提交、未推送。
- 最小验证：DOM 67 项通过，页面契约 1/1 通过，JavaScript 语法和差异检查通过。契约原来限制清空说明调用仅出现一次，按参数页与结果页两个用途更新。未跑全套。

### UI：真实创建进度与后台执行（2026-09-10）

- 原文：第 7994～8006 行。
- 前置提交已完成：`c262314`，仅包含上轮动态执行和就地结果页；未推送。本节为随后确认的真实反馈切片。
- 需求审计：复核设计第 19 页、里程碑同步执行缺口、需求清单 R-03 与 8.6/8.7 的密码清理及两种容器一致展示要求。原有 `VolumeCreator` 已有格式化百分比回调，上层未传递；当前 FormatDLL 配置为快速格式化，故显示“格式化容器”，不声称全容量写入随机数据。
- 最小验证：DOM 69 项通过；创建协议、会话核心、WebView 原生桥、HID 通知、页面契约五个相关测试均通过。新增后台创建实时快照、忙态并发拒绝、认证结束快照拒绝、含隐藏卷后台六参数与清理、发布前中断清理、原生阶段百分比及实际盘符展示验证。初次会话测试暴露无反馈时新增文件大小查询改变顺序，已改为仅反馈启用时查询，复测通过。未跑全套或真实卷创建。
- 安全审查：回调仅带阶段、百分比和已确认盘符，不带路径、卡标识、密码或档位；新位置副本清理，密码仍由 SecureBuffer 和原有消费/清理逻辑管理；无进度/耗时持久化或导出日志。后台不并发修改会话，UI 不乐观宣称权限/分配/挂载成功。
- 剩余限制：未接通用户主动取消创建；拔卡不能立即中断正在执行的底层格式化，等待该调用结束后在发布/挂载边界阻止继续，并处理会话卸载。这一同步底层调用边界仍需后续取消协议完善，不能将本次改动宣称为即时中止。当前真实反馈切片未提交、未推送。

### UI：创建完成步骤对勾样式（2026-09-10）

- 原文：第 8007～8012 行。
- 按截图及设计第 19 页复核完成项展示，检查现有步骤状态与样式：原 `.done` 将整行文字改为绿色，序号仍然显示。
- 完成步骤改为淡绿色背景圆形对勾，步骤名称恢复普通文字颜色，右侧状态使用次要文字颜色；仅调整样式，沿用真实完成状态。
- 最小验证：页面契约测试 1/1 通过，差异检查通过；HTML 同步开发版目录。安全复查：不改原生逻辑、敏感数据或权限边界。未提交、未推送。

### 诊断：新建容器无法主动卸载（2026-09-10）

- 原文：第 8013～8022 行。
- 复核需求及里程碑的普通卸载重试与拔卡升级边界，检查会话、驱动、COM 桥和页面卸载路径。页面将卸载异常统一显示为文件占用提示，该文案不能单独证明驱动返回 busy。
- 在当前用户身份下只读查询驱动，确认用户指定容器仍挂载；只读枚举文件句柄发现 Windows COM Surrogate 持有对应卷根目录句柄，未发现应用自身的匹配句柄。外部占用是当前最直接线索，但未获取应用本次卸载的原始错误码，尚不能断言唯一根因。
- 命令行非强制卸载探测被自动审批拒绝（当前仅授权诊断），未执行；改用只读诊断，未结束进程、关闭用户句柄、强制卸载或改动容器。建议用户关闭相关资源管理器/文件选择窗口后在应用内重试。
- 后续确认执行普通安全卸载；核验目标映射后执行非强制卸载，驱动返回 6（volume still has open files），复查仍挂载，确认真实文件占用而非仅页面提示。未升级强制卸载。
- 确认后关闭 WSL 重试：`wsl --shutdown` 正常退出，WSL 工作进程结束；重新核验目标映射并执行非强制卸载，驱动返回 0，挂载列表确认目标盘符已消失，并通知 Shell 刷新。关闭 WSL 后占用解除，验证本次失败与 WSL 后台占用有关；未使用强制卸载，未修改容器内容。应用会话可能仍保留外部卸载前的显示状态，本次无代码变更、未提交。

### UI：创建步骤对勾居中与完成态动画（2026-09-10）

- 原文：第 8023～8030 行。
- 按截图及既有设计第 19 页要求复核进度展示，检查步骤 CSS、轮询状态更新和已有 DOM 测试。原图标采用 grid，隐藏序号文本与伪元素仍参与布局，改为 flex 容器与绝对定位对勾，避免对勾偏下。
- 完成态显式清除动画及旋转，旋转选择器排除完成态。现有轮询已经在同一同步调用中更新状态类、右侧文字与 aria-current，未复现文字完成但仍旋转的情况，故保留真实反馈逻辑并加强样式约束。
- 新增快速创建跳过中间轮询直接进入挂载的回归验证：前置步骤均为 done、无 aria-current、动画为 none，分配/格式化文字完成，仅挂载项 current。DOM 70 项和页面契约 1/1 通过；差异检查通过，HTML 同步开发目录。
- 安全复查：仅样式及测试，不改变创建、挂载、权限或密码处理；原生 WebView 中的实际渲染仍待实机确认。未提交、未推送。
- 确认提交当前真实创建反馈及后续完成步骤样式修正，合为一个 `[ui]report real container creation progress in the background` 提交；沿用已通过的开发版构建、五项相关原生测试及最新 DOM 70 项验证，不重复全套测试。已复核差异，不推送。

### 测试版交付第一步：主 EXE 无参数启动（2026-09-10）

- 原文：第 8031～8041 行。
- 需求审计：本轮选定先替代启动脚本，后续再扩展配置器及制作交付包。本轮复核现有 P2.18 打包记录、里程碑、需求清单 R-03/8.6，以及联网开发模式、凭据管理器和固定本地磁盘的现有边界；不改变生产/开发模式区分或卡记录。
- 实现前检查主程序参数解析、启动脚本固定摘要、打包构建、凭据配置器及 VerifiedLocalArtifact 文件与目录锁定机制。主 EXE 无参数时以实际模块目录定位 helper 和 vendor，不使用工作目录、PATH 或可修改的摘要配置文件；保留原六参数显式开发入口兼容本地脚本。
- 新增构建时生成部署摘要头，依赖 helper、建卷 helper 与页面源文件；将实际构建摘要嵌入主程序，格式化及厂商 DLL 保留已审计固定摘要。无参数入口在加载页面和访问卡之前校验六个组件，整个程序生命周期保留只读验证句柄，拒绝替换、重解析路径及非固定本地磁盘。异常提示完整解压与配置入口，不暴露凭据。
- 最小验证：首次构建因命令行代码页 936 被仓库检查拒绝，改为既定 UTF-8 代码页后开发版构建成功。配置解析及完整性测试 2/2 通过，新增含中文/空格安装路径、固定子路径、错误目录及摘要拒绝测试；六个部署组件摘要与构建身份核对通过，打包 PowerShell 语法与差异检查通过。未重复全套测试，未操作真实卡或创建容器，无参数图形界面及干净机器验收待执行。
- 安全复查：摘要在构建时固化，运行时不按现有文件自签认可；复用文件/父目录锁定与路径校验，新增路径不含密码或档位。保留的带参数模式仍是原有显式开发接口；程序尚未签名，嵌入摘要不能防止攻击者替换整个 EXE。配置/检测向导与最终发行包属于后续切片，本轮未开展。当前未提交、未推送。

### 测试版交付第二步 A：配置器只读检测与启动（2026-09-10）

- 原文：第 8042～8052 行。
- 前置提交完成：`7f1a2e3 [ui]enable direct launch of the development application`，未推送。确认继续下一步；按独立可验收边界先完成配置、只读检测和启动入口，依赖安装自动化及可选临时容器写入测试留在后续切片。
- 需求审计与实现检查：沿用上一节 P2.18、R-03 与 8.6 审计，检查原生配置器字段读取/清空、凭据管理器、严格只读插卡 inspection/router、驱动查询、HID 探测与 WebView2 运行时查询。检测不调用初始化、bootstrap、卡记录更新或挂载/卸载接口。
- 扩展独立配置器：保留三字段掩码及 SecureBuffer 输入路径；保存后清空并提示检测。已保存配置可直接检测，尚未保存输入不会被检测静默丢弃。检测使用后台线程，窗口逐项显示七项结果：系统架构、运行组件、WebView2、驱动、凭据、卡连接、卡访问；全部通过后才启用启动按钮。编辑配置会作废通过状态，运行期间禁用输入/重复检测并阻止提前关闭。
- 运行组件校验覆盖主 EXE 及六个依赖，配置器嵌入依赖主程序构建结果的摘要；检测期间保留验证句柄，启动前再次验证主程序。卡访问仅在组件、凭据、插卡三项均通过后执行一次，复用既有有超时 helper；失败不自动重试。只呈现正式初始化/登录/不可用路由，不导出硬件标识或私有卡证据；区分认证失败、锁定与卡不可用的通用错误。
- 最小验证：初次编译发现缺少显式 COM 头及 WebView2 包含顺序问题，补正后配置器最终构建成功；配置/检测调度、凭据和只读入口路由三个测试通过。新增三项前置失败必须跳过卡访问、全通过可继续、异常消息不透传的确定性检查；配置器所嵌入的主程序摘要与产物一致，差异检查通过。未执行真实卡访问、凭据写入、图形窗口验收或全套测试。

### 测试版交付第二步 B：依赖安装入口（2026-09-10）

- 原文：第 8053～8063 行。
- 前置配置器只读检测已确认提交：`8e76dc3 [ui]add configuration diagnostics and application launch`，未推送。本轮为确认的下一独立边界：配置器内的依赖安装入口。
- 需求审计与检查：复核 P2.18 打包安装器的固定摘要、签名及来源边界、现有配置器检测线程与启动状态，沿用 R-03/8.6；查阅微软 WebView2 官方分发说明（https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution），采用官方认可的下载页引导方式，不引入未经审计的自动下载执行链。
- 配置器新增“安装 VeraCrypt（管理员）”与“获取 WebView2（微软官网）”。前者仅在用户点击后后台校验随包固定路径安装器的 SHA-256 和 Authenticode 信任链，通过后 runas 启动交互式官方安装器，保留文件/目录验证锁直到进程退出；用户取消 UAC 单独反馈。后者仅打开固定 HTTPS 微软下载页，不下载、不运行浏览器获取的文件。
- 安装期间与检测/保存/启动互斥，窗口保持响应并要求先完成或退出安装器再关闭；安装器退出只显示任务结束并要求重新检测，不将退出码当作安装成功。进入安装或下载入口时作废旧的通过状态。更新说明，正常首次配置不再要求用户手动执行 PowerShell，原验证命令作为可选核查保留。
- 安全复查：安装器路径/摘要/官网地址固定，不接受用户命令文本，不拼接 shell 命令，不静默提权；固定摘要也固定受审计签名者对应的原始字节，运行时额外验证证书信任链。验证失败不启动，验证句柄跨启动/等待存活，凭据不传给安装器或浏览器，诊断结果不落盘。

### 测试版交付第三步 A：单 ZIP 与完整安装依赖（2026-09-10）

- 原文：第 8064～8073 行。
- 前置依赖入口已提交：`c3e23ab [ui]add verified prerequisite installation entry points`，未推送。最终采用一个压缩包发给测试客户，按此优先补齐 WebView2 离线安装器与完整包；可选临时容器读写自检仍为后续独立切片。
- 需求审计与检查：复核现有打包白名单、Release 静态运行库配置、P2.18 第三方说明和配置器安装边界。微软官方提供 Evergreen Standalone Installer x64（https://go.microsoft.com/fwlink/p/?LinkId=2124701）。自动下载因连接/传输超时未完成，改用本地提供的完整文件；以该文件为准，没有将自动下载的部分文件入包。
- 打包新增必填 WebViewInstaller，读取源码中固定摘要并验证微软可信签名后才收包；静态 MSVC Release 主程序与配置器构建通过。两种安装器缺失/替换拒绝测试及两套真实安装器仅校验测试通过。说明同步为完整 ZIP，明确供应方凭据仍需单独配置，签名验证与厂商开发卡访问可能联网，不承诺整个应用离线运行。
- 候选包：out/packages/usb-vault-p2-test-0.2.0-preview6-full.zip，约 276 MiB；SHA-256 为 7D85301DF6EFC2879C1B32700CCC723E42B1E75AE27B8EF3798AA0CD370EFED3。直接读取 ZIP 逐一验证 20 个清单文件及清单自身的唯一性，无多余条目，没有测试容器、密钥文件或 PDB。构建身份如实标记 c3e23ab + dirty；本轮改动未提交，因此不将候选包描述为已提交发行版。
- 安全/剩余风险：只打包显式白名单，未包含用户凭据或容器；第三方原文件未修改，安装器执行仍需要明确用户点击。尚未实际执行两套安装器、干净机器或完整用户流程验收，也未增加临时容器读写自检。既有 THIRD-PARTY-NOTICES 的 ViKey DLL 再分发授权待确认仍适用，生成候选 ZIP 不等于对外分发获批。当前改动未提交、未推送，未发送给任何客户。

### 交付包精简（2026-09-10）

- 原文：第 8074～8080 行。
- 按要求检查 preview6 文件清单，移除交付 ZIP 中的 README、测试须知、第三方说明性 README、兼容启动脚本及构建/摘要记录；第三方许可、NOTICE 和补丁保留在 licenses，程序与两套安装器保留。源码说明文档不删除，构建身份及摘要清单改存 ZIP 外用于内部校验。
- 配置器缺依赖文案改为指向安装按钮，不再让用户查已移除的须知。Release 重建通过，重新生成 preview7-clean；ZIP 逐文件摘要校验通过，并确认没有被移除的文档或额外文件。仅作最小构建和包校验，不重复全套测试。
- 安全复核：未移除任何运行时校验或许可文本，不添加凭据/测试容器，不改变安装授权流程。新包 out/packages/usb-vault-p2-test-0.2.0-preview7-clean.zip，SHA-256 A3E85F421C8AE8CB590DE457DC45F2533F882B8EE6DF2D6BBBD471BCEFE1D19F。旧包保留，用户应使用本次新包。当前修改未提交、未推送；既有干净机器验收及厂商再分发授权待确认事项不变。
- 确认提交离线 WebView2 接入与交付包精简改动；沿用 Release 构建、安装器校验与 preview7 的 15 文件 ZIP 校验结果，合为一个英文 [ui] 提交。既有 ZIP 不重写，外部构建身份保留生成当时的 dirty 记录；不推送。

### 配置器卡访问错误细分（2026-09-11）

- 原文：第 8081～8096 行。
- 本轮要求一次提示尽量定位问题；已审计需求的厂商中立错误边界、R-03 与卡状态不可泄露约束，并检查 SDK 映射、helper 协议、配置器及现有测试。工作树开始时干净。
- 追加稳定错误分类，区分 API Key 拒绝/未设置/计算失败/校验失败、UserSN 拒绝、未激活、多卡和响应超时；同步 helper 接收范围，保持原有错误编号不变。厂商原始状态仍限于适配器。
- 验证：MSVC review-clean 下 SDK 映射、会话调用顺序/失败清理、helper 协议边界、配置器错误反馈四组测试通过；Release 配置器、主程序、helper、creator 均构建成功，部署摘要自动重生成。git diff --check 通过。
- 集成验证限制：vikey_helper_process_tests 在 development vendor runtime staging 遇到既有路径安全门（local artifact directory resolves through a redirection），尚未到新增超时断言；改 TEMP 及 Windows 原生参数路径仍复现。未放宽安全检查，未把该套测试记为通过；实际用户卡/API Key 测试待手工完成。
- 完整候选包：out/packages/usb-vault-p2-test-0.2.0-preview8-diagnostics.zip；SHA-256 B61D2CC4C8A4A19983BF5C1054E16119E3D4954CD2B5B33F3DDADFCFB34B8C00。ZIP 全条目与外部摘要清单逐项校验通过，未夹带凭据或测试数据；旧包保留。构建身份为 d8cad6c + dirty，当前未提交、未推送、未向客户发送。

### CARD-1 诊断全链路与反馈审查（2026-09-11，文案待确认）

- 原文：第 8097～8110 行。
- 确认细化 CARD-1 及其他粗粒度提示，明确先不打包，完成后提供全部文案。复核既有需求的厂商中立边界、密钥生命周期与卡状态隐私要求；延续未提交的诊断切片，不改初始化或卡操作授权。
- 初轮六组单元/协议/配置回归通过；正式 Release 二进制编译通过。后续细节变更将完成复验；没有运行真实安装器、读客户凭据或访问真实卡。仍未打包、未提交、未推送。
- 最终验证：八组回归（凭据、安装器、SDK、会话、卡记录 IO、文件校验、协议、反馈）均通过；Release 正式二进制重新构建通过，diff --check 通过。安全复查确认成功响应不能夹带错误元数据，未知阶段/数值来源被拒绝，错误原文不跨到界面，输入缓冲仍按原机制清理，无路径安全放宽、无自动认证重试。完整文案整理至 out/reports/配置器完整文案确认稿-2026-09-11.md，包含 18 类基础错误与 38 个诊断阶段及其余流程提示。按要求不打包、不提交；白屏另案本轮未修改。
- 确认生成新包：preview10-diagnostics 完整 ZIP 已生成，15 个文件摘要及四个 EXE 与本次 Release 构建一致性校验通过。SHA-256：566BA3CE2B1571BE20D3342772556622DEFBA32777DC23961289335D3CB86E6E。包含 helper v12 诊断阶段传递、细化反馈和安装退出结果；白屏问题未包含修复。既有八组测试通过，普通桌面完整集成与真卡复验仍待完成，未提交、未推送。
- 已确认提交当前工作区：将诊断分类、helper v12 元数据传递、配置器文案与安装结果反馈及相关测试纳入本次提交。沿用八组回归、Release 构建及 preview10 包校验结果，本次差异检查通过；白屏修复尚未实施，完整桌面集成与真卡复验仍待完成。不推送，现有 ZIP 保留打包时的 dirty 构建身份。

### 客户白屏定位与建卷取消（2026-09-11）

- 原文：第 8111～8121 行。
- 客户设备上配置器七项全通过、主 EXE 白屏。本机复现定位：`tools/app/main.cpp:174` 用 `UrlCreateFromPathW` 生成页面 URI，非 ASCII 字符原样保留，而 WebView2 在 `NavigationStarting` 回报的是 Chromium 规范化后的百分号编码形式；`:475` 的白名单做字符串全等比较，不等即 `put_Cancel(TRUE)`，把自己的首次导航取消掉，窗口类未设背景画刷因而呈纯白。同一套 preview10 文件在 ASCII 目录正常、在中文目录纯白，Edge headless 打印出两种 URI 形式的差异。
- 文件类型图标按 P3.18 对照表落地：`ui/card_session.js` 新增十一类 `kFileTypeIcons` 与 `fileTypePaths` 分类器，复用既有 `exportGlyph` 与 `kFilePaths`，文件浏览与导出选择器共用同一分类；契约测试补五条（每行一个图标、目录优先于扩展名、未知扩展名落兜底、分类不向桥多要字段、页面不加载外部图片）。可执行图标按产品真实尺寸渲染后改判为四根引脚，容器类保留。提交：`36e84e2 [ui]draw a type icon for every file in the list`。
- 建卷取消先查实句柄竞争：`spike/_vcref/src/Common/Format.c:377` 显示格式化期间标准容器以 `dwShareMode = 0` 独占打开，隐藏卷分支也不含 `FILE_SHARE_DELETE`；若父进程靠拆管道取消再删 `.preparing`，必然踩共享冲突。且助手自身的 `delete_target_as_caller` 依赖 `ImpersonateNamedPipeClient`，管道一关就无法模仿，两头都清理不掉，残留的 `.preparing` 又会被建卷前置检查当作 `file_exists`，永久堵死同名容器。故改为带内协作式取消，不拆管道。
- 协议新增 24 字节 `CancelRequest`（nonce 绑定）：父进程在进度回调里发一次并继续读，助手在 `FormatBackend::progress` 每个进度点 `PeekNamedPipe` 查询，命中即回 `FALSE`，落进它已有的 `catch` 走自清理并回报 `operation_cancelled`；父进程新增助手退出等待（上限 30 秒），先关管道、再等退出、最后才清理。提交：`76c856c [creator]let a running creation be called off`。
- 验证：协议、页面契约、宿主对象、会话四组目标测试通过，DOM 导航测试通过。WebView2 集成夹具在改动前后同样以 `fail-settings-remember`（退出码 23）失败，属既有问题，与本次无关。真实建卷取消需插卡实机验证，尚未执行；全套测试留待最后一并跑。未推送。

### P3.19 建卷取消补强切片 1：准确报告取消与清理失败（2026-09-12）

- 原文：第 8122～8133 行。
- 确认按三个小切片补强建卷取消，每片只跑最小必须测试，不跑全套。本片仅处理结果分类与对应页面反馈，取消截止点及跨层边界回归留给后两片。
- 计划与需求审计：复核 MILESTONES.md 的建卷范围、2026-09-11 取消实现记录，以及需求 R-03、8.6、8.7.2；沿用密钥易失、不得持久化档位、创建反馈不按登录身份分支的要求。里程碑未细列新取消提交，本片以现有实现和开发记录确定进度，不改阶段范围。
- 页面允许状态 5，两类容器共用固定提示“创建未完成，未能确认临时文件已清理。请检查保存位置中的残留文件后再重试。”只有确认取消状态保留“没有留下容器文件”。不把异常原文、文件路径或内部阶段回传页面。
- 最小验证：重新构建 card_product_webview_host_object_tests 和 card_product_ui_page_contract_tests，2/2 通过；DOM 76 条检查通过（新增两类容器各三种结果）。原生覆盖有/无取消请求时的 helper 清理失败、会话复合清理异常、无关异常和同数值不同错误类别，另覆盖确认取消。DOM 初次发现页面旧上限拒绝状态 5，修正后通过，并复跑页面契约通过；git diff --check 通过。未跑全套、未访问真实卡或执行真实建卷。
- 安全复查：不增加文件删除操作、权限、持久化字段或敏感信息输出；清理异常优先于取消成功展示，失败不再被取消请求掩盖，既有密码清理保持。没有修改底层取消协议及发布/挂载时序。

### P3.19 建卷取消补强切片 2：发布前关闭取消入口（2026-09-12）

- 原文：第 8134～8150 行。
- 切片 1 已提交为 `82614d0 [P3.19]distinguish creation cancellation from cleanup failure`，未推送。同时确认开始切片 2，继续执行每片最小必须测试的约束。
- 实现前检查 CreationWorkState、原生 cancelContainerCreation 与后台进度回调、WM_CLOSE、页面轮询/取消按钮，以及既有宿主对象、会话和 DOM 检查。原实现的独立取消布尔值与发布前查询存在竞争，窗口确认期间也可能跨过发布边界。
- 最小验证：宿主对象、页面契约、会话测试 3/3 通过；DOM 81 条检查通过，新增请求失败重试、接受后等待结果、下次创建恢复、过晚拒绝和完成阶段禁用。原生新增最后查询之后/发布之前取消，以及发布先开始时页面与窗口均拒绝取消且最终成功的检查；保留切片 1 的异常分类回归。未跑全套。
- 构建验证：review-clean 的主程序目标因缺少 USB_VAULT_FORMAT_DLL 配置被预期部署守卫拒绝；不放宽守卫，改用已有 DLL 配置的 package-release 目录，Release 主程序构建通过（没有制作安装包或 ZIP）。随后仅修正一处 JS 注释，不影响已验证行为。git diff --check 通过。
- 编号调整（2026-09-12）：本轮要求评估 ui 前缀，不适用则续开 P3 编号。原子取消/发布边界超出纯 UI，因此将三个补强切片编号为 P3.19（结果分类）、P3.20（取消截止点）、P3.21（跨层回归），避开已用于文件类型图标的 P3.18，并同步 MILESTONES.md。P3.19 对应现有提交 7c57bd5，保留历史提交标题，不重写历史；待批准的第二片使用 [P3.20]，第三片使用 [P3.21]。本次仅编号与范围文档调整，差异检查通过，不新增或重跑测试；P3.20 仍待提交决定，P3.21 尚未开工。

### P3.19 建卷取消补强切片 3：两类容器跨层回归（2026-09-12）

- 原文：第 8151～8163 行。
- 确认将三片统一使用 [P3.19]，重写第一片、提交第二片并开始本片。第一片已改为 `82614d0 [P3.19]distinguish creation cancellation from cleanup failure`（仅改提交消息，树与旧 7c57bd5 相同），第二片已提交为 `d1ef656 [P3.19]close cancellation before publishing a container`，均未推送。
- 计划与需求审计：按 MILESTONES.md 中 P3.19 切片 3 的取消、清理失败、发布边界与重试范围补回归，保留 R-03、8.6、8.7.2 的易失凭据与对称反馈约束。检查现有会话 FakeRuntime 的创建/发布/清理/挂载事件、两种创建核心、桥与控制器、原生 fake 会话和 DOM 场景；确认原生取消覆盖主要落在标准容器，尚未串联同会话同名重试及残留拒绝。
- 原生桥回归：共用两类 fake 创建的暂停与发布边界，通过实际控制器/COM 接口验证两种创建请求的取消与失败分类、最后查询后的取消、过晚取消被页面/窗口共用入口拒绝；每种结果后在同一宿主、同名和同认证会话重试，确认旧取消/结果不污染下一次成功。含隐藏卷入口的口令数组同步检查已清零。
- 最小验证：重新构建并运行 card_login_mount_session_tests、card_product_webview_host_object_tests，2/2 通过；宿主测试整理缩进后重新构建并单独复验通过；DOM 85 条检查通过，git diff --check 通过。不跑全套，不重复未改动的页面契约，不重建产品、不打包。
- 本片实现与审查完成，未提交、未推送，第三个 [P3.19] 提交待确定。

### P3.20 挂载与普通卸载的弹窗反馈（2026-09-15）

- 原文：第 8164～8192 行。
- 需求与计划审计：确认先做挂载／普通卸载弹窗反馈，并指定本片为 3.20；手动强制卸载确认不在本片。复核里程碑 P1.2/P1.7 的普通重试与拔卡升级边界、P3.19 既有后台机制，以及需求清单 R-03、8.4、8.6 的锁页秘密与无档位、无本地持久痕迹约束。此次无需求冲突。
- 提交状态：本片尚未提交，[P3.20] 提交待审查后确定；未推送。
- 本机构建启动修复：实测直接启动 package-release 主 EXE 报组件校验失败。只读核验发现 vendor/ViKey.dll 缺失，其余五项固定组件哈希均与生成的部署头一致；从已有本机 SDK 取白名单 SHA-256 匹配的 ViKey.dll，复制到构建目录 vendor 下并复验哈希后重新启动。此次只补齐被忽略的本机构建产物，不放宽校验、不修改产品代码或凭据，不生成正式测试包。此前仅确认进程创建便报告已启动，验证不足。
- 验证：宿主对象测试通过，覆盖驱动、通用、Win32 错误映射及未知错误；页面契约通过；DOM 104 项通过，含真实 2 秒计时、显式确定、失败持续显示、旧计时器隔离、各类原因和会话变化。DOM 总截止时间改为 15 秒以容纳真实等待验证。JavaScript 语法、差异检查通过，Release 主程序重新构建通过。真实布局、动画与驱动失败仍需实机检查。本片继续未提交、未推送。
- 验证：DOM 107 项通过（新增运行中忽略遮罩点击、失败态遮罩关闭、点击弹窗内部不关闭三条，并断言成功文案不再出现“关闭”字样）；失败态等待由 2100ms 提到 3100ms、成功态由 1800ms 提到 2800ms 以匹配新时长。宿主对象测试与页面契约测试各 1/1 通过，JavaScript 语法检查通过。dev 与 package-release 两个配置的主程序均重新构建，部署页面 SHA-256 与源文件及烘入 EXE 的摘要三方一致。

### P3.21 卸载失败后的手动强制卸载（2026-09-15）

- 原文：第 8193～8214 行。
- 基线与范围：本轮明确开工强制卸载并在失败弹窗提供红色按钮。开工只读检查确认 P3.20 已由此前流程提交为 `0900d7c`，随后有 `b99f1e2` 行尾规则提交，工作区干净；本片作为独立 P3.21 开发，不改写已有提交。开场对“未提交改动”的判断已在检查后纠正。
- 需求与实现审计：沿用需求清单 R-03、8.6、普通卸载重试与拔卡升级约束，审查 P3.20 弹窗／结果分类、COM 后台互斥、窗口卡事件、会话 Runtime 与现有卸载测试。已有 force_unmount_active 专供拔卡，会清除口令、派生秘密与历史，不能借给用户主动强制卸载。新增手动入口复用精确 busy 才升级的底层机制，保留当前登录能力；无需求冲突。
- 验证：会话、宿主对象、页面契约 3/3 通过；随后新增部分失败与“挂载 busy 不得强制卸载”回归，宿主与会话分别复验通过。DOM 120 项通过，覆盖红色按钮、错误分类限制、风险确认／取消、重复点击、强制失败与成功、单卷范围、全部卷确认以及卡事件失效。首轮 DOM 夹具未等卡状态刷新便点击下一步，已修正；部分失败测试的驱动状态查询本身记录事件，调整为先验证卸载事件再查询状态后通过。未跑全套，未执行真实卷卸载。
- 提交：`c0022c0 [P3.21]offer a way past a volume that will not let go`（实机验收通过后提交，未推送）。
- 强制卸载确认正文由两句祈使句压缩为一问句「未保存或尚未写入磁盘的数据可能丢失，文件或文件系统可能损坏，仍要强制卸载吗？」。原文末尾的「请先保存文件并关闭使用该卷的程序；仍要继续时，再确认强制卸载」与底部两个按钮重复表达，去掉后与标题「强制卸载 X ？」连读成完整问句。

### P3.22 一键挂载与卸载改名（2026-09-15）

- 原文：第 8215～8225 行。
- 范围与基线：本轮要求把右上角「安全卸载」改为「一键卸载」，并在其左侧加一个同风格的「一键挂载」。开工前只读核查发现两侧并不对称——卸载在原生已有整会话入口（`beginUnmount` → `controller_.unmount()` → `volume_session_.unmount_active()`），挂载只有按 id 的 `beginMountAdoptedContainer`，控制器、会话接口、宿主对象三层均无任何 mount_all。已说明这不是改按钮文字，而是需要补一条与卸载对称的原生链，确认走原生方案而非页面循环调单卷接口。
- 会话层：新增 `CardLoginMountSessionCore::mount_adopted_all()`，按 B→A 收集未挂载容器的 id 后逐个复用 `mount_adopted_container`，遇失败直接向上抛出、不继续挂剩余卷。先定序再挂载，避免遍历容器表的同时被写回。接口基类给出默认实现：不支持的会话实现抛出拒绝，而不是挂一部分。控制器 `mount_all()` 加认证状态校验。
- 验证：会话测试新增 B→A 顺序、重复调用无副作用、失败即中止不挂子集、部分挂载续挂共四组断言；宿主对象新增拒绝带参、后台线程隔离、挂载完成后强制卸载仍被拒；页面契约新增 `bridge.beginMountAll()` 契约；DOM 新增部分挂载双按钮并存与一键挂载后自行撤下。三个原生套件与 DOM 全部通过。调试中修正两处自造问题：`beginMountAll` 置于宿主测试循环末尾破坏了「循环后无挂载」的不变式，已移至 `beginUnmount` 之前；fake 运行时盘符池仅 3 个且卸载不归还，本用例需 5 个，已就地加大。
- 顺带清理：删除 `ui/card_session.js` 中 2026-09-10 引入即为空、从未被读取的 `kGripPaths`，注释随之落到它解释的 `gripGlyph` 上方。按本轮指示不单开提交，并入本片。
- 未跑全套；本片尚未做实机验收，package-release 未重建。

### P3.22 切片 1：所选容器的原生批量执行与进度（2026-09-16）

- 原文：第 8226～8237 行。
- 范围与基线：确认同一 P3.22 下分三片推进。本轮只做原生选择批量执行／进度／结果，不接入常显菜单和结果重试页面，不做用户主动中断。开工检查工作区干净，已有 `0a47ee6 [P3.22]give mounting the same one-press entry unmounting had`；沿用编号并以切片细分，不重写历史。
- 审计：检查 AGENTS、里程碑、需求清单 R-03／8.6／登录挂载事务，以及现有整会话 mount_all、逐 ID 操作、COM 工作线程／互斥／强制重试资格、卡事件代次和原生测试。发现上一轮把 Q-07 的 B→A／失败即中止沿用到整会话按钮，而最新要求选择批次中失败后继续。以最新决定为准，在技术需求表及里程碑明确：新的已认证手动多选入口按选择顺序独立操作；不改登录挂载事务或旧整会话接口。
- 安全边界：执行中仅批次查询可达，不允许同步查询会话、重复操作、旧单卷进度或建卷取消；批次不生成手动 force 资格，普通卸载保持普通策略。线程和结果快照共享锁，卡事件代次变化／拔卡标记在每项前后检查，停止剩余项；旧代次／未认证禁止读取，工作结束清除被中断批次的名称，其他修改操作丢弃旧批次。没有新增口令复制、档位分支、路径输出或磁盘结果缓存；仍依赖既有锁页秘密生命周期。
- 最小验证：宿主对象与会话两个套件 2/2 通过。新增选择顺序、中间失败后继续、挂载与卸载两种方向、已达目标跳过、仅重试指定项、完整请求拒绝（重复／未知／溢出／NUL／超长／错型）、实际后台线程、原生互斥、强制入口拒绝、过程中／完成后拔卡代次失效及新批次恢复。会话补充不可查询容量仍能返回无 I/O 选择快照、未认证不暴露名称。首次编译修正测试中误用 clear() 的问题，改为真实 clear_authentication() 后通过。未跑全套，无页面修改，因此不重复 DOM 测试。
- 提交状态：已确认提交切片 1，并进入切片 2；本次提交包含上述已验证原生改动，未推送。其余 UI 仍为原有按钮行为。

### P3.22 切片 2：常显分体按钮、选择菜单与批量进度（2026-09-16）

- 原文：第 8238～8247 行。
- 提交与范围：按确认先提交切片 1 为 `5c1d3a9 [P3.22]execute selected container batches with per-item progress`，未推送；随后开始本片。本轮要求只跑必要最小测试。本片不实现切片 3 的逐项结果列表／选择重试，也不引入主动中断。
- 审计：复核 AGENTS、里程碑 P3.22 三片边界、技术需求清单的显式多选规则及 R-03／8.6；检查现有顶栏按钮、单卷进度与强制确认、卡事件刷新、原生批次协议和页面契约／DOM 测试。新顶栏使用批次接口；非仪表盘的整会话清理保留原有入口，无新需求冲突。
- 最小验证：页面契约测试 1/1 通过；DOM 首轮 135 项通过，安全复查补充刷新期间清空下拉名称后定向复验 136 项通过。覆盖默认空选、禁用勾选、全选／半选／清空、多选只提交指定 ID、进度名称／计数、重复操作阻止、部分失败继续、未选项不变、完成计数、卡变化立即清空名称与新会话不继承选择。首次调用运行时依赖目录未找到 jsdom，改用项目已有 `out/ui-navigation-tools/node_modules/jsdom`，未安装新依赖。未重跑原生会话／宿主套件或全套测试。
- 构建与安全复查：package-release 主程序构建通过，git diff --check 通过。新增名称均通过 textContent 写入；选择与结果不持久化、不记录角色／路径／秘密，跨桥只传 ID。卡变化立即清空菜单及选择，异步返回在写入进度前检查卡变化标记，原生代次校验继续作为权限边界；运行／刷新时不重建旧菜单名称。未启动应用或操作真实卷。
- 风险与后续：DOM 模拟没有布局引擎，分体按钮、长名称及小窗口布局待实机验收；真实多卷／忙卷尚未验收。驱动中的单次操作仍不能立即中断。逐项结果页与选择重试留给切片 3。已确认先提交本片再进行切片 3，未推送。

### P3.22 切片 3：批次结果与选择重试（2026-09-16）

- 原文：第 8248～8258 行。
- 提交与基线：决定先提交切片 2 再继续；已提交 `ea3a152 [P3.22]add persistent batch actions with container selection`，未推送。本片从该提交开始，未改写前片历史。此前只做只读检查和忽略目录内差异备份，没有混入切片 3 代码。
- 实现：批量执行结束后从进度框切换到独立结果弹窗，逐项显示名称、成功／失败／无需重复操作及受限失败原因，失败优先，同组保持原批次顺序。默认不选失败项，支持全选可重试项、逐项多选、半选、清空；成功／已达目标项勾选禁用。重试候选还须存在于当前会话且状态未达目标，无法重试的失败项明确说明。没有有效选择时禁用重试。
- 安全复查：名称和原因只通过 textContent 呈现，结果／选择仅驻留当前页面，不存储路径、角色或秘密；批次失败仍不授予强制卸载资格。结果 ID 与本轮请求一一校验，只接受已定义终态；页面新增卡事件代次，异步结果读取前后校验，卡变化立即清空列表和原结果集合、禁止重试，迟到结果不会恢复。关闭结果清除列表与选择，运行中拒绝重复点击及后台卷操作入口。
- 契约与构建：页面契约 1/1 通过；编写断言时补齐漏读的 page 变量，契约随后发现精简 WebView 集成夹具遗漏新增结果控件，并同时补齐前片通过动态 ID 获取的下拉控件及旧显隐预期，增加动态控件契约覆盖。未运行昂贵的真实 WebView 集成测试或原生全套。Release 主程序构建通过，部署 HTML／JS 与源文件摘要一致，git diff --check 通过。
- 剩余风险与提交：实机布局、真实多卷和忙卷尚未验收；DOM 无布局引擎。驱动中的调用仍不可即时中断；卡变化后清除旧结果，需以当前会话状态重新确认。本片与其后四轮界面调整合并为同一个提交，见本文件末尾的提交行。

### P3.22 顶栏批量按钮的外观与空选行为（2026-09-16）

- 原文：第 8259～8268 行。
- 范围：本轮逐条提出的五项顶栏调整，只动 `ui/card_session.html`、`ui/card_session.js` 及受影响的断言，不碰原生批次协议、进度或结果页逻辑。切片 3 仍未提交，本轮并入其工作区。
- 外观：一键挂载与一键卸载去掉左侧图标，只留文字；主按钮与右侧箭头共用同一底色——挂载用「新建容器」同款白金渐变（箭头取渐变末端的纯 `--gold`，避免窄块重跑一遍渐变），卸载用 `--danger` 配白字，两段之间留一条 1px 接缝仍可区分点击目标。卸载侧的 `danger` 类随之移除，改由 `.batch-split.unmount` 承担，免得 `.shell-button.danger:disabled` 在红底上再压一层灰字。
- 箭头：`⌄` 字形靠基线排版，天然偏上且偏小，改为与容器详情同款 `M6 9l6 6 6-6` 描边 SVG，尺寸 1.15rem、`stroke-width: 2`，由按钮自身的 flex 居中对齐。
- 最小验证：DOM 155 项通过，其中改写五处断言以对齐新行为——两个按钮空选时可点、空选点击展开菜单且不提交批次、再次点击收起、清空全选不再禁用主按钮、新会话用「点开菜单无勾选」代替原来的「按钮禁用」，并新增完成后顶栏统计为空的检查。页面契约 1/1 通过。WebView 集成夹具断言同步改为要求认证后卸载按钮可点，未运行昂贵的真实集成测试与原生全套。
- 限制：jsdom 无布局引擎，配色、接缝、箭头尺寸与纵向居中仍须实机确认。未重建 package-release，未启动应用。

### P3.22 顶栏批量按钮的实机回修（2026-09-16）

- 原文：第 8269～8278 行。
- 来源：上一轮改动经 package-release 实机查看后，本轮四条修正，仍只动 `ui/` 两个文件及相应断言。
- 箭头段：`padding-inline` 由 .4rem 收到 .28rem，箭头本身仍是 1.15rem 的 SVG，只是两侧留白变窄。
- 菜单排布：行内顺序改为名称 → 已挂载／未挂载 → 复选框，`.batch-name` 取 `flex: 1` 撑开剩余空间，状态与勾选框因此贴右对齐；「全选」标签改成 `<span>全选</span>` 加复选框并用 `.batch-all { justify-content: flex-end }` 推到右上角，与下方各行的勾选框同在一条右边线上。
- 最小验证：DOM 159 项通过，新增四项——行内顺序与全选靠右、点名称即选中且菜单不关、菜单内 `focusout`（relatedTarget 为 null）不关菜单、再点一次取消。已临时回退 focusout 修复确认该断言会失败，非空断言。页面契约 1/1 通过。
- 构建：package-release 重建通过（首次链接因上一版 app 仍在运行占用 exe 失败，礼貌关闭窗口后重链），部署页面与源文件摘要一致，git diff --check 通过。实机复看待确认。

### P3.22 批次结果页精简（2026-09-16）

- 原文：第 8279～8291 行。
- 范围：实机看过后提的五条结果页调整，仍只动 `ui/` 两个文件、DOM 断言与精简集成夹具，不碰原生批次协议和重试流程。
- 标记与行勾选的隔离：行本身是包裹式 `<label>`，按规范 `<button>` 这类交互后代不会触发 label 激活，仍在点击处理里显式 `preventDefault()` 兜底，确保看原因永远不会顺手改动该行的重试勾选。
- 最小验证：DOM 168 项通过。新增／改写覆盖统计元素已不存在、单行结构与勾选框居末、只有失败行带标记、原因只在气泡与 aria-label 中、悬浮开合、固定后指针离开不收、再点取消、点别处与 Escape 收起且不关弹窗、全选位于页脚且紧邻重试，以及原先依赖统计文案的六处断言改为按行计数。页面契约 1/1 通过。
- 构建：重建前上一版 app 忽略两次 WM_CLOSE 仍占用 exe，经确认后 `Stop-Process -Force` 结束再重链；package-release 通过，部署页面与源文件摘要一致，git diff --check 通过。
- 遗留：成功项与「已处于目标状态」项现在都只显示名称加锁定勾选框，页面上不再区分，待确认是否需要补一个轻标记。气泡定位与红圈标记的观感须实机确认。

### P3.22 批次结果页标记与尺寸微调（2026-09-16）

- 原文：第 8292～8301 行。
- 范围：实机看过精简版后的四条调整，仍只动 `ui/` 两个文件与 DOM 断言。
- 页脚全选：`.batch-result-all` 去掉 `margin-right: auto`，随页脚的 `justify-content: flex-end` 归到「重试选中项」左侧；标签内改成 `<span>全选可重试项</span>` 加复选框，勾选框落在文字右边，与结果行右侧那一列勾选框同侧。
- 标记尺寸与成功标记：失败标记由 1.15rem 收到 1rem（感叹号竖杠与圆点的偏移同步下调）。新增 `.batch-result-ok`——同尺寸绿圈加一个用两条边框旋转出来的勾，非交互的 `role="img"` 加 `aria-label="<动作>成功"`，不占 Tab 位也不带气泡（成功没有需要解释的内容）。
- 最小验证：DOM 168 项通过，断言扩充为每行恰好一个标记槽、成功行带 `aria-label="卸载成功"` 的绿勾、已处于目标状态行的槽为空且不标记为失败、全选勾选框在标签末位且标签紧邻重试按钮。页面契约 1/1 通过。
- 构建：上一版 app 已退出，package-release 直接重链通过，部署页面与源文件摘要一致，git diff --check 通过。标记配色与弹窗新宽度待实机确认。

### P3.22 批次结果页第三轮微调（2026-09-16）

- 原文：第 8302～8312 行。
- 范围：实机复看后的五条，仍只动 `ui/` 两个文件与 DOM 断言。
- 弯路记录：上一轮的 `min(32rem, 100%)` → `min(27rem, 100%)` 规则本身生效（`#batch-result-dialog .modal` 特指度高于 `.modal`，部署副本已核对），实机反馈观感无变化。本轮进一步收到 `min(24rem, 100%)`，并补 `min-width: 0`——`.modal` 是 `.modal-overlay` 这个 grid 的项目，`min-width: auto` 会以页脚的最小内容宽度托底，估算约 315px 尚未触顶，但收窄后离触顶更近，显式置 0 免得日后加按钮时宽度悄悄卡住。
- 确定按钮：`#batch-result-confirm` 原来的 `primary` 类在 `.shell-button` 上没有任何规则（只有 `.modal-button.primary`），等于没生效；改为按 ID 给白金渐变，与「新建容器」一致。未改动同样挂着空 `primary` 的 `#volume-operation-confirm`，避免顺手改本轮没提的弹窗。
- 最小验证：DOM 170 项通过。新增／改写覆盖标题含计数、全成功时两个重试控件隐藏、失败批次中两者可见、成功标记可点且固定自身气泡并收起失败气泡、焦点循环不落在隐藏控件上。页面契约 1/1 通过。
- 构建：旧实例这次正常响应 WM_CLOSE，package-release 重链通过，部署页面与源文件摘要一致，并回读部署副本确认宽度规则为 24rem；git diff --check 通过。

### P3.22 顶栏下拉菜单收窄与拖选（2026-09-16）

- 原文：第 8313～8325 行。
- 范围：一键挂载／卸载下拉菜单的两条调整，仍只动 `ui/` 两个文件与 DOM 断言。
- 拖选：按下左键决定一个目标状态，之后指针经过的每个可操作行都刷成该状态，禁用行（已达目标状态）跳过，松开结束；`pointerup`／`pointercancel` 挂在 document 上，拖到菜单外松手也能收尾。菜单加 `user-select: none`，按下时再 `preventDefault()`，按住拖动不会选中容器名。
- 调试弯路：拖选断言最初写在挂载菜单上，而夹具里容器 1、3 本就已挂载，挂载方向只有容器 2 可选，「拖过第二行」实际落在锁定行上，看起来像功能没生效。加临时日志定位后把断言移到卸载菜单（此时三个容器都已挂载，三行皆可选）。另有一次临时探针没生效，原因是 `ui/card_session.js` 用 LF，探针脚本里硬写了 CRLF，替换未命中——正式改动一律按文件实际换行符处理。
- 最小验证：DOM 176 项通过。新增按下即选中、按住经过逐行刷同一状态、松手后经过不生效、从已选行按下则沿途取消，以及「按下＋其后的 click 只切换一次」正反两个方向。逐条回退验证：去掉捕获阶段的 `preventDefault` 会让「只切换一次」失败，去掉 `pointerover` 的刷新会让拖选失败，两条断言均非空断言。页面契约 1/1 通过。
- 提交：待定（`[P3.22]report every container in a batch and offer a retry`）——本轮要求合并为一个提交，内容涵盖切片 3 的批次结果与选择重试，以及其后四轮顶栏按钮、下拉菜单和结果弹窗的界面调整。哈希由下一个切片顺延补上。

### P3.22 追加：批量卸载显式启用强制释放（2026-09-16）

- 原文：第 8326～8360 行。
- 基线与审计：开工工作区干净，切片 3 及界面调整已提交为 `dd83ab1 [P3.22]report every container in a batch and offer a retry`。复核 AGENTS、P3.22 计划与技术清单、手动强制单卷流程、批次原生互斥／代次校验和页面选择／结果重试逻辑。决定暂不做暂停、中断、回退取消，改为在卸载菜单“全选”左侧提供“启用强制卸载”；需求与里程碑已同步。
- 构建／提交：Release 初次最终链接因旧实例占用 EXE（LNK1104）失败。确认可正常关闭；未强杀进程，后续检查确认旧实例已退出，Release 重链成功。本片尚未提交、未推送，真实忙卷与界面仍待实机验收。
- 最小验证与安全：DOM 187 项、页面契约 1/1 通过，新增名称／行空白单次勾选、成功项无复选框、卸载失败页显式强制重试所选项及全成功后撤去入口；原提示图标独立操作检查继续通过。原生代码未改，不重复原生测试。名称保持 textContent，强制只在当前结果上下文生效，busy／卡事件保护保留；未进行真实卷操作。
- 确认提交。本次仍使用 P3.22，范围为批量卸载的显式强制选择、结果页强制重试及其配套交互／布局调整；不包含暂停、中断或回退取消。
- 提交前核对待提交文件均属于上述已审查范围，git diff --check 通过；沿用已完成的宿主对象／页面契约、187 项 DOM 与最新 Release 构建记录，不重复运行测试。提交主题：`[P3.22]add explicit forced batch unmount and refine result selection`。未推送；真实忙卷操作未由自动化验证。

### P3.23 第一片：中性偏好跨会话保存（2026-09-17）

- 原文：第 8361～8373 行。
- 基线与审计：P3.22 已提交为 `6e12fb4`，本片开工工作区干净。本轮要求强制卸载也跨会话保存；复核现有设置文件、会话日志状态、窗口尺寸宿主、强制批次／结果重试、需求 R-03／8.6 与页面测试。发现记住容器位置和导入后挂载已有持久化；窗口、日志开关、强制偏好缺失。按已说明范围本片补这三项，登录自动挂载功能留下一片。
- 存储：复用当前 Windows 用户 `%LOCALAPPDATA%/usb-vault/settings.txt`，新增 force-unmount、operation-logging、window-size 三个中性字段。首次默认关闭强制、开启日志、宽敞窗口；旧文件缺字段使用对应默认。新字段逐行精确匹配、单字符范围校验，重复／畸形强制值关闭，窗口限 0～3；所有写入口保留其他字段，沿用临时文件刷新后原子替换。文件只保存开关与窗口预设，不包含日志内容、卡标识、档位、路径清单或批次结果。
- 最小验证：设置文件、会话、宿主对象、页面契约 4/4 通过；后续窗口回读与旧强制资格调整定向复验宿主通过，弹窗错误提示调整后页面契约与 DOM 190 项通过。覆盖旧文件默认、三个偏好与原字段互不覆盖、畸形／重复强制值拒绝、窗口范围、真实临时文件写失败、重新构造会话恢复设置、日志保存失败不改变状态、桥认证／参数校验、卡变化后恢复偏好且不恢复选择、下拉及弹窗内失败反馈。编译时补齐新增会话测试的 LoginMountPolicy 参数；未跑全套或真实卷操作。
- 构建与安全复查：Release 首次重链创建器遇 EXE 占用，确认相关进程已退出后重试成功；最终页面与原生主程序均已重建。git diff --check 通过。没有秘密复制、新的任意路径桥、强制模式自动执行；保存的强制偏好只影响用户随后主动提交的批次。现有配置文件不用于防篡改授权，也不保证多个应用实例并发修改的合并；单实例原子更新与失败反馈沿用既有模型。
- 待验收与提交：真实应用重启后的窗口／设置观感待实机确认，登录自动挂载尚未实现，本片未提交、未推送，确定后进入下一片。
- 提交决定：确认提交本片并进入下一片。沿用上述最小验证，不重复运行；提交主题为 [P3.23]persist neutral application preferences。下一片按最新要求实现加密索引内排序、新容器置顶及一次性高亮，登录自动挂载后续处理。

### P3.23 第二片：加密容器排序存储原语（2026-09-17）

- 原文：第 8374～8384 行。
- 基线与审计：第一片已提交为 `93ea9e4 [P3.23]persist neutral application preferences`，未推送。复核 AGENTS、P3.23 计划、R-03／8.6、索引编解码／随机槽位布局／原子文件替换、会话恢复及页面易失排序。按新要求把排序与置顶／高亮提前于登录自动挂载；技术清单与里程碑同步。遵守小片边界，本片只交付存储原语，页面及原生会话桥下一片接入。
- 实现：固定 768 字节槽位、1024 槽位的文件形态不变，密文内容 v2 在预留区保存 0～1023 顺序；v1 无顺序的旧记录仍可读，按原槽位顺序稳定回退。已排序记录优先，重复顺序稳定处理。密钥、明文解密缓冲仍使用 SecureBuffer；没有明文序号、卡标识或角色字段。
- 置顶插入：独立 prepend 接口组合随机空槽插入与重排，只有新路径置顶，重复导入保持原顺序且不改字节。原 add 入口保持旧行为，正式会话尚未改用新入口，因此当前构建不会自动把用户索引迁移为 v2；本片未修改 UI 或 Release 包。
- 最小验证：索引编解码、索引视图、会话三个定向测试初轮 3/3 通过；将置顶入口与旧入口分离后重建并复验两个索引测试 2/2 通过。新增覆盖 v1/v2 读回、密文内顺序布局、非法顺序、固定长度、重载顺序、遗漏记录保留、外部密钥／填充槽逐字节不变、重复／未知输入零修改、新记录置顶与重复导入不改序。首次测试编译发现 optional 无符号比较告警，修正测试常量后通过。git diff --check 通过；未重复页面测试或全套测试，未访问真实索引或操作真实卷。
- 安全复查与限制：顺序仅存在认证密文内，不引入档位推断或秘密副本；重排绝不写入不可读槽。新增记录随机选取不可读槽沿用现有共享根密钥模型，其多卡／多密钥共用一个文件时的覆盖风险为既有约束，本片未扩大为重排覆盖。旧程序无法读取 v2，未来接入写入后不能用旧程序继续修改同一索引（旧程序可能把不可读记录视为空槽）；下一片接入须明确此兼容边界。并发实例写入仍不保证合并。
- 提交决定：确认第二片提交并继续界面接入；沿用已通过的最小测试与安全复查，提交主题 [P3.23]add encrypted container ordering primitives，未推送。

### P3.23 第三片：排序接入、新容器置顶与短暂高亮（2026-09-17）

- 原文：第 8385～8397 行。
- 基线与审计：第二片已提交为 `b271289`，未推送。复核 P3.23 需求、里程碑、R-03／8.6、加密索引原语、会话接纳／恢复、桥参数与后台互斥、页面拖动／键盘排序、导入／两种创建完成路径及现有回归。此片覆盖已约定的界面接入边界，不包含登录自动挂载。
- 原生排序：新增认证限定的完整容器 ID 排序接口；拒绝空列表、重复、未知、遗漏及超过 1024 项的列表。开启记住位置时先更新加密索引，写入成功后才交换会话顺序；保存失败保留原顺序。关闭记住位置时只改会话，不读写索引。此前未记住的当前容器在显式保存时补入索引，离线记录保留。预先读取已有路径，避免每次拖动为每个已知容器重复解密整份索引。
- 最小验证：会话、宿主对象和页面契约 3/3 通过，优化索引扫描后会话定向复验 1/1 通过；DOM 最终 195 项通过。新增覆盖重建会话恢复、离线记录、非法 ID／列表、认证与工作互斥、关闭记住位置零索引访问、保存失败原序、跨会话页面顺序、导入置顶和一次性高亮、创建弹窗关闭后才高亮。原容量及创建断言按置顶调整；批次测试先显式恢复其预期起始顺序，避免把新持久化行为误报为失败。未跑全套、未操作真实卷或真实用户索引。
- 构建与安全复查：Release 成功，部署 HTML／JS 与源码哈希一致；git diff --check 通过。没有新路径输入桥、秘密复制、明文容器清单或档位字段。顺序只封装在固定大小索引中；手动排序保存失败不会提交部分会话状态。新容器记忆失败仍沿用现有尽力保存策略，不把成功创建／挂载回滚成失败。多实例并发写合并与多密钥共享索引的既有约束不变。
- 提交决定（2026-09-18）：确认第三片提交并继续下一片。沿用已完成的最小验证与安全复查，提交主题 [P3.23]persist container order and highlight new arrivals，未推送。

### P3.23 第四片：登录后后台自动挂载与偏好保存（2026-09-18）

- 原文：第 8398～8409 行。
- 基线与审计：第三片已提交为 `b5ef133 [P3.23]persist container order and highlight new arrivals`，未推送。检查 AGENTS、技术清单 8.7.5、P3.23 计划、设置编解码、登录手动策略、认证桥、批量后台进度／结果与对应测试。原规范要求自动挂载设数量上限或重新协商五秒预算；已提出推荐方案，确认继续后按开工说明采用“先显示仪表盘，再后台挂载全部可用容器，批次允许超过五秒”。技术清单与里程碑同步，不再把自动批次总耗时承诺为五秒；单卷原目标不变。
- 最小验证：设置文件、会话、宿主对象、页面契约 4/4 通过；DOM 最终 205 项通过。新增覆盖默认关闭、旧配置与畸形值、相邻设置保留、重建会话恢复、保存失败保留、桥认证／类型限制、设置不直接挂载、成功登录后台批次与失败继续、刷新不重复、关闭偏好、读偏好失败、错误登录、空容器及读取设置期间拔卡不发起批次。更新旧页面契约的“自动挂载不可接桥”约束；同步可选 WebView 集成夹具的原生设置与异步回读，其目标编译成功，未运行完整实机 WebView 集成套件。
- 构建与安全复查：Release 构建成功，部署 HTML／JS 与源码哈希一致，git diff --check 通过。未扩大任意路径／盘符能力、未持久化秘密／勾选／结果／日志内容、无档位字段；自动流程走同一认证和后台互斥边界，强制卸载偏好不影响自动挂载。未操作真实卷或真实用户设置；多实例设置并发合并限制沿用原实现。
- 待验收与提交：真实卡登录后的进度观感、重启开关恢复及大量卷的实际耗时待实机验收。暂停、中断、回退取消仍暂缓。本片未提交、未推送。
- 提交决定（2026-09-18）：决定先提交当前工作区。复核本片差异与安全边界，git diff --check 通过，沿用上述最小测试和 Release 构建结果，不重复运行。提交主题为 [P3.23]persist automatic mounting after login。批量导入暂缓，下一步仅审查配置工具整合方案；未推送。

### 主应用整合配置工具：方案审查（2026-09-18，尚未实施）

- 原文：第 8410～8418 行。
- 前置提交：决定先提交工作区，P3.23 第四片已提交为 `f058c5c [P3.23]persist automatic mounting after login`，未推送。批量导入暂缓，本次只审查配置工具整合，不执行激活、凭据修改或写卡。
- 审计范围：复核技术清单 R-03、8.6、P2 自动接纳约束及里程碑的开发凭据／量产边界；检查配置工具 main/diagnostics/checks/installers、WindowsCredentialProvider/store_credentials、主程序启动和 CardEnrollment，以及现有配置错误／安装器测试入口。产品初始化与厂商激活仍严格分离；当前 SDK 未确认支持的激活与 API Key 生成不纳入本次整合承诺。
- 方案：主 EXE 提供首次配置与环境修复入口；已配置可用时直接进入既有卡初始化／登录路由。抽取共享检测、凭据保存、安装器服务，避免启动另一个配置 EXE。WebView2 缺失或页面完整性检查失败时使用不依赖 WebView 的原生引导界面；正常环境下提供同一主窗口内的配置体验。未登录页提供配置入口，认证会话／卷操作期间不替换底层凭据。
- 已发现边界：现有配置检测包含对主 EXE 摘要的检查，直接链接回主 EXE 会产生构建摘要自引用，必须拆开共享依赖检查与外部包检查；不降低 helper、厂商 DLL、页面和安装器现有校验。三项凭据目前分项 CredWrite，失败可留下半更新；建议改为版本化整组凭据单条发布并兼容读取旧项，回读验证且不得把失败状态视为就绪。PIN 验证可能消耗卡尝试次数，不能因刷新／轮询无限重试。SDK 仍通过现有 helper 边界调用。
- 验证及风险：本次为代码和文档只读审查，未修改产品代码，未运行新测试或真实卡操作。后续重点验证旧凭据兼容、写失败、缺依赖、安装取消／需重启、错误 PIN 不自动重试、拔卡迟到回调、已初始化卡不重复初始化，以及安装器固定路径／摘要／签名检查。本审查记录未提交，实际整合尚未开始。

### P3.24 第一片：共享凭据服务与整组保存（2026-09-18）

- 原文：第 8419～8430 行。
- 开工依据：确认整合并要求效率优先、只跑最小测试。复核上一节方案、技术清单 R-03／8.6、里程碑及现有凭据解码／保存、配置 UI、诊断映射和测试。按独立可验证边界，本片完成共享凭据存储服务；环境检测和安装服务提取随下一片启动入口接入，尚未开始主应用配置页面。
- 实现：新增可替换的 ICredentialStorage 边界，共享 load/store 完成格式检查、读取策略、发布和回读。Windows 适配器固定目标，不接受任意路径。新记录为 172 字节（四字节标识／版本 + 32 字节 UserSN + 128 字节 API Key + 8 字节 PIN），只用一次 CredWriteW 发布整组，避免此前三次写入造成新旧字段混用。配置器已复用该服务，错误文案改为未能确认整组保存及验证。
- 兼容：新记录仅在 ERROR_NOT_FOUND 时回退旧三项，旧 raw／终止符／UTF-16 格式沿用；损坏、未知版本和权限错误均失败关闭。读取不自动写入，显式保存完成迁移，旧项保留。旧程序不认识整组记录，迁移后须使用新版工具；保留旧项意味着可能留有历史开发凭据，本片不擅自删除。若发布成功但回读失败，新记录可能已经保存，界面仍不得报告成功；并发保存不承诺合并。
- 安全复查：正式代码秘密组装／读取使用 SecureBuffer，Windows 返回缓冲继续清零后释放；固定尺寸、版本和逐字段格式检查先于使用，不输出秘密，不引入普通配置文件／日志／WebView 桥或档位字段。未降低组件、安装器或 helper 安全边界。真实凭据发布失败的系统级行为未做故障注入，现有 Windows API 适配保持单条调用；后续主窗口流程需验证拔卡、互斥及 PIN 显式重试。
- 提交决定：确认第一片提交并继续下一片，沿用上述凭据定向测试和 Release 构建结果。提交主题 [P3.24]store development credentials as a verified bundle；未推送。

### P3.24 第二片：主应用启动检查与原生修复入口（2026-09-18）

- 原文：第 8431～8443 行。
- 基线与审计：第一片已提交为 `5fc630e [P3.24]store development credentials as a verified bundle`，未推送。工作区已有上一轮把 `checks/diagnostics/installers/card_failure` 及配置窗口移入 `src/platform/windows/vikey_configuration/` 的未完成改动：窗口入口仍是配置器的 `wWinMain`、未接入构建、`WM_DESTROY` 缺少退出消息，按小片边界在本片收口。复核 AGENTS、P3.24 方案与需求 R-03／8.6、主程序启动流程、组件摘要生成、配置检测与安装器边界后实施。
- 启动检查：新增静默的 `configuration::environment_ready()`，复用既有检测项，只跑系统架构、组件完整性、WebView2、驱动和已保存凭据，不访问安全卡，因此启动不会消耗 PIN 尝试次数。检查通过直接进入既有卡初始化／登录路由；未通过或检查本身不可用时打开原生配置界面。界面在主 EXE 内用纯 Win32 绘制，缺少 WebView2 也能打开并修复。
- 入口策略：抽出可测的 `configuration::ready_to_start(check, setup)`；检查通过则不打开配置界面，未通过或抛异常都按未就绪处理，配置界面每次启动最多打开一次，界面无法运行时保持不启动。用户关闭配置界面即退出进程，不进入未确认的环境。界面在应用内打开时自动跑一次不含卡的环境检查，把具体原因直接显示出来；“验证安全卡”仍是独立按钮，只有用户点击才访问卡，失败后等待用户修正。保存成功后的提示改为提示重新检查环境。
- 最小验证：`vikey_development_app_config_tests`、`configuration_installer_tests`、`configuration_card_failure_tests` 3/3 通过，新增覆盖检查通过时不打开配置界面、失败时只打开一次、关闭配置界面后不启动、检查抛异常按未就绪处理、配置界面不可用时不启动。`package-release` 构建主程序与配置器成功，未跑全套测试，未访问真实凭据、真实卡或真实卷。
- 安全复查：安装器仍先校验固定摘要与 Authenticode 签名再由用户显式点击启动，路径固定在 EXE 同级 `prerequisites`，无任意路径输入；启动检查不启动 helper、不访问卡、不写日志或设置；没有新增 WebView 桥、秘密副本或档位字段。新增能力是主程序可在用户点击后启动提权安装器，边界与原配置器一致。`git diff --check` 通过。

### P3.24 第三片：配置入口、会话衔接与交付说明（2026-09-18）

- 原文：第 8444～8455 行。
- 基线审查：第二片已提交为 `cde411f`，工作区只留其提交状态记录修改。复核启动门、共享窗口、诊断服务、主窗口／WebView 生命周期、桥守卫、页面渲染与打包说明。发现两处遗漏：配置窗口收到 include_card=false 后调用 diagnose 时漏传参数，可能走默认卡验证；打包脚本安装器策略路径仍指向已移动文件。修复窗口参数传递并取消 diagnose 的默认参数，使遗漏在编译期暴露；更新打包脚本路径。
- 生命周期：配置请求以 PostMessage 退出旧主窗口，在回调返回后移除宿主对象、关闭 WebView、注销设备通知和定时器、释放会话与组件句柄。切换等待期间忽略旧窗口设备事件；重建时清除延迟卡事件，以新探测结果决定初始化／登录。WebView 创建回调带窗口代次，旧回调不能绑定新窗口或访问旧控制器。配置窗口仍为同进程原生窗口，缺 WebView 可用；点击继续后重建主窗口，关闭配置窗口退出应用，不默认重试旧登录。
- 最小验证：宿主桥与应用配置测试 2/2 通过；补充挂载未清理拒绝后宿主定向复验通过。覆盖错误参数、认证／挂载／后台拒绝、宿主请求失败、接受后旧桥失效；环境检测无论哪个前置失败都不调用设备或卡探测。DOM 208 项通过，覆盖入口、失败反馈／清空密码和切换后禁用旧页面。首次 DOM 在并行构建时触发 15 秒套件总预算，放宽为 30 秒后通过，单个条件等待不变。测试编译中的辅助函数名称及测试修改范围问题已修正；未运行全套或真实卡／卷操作。
- 构建与安全复查：package-release 主程序和配置器构建成功，部署 HTML／JS 哈希与源码一致，git diff --check 通过。安装器摘要／签名／固定路径和 helper 隔离不变；新桥不接受路径、命令行或凭据，不写本地角色／档位字段。旧会话在配置前销毁，已认证或残留卷不会被配置入口绕过。当前 Windows 用户下的其他进程可写开发凭据是既有边界，本片不提供跨进程配置锁。
- 待验收与提交：未触碰真实凭据、卡或安装器；原生窗口切换、拔插实机反馈与高 DPI 观感仍待验收，原生配置窗口沿用既有固定像素布局。厂商激活／API Key 生成仍未新增。本片未提交、未推送。
- 提交决定：提交当前 P3.24 第三片已验证工作区，主题 [P3.24]connect configuration entry and rebuild the application session，未推送。最新体验决定另行落实：正常环境下配置应成为主应用风格的一页，未配置显示引导提示；先开发统一安装入口补齐 WebView2／VeraCrypt，再回到内嵌配置页。本次提交仍保留原生配置窗口，不把内嵌页面描述为已完成。

### P3.24 第四片：统一安装入口与依赖许可（2026-09-19）

- 原文：第 8456～8466 行。
- 基线与审计：第三片已提交为 `e804f63 [P3.24]connect configuration entry and rebuild the application session`，未推送。复核技术清单 P3.24、R-03／8.6、里程碑、现有依赖验证／配置修复入口、开发包白名单与第三方许可材料。原工作区已实现 NSIS 外壳、探测 helper 和安装后复检，但仍无条件调用依赖，VeraCrypt 使用 EXE 并保留官方第二套向导；软件包只有一页总说明，未形成“缺失依赖选择 + VeraCrypt 单独明确接受”的统一体验。
- 方案与实现：VeraCrypt 前置改为官方 `VeraCrypt_Setup_x64_1.26.29.msi`，固定 SHA-256 为 `5BA6426983123CFB92BC1F09BD888FDBC0F53F300D0D9C5DA52CE3AEE8D474F0`，签名仍限定已审查的 IDRIX 证书。统一向导先用无提权 helper 探测 WebView2 与驱动：兼容版本勾选并禁用、缺失项默认勾选、不兼容或检测异常阻止继续；主应用始终为当前用户安装。需要安装 VeraCrypt 时才显示完整上游 License，必须勾选接受；随后以系统 `msiexec.exe`、`/qn`、`ACCEPTLICENSE=YES` 和禁止自动重启参数执行，WebView2 使用官方静默参数。
- 打包边界：打包脚本从安装策略头读取两个固定摘要，要求 VeraCrypt 输入为 MSI、验证 SHA-256 和 Authenticode，再以固定文件名暂存。安装器构建前逐文件比对包清单、拒绝 reparse 和不安全相对路径，只生成编译期枚举的安装／卸载清单；不把独立旧配置器装入主目录。NSIS 固定 3.12 并记录编译器、helper、包清单和最终安装器摘要；显式指定 UTF-8 输入。离线前置已压缩，改用非 solid zlib，避免 32 位 NSIS 编译器打包大文件时耗尽地址空间。
- 最小验证：Debug 与 package-release 的 `usb_vault_setup_helper` 构建成功，`configuration_installer_tests` 通过；测试覆盖已有依赖不覆盖、安装结果传播、成功后必须复检、退出码分类、缺失／伪造安装器拒绝。对官方 VeraCrypt MSI 和包内固定 WebView2 EXE 只做真实 SHA-256 与 Authenticode 校验，均通过且未执行。helper 只读探测返回 WebView2=0、VeraCrypt=0，非法参数=64。
- 安全与许可复查：官方发布说明确认 VeraCrypt MSI 支持 `/qn`、`ACCEPTLICENSE=YES` 及禁止自动重启参数；上游许可要求第三方明确接受，统一向导因此展示完整原文而不是摘要替代。MSI 在验证后的文件／目录锁存活期间交给固定 `%SystemRoot%\System32\msiexec.exe`，路径和参数不接受用户输入；WebView2 同样固定路径、摘要和签名。当前结论是工程实现边界，不替代正式法律审查；ViKey DLL 再分发授权、USB Vault 代码签名、干净机／不兼容旧版／真实缺依赖／取消／UAC 拒绝／重启及多机杀软验收仍未完成。
- 提交决定：确认提交统一安装入口，并在提交后启动本地验证安装器进行人工界面验收。提交主题 `[P3.24]add unified Windows installer`；不推送。安装器启动及后续人工操作不纳入该提交内容。

### P3.24 第五片：用户选择安装位置（2026-09-19）

- 原文：第 8467～8475 行。
- 反馈与审计：第四片已提交为 `dd43458 [P3.24]add unified Windows installer`，未推送。首次人工查看向导发现安装位置固定且没有目录页。复核 NSIS 页面顺序、`.onInit` 的固定 `$INSTDIR`、安装成功后 HKCU `InstallLocation`、显式文件卸载清单及卸载器固定默认路径守卫；自定义目录若只增加页面，会导致卸载器拒绝合法位置，因此必须同时调整安装与卸载校验。
- 实现：新增标准安装位置页。目录输入框表示最终安装文件夹并要求目标不存在或为空；默认路径为 `%LOCALAPPDATA%\Programs\USB Vault`。曾按“选择上级位置后追加固定子目录”实现，但人工查看发现 NSIS 标准浏览控件会保留输入框末级名称：默认输入为 `%LOCALAPPDATA%\Programs` 时，选择桌面会错误得到 `D:\Desktop\Programs`。现改为让默认输入末级直接为 `USB Vault`，选择桌面时得到 `D:\Desktop\USB Vault`，且不再二次追加子目录。网络盘、可移动盘和磁盘根目录拒绝，界面以易懂文字提示路径不要包含中文；不覆盖升级。安装注册仍写 HKCU，凭据归属和共享依赖范围不随目录改变。
- 卸载安全：卸载器读取 HKCU 保存的实际 `InstallLocation`，规范化后使用 Windows ordinal ignore-case 比较自身目录；注册缺失、路径不一致或复制／移动后的卸载器均拒绝执行。删除范围仍为构建时生成的显式文件和目录列表，不递归清除安装根，不删除依赖、凭据、设置、索引或容器。
- 最小验证：使用真实 NSIS 3.12、UTF-8 输入和小型依赖占位包完成 7 页安装器语法／链接验证；修正后的“最终安装目录”版本生成 201,615 字节测试 EXE，退出码 0。随后用清单一致的本地测试包完成 311,929,077 字节完整离线安装器重建，SHA-256 为 `13114F406201522A724862055335700A63B4083D497574F3D73E2AF84EB87C36`。本轮仅运行安装器脚本编译、完整包封装及产物哈希核对，未执行应用测试、依赖安装、正式安装、卸载或 UAC 流程；新版向导在构建后启动，交由人工确认选择桌面时的最终显示路径。
- 安全复查与状态：用户输入只作为最终安装位置，不进入命令行、依赖安装路径或生成脚本；`$INSTDIR` 在离开目录页时规范化，并继续限制为固定磁盘上的非根目录及不存在或为空的目标文件夹。卸载器仍以 HKCU 登记路径和实际运行位置双向校验，避免从复制或移动后的位置删除错误目录。当前改动未提交、未推送。

### P3.24 第六片：安装向导文案与产品显示名称（2026-09-19）

- 原文：第 8476～8485 行。
- 反馈与审计：人工查看欢迎、依赖、许可及目录四页后给出逐句文案，并要求从本片起将面向用户的“USB Vault（开发版）”统一为“USB Vault”。复核安装脚本、主窗口、配置窗口、许可说明和随包文档；区分产品显示名称与仍承担构建／兼容作用的 EXE 名、CMake 目标、窗口类、Credential Manager 目标及卸载注册表键，后者本片不改名，避免无关迁移风险。内部测试、未签名和第三方再分发待审查说明继续保留。
- 实现：欢迎页改为两段指定文案并移除“不会再打开单独向导”；依赖页精简顶部说明，兼容项仅显示“已安装”，底部只说明勾选项将请求管理员权限安装对应发布者原版组件；首个许可页顶部和目录页顶部改为指定短句。安装器 `Name`、卸载 `DisplayName`、主窗口、配置／凭据窗口、保存成功提示及第三方说明／README 标题统一使用 `USB Vault`，不再把“开发版”作为产品后缀。
- 验证与状态：首次直接调用 CMake 因未加载 MSVC／Windows SDK 环境而失败；加载仓库既有 VS2022 x64 开发环境并保持 UTF-8 后，Release 主程序和独立配置器定向重建成功。使用真实 NSIS 3.12 和小型依赖占位包完成安装器语法／链接验证，生成 201,421 字节测试 EXE；随后用新二进制、新许可说明和重新计算的 303 文件清单完成 311,978,626 字节完整离线安装器，SHA-256 为 `0185825C050FF5D64509F18C12542BC4C6991F7B6FF7208FC61A85CC5053E237`。本轮未跑全套业务测试，未安装／卸载依赖或应用；已关闭上一版向导并启动本版安装器（PID 66948），交由人工复查四页文案。
- 欢迎页视觉回修：人工截图显示简体中文语言文件默认的 9 磅宋体笔画发虚且行距紧，两个语义段也只换行未留空。第一版用全局 10 磅 `Microsoft YaHei UI` 改善字体，但 `SetFont` 同时改变 NSIS 对话框基准单位，导致窗口尺寸变化，不符合保持原尺寸的反馈。现撤销全局字体，让 NSIS 继续按原模板创建窗口；仅在欢迎页显示回调中把标题设为 12 磅、正文设为 10 磅 `Microsoft YaHei UI`，正文更大字体度量提高行高，两段之间保持双换行。未引入自绘或额外页面控件，页面逻辑与安全边界不变。
- 提交决定：人工迭代确认安装路径、产品显示名称、四页文案、欢迎页段间距、原窗口尺寸及 9 磅正文后，提交第五、六片的当前已验证改动；提交主题为 `[P3.24]polish installer path and product presentation`，不推送。

### P3.24 第七片：允许安装器创建新的目标目录（2026-09-19）

- 原文：第 8486～8493 行。
- 反馈与根因：`0b3e051 [P3.24]polish installer path and product presentation` 提交后，人工选择尚不存在的 `D:\Desktop\USB Vault` 被提示“请选择本机固定磁盘上的有效绝对路径”。该路径和父目录实际有效，目标目录按安装器设计应由安装过程创建。根因是目录页离开时把完整目标交给 NSIS `GetFullPathName` 指令；该指令在路径部分不存在时设置错误并返回空字符串，因此把正常的新目录误判为无根路径。
- 修复：先从原输入确认盘符根和 `X:\` 绝对路径形式，再调用 Windows `GetFullPathNameW` 规范化路径。该 API 不要求最终目录预先存在；返回失败或超过 NSIS 字符串容量时明确拒绝。规范化后继续检查不能直接使用磁盘根，并把带尾反斜杠的根传给 `GetDriveTypeW`，只接受 `DRIVE_FIXED`。目标应用存在与目录非空检查保持原样，新目录仍由安装阶段创建。
- 最小验证：使用真实 NSIS 3.12、UTF-8 输入和小型依赖占位包完成脚本语法／链接验证，生成 201,610 字节测试 EXE；复用已验证的 303 文件清单完成 311,978,835 字节完整安装器，SHA-256 为 `F0D076BE8280661AD5D08451CB50135EE1DEAF3C8D6EF3DE31A6CF4D1B9AD0D0`。本轮未重建主程序、未跑业务测试、未安装依赖或应用；已关闭旧安装器并启动修复版本（PID 58724），交由同一路径人工复查。
- 安全复查：相对路径、盘符相对路径、UNC／网络盘、可移动盘、磁盘根、过长路径、已有应用及非空目标仍拒绝；用户路径不进入依赖安装命令行或生成脚本。卸载器登记路径绑定、显式删除清单及用户数据保留策略不变。当前改动未提交、未推送。
- 提交决定：确认将第七片路径修复并入上一提交 `0b3e051`，保留 `[P3.24]polish installer path and product presentation` 主题；amend 后提交哈希更新，不推送。

### P3.24 第八片：配置界面并入主应用（2026-09-19）

- 原文：第 8494～8521 行。
- 计划审计：`需求梳理清单_v2.md` 2026-09-18 增补要求“在主 EXE 内打开不依赖 WebView2 的原生配置与修复界面”，并明确“凭据不经过网页或新增桥参数”。本次需求是把配置并入主应用界面并与产品主题一致，直接与这两条冲突。前提已变化：2026-09-19 的统一安装器在复制主应用前就装好 WebView2 与 VeraCrypt，原生界面存在的理由（缺 WebView2 时仍可修复）不再成立。冲突已记入需求清单 2026-09-19「配置界面并入主应用」一节与 `MILESTONES.md`，由该节取代旧约束。
- 待确认项与决定：用户给出的默认值 `111111` 只有 6 位，与固定 8 位冲突；确认按 8 位处理，配置页预填 `11111111`，不动凭据束格式与 helper 协议版本。界面形态确认为主窗口内的 WebView2 主题页。原生配置窗口确认彻底删除，依赖缺失时只说明原因并退出。
- 实现：新增 `usb_vault::ui::IConfigurationHost` 与 `create_card_configuration_webview_host_object`（`include/usb_vault/ui/card_configuration_webview_host_object.h`、`src/ui/card_configuration_webview_host_object.cpp`），只有 `getStartupView`／`getCredentialStatus`／`saveCredentialBytes`／`continueToSession`／`requestClose` 五个成员，桥本身厂商中立，落在 `usb_vault_core`。
- 未决风险：用户密码默认值按 8 位 `11111111` 预填，若实际出厂值是 6 位，需要改动凭据束布局和 helper 报文长度（协议版本要升），本次未做。依赖缺失时应用内不再有修复入口，直接解压的测试包必须重新运行安装器或手动补依赖。配置页的视觉效果本轮只做了静态与 JSDOM 验证，真实 WebView2 下的排版仍需人工看一次。
- 提交状态：改动未提交、未推送，等待用户决定。

### 设置页开关质感与自动挂载说明（2026-09-19）

- 原文：第 8522～8529 行。
- 需求：设置页的开关切换要有 iOS 那样的丝滑玻璃过渡；自动挂载那一行的说明文案改写。
- 可行性判断：开关本来就是自定义 `<button role="switch">` 加 `::after` 旋钮，已有 140ms `ease` 过渡，缺的是运动曲线和质感，纯 CSS 可做，不涉及原生、桥或新增资源。评估后明确不使用 `backdrop-filter`：它模糊的是元素背后的内容，而背后是 `.settings-card` 的一块纯色，真模糊会多出合成层却看不出效果，因此玻璃用分层半透明加亮边“画”出来。
- 实现：轨道改为分层半透明渐变加内阴影与亮边；点亮态拆成独立的 `::before` 图层，用 `opacity` 淡入，颜色变化不触发重绘也不会与旋钮脱拍；旋钮改为带顶部高光与投影的玻璃球，`transform` 用 `cubic-bezier(.34, 1.56, .64, 1)` 320ms 做过冲回落；按住时 `scaleX(1.22)` 拉成胶囊，`transform-origin` 按开关状态在左右端之间切换，使拉伸只向轨道内侧发生。动画只用 `transform` 与 `opacity`，留在合成器线程。`prefers-reduced-motion` 下过冲退化为 120ms 线性且不再拉伸，与页面既有动画的处理一致。
- 文案：自动挂载行的 `<b>` 与 `<small>` 都是行内元素且此前没有间隔，实际渲染是“登录后自动挂载全部容器登录后在后台依次挂载…”连成一句。按要求改为前置一个空格加括注“（根据容器数量可能造成较长耗时）”，顺带修掉这个粘连。
- 验证：`card_product_ui_page_contract_tests` 通过；`card_product_ui_navigation_dom_tests.cjs` 208 项通过；`card_product_ui_setup_dom_tests.cjs` 两轮 20／7 项通过；显式重建 `usb_vault_vikey_p2_development_app`（该目标是 `EXCLUDE_FROM_ALL`，不点名就不会带上新页面摘要）。动画与文案不写任何状态，与 8.4／8.6 的痕迹约束无关。渲染效果由人工在实机确认。

### 主程序可执行文件正式更名为 USB Vault（2026-09-19）

- 原文：第 8530～8536 行。
- 背景与冲突：需求清单 2026-09-19「统一安装入口」一节写明“内部构建目标、可执行文件名、注册表键及协议标识可以暂时保留既有 development 命名以维持兼容”。本次按要求把用户实际双击的那个文件也改成产品名，取代该条中关于可执行文件名的部分；构建目标名、helper 与建卷器的文件名、注册表键维持不变。
- 改动：`usb_vault_vikey_p2_development_app` 目标增加 `OUTPUT_NAME "USB Vault"`，产物变为 `USB Vault.exe`（安装目录里早已有带空格与中文的 `卸载 USB Vault.exe`，命名风格一致）。同步 NSIS 的完成页启动项、安装目录存在性判断和开始菜单快捷方式；同步打包脚本的必需文件清单与安装器脚本的载荷校验清单；同步 README 与测试须知里的文件名。打包默认名由 `usb-vault-p2-development-test-<时间戳>` 改为 `usb-vault-<时间戳>`。
- 顺带清理：删除 `packaging/p2-development/Start USB Vault.cmd.in`。它没有被任何构建或打包脚本引用，实际不进包，内容还指向旧的可执行文件名；统一安装器已经创建开始菜单快捷方式，这个兼容入口没有存在理由。README 中对应条目一并删除。
- 验证：显式重建 `usb_vault_vikey_p2_development_app`，产物为 `USB Vault.exe` 并可正常启动进入登录页。本轮不涉及 C++ 逻辑与页面，未重跑业务测试。

### 客户测试交付包（2026-09-19）

- 原文：第 8537～8545 行。
- 交付形态确认：统一安装器自身已内置 WebView2 离线安装器与 VeraCrypt 官方 MSI，因此软件侧只需要这一个 EXE，不再需要随附可解压的完整测试包。
- 打包过程：`package-release` 预设重建 `usb_vault_p2_development_package_binaries` 与 `usb_vault_setup_helper`；先删掉构建目录里残留的旧 `usb_vault_vikey_p2_development_app.exe` 与已移除的独立配置器产物（打包脚本按白名单复制，本来也不会带上，删除只是避免误解）。`package-p2-development.ps1` 产出 `out/packages/usb-vault-20260919-055648`（14 个文件，约 318 MB），构建标识记录源码修订 `82fed0f`、工作区 clean。
- 依赖来源的一个坑：`out/downloads/MicrosoftEdgeWebView2RuntimeInstallerX64.exe` 的 SHA-256 已变成 `70DBDC1A…`，与 `installers.h` 中评审过的 `E7FA3575…` 不符——微软的 Evergreen 离线安装器同一 URL 内容会被替换。本次改用 `out/build/dev/prerequisites/` 下仍为评审版本的副本。打包脚本的摘要校验拦住了这个问题，说明该校验是必要的。
- 交付内容：`out/deliveries/USB-Vault-20260919.zip`（300,290,516 字节，SHA-256 `AA803000BC76E550AEB952FC5DEEB1047DBBC2B7EA924D0C5719209CFBDCC705`），含 `USB-Vault-Setup.exe`、`测试须知.txt` 和 `SHA256.txt`。
- 仍需人工完成：安装器未签名，客户首次运行会遇到 SmartScreen 提示；安装前必须关闭正在运行的 USB Vault，否则向导会拒绝；客户还需要自备 ViKeyPRO 安全卡与供应方下发的 UserSN／API Key／用户密码，这三项不包含在交付包中。本轮未在本机安装该安装器，实机安装验证交由人工。

### 全面清理前盘点（2026-09-19）

- 原文：第 8546～8558 行。
- 本轮范围：按用户要求检查可清理内容，未删除文件、未修改生产代码、未提交或推送。保留进入本轮前 DEVELOPMENT_LOG.md 与 packaging/p2-development/测试须知.txt 的未提交改动。
- 计划审计：核对 MILESTONES.md、需求清单 R-03／8.6、第三方 FormatDLL 说明和近期交付记录；spike 为可行性证据，但现有本机构建仍依赖其中的 DLL，不能按目录名直接清空。
- 检查范围：主工程与测试 CMake、全部 preset、打包脚本白名单与摘要检查、现有 CMakeCache 的 FormatDLL 路径、第三方 README、设计稿引用、Git 对象统计。src/tools/tests/third_party 中 C/C++ 文件名均能在主工程或测试 CMake 文本中找到；仅为构建引用检查，不等于函数级死代码证明。
- 依赖风险：dev、integration、p1_7_acceptance 指向 spike/_vcref/src/x64/Release/VeraCryptFormat.dll；asan、analyze、package-release 指向 out/build/integration-release/VeraCryptFormat.dll。先固定第三方依赖位置并重配验证，才能清掉这些目录。dev/prerequisites 下 WebView2 的 SHA-256 为 E7FA35755196AD9223596EF021A1CE6799509142EAA40BA35F634026BE50B831，与近期打包使用版本一致，必须保留有效副本。
- 提交状态：本轮仅追加本审计记录，未执行清理，等待用户确定后续清理范围；具体实施按小片验证和报告。

### 清理第一片：历史交付产物（2026-09-19）

- 原文：第 8559～8569 行。
- 授权与边界：用户确认“开工吧”。沿用前述需求/计划审计及 R-03、8.6 约束，本片只删除历史交付产物，未调整源码、测试、构建配置、第三方依赖及真实/实验容器。下一片依赖整理与旧构建清理须在本片报告和提交决定后进行。
- 实施：删除 13 个旧安装器目录，保留 client-20260919；删除 out/packages 下旧 usb-vault-p2-* 包及随附清单；清理 out/package-validation 的旧解压测试副本及其中 WebView2 测试缓存；删除根目录 final_path_probe.obj、p1_3_fixture_prepare.obj。共 46 个顶层目标、1447 个文件，逻辑大小 7.407 GiB。删除清单保存在 out/reports/cleanup-20260919-manifest.json（本地忽略文件）。
- 安全复查：删除前解析每个绝对路径并验证位于仓库内，拒绝目标或子项重解析点，核对目标没有 Git 跟踪文件，检查没有 hc/tc/vc 容器及指定敏感配置名称；使用 PowerShell Remove-Item -LiteralPath，未跨 shell 删除。未接触 .git、凭据存储、当前构建和 spike。一般文件删除不等于安全擦除。
- 保留与验证：最新客户安装器、交付 ZIP/目录、最新完整包及关键 FormatDLL/WebView2 依赖共 25 个文件删除前后 SHA-256 完全一致；最新完整包全部 14 文件通过已有 sha256sums 清单验证；交付 ZIP 与此前记录 AA803000…DCC705 一致；46 个删除目标均已不存在。
- 剩余风险：构建目录间及 spike 的 DLL 交叉引用尚未解除，故旧构建、实验容器与资料归档暂未处理。后续继续时先固定依赖来源并验证重建。
- 提交状态：未提交、未推送；Git 中本片只追加开发记录，原有 packaging/p2-development/测试须知.txt 改动保持原样。按仓库小片规则，报告后由用户决定是否提交本片记录或暂不提交再继续。

### 清理第二片：独立依赖与旧阶段构建（2026-09-19）

- 原文：第 8570～8583 行。
- 授权与计划审计：用户要求继续下一步，保持未提交。复核阶段视图、R-03／8.6、FormatDLL 第三方边界、CMake 配置、打包摘要/签名检查；本片只整理本机依赖与旧构建，不改变生产安全逻辑。
- 依赖核对：dev 使用的 spike DLL 与 package-release 使用的 integration-release DLL、最新包内 DLL 均为 E411BB95…28AB3；out/vendor 的同名 DLL 为 3175C87A…BEBA71，不能直接作为替换来源，保留原件未覆盖。
- 校验脚本修正：首轮删除后检查误把 p1_7_acceptance 下全部根文件（含应删除的 .ninja_deps）当成保留对象，导致校验报错；删除范围本身未受影响。随后逐项确认清单所有目标均不存在、仅两个容器保留且大小符合原值，并记录当前摘要。未宣称容器通过前后摘要对比，因为前置摘要未持久化。
- 验证：dev 主应用、package-release 发布集合及 setup helper 构建通过；secure_buffer_tests、creator_protocol_tests、configuration_installer_tests、vikey_artifact_tests、card_product_ui_page_contract_tests 共 5/5 通过；git diff --check 通过。独立依赖全部摘要通过，上一片保护清单中除有意退休的 integration-release DLL 外其余交付/依赖文件摘要均未变。本片未重新制作安装包、未执行硬件测试。
- 未决与提交：旧实验容器、资料归档及更细的死代码审计仍留待后续片段。现有 out/vendor 旧 DLL 未删除，以免影响未纳入正式构建的外部实验脚本。未提交、未推送；本片受版本控制的改动仅第三方 README 和开发记录，原测试须知改动保持不变。按小片工作约定，报告后等待用户提交决定或继续指示。

### 清理第三片：设计稿、历史资料与临时工具（2026-09-19）

- 原文：第 8584～8593 行。
- 授权与审计：用户要求整理设计稿、旧资料和遗留工具；按此前逐片约定保持未提交。复核 AGENTS.md、MILESTONES.md 对两版设计稿的定位、现行需求 8.6、CMake 工具目标及测试引用。技术需求 v2、阶段视图、开发日志继续留在根目录，不改变其优先级。
- 资料归档：全流程 UI 稿移至 docs/design/usb-vault_ui-design.html；已作废账号/分类导航的旧原型移至 docs/archive/design/usb-vault_ui-disign.html；初版需求清单、基本需求图片与客户 Word 原件移至 docs/archive/requirements。五个文件均原样移动，移动前后 SHA-256 一致。两版 HTML 没有需要随迁的外部相对资源；没有执行其中脚本或将原始资料中的内容当成本轮指令。
- 导航与引用：新增根 README、docs/README.md 和 tools/README.md，说明现行需求入口、资料定位、正式与开发工具用途及 out 的清理边界；更新 MILESTONES.md 三处设计稿路径。历史开发日志保持原路径，docs 索引给出映射；正式 UI 中按文件名提及设计稿的注释仍有效，不为注释变更触发部署摘要重建。
- 临时工具清理：删除 out/p1_7_diag_acl.ps1、p1_7_diag_pipe.ps1、p1_7_step1_fixture.ps1、p1_7_step2_dev_app.ps1、p2_run_dev_app.ps1。复查构建/批处理/脚本引用仅发现这五个文件内部的互相提示，不是正式构建或打包输入。未运行脚本，未提权、建卷、读卡或重置卡。
- 验证：五份归档文件摘要一致且旧路径不存在；三份新 README 的本地链接全部可解析；五个旧脚本全部不存在；git diff --check 通过；现有 dev 的 card_product_ui_page_contract_tests 1/1 通过。仅资料与忽略脚本整理，未改生产/测试实现，未做全量构建或硬件测试。

## 后续日志写法

每个可独立审查的切片只写一条，通常 8～15 行；同片多轮反馈合并为最终行为，避免重复追加过程。

- 日期、阶段与范围：做什么，以及需求冲突或明确取舍。
- 检查与实现：涉及的模块、关键决策；不逐文件罗列机械改动。
- 验证：实际执行的构建/测试及结果；注明未执行的硬件或全量验收。
- 安全与风险：本片修复、仍存在的限制和后续事项；不能只写“安全通过”。
- 提交：未提交/用户决定/实际提交哈希；未经决定不提交，不推送。

## 2026-09-19 — 开发日志压缩

- 范围：按用户要求保留 1000～2000 行；复核原日志结构、阶段/切片记录、近期 Git 历史与现有文档规则。
- 实现：完整原文按字节归档，主日志按阶段提取关键结论，保留原文行号；不改写技术需求或历史提交事实。
- 安全：原文中的详细安全发现、失败尝试和测试限制可完整追溯；主日志明确区分历史记录、当前状态和待验收边界。
- 验证：归档 SHA-256、目标行数、阶段覆盖与文档链接检查；仅文档变更，不重跑业务测试。
- 提交：2026-09-19 用户已批准提交；资料归档和日志压缩随本条记录落库，交付说明与依赖说明已分别提交；未推送。

## 2026-09-19 — 简短日志规范与提交

- 范围与审计：用户要求将简短模板写入 AGENTS 并提交；复核本地工作约定、忽略规则和全部待提交文档差异。
- 检查与实现：AGENTS 新增每片 8～15 行模板、同片反馈合并、1000～2000 行归档阈值及历史状态区分规则；该文件按既有规则仅本地保存，不强制入库。
- 验证：归档 SHA-256、主日志行数、资料移动摘要、文档链接与 git diff --check 通过；沿用各清理片已记录的构建/测试，不重复业务测试。
- 安全与风险：原始需求和历史安全记录完整保留，没有生产代码变更；本地 AGENTS 规则不会通过此次 Git 提交分发。
- 提交：交付说明 `969bacb`；依赖说明 `419ed62`；资料整理与日志压缩按用户授权随本条提交，提交哈希以 Git 历史为准。未推送。

### 2026-09-19 — P3.25 第一片：保护挂载的驱动边界

- 范围与审计：本片按用户编号记为 P3.25，实现模式 C（8.5「更新诱饵」）的第一层。`MILESTONES.md:164` 把模式 C 列在 P2 任务表里，编号与实际开发阶段不一致，功能归属和验收口径不变，冲突记录于此，不在本片改动阶段视图。"剩余可写空间"的口径已定为乙案：提权复用簇位图扫描，落在后续切片，本片不涉及。
- 检查与实现：`veracrypt_driver_abi.h` 早已有 `protect_hidden_volume`、`protected_hidden_volume_password/pkcs5_prf/pim` 四个字段及偏移断言（716/720/868/884），缺的只是产品侧入口——`populate_mount_request` 此前显式把它们留零。现给 `MountOptions` 增加 `std::optional<HiddenVolumeProtection>`：内含借用的隐藏卷口令指针、PRF 与 PIM，缺省即不开保护，保护是单次挂载的属性，不落任何本地产物。校验与既有边界同级：空指针、空口令、超 128 字节、非 SHA-512、PIM 越界一律拒绝；两处口令拷贝合并为 `copy_password`，请求缓冲区本就清零，短口令的终止字节自然为零。
- 验证：`cmake --build out/build/dev --target volume_provider_tests` 通过，`volume_provider_tests.exe` 退出码 0。新增用例覆盖保护字段落位、两个口令槽互不串位（含 PIM 分别落在相距 4 字节的两个字段）、以及五种非法保护参数被拒。本片只跑这一个最小目标，未跑全套，未触碰驱动、未做实机挂载。
- 安全与风险：保护口令随请求拷入 `LockedDriverBuffer` 的锁页 `SecureBuffer`，`SecureBuffer::release()` 释放前先 `SecureZeroMemory`，故 `mount_file` 的成功、驱动拒绝与 IOCTL 异常三条路径都会抹掉两份口令，符合 R-03。残留风险一条：`HiddenVolumeProtection` 持有裸指针，调用方必须保证口令在挂载调用期间存活，该约束只由头注释声明，编译期无法强制。统一拒绝、事务性回滚、恒显入口等否认性要求属会话层与页面层，本片不承担。
- 提交：`f1d5b0b [P3.25]carry hidden-volume protection into the mount request`。未推送。

### 2026-09-19 — P3.25 阶段计划落库

- 范围与审计：按已定的页面设计把模式 C 拆成九片写进 `MILESTONES.md`。复核 8.5、8.7.2、8.6 与设计稿的两块维护屏；模式 C 原列于 P2 任务表，本轮按 P3.25 推进，编号错位已在该节写明，功能归属与验收口径不变。
- 检查与实现：记录三项取代关系——入口由设计稿的容器详情行按钮改为左侧导航恒显一列；剩余可写空间取提权簇位图而非"外层剩余减内层容量"的估算；维护屏不含完整性告警，N-07 机制 1 仍独立未做。"内外层文件列表不能同屏"按物理约束写明，不是降级方案。P3 段标题补注 P3.25 仅第一片完成，避免"全部切片已实现"覆盖到尚未开始的切片。
- 验证：仅文档变更，未重跑业务测试；`git diff --check` 通过。
- 安全与风险：计划内明确记下三条已定并接受的行为——强制卸载会丢失被卸容器中未落盘的写入；过滤后的列表在内存中枚举了哪些容器带隐藏卷；验证通过的诱饵口令驻留至登出。强制卸载仍可能失败的少数容器不会进入列表，改由聚合提示说明数量。
- 提交：`d22bfdf [docs]plan the decoy maintenance stage`。未推送。

### 2026-09-19 — P3.25 第二片：单容器维护事务

- 范围与审计：按 `MILESTONES.md` P3.25 切片表实现第 2 片。复核 8.5 的两个密码分工、8.6 的不存档位、8.7.2 的统一拒绝，以及 `unmount_retry.cpp` 既有的升级策略。本片只做单容器，批量遍历留给第 3 片。
- 检查与实现：会话新增 `open_decoy_maintenance` 与 `verify_decoy_maintenance`。外层口令由调用方传入的诱饵口令派生，隐藏卷口令取会话已缓存的登录派生值，用户只输一个。已挂载的容器先释放，走既有的 `unmount_forced`（先正常卸、仅在忙时升级为强制），因为同一容器文件不能同时挂出两个卷。挂载后必须由驱动报外层类型才算成功，报其他类型（含隐藏卷本身应答）立即卸载并拒绝。运行时接口新增 `mounted_volume_type`，只在这一处调用，不持久化。试挂模式不记操作历史、不通知外壳，验证完即卸。
- 验证：`card_login_mount_session_tests` 全部通过（含新增 4 组：成功路径的事件序列与两个口令各自落位、三种失败逐字同一条拒绝、试挂不留挂载且只记一次卸载、前置条件与释放失败不被折叠进拒绝）；顺带重建并运行 `volume_provider_tests`、`card_product_ui_session_controller_tests` 均通过。未接真实驱动，未实机。
- 安全与风险：全部驱动失败与类型不符收敛为 `DecoyMaintenanceRefused` 单一消息，会话本身也分辨不出是哪一种。派生出的外层口令在所有返回路径清零；隐藏卷口令不复制，仅以指针交给挂载请求，由第一片的锁页缓冲区负责擦除。未新增操作历史种类，未写任何本地产物。遗留：先卸载后验证是设计使然，诱饵口令打错也会先失去已挂载的隐藏卷，由确认弹窗承担知情同意；强制释放失败的容器仍记为已挂载以便登出时回收，错误照实上抛；类型判据依赖"驱动仅在开保护时报外层"，该条仍在第 9 片实机清单首位。
- 顺带：按本轮要求把本阶段两片的代码注释压到最多两行，包含第一片已提交文件中的长注释。
- 提交：`3593416 [P3.25]open an outer volume for decoy maintenance`。未推送。

### 2026-09-19 — P3.25 第三片：批量验证

- 范围与审计：按切片表实现第 3 片。复核已定流程的跳过与强制卸载规则、8.6 的不存档位，以及第 2 片留下的单容器事务边界。进度与取消只做会话侧的同步接口，线程与轮询归第 5 片的桥。
- 检查与实现：新增 `survey_decoy_maintenance`，一次派生外层口令后走完全部容器，逐个复用第 2 片的试挂事务，通过即记下并卸载。跳过规则落为「已知走标准派生的容器一律不碰」——不只是当前挂载的 A 类，未挂载但类型已知的同样跳过，因为它们没有外层可开，试了必然失败；类型只来自本会话挂载时记下的运行时事实，不读也不写任何持久化判据。释放不掉或取不到盘符的容器计入 `unreachable`，不进列表，walk 继续。派生出的外层口令在至少一个容器应答时留到登出，`open_surveyed_maintenance` 据此打开，用户不再输第二次。
- 验证：`card_login_mount_session_tests` 全过，新增 4 组（只列应答的容器且标准容器不被释放、一次 KDF、释放失败计数后继续、取消标记与空结果不留口令、已验证容器免口令打开并随密钥释放失效）；`card_product_ui_session_controller_tests`、`card_product_webview_host_object_tests` 重建后通过。未接真实驱动，未实机。
- 安全与风险：审计中发现 `force_unmount_active`（拔卡路径）此前内联清理两个密钥，不会清到本片新增的口令；已改为复用 `release_session_secrets` 并补测试，避免同类遗漏再出现。留存的是派生值而非用户输入的明文。`unreachable` 的 catch-all 也会吞掉分配失败一类的异常，只计数不终止，判断依据是「这不是关于口令的答案」。
- 提交：`8e0de0d [P3.25]survey every container with one decoy password`。首次提交的正文 107 词超出 50～90 的硬上限，已改写为 86 词并 amend，哈希随之更新。未推送。

### 2026-09-19 — P3.25 第四片：容量事实

- 范围与审计：按切片表实现第 4 片，落地已定的乙案（提权簇位图）。复核 8.5 对"还可添加"的要求、建卷 helper 既有的管道安全与提权边界。本片只做后端度量，页面显示归第 6 片。
- 检查与实现：新增 `usb_vault/volume/volume_space.h`，两件事分开——`volume_space()` 走 `GetDiskFreeSpaceExW` 取总量与本调用方可写空间，不需要提权，可随时刷新；`scan_volume_tail_space()` 是从 `creator_helper_engine.cpp` 原样抽出的簇位图扫描，改抛通用异常，建卷路径的包装层仍映射为 `bitmap_scan_failed`，对外行为不变。提权通道复用既有的建卷 helper：协议新增 `Operation::scan_tail_space`，`Response` 增加 `value` 字段承载测得的字节数，协议版本 1→2。度量请求不带任何口令和尺寸，两侧都按操作分别校验；helper 侧还要求路径恰好是 `X:\` 形式的卷根。扫描不加载 VeraCrypt 格式化 DLL、不碰任何文件，测完即退，不常驻。
- 验证：`creator_protocol_tests` 通过（新增 3 组：度量请求不得带口令或尺寸、无口令往返、测得的数字只随成功结果传输且进度报文不得携带）；新建 `volume_space_tests` 通过（真实卷的总量与可写空间、大小写盘符等价、非盘符一律拒绝、不存在的盘符报系统错误而非零值——用 `GetLogicalDrives` 挑选未占用盘符，不写死）。重建 `usb_vault_volume_creator` 与 `usb_vault_core` 通过。真实外层卷上的扫描数值准确性与提权流程仍归第 9 片实机。
- 安全与风险：新增的提权操作不接收任何秘密，最坏情况只能让一个已经通过 UAC 的调用方读到自己指定卷的尾部空闲量；管道仍是一次性、带 nonce、DACL 只列 SYSTEM 与令牌用户。`valid_response` 拒绝"带着数字的失败"和"带着数字的进度"，恶意 helper 无法借这个字段夹带。协议版本已升，旧 helper 配新应用会在报文校验处失败并报协议错误，属失败关闭；打包脚本本就同时构建两者，需保持同版本分发。
- 提交：`504e479 [P3.25]measure what an outer volume can still take`。未推送。

### 2026-09-19 — P3.25 第五片 a：控制器投影

- 范围与审计：切片表第 5 片（桥与页面骨架）体量偏大，按 P2.15、P3.6 的先例拆为 a 控制器投影、b 原生桥、c 页面三段，先做 a。复核控制器既有的口令入口写法（`MutableInputWiper` + `consume_password_utf8`）与路由校验。
- 检查与实现：控制器新增 `survey_decoy_maintenance` 与 `open_decoy_maintenance`。诱饵口令以可变缓冲区进入，成功、失败、拒绝三条路径都抹零，口令本身留在会话而不是页面。向页面投影的是 `DecoyMaintenanceUiSurvey`：可维护容器的标识符、未能判定的数量、是否被中止；容器路径、卷类型、档位一律不过这层边界。两个入口都要求已认证状态，否则按既有口径抛 `CardError::invalid_operation`。
- 验证：`card_product_ui_session_controller_tests` 通过，新增 4 组（口令原样传到会话并在成功后抹零、失败路径同样抹零、未认证时不触达会话且缓冲区抹零、打开维护只传标识符）。首轮用 `std::logic_error` 断言拒绝类型失败，实际是带 `CardError::invalid_operation` 的 `std::system_error`，已按真实类型改断言。
- 安全与风险：本片不新增持久化、不新增线程，页面仍拿不到路径与卷类型。b 段（原生桥）要把遍历放到工作线程并复用既有的卡纪元／中断机制，c 段才是导航列与页面。
- 提交：`355a670 [P3.25]project the decoy survey onto the page boundary`。未推送。

### 2026-09-19 — P3.25 三项决策与第五片 b：原生桥

- 决策回写：二级页由 P3 既有文件管理组件接管临时挂载；诱饵新鲜度面板要做，只扫顶层并限条目数、每次现算不落地；「还可添加」改为维护会话期间常驻一个提权 helper（我原建议一次性测量加手动重算，用户选择常驻，已按其决定执行）。三条连同新增的 P3.25.4b 切片已写入 `MILESTONES.md`（`95ea802`），常驻 helper 的三条硬约束同时写死：只接受度量请求、管道关闭即退出、带超时看门狗并由应用在维护结束／登出／拔卡时收束。
- 检查与实现：桥新增 `beginDecoySurvey`、`getDecoySurveyProgress`、`getDecoySurveyId`、`cancelDecoySurvey`、`beginOpenDecoyMaintenance` 五个成员（DISPID 72～76）。遍历放在既有的工作线程上，复用卡纪元与中断机制；新增 `survey_mode_` 与 `is_survey_query`，遍历期间只有这三个读取方法应答，其余一律 `E_PENDING`，与批处理的门控并列而非嵌套。进度打包为 `状态,总数,已走,已找到,未能判定,是否中止`，结果按下标只吐标识符。打开维护挂载归入既有的 volume 操作分支，沿用其访问检查与状态位。
- 验证：`card_product_webview_host_object_tests` 通过，新增 3 组（工作线程上走完并逐项读出标识符、遍历期间拒绝无关调用且口令数组被抹零、取消后标记为中止、非口令参数与缺参被拒、打开维护在非 UI 线程上执行并只传标识符）；`card_login_mount_session_tests`、`card_product_ui_session_controller_tests` 重跑通过。仍未接真实驱动。
- 安全与风险：诱饵口令走与登录同一条 `consume_webview_password_array` 通道，SAFEARRAY 在所有路径抹零（测试断言），工作线程持有的副本用完即清。拔卡或纪元变化时清空已找到的列表并拒绝后续读取，无关变更调用 `clear_survey()`。过桥的只有标识符，没有名称、路径或卷类型。
- 提交：`cee9c0c [P3.25]walk the decoy survey from the page`。未推送。

### 2026-09-19 — P3.25 第五片 c：导航列与验证页

- 范围与审计：完成第 5 片的页面段。复核 8.7.2 的恒显要求、8.3 对档一档二界面不得解释隐藏卷机制的限制，以及页面契约测试既有的口令面计数。
- 检查与实现：侧栏「数据管理」下新增恒显的「更新诱饵内容」一列，无任何脚本决定它是否出现。该页三段：请验证（口令输入 + 显隐控件）、进行中（进度条与取消）、结果（容器列表）。提交口令先弹确认，文案按定稿原样使用，未改一字。确认后口令经 `TextEncoder` 转字节数组过桥、输入框立即清空，轮询进度直到结束再按下标取回标识符。结果行复用容器行的外观但只有「打开外层卷」一个动作，没有排序手柄、没有「从列表移除」。会话变化时整页复位，走完的结果不跨会话存在。点击行调用 `beginOpenDecoyMaintenance` 并给出状态提示，二级页面归第 6 片。
- 验证：`card_product_ui_page_contract_tests` 通过——新增一组契约（导航项恒显且脚本不参与其显隐、确认文案逐字、该段不出现"诱饵密码／胁迫密码"、出现"请验证"），并按新增输入更新三处计数（口令输入 5→6、`autocomplete`／`spellcheck` 11→12、显隐控件 3→4）。`card_product_ui_navigation_dom_tests.cjs` 221 项通过（原 208 项，新增 13 项覆盖空口令不过桥、取消确认不触达原生、口令以字节过桥一次且清空、只列出找到的容器、行内无排序与移除、打开只传标识符、空结果与"未能验证"计数的措辞、会话变化清空）。`card_product_ui_setup_dom_tests.cjs` 20／7 项通过。WebView2 集成 fixture 页补齐新元素（该契约正是为这类遗漏而设）。显式重建 `usb_vault_vikey_p2_development_app`。渲染效果仍需人工在实机确认。
- 安全与风险：口令只以字节数组过桥，页面侧用完即清；结果只在内存，不写任何本地产物；页面文案不出现"诱饵密码""胁迫密码"。空结果与口令错误共用同一条措辞，页面无从区分。
- 提交：`807e6a2 [P3.25]give the maintenance column its verification and list`。未推送。

### 2026-09-19 — P3.25 第四片 b：常驻度量会话

- 范围与审计：落地用户选定的常驻方案。复核既有 helper 的提权、管道 DACL 与 nonce 边界，以及计划中写死的三条约束。
- 检查与实现：不新增命令行开关，也不新增协议消息——**第一个请求是度量，就说明这是一次度量会话**。helper 收到度量请求后进入 `serve_measurement_session`，在同一条管道上连续应答度量，直到对端关闭（读失败即正常结束）；期间任何非度量请求一律回 `invalid_request` 并结束会话，一次提权只覆盖度量、不覆盖建卷。看门狗用条件变量实现，空闲超过 15 分钟直接结束进程；正常结束时立即唤醒，不拖慢退出。单次度量失败只回错误、不终止会话，否则用户拖文件途中一次失败就得重新提权。应用侧新增 `VolumeMeasurementSession`：构造不提权，首次度量才拉起 helper 并连上管道，`close()`／析构放掉管道即结束会话并等待进程退出；管道断开时自动置为未连接，下次度量重新征求同意。
- 验证：`creator_protocol_tests` 通过，新增 1 组——用真实命名管道对驱动整个会话：连续两次度量都得到应答（故意用不存在的盘符，确认失败不终止会话）、随后的建卷请求被拒为 `invalid_request`、关闭客户端后服务端线程正常退出。该测试目标现在一并编译 helper 引擎。`volume_space_tests` 重跑通过，helper 与 core 重建通过。真实外层卷上的提权流程与数值仍归第 9 片实机。
- 安全与风险：常驻提权进程是已知并接受的取舍（用户决定）。边界为：只答度量、非度量即断、对端关闭即退、空闲 15 分钟自杀。管道仍是一次性、带 nonce、DACL 只列 SYSTEM 与令牌用户，且 helper 自身仍校验提权。尚未接入页面——把会话挂到维护开始／结束、登出与拔卡上是第 6 片的事，在那之前不会有进程被拉起。
- 提交：`ae2e937 [P3.25]hold one elevation for a run of measurements`。未推送。

### 2026-09-19 — P3.25 第六片 a：二级页后端事实

- 范围与审计：按用户决定把第 6 片拆为 6a 后端事实与 6b 页面；复核 8.5、8.6、P3.25 计划、3.25 提交链及工作区既有草稿。真实所有权是 `CardLoginMountSession::Impl → ProductionCardLoginMountSessionRuntime`，生产 runtime 原已持有 `VolumeCreator`，故常驻度量器并列放在同一 runtime，不放页面或全局。
- 检查与实现：仅维护方式挂载的外层卷可读取总量、调用方可用量与尾部可写量；结果向 UI 保守下取整为 MiB。外层根目录继续走 P3 文件管理既有的受限目录接口，不复制第二套读写路径。桥以工作线程度量并按 `总量,可用,可写` 返回，刷新开始即废弃旧值，结束或卡纪元变化后拒绝旧快照。
- 生命周期：同一时刻只保留一个维护挂载；右上角后续切换所需的后端事务会正常卸载旧外层、复用同一度量连接，再打开新外层。结束维护、普通单卷卸载、整会话卸载、登出与拔卡均收束 helper；普通卷不能冒充维护目标调用结束接口。
- 审计修正：发现 4b 客户端把 helper 明确返回的单次扫描错误也落进断管 catch，实际会关闭连接并再次触发 UAC；现只在传输／协议失败时断开，合法扫描错误保留会话。连接建立失败也统一清理已取得的管道／进程句柄。
- 验证：点名构建 `card_login_mount_session_tests`、`card_product_ui_session_controller_tests`、`card_product_webview_host_object_tests`、`creator_protocol_tests`、`volume_space_tests` 与 `usb_vault_vikey_p2_development_app`，全部成功；五个测试程序全部通过，`git diff --check` 通过。按计划未跑全套，未触发真实 UAC、未接真实外层卷。
- 安全与风险：新增结果只含三个容量整数和会话内容器标识，不含路径、卷类型、密码或档位，不新增落盘；桥快照受认证状态和卡纪元约束。常驻提权进程风险仍由 4b 的仅度量、断管退出、看门狗三层约束承担；真实数值、UAC 与驱动行为留第 9 片实机。
- 提交：`0eee1ca [P3.25]project maintenance facts through the session`。未推送；6b 页面随后开始。

### 2026-09-19 — P3.25 第六片 b：外层维护二级页

- 范围与审计：复核 8.5、8.6、P3.25 切片表、设计稿维护屏与 P3 文件管理现状。本片只落二级页面；完整性／保护触发告警仍归第 8 片，不提前混入。
- 检查与实现：不复制文件管理器，进入维护后把既有面包屑和 `management-files` 单实例移入二级页，结束或会话变化再原位归还；维护期间普通「容器管理」导航不能拆走组件。目录浏览、创建、重命名、删除与刷新继续复用原路径。
- 页面事实：容量严格解析 6a 的总量／可用／尾部可写 MiB，显示「外层剩余可写」及已用／总量；新鲜度复用受限根目录读取，只取顶层条目的最新修改时间，每次进入或刷新现算，结果不落地。容器下拉只列本次验证结果，切换和结束分别走 6a 的专用事务。
- 竞态修正：容量或根目录刷新期间若拔卡／换卡，旧修订不得回写；异步任务仍释放全局忙碌状态并补跑已排队的会话刷新，避免页面永久锁定。
- 验证：`card_product_ui_page_contract_tests` 通过；`card_product_ui_navigation_dom_tests.cjs` 227 项通过；`card_product_ui_setup_dom_tests.cjs` 两组 20／7 项通过；`usb_vault_vikey_p2_development_app` 构建成功；`git diff --check` 通过。首次直接构建因未加载 MSVC 环境找不到标准库，改在 `vcvars64.bat` 环境重跑后通过，不影响测试结论。
- 安全与风险：页面只持有会话内容器标识、三个容量整数与临时目录条目；下拉名称用 `textContent`，容量包先做严格数字与范围校验，不接收路径、卷类型、口令或档位，不新增浏览器存储。根目录仍受原生侧条目上限约束。
- 剩余：尚未做真实 WebView2 视觉验收、真实外层卷容量／时间显示和 UAC 切换验收；这些不冒充已完成。第 7 片外部拖入上传与第 8 片保护告警仍未开始。
- 提交：`660c9fa [P3.25]run maintenance inside the file manager it already has`。该条记录随代码一并提交，故本行的哈希由下一笔补正。未推送。

### 2026-09-19 — P3.25 第六片 b：会话变化审阅补丁

- 范围与审计：在 6b 已提交后补测「容量刷新正等待原生结果时换卡」。旧修订已能阻止事实回写并归还文件管理器，但验证按钮仍保留刷新开始时的禁用态，原因是补跑会话刷新只重绘主会话，没有重绘诱饵页自身。
- 检查与实现：旧事实任务释放 `busy` 后改为等待排队的会话刷新完成，再按最新认证状态重绘诱饵页；不改变后端事务、容量口径或持久化边界。
- 验证：新增竞态断言后 `card_product_ui_navigation_dom_tests.cjs` 228 项通过；`card_product_ui_page_contract_tests`、脚本语法检查与 `git diff --check` 通过。其余 6b 验证沿用上一条，真实 WebView2／外层卷／UAC 验收仍未执行。
- 安全与风险：晚到容量结果仍由修订号丢弃；补丁只恢复最新会话允许的控件状态，不会恢复旧容器标识、目录条目或容量事实。
- 提交：用户已决定提交；本条随补丁提交，未推送。

### 2026-09-20 — P3.25 第七片 a：卷外到卷内的导入入口

- 范围与审计：接手工作区里第 7 片的半成品。复核切片表第 7 片、8.5、2370 行「只管容器内部」的边界，以及既有存储原语。确认 `copy_volume_entry` 本就支持跨卷与取消，第 7 片不需要新的存储层实现，只缺会话层入口。按前几片先例把本片拆为 a 会话层与控制器、b 原生窗口与桥、c 页面。
- 检查与实现：会话层新增 `import_external_entry`，把 shell 给出的一个绝对路径复制进**仅维护方式挂载**的外层卷当前目录。源路径只取最后一段作为卷内名称，再走与页面提供的任何组件同一条 `resolve_volume_child_path` 校验，因此盘根、结尾分隔符、`..` 与 ADS 冒号一律拒绝；普通方式挂载的容器、未挂载、标识符不存在与未认证各自按既有口径抛。运行时接口 `copy_entry` 补上取消参数：虚函数不给默认实参（默认实参按静态类型绑定，覆写不一致会静默收到调用方没写的值），既有两处调用显式传 `{}`。控制器只加认证路由校验后原样转发，取消回调一并过去。
- 验证：点名构建并运行 `card_login_mount_session_tests`、`card_product_ui_session_controller_tests`，全部通过；新增 5 组会话层断言（落到页面指定目录并保留 shell 给的文件名、只有维护卷可写入、命名不出目录、取消上抛且不复制、失去认证即拒）与 2 组控制器断言（非认证路由不触达会话、源路径与取消原样转发）。`git diff --check` 通过。按计划未跑全套，未实机。
- 安全与风险：导入不写操作历史、不落任何本地产物，卷外源路径不进入任何持久化。剩余边界：源若位于另一个已挂载容器（例如隐藏卷盘符），本入口视作普通复制不拒绝——与文件管理器既有的跨容器复制同口径，但把隐藏卷内容拖进诱饵外层卷是用户可做到的否认性自伤，第 9 片实机清单需要复核是否要在页面层提示。
- 提交：随本片代码一并提交，哈希由下一条补正。未推送。

### 2026-09-20 — P3.25 第七片 b：宿主窗口接住 shell 拖放

- 补正：7a 为 `0b03c6d`；另已在只含 7a 的工作区单独构建并跑过三个测试目标，确认该片可独立回退。
- 范围与审计：落地切片表第 7 片的原生半边。复核桥既有的忙碌闸门（批处理／遍历／卷操作三条并列）、卡纪元与中断机制，以及 WebView2 设置块的失败关闭写法。
- 检查与实现：`put_AllowExternalDrop(FALSE)` 关掉页面侧外部拖放，窗口自己 `RegisterDragDrop` 一个 `ExternalDropTarget`。拖放的两半只在这个对象里汇合——页面只说容器标识与当前相对目录，shell 只给路径，两边都拿不到对方那一半。**只接受 `CF_HDROP`**：其余格式（邮件客户端、压缩包里的虚拟文件）要先渲染成明文临时文件才能复制，那正是本产品要避免的痕迹面，且本层无法承诺清除它。拖入在工作线程上跑，按卷操作口径占用会话（`volume_operation`），逐项失败只计数不中断，取消走自己的标志而非创建闸门。桥新增 `setExternalDropTarget`／`clearExternalDropTarget`／`getExternalDropStatus`／`cancelExternalDrop`（DISPID 80～83）；前两个只武装窗口、不动会话，故与批处理／遍历／容量快照并列进入"观察类"豁免，后两个在任何 worker 占用期间都必须应答，否则页面无法轮询自己发起的那次拖入。状态只回 `状态,总数,已完成,已失败` 四个数字；页面每次重新指定目的地即让已结束的那次退役归零，这样「读过一次」与「两次结果相同的拖入」不会混为一谈，正在跑的那次则保留计数不受影响。窗口在换卡、切到设置页、清会话与 `WM_DESTROY` 时解除武装；`WM_DESTROY` 先撤销并等待在跑的拷贝，再放掉它写入的会话。进程改用 `OleInitialize`（`RegisterDragDrop` 不接受只初始化了 COM 的单元）。页面通知复用既有的 100ms 卡计时器：状态变化即执行一条无数据脚本事件，运行中每秒补发一次，避免页面漏掉"开始"后一直不知情。
- 验证：新建 `external_drop_target_tests`（8 组：多路径落到页面指定目的地、未武装／离开维护即拒、只收 `CF_HDROP`、运行期间独占会话并拒绝第二次拖入、单项失败不中断且如实计数、取消停在当前项、拔卡停止并让窗口不再接受、指定下一个目的地让已结束的一次退役、正在跑的一次不受影响）；`card_product_webview_host_object_tests` 新增 2 组（非认证路由不能武装且顺带解除、零标识符与错类型被拒、状态只回四个数字、运行期间只有这两个成员应答）。四个测试目标与 `usb_vault_vikey_p2_development_app` 全部构建并通过，`git diff --check` 通过。按计划未跑全套。
- 未验证：真实 shell 拖放、`AllowExternalDrop(FALSE)` 后拖拽能否如预期落到父窗口的 `IDropTarget`、真实外层卷上的写入与容量变化，全部留第 9 片实机，不冒充已完成。
- 安全与风险：过桥的仍然只有标识符、相对目录和四个数字，没有名称、路径或卷类型。窗口默认解除武装，页面不在维护页时整窗拒绝拖放。`DragEnter` 的结论在 `Drop` 时重新校验一次，覆盖"光标还在窗口上但卡已拔出或已离开维护"。1024 项上限约束的是窗口失去应答的时长与误拖规模，不是存储上限。取得 `ICoreWebView2Controller4` 失败按既有写法失败关闭，不静默降级为页面接管拖放。
- 提交：随本片代码一并提交，哈希由下一条补正。未推送。

### 2026-09-20 — P3.25 第七片 c：维护页的投递目标与刷新

- 补正：7b 为 `171668f`。
- 范围与审计：完成第 7 片的页面段。复核 6b 的文件管理接管与竞态修正、页面既有的 `busy` 口径与 `batchSessionRevision` 会话修订号。
- 检查与实现：维护页固定一块拖入说明区，进入维护即以「容器标识＋当前相对目录」武装窗口，目录每次刷新后按最新路径重新武装，离开维护、会话变化或未认证即解除。运行中显示进度与「停止」，结束后在自己的一行给出结果，再刷新目录与容量——结果不与维护状态行共用，因为紧随其后的容量刷新会清掉那一行。页面侧全程没有源路径这一概念：过桥的只有目的地，回来的只有四个数字，放进来的是什么靠重新列目录得知。
- 竞态修正（本片发现并同时改了 7b 的原生侧）：小文件可能在窗口两次通知之间跑完，页面只按"看到状态为进行中"才接管会漏报；改为按读到的状态本身判断，终态未报过就直接报。随之而来的是"两次结果完全相同的拖入会被当成同一次"，因此双方对称地加了退役：原生 `set_target` 在没有正在跑的运行时把计数归零，页面成功武装后清掉"已报告"标记。正在跑的那次不受影响。
- 验证：`card_product_ui_navigation_dom_tests.cjs` 236 项通过（原 228，新增 8 项：只投递容器与目录、进目录后投递目标跟随、运行中进度与停止、停止只触达窗口、结束后独立一行给结果、离开维护即解除、没看见开始的那次仍被报告且退役、连续两次相同结果各报一次）；`card_product_ui_setup_dom_tests.cjs` 20／7 项通过；`card_product_ui_page_contract_tests` 通过（新增一组契约：维护页有投递区，脚本不得出现 `dataTransfer.files`／`items`／`webkitGetAsEntry`／`getAsFile`／`FileReader`／`createObjectURL`，页面标记不得自带 `ondrop`／`ondragover`，四个桥成员与通知事件已接线，文案指明拿出去是明文导出）；`external_drop_target_tests`、`card_product_webview_host_object_tests`、`volume_file_operations_tests` 重跑通过；`usb_vault_vikey_p2_development_app` 构建成功；`git diff --check` 通过。
- 验证列偏离：计划写的 `volume_file_operations_tests` 未新增用例。拖入复用既有 `copy_volume_entry`，其跨卷接受、目的地不覆盖、取消与半成品清理都已有覆盖；本机也造不出第二个真实卷（`subst` 盘会被 `GetFinalPathNameByHandleW` 解回原卷，按越卷拒绝，既有用例正是这么断言的），再补只会得到一条什么都不断言的用例。已在 `MILESTONES.md` 第 7 片下记明。
- 未验证：真实 shell 拖放、`AllowExternalDrop(FALSE)` 后拖拽是否如预期落到父窗口的 `IDropTarget`、真实外层卷上的写入与容量变化、WebView2 实际渲染，全部留第 9 片实机。另：若窗口那条脚本通知恰好执行失败，页面可能漏掉一次"开始"，只能在下一次状态变化或手动刷新时才看到结果——这是已知的降级，不是已解决项。
- 安全与风险：页面既拿不到也无法读取拖拽内容，契约测试把这条钉成可回归的约束。投递目标只在维护页存在，其余界面整窗拒绝拖放。会话变化时目标、已报告标记与结果一并复位，不跨会话留存。
- 提交：随本片代码一并提交，哈希由下一条补正。未推送。

### 2026-09-20 — P3.25 第七片 c 后续：暂时收起「更新诱饵内容」入口

- 补正：7c 为 `8ba6e7c`。
- 范围与审计：第 8 片（保护触发告警）未做，维护屏目前拿不出写入被阻止时的告警，不应让测试者走进去。复核 8.7.2 的恒显条款：它禁止的是**同一构建内入口随机器状态或登录身份出现／消失**，因为对手比对两份安装即可读出档位；整个构建统一没有这一列，比对不出任何差异，因此不违反 8.7.2 与 8.4。
- 检查与实现：只在标记里给导航按钮加 `hidden`，不经任何脚本。页面本体、四个面板、拖入与所有后端接线原样保留，`[hidden] { display: none !important; }` 已在页首，nav-item 的 flex 不会盖过它。脚本中唯一会改导航项 `hidden` 的是操作历史那一项，按 `data-section === "history"` 取，碰不到这一列。
- 验证：`card_product_ui_page_contract_tests` 通过——原「不得以 hidden 起始」断言改为「必须静态 hidden 且脚本不得参与」，并把不变量写进测试名与注释；`card_product_ui_navigation_dom_tests.cjs` 仍 236 项通过（该列的流程继续被驱动，只有入口收起）；`card_product_ui_setup_dom_tests.cjs` 20／7 项通过；`usb_vault_vikey_p2_development_app` 构建成功；`git diff --check` 通过。
- 安全与风险：收起是构建级、无条件的，不随容器、登录身份或任何运行时事实变化。风险是这条临时状态被忘记带进正式交付——已在 `MILESTONES.md` 第 9 片写成必须撤销的收尾项。
- 提交：随本条一并提交，哈希由下一条补正。未推送。

### 2026-09-20 — 安装维护第一片：失效目录登记不阻止重装
- 范围与审计：核对统一安装入口要求、R-03／8.6 边界和现有 NSIS 脚本；用户新增保留数据更新要求，已同步需求与里程碑，分两片交付。
- 本机处理：用户授权后再次确认登记指向 D:\Desktop\USB Vault 且目录不存在，仅删除当前用户 USBVaultDevelopment 卸载登记，复查已不存在；未修改凭据、设置、索引或容器。
- 检查与实现：检查安装初始化、目录校验、卸载 DeleteRegKey、打包清单校验及既有依赖安装测试；卸载脚本原本已有清登记操作，未确定本次残留的历史原因。
- 检查与实现：新增共享 NSIS 探测宏，只有 GetFileAttributesW 明确返回文件／路径不存在才放行；目录或文件仍在则保留旧拦截，其他错误单独提示状态无法确认。
- 检查与实现：初始化不删除旧记录，安装成功时沿用原流程登记新位置；取消向导不清理原记录，原目录非空限制不变。
- 验证：编译并执行真实 NSIS 宏的五个测试程序，已有目录、目录缺失、父目录缺失、文件占位、无效路径全部通过；最初无效路径样例被 Windows 判为路径不存在，改用实际返回 ERROR_INVALID_NAME 的样例后通过。
- 验证：完整安装包首次构建因原软件包多出 WebView2 运行缓存而被清单校验拒绝；逐文件核对原摘要并复制清单文件到独立干净目录后，正式构建脚本成功生成 out/installers/stale-record-20260920/USB-Vault-Setup.exe，未放宽打包校验。
- 安全与风险：复核无递归删除、无执行登记命令、无敏感数据读写；目录不可达返回路径不存在可能放行新位置安装，但不会清理原位置，已有目录及非空目标保护仍在。
- 验证边界：未执行完整向导安装／卸载实机回归、权限拒绝实机用例或全量产品测试；旧目录仍在但程序缺失、替换更新及失败恢复留第二片，client-20260920 原安装器未替换。
- 提交：待用户决定是否提交本片，未提交、未推送；后续更新切片在本片提交决定后开始。
