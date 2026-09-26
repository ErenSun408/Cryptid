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

### 2026-09-20 — 安装维护第二片 a：逐文件替换与进程内回滚
- 范围与审计：第一片经用户批准已提交为 7b43e71，未推送；复核需求补充、原打包白名单／卸载清单与测试，第二片再拆为替换原语、路径与恢复保护、向导集成三个边界。
- 检查与实现：新增未接入产品入口的 NSIS 文件事务宏，路径／编号由编译期清单指定，先将旧文件移到备份，再把暂存文件移入目标，成功后精确清理备份。
- 检查与实现：失败后逆序撤销已新增文件并移回旧文件；MoveFile 不覆盖已存在备份或回滚目标，源／目标叶节点是目录或 reparse point 时拒绝。
- 检查与实现：区分替换失败、回滚失败、备份清理失败；恢复受阻时保留旧文件备份，不把部分恢复报告为成功，不递归删除目录。
- 验证：真实 NSIS 编译与执行的 11 个场景全部通过：替换、新增、暂存缺失、源为目录、目标为目录、备份冲突、旧文件占用、暂存占用、新增后回滚、回滚受阻、清理受阻；各场景均断言模拟用户文件不变及旧／新文件内容。
- 验证：git diff --check 通过；本片未改变产品入口，不重新打正式包，不冒充完整向导或真实容器验收；未跑全量产品测试。
- 安全与风险：本片仅是有明确前置条件的底层原语；调用者仍须提供同卷私有暂存／备份目录、验证所有父目录无路径跳转、停止应用并串行安装／卸载。每个编号仅允许单次使用。
- 安全与风险：仅处理进程存活时报告的错误；断电／进程终止后的恢复、文件系统竞态、登记／快捷方式事务及废弃产品文件清理尚未接入。已有安装仍被旧向导拦截，不能作为可用更新包交付。
- 提交：本片待用户决定，未提交、未推送；下一边界为位置与路径保护及中断恢复，之后才开放向导更新。

### 2026-09-20 — 安装维护第二片 b：路径固定与中断恢复原语
- 范围与审计：第二片 a 经用户批准提交为 45a7783，未推送；复核 R-03、8.6、统一安装入口、NSIS 替换原语与测试。本片只接触产品文件和中性安装状态，不读取凭据、容器或卡。
- 检查与实现：从固定盘根逐级打开目录，拒绝相对路径、ADS、路径别名、目录联接／重解析点，持有不共享删除的目录读取句柄；排他创建关闭即删除的事务锁，冲突文件保留。
- 检查与实现：替换前排他写入并刷新四字节意图，记录文件原先存在与否；全局提交标记独立刷新，恢复只按编译期条目处理，不从日志读取路径或执行命令。
- 检查与实现：未提交时把新文件移回暂存再恢复旧文件，已提交时保留新文件并精确清理备份；记录损坏、未知备份、状态矛盾或占用均保留现场并失败，恢复可再次中断后重试。
- 验证：19 项路径／锁用例、17 项独立进程中断／恢复用例及既有 11 项替换用例全部通过；含 TerminateProcess、恢复中再终止、错误记录值、路径跳转、回滚占用和重入恢复，均检查模拟用户文件未变。
- 验证修正：目录仅请求属性权限不能阻止改名，改为同时请求目录读取权限后通过持有期改名拒绝及显式释放后改名测试；NSIS 字符串比较与十六进制值表示不一致导致首轮恢复失败，统一记录常量表示后全量本片用例通过。
- 安全与风险：接入约束写入 packaging/windows/UPDATE_PROTOCOL.md；同包身份绑定、同卷私有暂存目录创建、安装／卸载共享锁、修改期阻止应用启动及注册事务仍须由集成片完成，原语尚未接入产品向导。
- 验证边界：测试证明进程强制终止后的恢复，不等于物理断电／磁盘损坏验收；未重建正式安装包、未跑全量产品测试、未执行真实安装／卸载或容器验收。原 client-20260920 安装包未变。
- 日志维护：按用户明确要求，本次不归档或压缩，完整保留主日志并继续追加；此项覆盖 AGENTS.md 的行数归档约定，既有 2026-09-19 归档不变。
- 提交：用户已批准提交并继续，后又要求重写本片的日志归档与类型前缀；功能和测试保持不变，未推送。下一片仍为向导及安装／卸载集成。

### 2026-09-20 — 安装维护第二片 c：完整向导与更新恢复集成
- 范围与审计：复核需求主文档、里程碑、既有向导／打包清单与事务测试；将先前原语接入真实新装、原位更新、缺失程序修复及卸载入口，保留依赖检测与许可页。
- 历史状态：此前按用户要求重写提交类型及主日志；当前相关提交为 d18282d、58d0e2a、24739a2，隐藏维护入口为 d3b5a2d [ui]，上文旧哈希仅为历史记录。
- 检查与实现：构建时生成固定文件计划及包身份；已登记目录更新时固定原位置，目录缺失的失效登记允许新装，未登记非空目录拒绝覆盖，同包未完成事务允许恢复。
- 检查与实现：安装／卸载共用互斥、固定目录句柄与文件占用保护，事务目录只授权当前用户和 SYSTEM；校验身份、所有者、DACL 与未知条目，拒绝目录联接及异包恢复。
- 检查与实现：全部文件提交后发布卸载登记与快捷方式，登记失败保留新文件、旧备份和提交记录，重跑同包向前完成；此取舍已同步需求及 UPDATE_PROTOCOL.md。卸载只删除编译期产品清单并清理登记，保留用户文件。
- 检查与实现：主程序在依赖、凭据或卡访问前检查维护目录／锁并阻止启动；未完成更新拒绝卸载，避免删除恢复备份。
- 验证：11 组真实 NSIS 隔离集成场景全部通过，覆盖新装／更新／修复／卸载保留数据、四个强制终止位置、登记失败、占用、异包／未知条目／待恢复卸载拒绝、修改 ACL、目录联接及非空目录保护；复跑 19 项路径、17 项恢复、11 项替换测试全部通过，原生启动保护所在测试 1/1 通过。
- 验证修正：修复 NSIS 安全描述符字符串越界读取及回滚后句柄仍占用暂存文件；测试改用隔离窗口类避免真实应用干扰，并按卸载子进程的实际文件／登记状态断言。ACL 测试手动还原未得到可接受描述符，改为独立断言拒绝且保留备份，不将人工 ACL 修复记为通过。
- 交付：Release 应用及完整安装器构建成功，输出 out/installers/update-wizard-20260920-final/USB-Vault-Setup.exe；SHA-256 为 0B3F0EBA37C801E088B12A035EC3AC38B8D04B9B1FF07BC7B110F79B629601E0，原 client-20260920 文件未替换。
- 验证边界：真实界面检查欢迎、依赖、许可与目录页；完整安装／卸载通过共用引擎的隔离测试执行，未完成真实产品全流程、多机、真实容器或物理断电验收，未跑全量产品测试。
- 安全与风险：复核无递归删除、无执行旧登记命令、无敏感数据访问；未知状态保留现场。最终清理身份记录与空目录之间被终止，可能留下需人工检查的空事务目录；不承诺任意物理断电自动恢复。当前没有退役清单文件，未知历史文件不自动清理。
- 提交：按用户要求继续写主日志，不归档；用户已审阅并批准提交本片，随本次安装向导集成提交记录保存，未推送。

### 2026-09-20 — 安装后首次启动排查：统一启动目录
- 范围与审计：用户澄清首次从完成页勾选“打开 USB Vault”，失败后拔卡能变为等待，再次直接运行 EXE 可进入；复核卡入口／HID 生命周期、helper 隔离、安装器发布与完成页启动，沿用主需求 R-03／8.6，不自动重试卡认证。
- 检查与实现：安全组件不可用是入口异常和不受支持卡记录的统一投影，具体探测异常被吞掉；拔卡恢复等待说明移除通知有效，尚不能确定重插后具体失败阶段。
- 检查与实现：本机真实快捷方式起始目录指向已删除的 .usb-vault-update/stage，完成页 Exec 则继承清理阶段的 TEMP。发布快捷方式前及完成页启动前明确设置 INSTDIR，消除这两个已确认的目录问题，不将其直接宣称为卡失败根因。
- 验证：11 组隔离安装集成回归通过，新增检查新装／更新／恢复后的应用及卸载快捷方式目标、空参数及工作目录；从实际完成页启动函数生成 NSIS 探针，原生无卡子进程实测工作目录为安装目录。
- 验证限制：无卡辅助进程对比首轮误用不存在的模拟 DLL，修正后直接与 NSIS 两路均在模拟运行时暂存的最终路径校验失败，未得到真实卡故障复现证据，不能报告该套件通过或因此放宽路径保护；只读核对当前运行应用非提权。
- 交付：完整安装器构建成功，输出 out/installers/startup-directory-20260920/USB-Vault-Setup.exe，继续使用已验证负载；未替换本机应用、未打开卡认证或读取凭据，也未自动启动真实产品。
- 安全与风险：只改变固定安装目录的启动上下文，无新增参数／路径来源、无安全校验放宽或认证重试；真实完成页启动是否仍报错待用户复验，若仍存在则需捕获单次失败的中性阶段和错误码，不能把重插当作通用修复。
- 提交：用户已批准随安装器命名及提示调整一并提交；主日志继续追加，不归档，未推送。

### 2026-09-20 — 安装器交付命名约定
- 范围与审计：按用户要求，后续安装器统一命名为 installer-yyyymmdd-hhmmss；检查构建脚本、工具说明及既有安装更新要求，本片仅调整交付名称。
- 检查与实现：构建开始时按本机时间生成 installer-yyyyMMdd-HHmmss.exe，使用固定文化及 24 小时制；同名碰撞继续拒绝覆盖，摘要记录随 EXE 使用同名 .exe.build.txt，工具说明同步。
- 文案：按用户最终要求，将已有安装提示改为“检测到当前设备已安装 USB Vault。将更新原位置：”，保持原位更新与修复判定不变。
- 验证：PowerShell AST 语法解析及脚本实际命名表达式检查通过，git diff --check 通过；仅命名变更，不重复构建大包或卡测试，既有安装包名称保持历史记录。
- 安全与风险：文件名仅由固定前缀及时间产生，不增加外部输入，不改变载荷校验、安装路径或运行行为；时间以构建机器本地时区为准。
- 提交：用户已批准本片与前一启动目录修正一并提交，随本次提交记录保存；未推送。最终文案与命名已检查，未重建含这两项调整的完整安装包。

### 2026-09-20 — 应用启动失败审计与数字错误码方案
- 范围与审计：按用户要求检查主 EXE 入口、环境检查、原生窗口／会话、WebView 与页面、HID 拔插、凭据、helper／SDK 及只读卡入口，遵循 R-03／8.6；本片仅制作对照表，不实现运行时诊断。
- 检查与实现：新增 docs/design/startup-failure-codes.md 并加入 docs 索引，整理 120 个不重复拟主码，完整对应既有 38 个 DiagnosticStage 和 18 个 CardError；页面拟显示四位十进制主码，原因、底层类别及底层数值分别保留，不改原协议枚举。
- 检查与实现：逐项注明触发条件、现有捕获／合并／丢失位置、建议处理、正常非错误状态、启动不可达的预留阶段、原生兜底及后续验收矩阵；区分候选外因和源码已证明的失败边界。
- 审计发现：入口与路由吞异常、SDK Key／PIN 阶段枚举未完整接入、清理可覆盖原错、WebView 多处仅返回 HRESULT、导航完成／进程故障缺少诊断；配置返回提示也可能错误声称凭据已保存。
- 审计发现：前置检查不访问卡，但随后会话构建会同步探测已插入卡；默认 5 秒是 helper 恢复执行后的共享响应／退出期限，不是全应用启动预算，也不保证每一步各有 5 秒。
- 验证：以脚本逐项核对源码枚举，38 个阶段与 18 个原因无遗漏，120 个主码无重复，27 个源码链接存在，git diff --check 通过；本片未执行故障注入、真实卡操作或完整产品测试。
- 安全与风险：数字本身不保证安全，成功 IPC detail 可带硬件身份，明确禁止原样导出；仅建议固定阶段／数值与有界内存诊断，不采集全量用户操作、凭据、卡内容、设备身份、容器路径或可推断档位的信息。
- 剩余边界：用户首次启动故障仍未取得具体错误证据；表中明确区分拟接入码与现有能力，不将静态审计当作复现或修复。运行时接入应作为下一独立切片审阅。
- 提交：本片未提交、未推送；主日志按用户要求继续追加、不归档，等待用户决定。

### 2026-09-20 — 启动诊断第一片：保留原始失败与清理失败
- 范围与审计：用户已授权逐片实现、验证后自行提交并继续；检查入口／路由 catch、helper 清理路径及既有生命周期测试，沿用 R-03／8.6，不增加认证重试或后台日志。
- 检查与实现：引入仅含固定数值的 FailureDiagnostic；入口 source／只读路由错误保存主码、原因及带类别的底层码，未带类别的 detail 不输出，系统错误不会误当 CardError。
- 检查与实现：helper 暂存失败、启动失败、响应失败与清理失败分别保存；清理仍执行一次，成功清理继续抛原始异常，二次失败保留第一主错和首个附加清理错。固定容量不保存无限异常链。
- 检查与实现：新到达和移除清除旧诊断，重复到达保持原错误且不重试；损坏／不支持介质仍不可用，不改初始化和认证门槛。原始异常指针仅局部用于异常展开，不存入会话。
- 验证：startup_failure_tests、card_entry_router_tests、card_entry_context_controller_tests、card_device_lifecycle_controller_tests 4/4 通过；Debug 主应用构建通过。首次构建因代码页 936 被仓库检查拒绝，改用 65001 后重建通过。
- 安全与风险：复核数值结构为固定大小，无凭据、卡身份、路径或 exception.what()，不改 IPC 数据格式；此片提供内存读取接口，页面及原生失败显示在后续切片接入。未运行真实卡或全量产品测试。
- 文档：一并保存上一轮启动审计表及文档索引；四位数字完整表仍属方案，本片只实现入口与异常转换子集。
- 提交：按用户授权提交本片并继续 SDK 阶段切片；不推送，保留首次实机故障尚未复现的边界。

### 2026-09-20 — 启动诊断第二片：API Key／PIN 阶段
- 范围与审计：第一片已提交 b5c73cb；复核 SDK 认证顺序、错误映射与清理断言，按用户授权继续独立小片。
- 检查与实现：set_api_key 和 user_login 调用补 sdk_key／sdk_pin 阶段，对应 2023／2025；保留 CardError 原因，计算失败 2032／2033 的优先级不变，不改协议。
- 验证：vikey_session_tests 通过；新增六种模拟故障覆盖普通 Key 错误、两类计算错误、PIN 错误、锁定、通信失败，断言完整调用顺序、零额外认证以及 Key 失败不进入 PIN；helper 构建通过。
- 安全与风险：仅标记失败发生阶段，不记录 SDK 参数或原始私有码，不增加自动重试／写卡，不访问真实卡；首次启动实机故障仍待后续诊断，未跑全量产品测试。
- 提交：按用户授权提交并继续 WebView 可见失败切片；未推送。

### 2026-09-20 — 启动诊断第三片：页面数字提示与 WebView 原生兜底
- 范围与审计：第二片已提交 da82c49；对照启动错误表、需求 R-03／8.6，检查 WebView 初始化、页面启动、视图切换、卡事件刷新和关闭保护，保留既有五态与权限边界。
- 检查与实现：页面显示主码、原因码、带类别的底层数值及首个清理附码；不再统一要求重新插卡，PIN 错误／锁定提示停止连续重试；拔卡和新到达清除旧诊断。
- 检查与实现：环境／控制器／安全设置／桥接／导航失败提供原生数字提示；新增 NavigationCompleted、ProcessFailed 和 30 秒页面就绪监测，页面启动视图／窗口预设／状态读取失败不再静默降级。
- 检查与实现：导航代次与导航 ID 隔离旧回调，失败只报告一次，失败后停止页面诊断注入；错误关闭走原有活动卷保护，不因渲染故障强行销毁已认证会话。
- 验证：webview_startup_state_tests、card_product_ui_page_contract_tests 2/2 通过；新增 7 个启动 DOM 场景通过且无额外认证；导航回归 236 项、配置 DOM 两组 20／7 项通过；Debug／Release 主应用和 Release 安装辅助程序构建通过，git diff --check 通过。
- 验证边界：一次增量构建误用不存在的 usb_vault_app 目标，改用实际目标后通过；未执行全量产品测试、真实 WebView 故障注入或真实卡验收，不将模型／DOM 测试视为实机覆盖。
- 安全与风险：输出只来自固定整数快照，拒绝无类别 detail，不输出 exception.what()、凭据、身份、路径或档位信息；不写后台操作日志，不改变认证次数或写卡行为。导航／进程故障暂不细分 WebErrorStatus／ProcessFailedKind，120 个方案码尚未全部接入，首次启动根因仍待实机数字证据。
- 交付：生成 out/installers/startup-diagnostics-20260920/installer-20260920-155723.exe（300305612 字节），SHA-256 为 2B72E666C2EF519D37A9D453DC525AB270B9040D7FF60944485908D0ECDD05C8；清单与构建记录校验完成，NSIS 保留两个既有未使用变量告警，未执行安装。
- 构建来源：包清单如实标记 da82c49 + dirty，包含本片待提交源码；随后仅补充开发日志，不改产物代码。主日志继续追加，不归档。
- 提交：按用户授权提交第三片；本轮三片计划完成，未推送。后续用新包复现首次安装完成页启动，并按实际错误码继续定位。

### 2026-09-20 — 安装器交付命名订正
- 范围与审计：用户最终明确安装器本身固定为 USB-Vault-Setup.exe，installer-时间指外层包；复核需求、里程碑、构建脚本及安装测试，此前将时间写入 EXE 名称是理解偏差，本条取代旧命名约定。
- 检查与实现：构建脚本在 OutputDirectory 下新建 installer-yyyyMMdd-HHmmss 文件夹，内含 USB-Vault-Setup.exe、USB-Vault-Setup.exe.build.txt 和 package-plan.nsh；时间仍为构建开始时本机时间，同名文件夹拒绝覆盖；同步工具说明与需求。
- 验证：PowerShell 语法检查、git diff --check 通过；使用现有已验证诊断载荷实际重建后按最终名称重命名，输出 out/installers/installer-20260920-160715/USB-Vault-Setup.exe，300305612 字节，重命名后摘要记录与实际 SHA-256 一致。
- 验证：SHA-256 为 2B72E666C2EF519D37A9D453DC525AB270B9040D7FF60944485908D0ECDD05C8，与上一个诊断安装器一致；仅交付目录和文件名变化，无需重跑卡／UI 测试，未执行安装。
- 安全与风险：保留原有载荷清单、路径字符、重解析点和摘要检查；不覆盖已有交付包。该次实际编译保留 NSIS 两个既有未使用变量告警，并因当时使用 setup.exe 名称产生 9000 兼容性 shim 加载告警；最终按用户指定名称重命名，未重新编译或声称已完成兼容性验收。
- 提交：本片尚未提交、未推送；此前自行提交授权所对应的三片启动诊断计划已完成，本次命名订正单独保留供用户决定。

### 2026-09-20 — 启动诊断补齐第一片：前置检查与入口兜底
- 范围与审计：用户授权本轮逐片自行提交；命名订正已独立提交 d9f5554。对照启动表、需求 R-03／8.6、前置探针与窗口入口，修正命令行解析异常被当作开发部署的降级。
- 检查与实现：新增无动态消息的本地数字异常，保留既有更具体阶段与内存不足码；系统架构、组件、WebView 查询、驱动、凭据探针携带固定诊断，缺少凭据 1107 继续正常引导配置，其他读取失败先提示再进入配置。
- 检查与实现：维护条目不可查询与存在分别为 1004／1003；模块路径、参数、盘符、制品固定、窗口、拖放、设备通知、定时器、消息循环出口补数字；入口增加 C++ 异常兜底与 OLE 作用域释放，系统错误不混同卡锁定。
- 验证：Debug 主应用构建通过；startup_failure_tests、vikey_development_app_config_tests、configuration_installer_tests、configuration_card_failure_tests 4/4 通过；新增局部错误穿透、系统码分类、未知阶段回退、bad_alloc 优先级及探针异常 1106 断言；git diff --check 通过。
- 安全与风险：无异常原文或路径输出，不绕过维护锁／制品校验，不自动重试卡；预检本身不访问卡，后续会话探测可能认证，修正原注释的过度承诺。仅覆盖可捕获 C++ 异常，不声称捕获系统强杀／内存破坏或完整崩溃报告。
- 剩余边界：真实权限、驱动、窗口 API 故障尚未逐项实机注入；错误表覆盖清单在第四片统一更新，SDK 分支与 WebView 细分继续下一片。首次安装启动根因尚未确认。
- 提交：按用户授权提交本片，不推送；主日志继续追加。

### 2026-09-20 — 启动诊断补齐第二片：SDK 启动出口阶段
- 范围与审计：第一片已提交 d92e3da；逐项检查 VikeySession 打开／只读探测、helper card_media_probe 的捕获和关闭路径，保持既有错误原因与协议值。
- 检查与实现：补齐函数表、零／多设备、零／变化身份、PIN 已锁定、异常权限级别及容量不足的阶段；操作状态／范围不合法归 input_shape。SDK 返回失败继续保留已有具体阶段与计算错误优先级。
- 检查与实现：只读媒体探测成功后的显式 SDK 关闭失败标记 2015；该探测未分类异常标记 2038。探测已失败时继续保留主错，不以关闭失败覆盖。
- 验证：vikey_session_tests 通过，新增 16 个模拟场景覆盖身份／设备／次数／权限／容量／读取，断言至多一次认证、一次反初始化、零写卡及不输出硬件值；已有六类 Key／PIN 和清理顺序测试继续通过，helper 构建通过，git diff --check 通过。
- 安全与风险：不新增 SDK 调用或自动认证重试，不修改 IPC 或认证门槛；无真实卡操作。既有协议只携带一个主失败，SDK 探测与 SDK 清理同时失败时附加清理信息仍无法传回，失败打开时析构清理也不另行上报；主进程文件清理的附码机制不能被误称为所有 SDK 清理均已覆盖。
- 剩余边界：真实供应商故障尚未逐项注入，写卡／挂载等非启动链不扩展。下一片处理 WebView 导航、进程失败和回调边界。
- 提交：按用户授权提交本片，不推送。

### 2026-09-20 — 启动诊断补齐第三片：WebView 事件细分与页面异常
- 范围与审计：第二片已提交 8e826c6；检查环境／控制器回调、导航代次、关闭保护与配置页启动，维持固定本地页面和安全设置失败关闭。
- 检查与实现：1313 增加 WebErrorStatus，1314 增加 ProcessFailedKind，事件类别 1／2 与 HRESULT 字段分开；1308 增加九个固定设置步骤，1316 检查窗口区域和 Bounds，失败后不继续导航。
- 检查与实现：导航 URI 使用借用视图比较后立即释放，避免回调分配；窗口过程增加 C++ 异常边界，经原关闭保护处理。配置查询异常／非法状态显示 1407，真实 unreadable 显示 1108；页面就绪前 error／unhandledrejection 只标记 1408，无消息、堆栈或拒绝内容采集。
- 验证：Debug 主应用构建通过，webview_startup_state_tests 与页面契约 2/2 通过；启动 DOM 11 个场景、导航 236 项、配置 20／7 项通过。新增页面错误／拒绝、凭据查询失败／非法值及原生结果白名单测试，均不触发认证。
- 安全与风险：错误字段仅为固定编号／WebView 数值枚举，不放宽导航、不绕过活动卷关闭保护；旧回调仍按代次／导航 ID 丢弃。脚本无法解析时仍由原生 30 秒超时兜底；事件监听仅覆盖就绪前，不宣称所有运行期 JS 异常已接入。
- 验证边界：未真实终止 WebView 进程或注入每个 COM 失败；C++ 兜底不覆盖访问违规、栈损坏或系统强杀。后续交付清单将明确模型验证与实机验证的区别。
- 提交：按用户授权提交本片，不推送；下一片汇总覆盖状态、检查交付入口并生成安装包。

### 2026-09-20 — 启动诊断补齐第四片：交叉验证、覆盖清单与交付
- 范围与审计：第三片已提交 87d7e7c；逐码核对原审计表 120 个编号，并再次检查安装完成页、SDK 模块／媒体读取、前置组件提示及页面桥的实际出口。
- 检查与实现：安装完成页设置工作目录或 Exec 失败显示 1001；补媒体探测容量不足 2029 和动态模块接口不完整 2013；前置组件失败不再混用旧 CARD/D 字母前缀。启动路由补不支持版本／未知枚举诊断断言，页面补缺少原生桥场景。
- 文档：新增 docs/design/startup-failure-coverage.md，120 个编号无重复、无遗漏；31 项有针对性模拟验证，77 项已接线但待对应原生故障实机验证，12 项为正常引导、具体码合并、范围外或未实现。同步原审计表、文档索引和需求边界，保留旧表的历史审计含义。
- 验证：10 组原生相关测试通过；最后补充路由断言后 startup_failure_tests 重跑通过；启动 DOM 12 场景通过，导航 236 项与配置 20／7 项沿用第三片已通过结果。Debug／Release 主应用和 Release setup helper 构建通过；git diff --check 及覆盖清单与原表的集合核对通过。
- 安全与风险：只显示固定数字与固定中文原因，无秘密、异常原文、路径或卡身份；没有新增认证重试、写卡或日志落盘。SDK 主错／清理双错 IPC 附码与完整崩溃捕获仍未实现，原生 COM／系统错误并未逐项实机注入，不声称全量产品测试或启动故障根因已修复。
- 交付：最终安装器为 out/installers/installer-20260920-164137/USB-Vault-Setup.exe，300307580 字节，SHA-256 A685A67B75E93EACB8A8C57A37DAF74B72030E200F9506729796D3AEA5E05BB8；实际摘要与构建记录一致，载荷校验通过。NSIS 仅保留两个既有未使用变量告警。
- 构建来源：最终载荷为 usb-vault-startup-coverage-final-20260920，清单如实标记 87d7e7c + dirty（本片源码）；163846 为补最后文案之前的中间构建，交付使用 164137。后续只补充记录，不改产品代码。
- 验证限制：本轮未运行安装、真实卡认证／拔插及安装完成页首次启动复现；仍需取得该路径上的实际数字码并与直接启动对比。本轮完成接入、自动化验证和包交付，不替代实机验收。
- 提交：按用户授权提交第四片，不推送；主日志继续追加。下一步以新包实际错误码为依据定位根因，不自动循环认证。

### 2026-09-20 — 启动错误码与覆盖状态文档合并
- 范围与审计：用户指定撤回 afe6685c4b31cc755d67e02947a962d4eb26f955 后合并两份文档；该提交已 mixed reset，状态简化内容保留。操作中先撤回了 56bd167，页面内容和测试记录完整保留，待本片之后继续。
- 检查与实现：以 docs/design/startup-failure-codes.md 为唯一编号入口，把 120 项触发条件、接入状态、排查建议、验证边界合并到各行；108 项状态统一“已接入”，12 项保留其他分类。
- 检查与实现：移除独立 coverage 文件，更新文档索引与需求引用；清除与现状冲突的旧缺口列和过期实施顺序，历史审计可由原提交及本主日志追溯。
- 验证：核对编号集合完全一致、120 项无重复，108 项接入状态保留；逐项检查当前文档引用与本地源码链接、git diff --check 通过。仅文档提交，不重跑产品构建或测试。
- 安全与风险：保留模拟／实机边界以及崩溃捕获、SDK 双错限制；不把文档简化写成验证完成，不改诊断语义、页面或持久化行为。
- 提交：按用户明确要求单独提交合并文档，不推送；页面精简仍在工作区，随后继续 EXE 同目录、年月日时分秒命名的 TXT 启动日志。

### 2026-09-20 — 页面诊断简化：只显示主错误码
- 范围与审计：用户要求页面只展示错误码，不增加复制诊断入口；复核状态区域、配置查询和启动 DOM 测试。
- 检查与实现：安全组件不可用时保留标题，下方仅显示红色“错误码：xxxx”；未知诊断使用 9001，拔卡恢复普通等待提示。启动桥失败与配置查询错误也使用精简主码；原生不可用兜底弹窗暂保留原有数值细节。
- 验证：12 个启动 DOM 场景、236 项导航、配置 20／7 项与页面契约通过；Debug 主应用构建及 git diff --check 通过。
- 安全与风险：页面不再输出原因／底层／清理附码，无复制按钮，不增加认证；内存诊断保持完整，为下一片启动日志提供固定数字输入。没有访问真实卡或执行安装。
- 提交：文档合并 a876277 后恢复独立页面提交，再继续 TXT 日志；日志目录与年月日时分秒命名已由用户确认。

### 2026-09-20 — 启动诊断明文 TXT 日志
- 范围与审计：在文档合并 a876277、页面精简 dd25a97 之后继续本片；用户确认 EXE 同目录、年月日时分秒，并最终指定 startup-log-yyyyMMdd-HHmmss.txt 命名，替代原默认不落盘约定。
- 检查与实现：检查主入口、前置检查、会话首次探测、WebView 回调与页面就绪协议；记录固定数字阶段、单调耗时、主错及已有清理附码，不接受任意文本输入。
- 检查与实现：使用 CREATE_NEW，同秒追加序号且不覆盖；逐条刷新，最多 128 条且单份小于 64 KiB，页面就绪／致命失败／提前退出后停止，异常展开保留原错记录机会。
- 检查与实现：补前置检查观察回调与配置查询失败就绪码；写入异常仅警告 9010，不覆盖原始启动错误或增加认证重试，不自动删除历史日志。
- 验证：真实文件 I/O 测试覆盖命名、同秒冲突、刷新、原错与清理码、成功载荷及无类别字段过滤、容量上限、停止追加、无效目录和禁止覆盖；最终命名变更后重跑通过。
- 验证：启动 DOM 12 场景、WebView 状态、页面契约、应用配置测试通过；Debug／Release 主应用构建通过，git diff --check 通过；页面片导航 236 项与配置 20／7 项结果沿用。
- 安全与风险：数值输入过滤避免成功 IPC 身份泄露；不记录凭据、卡内容、路径、档位或异常原文。创建目录拒绝根重解析点，创建新文件避免覆盖既有链接目标；不宣称完整祖先目录防竞态保护。
- 验证限制：未执行真实安装／首次启动／卡认证和拔插；真实 ACL 拒绝、磁盘满、原生故障逐项注入仍待实机。没有完整崩溃捕获、逐个 SDK 成功调用耗时或跨进程双错扩展，首启根因仍待日志证据。
- 提交：按用户已授权自行提交本片，不推送；主日志继续追加，错误码与覆盖状态保持唯一文档入口。
- 交付：out/installers/installer-20260920-171922/USB-Vault-Setup.exe，300310136 字节，SHA-256 AD5EFC21A8EBE4AEE420E9FFA7C88E48A77890244CDA880C75F23DB563758858，与构建记录一致；载荷 usb-vault-startup-log-final-20260920 如实标记 dd25a97 + dirty。NSIS 仅两个既有未使用变量告警；171634 为旧命名前的中间包。

### 2026-09-20 — 启动日志中文与本地毫秒时间
- 范围与审计：用户反馈纯数字日志不可读，明确要求中文及设备现实时间；复核唯一错误码文档、需求日志边界、现有日志实现和文件 I/O 测试。
- 检查与实现：改为格式 2、带 BOM 的 UTF-8 中文；每行显示设备本地 yyyy-MM-dd HH:mm:ss.fff 和开始／完成／失败、中文阶段，保留原文件命名与不覆盖规则。
- 检查与实现：成功记录不再输出零值字段；仅有效诊断追加主码、原因、具名底层码、清理码和事件步骤，不改既有日志。
- 验证：文件 I/O 测试通过，新增中文内容、BOM、毫秒时间格式和成功记录简洁性检查，既有碰撞／上限／敏感字段过滤／停止追加检查通过；Debug／Release 主应用构建通过。
- 安全与风险：只映射内置固定中文，不记录任意输入或异常文本；保持 128 条、单份小于 64 KiB、逐条刷新和认证规则。系统校时可能使墙上时间跳变，已在文档说明；未进行真实卡故障复现。
- 提交：按用户此前授权自行提交，不推送；按钮位置调整作为独立 UI 切片随后提交。

### 2026-09-20 — 配置凭据与返回按钮位置统一
- 范围与审计：用户要求配置页返回放到右下角并与配置凭据入口重合；检查两个按钮的 DOM、固定定位样式及既有配置切换测试。
- 检查与实现：两个按钮共用 right/bottom 1.25rem、宽度 7rem、居中内容和相同层级，移除返回按钮原左上角定位；保留图标、禁用状态与既有切换逻辑。
- 验证：配置 DOM 20／7 项通过，覆盖返回可用性、拒绝切换及完成切换后的旧页面停用；检查共用 CSS 定位规则与 git diff --check。未做真实 WebView 像素截图或实际连续点击验收。
- 安全与风险：仅调整布局，不改凭据保存、认证或卡访问逻辑，无新增输入和持久化行为。
- 提交：按用户此前授权自行提交，不推送；后续生成包含本片及中文日志的新安装包。
- 交付：out/installers/installer-20260920-173551/USB-Vault-Setup.exe，300310729 字节，SHA-256 3DEEAB57ED29E7E7D4288393D058CF2D156335AF86551254C6130294C9B4E4F9，与构建记录一致；载荷来源 c8cac10 + clean（补此交付记录前），NSIS 仅两个既有未使用变量告警。

### 2026-09-20 — P3.25 第八片 a：保护触发只读查询
- 范围与审计：复核模式 C 8.5、8.6、8.7.2 与第 8 片计划；底层已有 outer_write_prevented 类型，页面尚无查询。8.5 旧入口身份条件被 8.7.2 恒显要求取代，保持当前统一隐藏至第 9 片。
- 检查与实现：会话仅允许已认证且当前维护挂载的容器查询保护状态；outer 返回未触发、outer_write_prevented 返回触发，失踪或其他类型抛错，不伪造安全事实。
- 检查与实现：控制器保留认证路由检查，桥新增固定 DISPID 84 与布尔返回，拒绝零标识符和错误参数，卡中断后拒绝；保留 worker 独占会话闸门，避免读取与复制／卸载竞态。
- 验证：会话、控制器、原生桥三组测试全部通过；新增未认证／普通卷拒绝、类型转换、失踪卷、无提权、参数、布尔投影及后台占用／中断断言。初次测试命令引号被 PowerShell 解析失败，拆分构建与测试后运行通过。
- 安全与风险：查询不挂载、不认证、不提权、不记录历史或启动日志，不将驱动类型直接交给页面；查询在后台任务期间返回 E_PENDING，页面须暂停轮询并在任务结束后重查。真实驱动转换时延留第 9 片实机。
- 提交：按用户此前授权自行提交本片，不推送；下一片接入维护页轮询、独立告警与指引。

### 2026-09-20 — P3.25 第八片 b：维护页保护触发告警
- 范围与审计：第 8a 片 7b0ff98 已提交；接入维护页轮询、独立告警与操作指引，完整性校验 N-07 不在本片，入口统一隐藏保持到第 9 片。
- 检查与实现：维护期间每秒查询当前容器的布尔保护状态，页面忙碌时暂缓；已触发状态在本次维护挂载内保留，容量刷新不再把“写入已阻止”改回可写容量。
- 检查与实现：红色独立告警说明整卷停止写入、复制可能未完整保存，指引停止修改、保留源文件、关闭占用程序、结束卸载后重入核对，不建议关闭保护或反复写入。
- 检查与实现：无效返回和查询失败显示“无法确认”，不伪造安全；切换／结束／换卡清理状态，卡修订号、容器标识及查询代次共同阻止迟到结果，页面关闭停止轮询。
- 验证：导航 DOM 243 项通过（新增 7 项覆盖无效结果、触发投影、保持告警、刷新不覆盖、重入清理、查询等待及换卡迟到结果）；页面契约通过，Debug 主应用构建通过。8a 三组原生测试沿用通过结果，未跑全套。
- 验证修正：首次新增 DOM 测试因生成代码的选择器语法错误失败，页面契约发现集成夹具缺少新元素；修正测试代码与夹具后两者重跑通过，未以首轮失败结果作验收。
- 安全与风险：只在维护页内存保存状态，不新增档位／容器事实落盘，不改变驱动保护、认证次数或挂载行为；告警不会宣称完成完整性校验。后台独占任务期间不实时查询，下一轮空闲轮询补查；真实驱动时延、实际 WebView 显示及复制中触发留第 9 片。
- 提交：按用户授权自行提交，不推送；里程碑更新为第 8 片已实现，第 9 片全套测试、实机与恢复导航仍未开始。

### 2026-09-20 — P3.25 第九片：入口恢复与回归收尾，专项实机待验
- 范围与审计：复核第 9 片七项实机清单、8.5／8.6／8.7.2、模式 C 实现及设计稿；8.5 原“按身份渲染入口”与 8.7.2 冲突，按后者统一入口并回写前者。
- 检查与实现：移除导航统一 hidden，契约与 DOM 断言改为恒显且不由脚本按身份决定；补模式 C 行、8.5 实施口径及设计稿偏离说明，明确完整性校验仍未实现。
- 验证：Debug 全量构建及 61 项 CTest 执行完成（492.17 秒），59 通过、2 因父令牌受限跳过；单独非受限复跑后 HID 兼容性通过，helper 进程测试失败，合计 60 通过、1 未通过，不声称全绿。
- 验证：导航 243、配置 20／7、启动 12 场景通过；安装登记、更新路径、文件事务、恢复、安装引擎五组回归通过，引擎首轮沙箱退出 10，非受限重跑通过。Release 主应用及 setup helper 构建通过，git diff --check 通过。
- 验证：真实 VeraCrypt 0x126 专用普通容器挂载／探针读写／卸载通过（派生 2915ms、挂载 640ms、卸载 10ms）；首次相对路径被拒，改绝对路径后通过。未触碰用户容器或认证实体卡，此项不代替受保护外层专项测试。
- 集成修正：真实 WebView 假后端夹具首轮沙箱退出 1；非受限依次发现开关回读尚未完成即重击、隐藏窗口绘制帧被暂停、过期卸载文案及完成等待问题。仅修测试等待与固定文案，隐藏夹具以定时器替代绘制帧调度，最终退出 0；不把这项称为视觉验收。
- 既有阻塞：helper 进程测试在 development vendor runtime staging 被路径安全门拒绝，主日志早期已有相同失败；LocalAppData 根目录只读核查未发现重解析点，尚未定位实际不匹配目录，不放宽防重定向检查，不推断为首次启动故障根因。
- 安全与风险：导航恒显不泄露身份差异，维护列表／保护状态不新增落盘；跨卷导入仍按普通复制，用户需确认外层内容，不添加依赖源卷身份的条件 UI。强制卸载数据丢失、后台期间轮询延迟、原生通知失败可能延迟展示仍保留。
- 剩余：七项模式 C 专项实机（含真实保护触发、容量／UAC、占用卸载、GiB 拖放、不同口令表现）全部待验，需要专用可破坏测试卡／容器及交互条件。第 9 片标记部分完成，不把恢复入口或普通卷实测冒充这些验收。
- 提交：按用户授权提交可独立审阅的入口、夹具与文档收尾，不推送；P3.25 不标为全部验收完成。
- 交付：out/installers/installer-20260920-182138/USB-Vault-Setup.exe，300312024 字节，SHA-256 FCA8202D4F0661CF58ED19261C046E43BD2D94A125D005F624200E01C4745273 与构建记录一致，载荷来源 2c06db2 + dirty；NSIS 仅两个既有未使用变量告警。用户随后指定可使用桌面 vctest 容器，实机部分另片继续，现有卡不重置。

### 2026-09-20 — P3.25 第九片实机补测：真实保护触发与容量反例
- 范围与审计：用户说明仅一张卡、桌面 vctest 容器可用于测试；未获取或猜测卡登录口令。为独立验证驱动行为，新建 128 MiB 外层／64 MiB 隐藏卷的固定测试口令空容器，不动卡记录或 vctest 原文件。
- 检查与实现：新增显式参数、默认不构建且不注册 CTest 的实机夹具；新建模式拒绝覆盖文件，复用模式须显式选择已创建测试副本，数据写入有 129 MiB 上限；仅卸载本次成功挂载的盘符，容器保留供复核。
- 验证：首次空容器创建成功，但复用 Z 挂载报驱动码 5；再次新建报 helper I/O 错误。检查发现非受限环境留有 Z 到 VeraCryptVolumeZ 的失效映射，驱动无活动挂载；测试改在建卷后合并系统与驱动占用列表选 Y。
- 验证：对首个空容器的新副本执行通过：不开保护外层报 normal，开保护报 outer；两次常驻度量一致且连接可关闭；占用文件下普通卸载拒绝，强制卸载后立即重挂成功。
- 验证：写入约 62 个 MiB 块后驱动变 outer_write_prevented，后续写被拒绝，卸载返回保护触发标志；隐藏卷哨兵文件字节保持一致。整个填充至发现触发共 297ms，不将其误写为页面轮询时延或全卷完整性保证。
- 重要反例：位图返回尾部空闲 132887552 字节（约 126 MiB），明显包含受保护区域，不能表示“还能安全写入”。当前公共驱动属性不提供保护区间；安全可写额度需求未完成，下一片立即撤下误导数值并纠正接口／文档语义。
- 证据：out/p3259-hardware-fe8cb8b3456f477e9bd06ee4130de997/result.txt 与 disposable.hc；原始成功建卷夹具在 out/p3259-hardware-15f9f25a57ea4e31b926976e0dcb42b5。未实测 GiB 拖放与实体卡登录拒绝对称性。
- 安全与风险：不修改认证重试、真实卡或原容器；仅使用固定非生产测试口令。结束后驱动无活动测试挂载；精确清理失效 Z 映射被系统拒绝，未扩大清理范围，建卷的跨提权盘符收束问题仍待修复。
- 提交：按用户授权提交实机夹具和真实结果，不推送；第 9 片继续，容量口径与建卷残留均不得标为已解决。

### 2026-09-20 — P3.25 容量反例修正：撤下不可靠可写额度
- 范围与审计：按实机反例复核 8.5 与 Q-01；文件系统尾部空闲包含受保护隐藏区域，过去把扫描正确等同安全可写额度正确的结论不成立。
- 检查与实现：会话、控制器及桥中的 writable 字段改名 tail_free，明确其只是文件系统事实；页面保留已用／总量，额度显示“暂无法可靠计算”，保护触发后仍显示“写入已阻止”。
- 检查与实现：回写需求、里程碑与设计稿，可靠额度算法明确未完成；实机清单区分通过、部分通过、反例及未执行项目，不把哨兵保持等同全卷完整性。
- 验证：Debug 与 Release 主应用、Release setup helper 构建通过；会话、控制器、桥、页面契约四项 CTest 全通过，导航 DOM 243 项通过，git diff --check 通过。
- 验证边界：沿用本轮全量 60 项通过／1 项既有 helper 进程失败结果，未重复全量；页面交互时延、实体卡完整流程、GiB 拖放仍待验。
- 安全审查：不改变驱动保护、认证次数、强制卸载范围或秘密处理；不新增容量／结构事实落盘，协议仍为三个数字但不再将第三项作为写入许可展示。
- 剩余风险：可靠安全可写额度需要新的边界依据；建卷跨提权的失效 Z 映射尚未修复。当前没有活动测试挂载，未清除真实卡或用户原容器。
- 提交：按用户此前授权自行提交此独立修正，不推送；P3.25.9 保持部分完成。
- 交付：out/installers/installer-20260920-184152/USB-Vault-Setup.exe，300312175 字节，SHA-256 66DD417E04F2EF1A503F266B86462000CD6C912AB7CF5AA75267DF4247E43ABA 与构建记录一致；来源 1781a3b + dirty。替代旧 182138 包的误导性额度显示，NSIS 仅两个既有未使用变量告警；未自动替换用户已安装应用。

### 2026-09-20 — P3.25 容量修复第一片：只读卷头几何接口
- 范围与审计：复核 Q-01、8.5／8.6、R-03、L-01 及驱动保护实现；现有 FormatDLL 无只读卷头接口，按预案把原四片拆成五片，本片仅交付隔离查询能力，helper／页面均未接入。
- 检查与实现：新增第三方候选补丁和 UsbVaultGeometry.inl，由独立 DLL 解密两个 512 字节卷头，固定 SHA-512／PIM 0，返回外层起点／长度与隐藏保护起点／长度；主程序不加载 DLL，不新增挂载或文件写入。
- 检查与实现：使用显式二进制口令长度，两个卷头均校验；仅接受当前 v5、非系统、完整加密、512 字节扇区布局，以减法验证容器边界并统一拒绝错误口令／损坏头／不支持布局。无效结果清零，不导出卷头明文或密钥。
- 验证：从固定 b48e31f5 源码独立构建 Crypto、Zip 和 Release FormatDLL；显式构建并运行 hidden_geometry_integration_tests 通过，覆盖真实几何、双口令失败、损坏／对调头、NUL 后缀、尺寸与空指针、初始化门禁及失败后重复成功。未运行无关全套测试。
- 验证：128 MiB／64 MiB 空容器返回外层起点 131072、长度 133955584，隐藏保护起点 66977792、长度 66977792；保护长度比创建参数少 128 KiB，不能把创建容量直接当驱动保护长度。
- 验证修正：MSBuild 沙箱内被 SDK 目录读取拒绝，非受限构建后补齐上游 MIDL 生成头的工作目录与名称，最终编译通过；上游有代码页与既有类型转换告警，本片新增接口无编译错误。测试输出见 out/p325-capacity-format/geometry-test.txt；首次暂存检查把新补丁上下文空白判为格式问题，缩减无关上下文后反向应用校验及 diff 检查通过。
- 安全审查：新增口令副本使用专用锁页并在所有退出路径清零，crypto_close 清理上游密钥对象；接口无路径输入、文件写入、驱动挂载或日志。上游密码学分配／锁页行为沿用现状，查询仅在未来认证维护会话内调用，不提供通用页面入口。
- 边界与后续：两个头是否来自同一实际文件须由下一片 helper 持有并验证文件句柄，输出须绑定维护会话且及时释放；本接口不把几何事实当可写额度，尚未进行文件系统映射、页面显示或最终实机验收。
- 产物：候选 DLL SHA-256 3AFB42E3637901F38EEDCC0FF1C0CC82E11F09B7F9DD64E1079123AB88DC46E6；正式依赖摘要仍为 E411BB95…AF28AB3。测试容器前后 SHA-256 均为 D790DFAC…5FBFECA，未操作实体卡或用户原容器，未打包候选 DLL。
- 提交：按用户此前自行提交的授权提交本片，不推送；下一片为 helper 通道、认证会话绑定与分发接入，不能将本片记为容量修复完成。

### 2026-09-21 — 2011 安全诊断补齐
- 范围与审计：复核里程碑、R-03、8.6 与启动日志增补；用户日志只有 2011／原因 1，无系统码，不能据此认定权限、密码或拦截根因；本片仅补可定位证据。
- 检查与实现：审阅 helper 客户端全部 process_security 出口、异常捕获、清理保留、启动日志和现有测试；本地新增 89 个固定步骤编号，保持主码 2011／原因 1、页面和 IPC 不变。
- 检查与实现：Windows API 失败即时捕获 GetLastError；AppContainer 创建使用原 HRESULT；成功但结果形状异常、安全属性缺失均不伪造系统码，缓解策略逐项区分，原拒绝条件保留。
- 检查与实现：主错及清理错各携带独立步骤，捕获和日志均仅允许 2011 的已知步骤；成功、未知编号、无类别底层值清零，不增加认证或后台日志。
- 验证：Debug 主程序、helper 客户端与相关测试构建通过；startup_failure、startup_log、card_entry_context_controller、vikey_helper_protocol 四组通过，覆盖 Win32／HRESULT／逻辑拒绝、清理不覆盖、非法步骤及落盘过滤。
- 验证限制：helper_process 沙箱父令牌受限跳过，非受限重跑在 development vendor runtime staging 被既有路径重定向校验拒绝；其前面的隔离启动、网络隔离与开发 helper 测试已执行通过，整组不算通过；未跑全量或用户机器 2011 复现。
- 构建修正：初次缺少 UTF-8/MSVC 环境并触发 CMake 缓存重建，随后恢复正式 FormatDLL 路径；曾误用不存在的应用目标，最终使用正式目标。helper 对象出现 LNK1163，删除该生成对象及库后重编通过，不以此前失败构建作验证。
- 安全与风险：只记录固定检查点及有类别系统数值，不记录 token／SID／权限内容、路径、凭据、卡身份或档位；无重试、无安全降级，旧日志无法回补，89 个出口尚未逐项故障注入。
- 用户审阅：已授权提交并打压缩包；按要求去掉启动日志标题与结束行的两处括号说明，诊断字段与时间精度保持不变。
- 验证补充：删除两处括号说明后 startup_log_tests 复跑通过；package-release 主程序、helper、创建器及 setup helper 构建通过，diff 检查通过；仅修改日志展示文字，安全字段与失败行为不变。
- 提交与交付：用户已授权，提交本片后生成完整安装压缩包并记录来源哈希；不推送。

### 2026-09-21 — 2011 诊断交付压缩包
- 范围与审计：用户授权删除日志两处括号说明、提交并打压缩包；采用现有离线载荷与安装器流程，不修改依赖白名单或安全检查。
- 提交：实现已提交 `ea410ca`；Release 载荷标识为该完整修订、打包时 clean，未推送；本条交付记录单独提交，不改变二进制来源。
- 检查与实现：运行文件包保留正式 FormatDLL 与供应商 DLL，固定摘要及前置依赖签名校验通过；未使用容量接口候选 DLL。NSIS 3.12 安装器生成成功，仅两个既有未使用变量告警。
- 验证：运行文件 ZIP 的 14 个清单项逐项摘要匹配，无额外文件；最终 ZIP 只含安装器、测试须知、SHA256.txt，逐项重读核对通过；Release 主程序中两处旧说明均不存在。
- 安全与风险：沿用前片四组测试通过与 helper 集成路径校验阻塞结果；本包未进行实机安装或用户原故障复现，未宣称修复 2011 根因；无凭据、测试容器或调试产物入包。
- 交付：`out/deliveries/USB-Vault-2011-ea410ca-20260921.zip`，300358946 字节；SHA-256 `64C9832B8DA8DA9A78B4841B9170A7F5A52FD664452FEFEFF41CEC29B0C79B19`。

### 2026-09-20 — 撤销 2011 诊断提交中的日志压缩与新增归档
- 范围与审计：按用户要求重写原 ea410ca；恢复该提交父版本主日志，诊断与交付记录继续追加，移除本次新增归档及其引用，保留更早历史归档。
- 检查与实现：诊断业务代码及测试与原提交逐项一致，交付来源仍记录实际打包时的旧修订；后继交付提交同步重写，不重打包、不伪造新来源。
- 验证与安全：对比两次重写树，除主日志与被移除归档外无文件变化；当前未提交实验文件摘要保持一致，原提交和主日志有本地恢复点。仅历史与文档调整，未重复业务测试。
- 提交：本次重写与后续阶段逐片提交均获用户授权；不推送，FAT32 第一片失败恢复验证待继续。

### 2026-09-21 — 安装包规则统一与重新交付
- 范围与审计：按用户更正检查 AGENTS、既有打包脚本与上次交付；所有安装交付固定到 out/installers，目录及 ZIP 主名为 installer-yyyymmdd-hhmmss，时间取打包机器本地时间。
- 检查与实现：AGENTS 移除总字数／行数上限及超限归档要求，继续简写并沿用当前 DEVELOPMENT_LOG.md；本轮不创建、拆分或归档开发日志。AGENTS 沿用仓库已有忽略规则，仅本地保存。
- 检查与实现：使用原 ea410ca Release 载荷重新生成 NSIS 安装器，身份和载荷清单随新目录保留；未修改程序或安全校验。上次 out/packages、out/deliveries 的六个本轮产物已删除，保留其他历史包。
- 验证：新安装器构建成功，仅两个既有未使用变量告警；安装器摘要匹配构建记录，ZIP 三个条目与磁盘文件及 SHA256.txt 逐项匹配，无额外文件；删除前验证精确路径在仓库 out 内且无重解析点，删除后确认旧路径不存在。
- 验证限制：前次构建随会话中断，重启时沙箱启动器发生 ACL 配置故障，非受限重建成功；未产出安装器的暂存目录已清理。未重复执行不涉及修改的代码测试，沿用既有 helper 集成阻塞及未实机复现边界。
- 交付：out/installers/installer-20260920-194305.zip，300358946 字节，SHA-256 C8E80436A339A0351838F92A983510A8E6673836E21B6AE1E62D1BE0F1B3B304；同名目录含 USB-Vault-Setup.exe、说明、校验与构建记录。此前不规则交付链接已失效。
- 提交：本轮规则与交付记录尚未新增提交，等待用户决定；未推送。

### 2026-09-20 — 模式 C 容量提示可靠性实测与取舍建议
- 范围与审计：按用户“实测行为再判断可靠性”复核 8.5、容量修复计划、文件复制与 FAT 分配机制；本片不继续 helper 接入、不更改产品 UI 或原容量硬性需求，保留既有未提交交付日志。
- 检查与实现：新增显式、默认不构建且不注册 CTest 的空容器实验；FAT16（32/16 MiB）与 FAT32（128/64 MiB），覆盖空卷新增、删除前部／交错文件、新增／追加及卸载重挂，每场景重复两次。
- 验证：首轮 24 组观察删除簇复用，第二轮 24 组增加文件系统空闲计数与卷头边界的保守下限交叉验证；共 48 组均触发驱动保护并保留卸载触发标志，整个隐藏保护区 SHA-256 与原始空卷一致。
- 关键结果：128/64 MiB FAT32 交错删除释放 24 MiB 后，仅新增 14.75 MiB 即触发，保护区前仍至少 23.97 MiB 空闲；重挂后相同。追加末尾已有文件仅新增约 14.69 MiB，释放的 24 MiB 未被该文件复用。
- 判定：保护区前空闲不能作为保证可复制额度；不能据样本指定通用百分比余量。建议暂停仅为“精确可写额度”的后续实现，是否展示明确标注的空间参考交由用户决定；连续尾部指标尚未建立误差上界。
- 验证限制：管理员位图实验返回“操作已被用户取消”，未执行且未绕过，后续普通权限实验独立完成；未测其他簇大小、NTFS／exFAT、GiB 文件、应用页面与拖放，未跑无关全套回归。
- 安全审查：只创建新目录与新容器副本，仅卸载本次成功挂载的测试卷，写入上限固定；失败记录限定新目录，已有目录及原 failure.txt 拒绝覆盖用例通过。未操作卡或 vctest 原容器，最后驱动无活动测试挂载。
- 记录与验证：最终夹具构建通过；合并结论存于 docs/decoy-capacity-allocation-20260920.md；逐次 CSV、旧报告和本机摘要仅保留在本地 out/experiments/decoy-capacity-20260920-215609；原始容器及 complete.txt 保留在 out/allocation-basic-* 与 out/allocation-lower-bound-*。沙箱 ACL 初始化故障需非受限执行，不将它当实验失败。
- 提交：实现、结果、限制与建议已报告；本片尚未提交，按最新仓库约定等待用户决定，未推送、未打包。

### 2026-09-20 — 模式 C 保护触发后的小文件重试补测
- 范围与审计：按用户要求验证“触发时仍有空闲，继续上传较小文件能否成功”，复用既有 FAT32 空容器、驱动保护状态及卷头边界，不扩张产品功能或修改用户原容器。
- 检查与实现：新增显式 recovery 实验入口；每轮独立构造三次交错删除后触发的基线，各文件大小从已卸载基线复制新容器，避免重试之间相互影响。
- 验证：首轮 36 次尝试，第二轮 48 次含 12 次对照。4 KiB、64 KiB、1／4／16／23 MiB 在同次挂载与重挂后均未成功；第二轮每大小各三次，均返回错误 87 且保护再次触发。
- 对照：重挂后删除失败 fill.bin，再新增 4 KiB／1 MiB／23 MiB 仍各 0/3 成功；覆盖已有 1 MiB 文件 3/3 成功，完整写入、刷新及再次重挂后逐字节读取均通过。
- 解释边界：重挂前空闲下限约 23.98 MiB，重挂后约 22.67 MiB；不能把缓存时点不同的数字称为固定额度。更小的文件仍失败，删除失败文件后下限回升约 38.73 MiB 也未恢复新增；不外推所有容器永远如此。
- 安全与风险：只有自建副本被删除／覆盖，整个隐藏保护区结束摘要均一致，未操作实体卡或 vctest 原文件，不进行文件系统修复；代码与数据均无真实凭据。成功判断包含重挂后持久性验证。
- 验证修正：首次构建因生成代码换行转义错误失败，修正后构建及两轮实测通过；已有目录拒绝覆盖与卸载状态复查另行验证。未跑无关全套，不把本片算作产品 UI 验收。
- 证据与提交：扩展 docs/decoy-capacity-allocation-20260920.md，补两份 recovery CSV 并更新当前夹具校验清单；本片尚未提交，等待用户决定，未推送、未打包。

### 2026-09-20 — 模式 C 指定安全空闲簇写入可行性验证
- 范围与审计：按用户要求验证“检测最大空闲后指定写入”，审计 8.5／8.6、现有分配与重试夹具及 Microsoft 搬移接口；仅测试新容器副本，不接入正式产品。
- 检查与实现：新增显式 placement 实验入口；完整 FAT 位图与 BPB／系统空闲计数交叉验证，选择保护区前最大连续空闲段，预分配并设置 EOF，搬移后强制核验簇位置与边界才写入。
- 验证：普通权限 8 组预分配成功但卷句柄错误 5，搬移未测；本轮 UAC 授权后最终管理员 8 组全部通过，4 KiB／1 MiB、两种准备方式、各重复两次；最大连续安全段 1 MiB。
- 完整性：所有最终文件写入与刷新成功，卸载重挂后长度及逐字节内容一致；本轮包括提前退出轮次在内的全部容器隐藏保护区摘要复核一致，最终无活动测试挂载。
- 修正与限制：前两次管理员运行因 EOF 为零时簇查询返回错误 38 提前退出；补齐设置 EOF 后完整运行通过，不把提前退出轮次计为完成。旧普通复制与本轮不是同轮随机配对对照，不声称通用成功率。
- 安全审查：始终开启隐藏保护，只修改自建测试副本，输出目录拒绝覆盖通过；未操作真实卡与原容器，未直接改 FAT，未使用 SetFileValidData。实际产品不能持久化这些保护区布局信息。
- 验证与证据：/W4 /WX 构建通过；docs/decoy-capacity-allocation-20260920.md 与两份 CSV 记录结果，更新当前夹具摘要；未测其他文件系统、大文件、并发与中断恢复，容量保证仍不能成立。
- 提交：可行性已验证，产品实现与需求口径未改变；本片未提交，等待用户决定，未推送、未打包。

### 2026-09-20 — 模式 C 多段安全空闲空间写入验证
- 范围与审计：按用户要求验证 8 MiB 文件利用多个安全空闲段；复核 8.5／8.6、P3.25 状态和已有指定簇实验，不实施整卷整理或修改产品需求。
- 检查与实现：新增显式 multirun 入口，每段重读位图并搬移对应 VCN，最终逐段核对位置及保护边界后写入；采用随偏移变化的内容图案检测错序。
- 验证：/W4 /WX 构建通过；UAC 授权后两种预分配方式、两份独立基线共 4 组，最大连续安全段均 1 MiB，8 MiB 文件分别占用 8 段，32 次搬移全部成功。
- 完整性：4 组新文件刷新及重挂逐字节校验通过；每组已有 25 个普通文件名称、长度、全内容摘要均未变，整个隐藏保护区摘要一致，未观察到保护触发。
- 安全审查：仅写新建测试容器副本，全程驱动保护开启；移动目标来自新位图，写入前强制验证全部位置；已有输出目录拒绝覆盖且结果摘要不变，结束后无活动测试挂载。
- 边界与结论：无需先整理整卷即可利用碎片的路线在该样本成立；未验证并发、取消、部分失败或进程中断，未测其他文件系统与大文件，不推出容量保证；原文件核对不涵盖通用目录树与所有元数据。
- 证据：docs/decoy-capacity-allocation-20260920.md 与完整 CSV，更新当前夹具校验清单；未执行无关全量回归或产品页面验收。
- 提交：夹具与报告已完成，正式上传流程未改；未提交，等待用户决定，未推送、未打包。

### 2026-09-20 — 模式 C 顺序填充不等长空闲段与空间耗尽验证
- 范围与审计：按用户要求复核 8.5／8.6、P3.25 状态和多段夹具，验证 1／2／3／4／5 MiB 空洞、连续上传与提前停止；不接入产品或更改容量承诺。
- 检查与实现：新增 sequential 显式入口，复制空样本并创建／删除测试种子，重挂后位图实证五段布局；创建前按地址顺序规划、预分配后保留位置正确的部分，其余部分核验目标空闲再搬移。
- 验证：/W4 /WX 构建通过，两轮分别为连续挂载、每文件重挂；8／3／4 MiB 文件实际分段为 1+2+3+2、2+1、4 MiB，均与计划逐段一致。
- 空间边界：五段用完后另有 15,446,016 字节安全空闲；两轮均成功写满该部分，安全空闲归零；超出剩余量一簇以及归零后的 4 KiB 请求均在创建前拒绝，文件集合、摘要和空闲量不变。
- 完整性：共八个成功新文件重挂后长度与全部内容正确；每轮 33 个原文件名称、长度和内容摘要未变，整个隐藏区摘要一致，阶段状态与卸载返回值未显示保护触发。
- 失败修正：首次第三文件已由预分配占到正确目标，旧算法仅看空闲而将其移走，断言失败；修正为保留正确分配后两轮完整通过。保留首次失败证据，不计入成功；全部三个副本隐藏区经离线复核一致。
- 安全与限制：始终驱动保护，仅自建副本；目标冲突中止而不覆盖，输出目录拒绝覆盖通过，最后无测试挂载。未验证部分失败回滚、取消、并发、目录增长、非整簇、大文件或其他文件系统，不宣称容量保证。
- 证据与提交：docs/decoy-capacity-allocation-20260920.md、CSV 与更新后的当前夹具摘要；未跑无关全量、未改产品、未打包；本片未提交，等待用户决定，未推送。
### 2026-09-20 — FAT32 受控上传阶段启动与实测基线提交
- 范围与审计：用户确认先采用已实测的 FAT32 路线，并授权后续完成验证与安全审查后自主逐片提交；不再等待每片提交确认，不推送。
- 基线：汇总此前普通分配反例、保护后重试、指定簇、多段与顺序填空洞的已完成实验；保留各报告的失败记录、验证边界与真实结果，不重复无关测试。
- 检查与验证：既有夹具构建、实机摘要、重挂读取和输出目录防覆盖结果见各片记录；提交前复核补丁格式、当前证据摘要及源文件范围。
- 安全与风险：基线只涉及测试与证据，不将其作为正式上传或容量保证；保留与本阶段无关的安装交付日志在工作区。
- 提交：用户已授权提交本实验基线；随后按六片计划从失败恢复验证开始，正式阶段状态另行维护。

### 2026-09-20 — 精简容量实验基线提交
- 范围与审计：按用户要求重写原 8a139fc；保留可复验测试代码、一份合并结论及必要主日志和计划记录，逐次 CSV、旧报告及本机 out 二进制摘要不再跟踪。
- 检查与实现：合并普通分配、重试、指定单段、多段、顺序填空洞的结论、失败记录与复验入口；原始材料按原始字节保留在本地 out/experiments，未新增或拆分开发日志。
- 验证与安全：重写树中测试代码与原提交一致；当前未提交失败恢复代码及安装交付日志保留；文档引用与差异检查通过，仅精简提交资料，未重跑实机测试。
- 提交：用户授权重写，保留本地旧历史恢复点；无产品行为变化，未推送，失败恢复切片尚未提交。

### 2026-09-20 — FAT32 受控上传第一片：失败与恢复边界
- 范围与审计：复核 8.5／8.6、现有 helper 只读边界与六片计划；仅扩展故障夹具，不接入产品。
- 检查与实现：六类故障覆盖预分配权限错、部分搬移取消、目标占用、部分写入权限错、真实隐藏保护拒绝、继承测试句柄的子进程强制退出。
- 验证：最终六例完成，各例 25 个原文件集合与内容摘要保持一致，清理重挂后安全空闲均回到 25,100,288 字节，整个隐藏区摘要一致；构建、防覆盖、无效子句柄拒绝及挂载清理通过。
- 关键发现：保护触发后删除返回成功仍可能在重挂后出现；关闭部分分配文件也可能伴随保护触发。正式队列需停止，清理状态不能只凭 API 返回；后台具体触发点未被唯一定位。
- 安全取舍：只清理本次独占目标并保留原错；进程结束后不凭名称删除历史文件，不新增泄露维护身份或布局的恢复日志。所有操作只在测试副本。
- 失败与限制：首轮空洞大小假设错误、第二轮路径式子进程超时均不算通过；修正继承句柄后完成。未模拟整机断电、介质坏块、删除权限错、任意时刻崩溃或压力。
- 证据：结果追加至唯一合并报告，原始 CSV 留在 out；MILESTONES 更新当前六片顺序，既有安装交付日志不纳入本片。
- 提交：按用户授权自主提交，随后继续第二片；未推送，未改正式上传／页面，未打包。

### 2026-09-21 — FAT32 第二片 2a：安全空间扫描核心
- 范围与审计：承接 b5067f9，复核 8.5／8.6、只读 helper 与六片计划；本片仅落地正式扫描核心，不开放页面或上传。
- 检查与实现：借用调用方卷句柄，核对卷长并校验 FAT32 BPB、FAT 容量、根簇和保护边界；只计边界前完整空闲簇，拒绝位图缺页、短帧、负值和无进展，重叠前缀不重复统计。
- 单元验证：MSVC /W4 /WX 三目标构建、fat32_safe_space_tests 与 volume_space_tests 2/2 通过；覆盖跨边界簇、全占用、零安全簇、重叠分页、异常几何及 4096 字节扇区。
- 实机验证：新建两轮 FAT32 128/64 MiB 副本，正式扫描与独立位图统计逐次相等；按 1～5 MiB 空洞顺序写入、余量耗尽及创建前超额拒绝通过，第二轮文件间重挂后继续核验。
- 完整性：每轮 33 个原文件与全部新文件重挂后内容通过，整个隐藏区摘要不变，无意外保护触发；实验输出留在 out，结论只追加既有合并报告。
- 失败与恢复：此前 UAC 被取消不算通过；本轮按“继续”重新授权。已清理的产物从固定源码恢复，修正 MIDL 环境后候选 DLL 构建通过；新增仅生成基础样本入口，避免将准备样本误记成 24 场景通过。
- 安全与限制：核心只读、不打开任意路径、不落盘敏感布局；几何来源与实际挂载身份仍待 2b 绑定。空闲快照不保证文件负载容量，未扩展至 NTFS／exFAT、介质故障或一般目录元数据验收。
- 提交：按用户既有授权自主提交本子片，不推送；其他安装交付和登录前重置计划的工作区改动保留，不纳入。

### 2026-09-21 — 登录前破坏性重新初始化审计
- 范围与审计：用户明确遗忘的是应用登录密码，期望复用设置页初始化语义；复核 N-06／Q-02、R-03／8.6、里程碑 17g／17h 与 UI→入口→记录服务→helper→卡日志调用链，只审计不实现、不操作实体卡。
- 现有行为：UI 与入口控制器均要求已认证；设置页先卸载本会话卷，仍挂载则拒绝写卡。重新探测当前卡并构造绑定硬件 ID 的 update target，写入 provisioning marker、复读核对，成功清除会话并关闭应用。
- 数据范围：216 字节逻辑记录位于用户区前 736 字节双槽日志；提交新标记后用随机数据覆盖旧槽并镜像描述符。源码写入仅在此范围，无厂商 PIN／UserSN／API Key 改写或全卡清空；旧 P1 全用户区擦除工具不得复用。
- 数据后果：两个密码信封同时失效；下次初始化生成新根秘密，旧容器文件与本地加密索引未删除，但新卡记录不能恢复旧访问能力。正常完成只证明 SDK 可寻址区域覆盖，不承诺芯片物理安全擦除或清除外部副本。
- 接入边界：底层 reset_to_provisioning 无旧应用密码参数，技术上可复用；应新增登录前独立破坏性动作，保留设置页认证门禁，并仅接受当前可识别且有效的已初始化产品记录，拒绝损坏、未知版本及外来卡。
- 安全与风险：确认须绑定同次插卡与硬件身份，换卡失效；保留 SDK 凭据认证、后台互斥、回读和不确定失败后上下文退休。不凭未登录推断全机无挂载，其他实例残留卷需专项设计；当前对话框确认在页面层，新增入口需原生状态约束。
- 需求取舍：需求仍明确暂停忘记密码入口；新的评审边界应为销毁全部应用卡记录并重新开始，不能改成保留根秘密、选择性替换信封或恢复旧数据。未认证但持卡且具备 SDK 凭据者可破坏旧访问能力，是新增授权边界，需在实施前明确。
- 验证：静态审阅已有服务重置、拒绝未认证／挂载中、复读失败及日志中断恢复测试；本轮未执行构建、测试或实机断电／换卡测试，不把既有测试源码视为本轮通过。发现工作区有独立容量开发修改，均未触碰。
- 提交：仅追加审计记录，未改业务代码、未提交、未推送。

### 2026-09-21 — 登录前重新初始化切片计划
- 范围与审计：依据本轮重置审计、N-06／Q-02 与设置页既有语义制定六片计划；不把制定计划视为取消需求限制或自动提交授权。
- 检查与实现：新增 docs/design/login-card-reset-plan.md，并在里程碑追加范围入口；需求与确认、操作互斥、写入恢复、原生桥、页面流程、综合验收各自独立审阅，尚未实现业务代码。
- 关键取舍：只重置应用前 736 字节记录，废弃双信封和旧根秘密；不改 SDK 凭据、不擦全卡、不删容器，不提供旧数据恢复。
- 用户决定与风险：取消计划最初提出的挂载门禁；已有卷、查询失败及状态未知均允许登录前重置，不要求或执行卸载。确认绑定插卡代次及硬件身份，准备只读，执行单次消费，同卡并发写入仍互斥；不将静态挂载或其他实例已登录作为互斥失败。
- 安全边界：未登录不等于全机无挂载；其他进程仍可能持有旧密钥、继续访问已有卷。确认文案固定说明本流程不撤销既有访问，卸载后不能凭新卡记录恢复旧容器；设置页原认证及卸载行为不在本次调整范围。
- 验证安排：每片列出拒绝、失败及验收出口；末片需要明确授权的可破坏测试卡与区域外哨兵，不以当前用户卡代替。现有 helper 集成问题和未执行的实机检查不会据计划变成通过。
- 验证：检查文档链接、六片完整性、挂载门禁相关条目一致性和差异格式；计划补四种挂载状态均不阻断、零卸载调用及跨实例写入竞争验收。未运行构建或程序测试，无业务代码修改，未触碰独立 FAT32 开发文件。
- 提交：计划待用户审阅，本轮未提交、未推送；进展继续写入原日志，不新建或归档开发日志。

### 2026-09-21 — FAT32 第二片 2b 前置：只读挂载身份
- 范围与审计：2a 已提交 3bfd933；复核只读 helper 接入边界，盘符、路径和卷长无法区分同盘符重挂，先补驱动挂载身份原语。
- 检查与实现：按固定 VeraCrypt 1.26.29 外部 ABI 新增只读属性查询，校验版本、完整返回、盘符、编号、路径终止与状态值；缓冲区沿用锁页销毁机制。
- 生命周期：返回临时挂载编号、路径、卷长及受支持可写保护状态，不返回密钥或持久身份，不写日志／索引；编号零合法，负值与畸形字段拒绝。
- 安全审查：无写卷／卸载动作；只读、隐藏卷、无保护、已触发保护、系统范围及不支持的 PRF／PIM／标志均不能标记为受支持可写外层。
- 单元验证：/W4 /WX 构建通过，volume_provider_tests、volume_space_tests、fat32_safe_space_tests 3/3 通过；新增重挂编号、畸形字段及状态拒绝断言。
- 实机验证：两个全新 128/64 MiB 副本顺序上传通过；测量期间编号稳定，第二轮每次文件间重挂编号均改变；原有 33 文件、全部新文件和隐藏区域摘要保持通过。
- 限制与后续：编号仅适用于驱动本次生命周期，不可跨驱动重启复用；它不能独自证明容器文件身份。后续 helper 仍须核对持有文件身份、实际卷句柄、凭据、前后挂载状态及认证会话。
- 提交：按用户授权自主提交该前置，不推送；未接入页面、未替换正式 DLL，其他任务的工作区记录保留。

### 2026-09-21 — FAT32 第二片 2b：绑定真实挂载的只读 helper
- 范围与审计：承接 82ee6d2，复核六片计划、8.6、命名管道权限、驱动属性与候选几何 ABI；只新增度量，不新增提权写入入口。
- 检查与实现：协议升级 v3，保护测量必须带盘符、挂载编号、卷长和双凭据；无客户端保护边界。helper 模拟调用方打开容器，以文件 ID 和驱动保护卷头摘要核对身份，认证同一文件的双卷头后扫描固定 VeraCrypt 设备。
- 身份与生命周期：前后重查卷头和挂载身份，异常不返回空间；凭据经 SecureBuffer／管道传递，回复前清理；只读会话拒绝创建，修复回复异常时看门狗线程未收束风险。
- 安全审查：拒绝目录、文件重解析点、备用流、非受保护可写外层及不匹配卷长；文件访问依调用方权限，原始卷只读且路径不由用户指定；不记录敏感几何、摘要或凭据。
- 单元验证：/W4 /WX helper、核心与夹具构建通过，协议、驱动、容量相关 CTest 4/4；覆盖版本、长度、空凭据、非法身份、失败值为零及度量会话不能创建。
- 实机验证：普通权限客户端经标准 UAC 启动 helper，最终测试通过错文件、错编号、错卷长、双错凭据及客户端备用流拒绝；失败后复用、写入 1 MiB 后刷新、重挂旧编号拒绝和新编号测量通过，隐藏区及对照容器摘要不变。
- 失败与修正：首轮错误地对照外层卷头摘要被拒绝；固定驱动源码表明保护挂载最终摘要来自保护卷头，按该语义修正后通过。备用流被客户端提前拒绝，修正夹具异常类型预期后最终通过；失败轮次不计通过。
- 边界：协议与 helper 完成，认证应用生命周期接入另片；正式 DLL 白名单和安装包尚未切换，未知 DLL 缺导出拒绝。未模拟管理员篡改内核、驱动重载或高并发文件系统变化。
- 提交：按用户授权自主提交本子片，不推送；单一实验报告追加结论，原始产物留 out，其他工作区记录保留。

### 2026-09-21 — FAT32 第二片 2c：认证维护会话接入
- 范围与审计：承接 e9f4995，复核维护打开、筛选后打开、切换、主动卸载及拔卡释放路径；第二片完成代码接入，不提前改变第五片的页面显示。
- 检查与实现：正式维护挂载时捕获只读身份，无法确认则回收新挂载，回收失败仍记录残留供清理；度量前后核对同一身份，失配关闭 helper 并拒绝返回容量。
- 凭据生命周期：直接打开与筛选流程都保留锁页派生外层凭据，测量使用当前认证会话派生的隐藏凭据；会话释放清理全部派生值与身份，卸载清理该挂载身份。
- 数据口径：应用层返回独立 safe_free_bytes，失败抛错而非保留旧值；旧尾部字段置零，不再把尾部空闲当安全度量，页面暂维持不可可靠计算提示。
- 验证：相关核心、会话与界面控制器 /W4 /WX 构建通过；六组 CTest 通过，新增认证失效拒绝后两组会话回归再次通过。
- 拒绝覆盖：普通挂载不测量、错误凭据不外发、失效认证、测量前／中替换挂载、身份读取失败回收、测量失败后重试、结束与切换会话的现有断言均通过。
- 安全审查：新增身份仅在 SessionContainer 内存字段，不进入索引或日志；保持普通挂载与维护门禁，不新增写入通道。空闲快照仍不承诺任意文件负载额度。
- 限制与后续：本片应用生命周期由测试替身验证，真实 helper 测试见上一片；未进行完整应用实卡与安装包验收。候选 DLL 的正式白名单／交付仍待第六片，第 3～5 片正式受控写入和页面尚未实现。
- 提交：按用户授权自主提交，不推送；更新六片进度，仅暂存本片，保留其他安装及登录前重置计划记录。

### 2026-09-21 — FAT32 第三片 3a：正式顺序分配规划
- 范围与审计：复核六片计划、8.5／8.6、失败恢复结论和现有 CopyFileEx 路径；先落地只读规划，不开放新的写入或提权操作。
- 检查与实现：正式扫描与规划共用卷长、BPB 和完整分页校验；按地址顺序收集保护边界前空闲簇，合并连续段，扫描完成后才返回计划，不使用最大空洞优先。
- 文件边界：非整簇文件向上取整，零字节文件无数据簇，超过 FAT32 单文件 4 GiB−1 字节或安全空间不足拒绝；分配计划不等于簇预留，写入前必须再核验。
- 资源边界：单计划最多 65,536 段，超过时在创建文件前拒绝，避免碎片输入造成无界内存／搬移成本；上限不改变空间测量数字，不假装能写入该碎片负载。
- 单元验证：/W4 /WX 构建及规划、扫描、卷空间三组 CTest 3/3；覆盖 1～5 MiB 空洞依次写入 8／3／4 MiB、非对齐、耗尽、边界、最大文件、重复／越界簇、碎片上限和 100 轮确定性随机参考。
- 实机验证：两个新 128/64 MiB 副本，正式规划每段位置与夹具独立规划相等，第二轮文件间重挂；顺序写入、余量耗尽、超额拒绝及原有 33 文件／新文件／隐藏区完整性通过。
- 安全审查：只读且不打开用户路径、不落盘布局；整数计算以文件上限和 FAT32 簇上限约束，完整位图异常不发布计划。预分配、目标竞争、搬移和安全写入尚待 3b 后续接入。
- 失败与限制：初次测试编译因丢弃 nodiscard 返回值失败，修正后通过；实机仍为 1 KiB 簇和整簇负载，其他簇大小、非整簇写入和极端碎片写入验收留第六片。
- 提交：按用户授权自主提交本子片，不推送；主日志和唯一实验报告续写，原始实验留 out，未改变正式上传或页面。

### 2026-09-21 — FAT32 第三片 3b：实际文件簇映射核验
- 范围与审计：承接 54f948f，检查夹具预分配／搬移链与正式句柄接口；独立实现实际簇映射读取，不复用实验代码作为生产入口。
- 检查与实现：通过调用方独占文件句柄读取完整逻辑簇链，前后核对文件大小及分配字节数，保持逻辑文件顺序，连续物理段合并；不按物理排序改变文件内容顺序。
- 拒绝边界：负／稀疏 LCN、重复或缺口 VCN、不前进／不完整分页、超卷末尾、物理段重叠、尺寸变化和超过 65,536 段拒绝；SDK 尾数组按有效返回长度复制解析。
- 单元验证：/W4 /WX 构建通过；实际映射、规划、扫描和卷空间四组 CTest 4/4，覆盖分页、空文件、逆序物理段、畸形映射、重复使用、资源上限与 97 组排列参考。
- 实机验证：两个新 128/64 MiB FAT32 副本，每个文件在预分配后及搬移完成后，正式读取与独立夹具映射／计划逐段一致；重挂、耗尽、超额拒绝均通过。
- 完整性：每轮原有 33 文件、新文件及隐藏区摘要保持通过；输出由 out/file-allocation-active.txt 指向，未跟踪原始 CSV。
- 安全审查：读取函数不打开路径、不改变 EOF、不搬移或写入；完整映射校验后才返回，局部失败不作为可写证明。调用方仍须保证句柄属于认证卷和本次独占目标。
- 限制与后续：生产预分配、搬移执行和数据写入仍待下一子片，实际实机样本为整簇文件；更大分页和非整簇实机验收留第六片，不把核验原语称为上传完成。
- 提交：按用户授权自主提交，不推送；续写原日志与合并报告，保留其他工作区记录，未改变正式上传或页面。

### 2026-09-21 — FAT32 第三片 3c：正式预分配与受限搬移
- 范围与审计：核对六片计划、R-03／8.6 和既有规划／簇映射；本片实现进程内准备原语，未开放写入 IPC 或切换正式上传。
- 检查与实现：要求本次 CREATE_NEW 独占空文件，以 NT 设备路径绑定目标卷；固定驱动设备打开卷，认证边界重新解析 FAT32 几何并验证全部计划后才扩展 EOF。
- 搬移规则：保留已在正确位置的簇；其余按实际 VCN 段及重新扫描的空闲前缀限制搬移，每次操作前后核对挂载身份，支持取消并最终核对完整映射。
- 失败边界：取消、旧挂载身份、错误分配字节数及越界计划在分配前拒绝；不覆盖占用目标，不持久化布局。失败保留调用方持有的部分文件及原始 Windows 错误，由后续任务层负责清理。
- 验证：/W4 /WX 构建通过，四组相关 CTest 4/4；两个新 128/64 MiB 副本使用正式准备原语，独立核对五空洞顺序、余量耗尽、超额拒绝及第二轮逐文件重挂。
- 完整性：两轮各 33 个原文件、4 个新文件及隐藏区摘要通过；四类分配前拒绝同时核对 EOF／分配长度为零，原始输出见 out/production-placement-active.txt。
- 失败与修复：初次构建漏登记源文件导致链接失败，已修正；实机发现 FAT32 不支持 FileAttributeTagInfo，改用 GetFileInformationByHandle 后两轮通过。
- 安全审查：文件和卷句柄绑定、整数和段数界限、重挂拒绝及搬移前竞争检查已复核；查询与执行仍有竞争窗口，依赖文件系统拒绝占用簇和驱动保护兜底，不能视为容量保证。
- 后续与限制：数据写入仍由实验夹具执行；生产写入、取消后持久清理、故障注入、非整簇及其他簇大小验收未完成，不据此宣称第三片完成。
- 提交：依用户持续授权自主提交本子片，禁止推送；主日志仅追加本片，保留其他任务的安装交付及卡重置未提交记录。

### 2026-09-21 — FAT32 第三片 3d：正式分块数据写入
- 范围与审计：承接 252a2ff，核对 R-03、六片计划、源文件句柄与准备后映射；仍不开放特权写入协议。
- 检查与实现：准备／写入共享设备、几何及计划校验，写入前后核对完整簇映射；以 64 KiB 锁页清零缓冲复制，逐块核对挂载和取消，处理短写并显式刷新。
- 源文件约束：要求无写入／删除共享的同步句柄，拒绝目录、重解析点、自复制和尺寸不符，完成后复核源长度与最后写入时间；调用方独占使用源／目标句柄。
- 验证：/W4 /WX 构建和四组 CTest 4/4；两个新副本使用正式准备与写入，空文件、513／65,537 字节及五空洞大文件通过；每个大文件 64 KiB 后取消并核对停止位置，再重新复制成功。
- 完整性：两轮各 33 个原文件、4 个新大文件及隐藏区摘要通过；第二轮逐文件重挂，结果见 out/production-write-active.txt。
- 安全审查：固定大小明文缓冲每块后清零，异常析构清零；写入不扩展预分配长度、不改变目标路径，进度回调异常传播，挂载异常不继续写。
- 失败与限制：测试局部变量遮蔽触发 /WX，改名后通过；刷新不是掉电保证，外部特权映射竞争仍依赖驱动保护，失败清理归下一子片。
- 提交：依持续授权自主提交，不推送；正式上传入口尚未切换，保留其他任务未提交记录。

### 2026-09-21 — FAT32 第三片 3e：新文件所有权与失败清理
- 范围与审计：检查现有按路径回滚和此前保护触发后文件复现证据；本片只处理本操作 CREATE_NEW 的单文件所有权，不复用普通复制的路径删除。
- 检查与实现：新目标独占打开并取得 DELETE 权限；准备／写入异常通过原句柄请求删除，随后关闭，保留原始 exception_ptr 并单独记录清理错误。
- 生命周期：目标只能消费一次；同名已有文件创建失败时不触碰原文件，析构仅关闭未使用句柄，不根据名称猜测所有权。
- 验证：/W4 /WX 构建与四组 CTest 4/4；两个新副本覆盖中途取消、进度回调异常、原错保留、成功内容、同名碰撞及重复消费拒绝。
- 实机恢复：本次取消／回调失败后文件不可见，重挂后原文件一致、空间恢复，随后完整顺序写入与隐藏区校验通过；输出见 out/production-transfer-active.txt。
- 安全与风险：删除 API 接受只标记 deletion_requested，不能承诺落盘或掉电恢复；任何传输失败须由会话层停止队列并恢复检查，崩溃遗留不按名称自动删。
- 边界：创建必须由调用用户权限执行，当前原语不是特权路径授权入口；认证 helper 路径和会话接入留第四片，不对用户容器做恢复实验。
- 提交：依持续授权自主提交，不推送；第三片正式单文件核心已验证，第四片接入口和恢复状态，完整异常验收仍留第六片。

### 2026-09-21 — FAT32 第四片 4a：独立认证的上传 helper 边界
- 范围与审计：核对只读度量协议、调用方模拟和上传原语；新增单次上传入口，未开放任意管理员路径写入，也未切换页面入口。
- 检查与实现：协议 v4 为上传单列源路径和卷内相对目标；身份、双凭据、容器句柄／头摘要和真实几何由 helper 重新验证，测量与上传复用认证核验。
- 权限与路径：源只读且不共享写入／删除，目标及逐级锁定的非重解析父目录在调用用户权限下打开；只使用已验证 FAT32 的固定驱动设备操作簇，目标 CREATE_NEW 不覆盖。
- 生命周期：上传原语传递进度和取消，失败保留原错误并退出；只读测量会话明确拒绝上传请求，凭据仍仅经随机 nonce 本地管道传递并清零。
- 验证：/W4 /WX 构建和五组 CTest 5/5；新增字段边界／旧版本／缺凭据／测量夹带写入字段及活跃测量会话拒绝上传均通过。
- 实机验证：普通权限客户端经独立提权 helper 上传 8 MiB+513 字节到子目录，拒绝越界路径、旧身份、错误凭据、同名目标，中途取消清理和重挂验证通过，隐藏区不变。
- 回归：v4 只读 helper 六类拒绝、失败复用、容量刷新、旧挂载拒绝和隐藏区完整性通过；输出分别见 out/import-helper-active.txt、out/measurement-v4-active.txt。
- 安全与风险：客户端不传保护边界，操作前后核对头和身份，父目录持有期间禁止写／删除共享；特权竞争仍不能完全原子化，保护驱动为最终边界。
- 后续：目前一次文件一次 helper 同意，下一子片建立专用上传会话并接队列；任何失败须锁住当前维护上传，尚未把失败恢复状态接到页面。
- 提交：依持续授权自主提交，不推送；主日志追加且保留其他任务未提交内容。

### 2026-09-21 — FAT32 第四片 4b：专用上传会话与失败锁定
- 范围与审计：检查单次 helper 同意频率及测量会话限制；上传独立建会话，绑定首个容器路径、挂载 ID、盘符和容量，不复用测量授权。
- 检查与实现：同一卷多个文件复用一次同意；客户端和 helper 双侧拒绝换卷，任一失败关闭管道并锁定客户端直到显式结束会话，密码仍每请求验证并清零。
- 生命周期：helper 只在等下一请求时计算空闲超时，不在长时间传输中强制退出；管道断开结束会话，传输中断由已接入的句柄清理处理。
- 验证：/W4 /WX 构建、协议回归通过；普通权限客户端在同一 helper 连续两次上传成功，换挂载身份拒绝、失败后再次调用不重启且不创建文件。
- 实机回归：错误凭据／路径／同名、中途取消、子目录非整簇复制、重挂及隐藏区摘要通过，结果见 out/import-session-active.txt。
- 安全与风险：会话只接收上传操作，绑定元数据仅在内存；显式 close 是结束授权，不代表故障磁盘已恢复，应用层仍须在失败后禁止当前维护卷继续上传。
- 后续：下一子片接应用会话、文件选择／拖放和队列失败提示；目录递归和完整 UI 实卡验收尚未完成。
- 提交：依持续授权自主提交，不推送；其余工作区内容保留。

### 2026-09-21 — FAT32 第四片 4c：维护上传接入与失败队列停止
- 范围与审计：检查实际拖放入口、普通 CopyFileEx 路径、维护身份与队列；维护上传切至已认证的 FAT32 helper，普通文件操作保持原路径。
- 检查与实现：上传前后核对卡会话及挂载身份，传入会话派生双凭据；失败关闭上传会话并锁定该维护挂载，必须结束维护并重新挂载，直接重复打开当前失败挂载拒绝。
- 生命周期：切换／卸载维护、释放认证秘密时结束上传 helper；测量会话仍按原规则复用，不把两类授权混在一起。
- 队列与页面：首个失败停止后续项目，取消／失败显示未完成及恢复提示；页面保持禁止拖入直至重新进入维护，刷新不解除，驱动保护提示与上传失败分开。
- 验证：/W4 /WX 构建；原生会话、拖放、UI 控制器 3/3，补测复制前后身份变化、失败锁定、重新挂载恢复；DOM 导航 244 项通过。
- 验证修复：历史 jsdom 目录已不存在，在忽略的 out 恢复 26.1.0；新增恢复步骤使两条旧计数断言失效，改为相对卸载次数／实际禁止状态后通过，未改生产依赖。
- 安全审查：普通路径复制已不能绕过维护上传，失败不默默继续队列，进度／源路径不写历史；后台排他工作门保持卡状态变化与退出约束。
- 限制：新通道暂支持单文件，文件选择按钮、文件内进度及目录递归仍待后续；未执行完整应用实卡 UI 验收，不将第四片标记完成。
- 提交：依持续授权自主提交，不推送；保留其他任务的未提交记录。

### 2026-09-21 — FAT32 第四片 4d：原生多文件选择入口
- 范围与审计：检查原有仅拖放入口和四数字返回契约；新增“选择文件”，与拖放共享同一排他队列、取消和失败停止流程。
- 检查与实现：原生 IFileOpenDialog 只取本地现有文件，多选上限 1024，不加入最近项目、不改变工作目录；路径不传页面，页面只得到是否开始。
- 授权与生命周期：桥接要求认证且无后台任务，原生要求维护目标已启用；对话框返回后复核卡 epoch、目标启用和工作门，卡变化不启动旧选择。
- 验证：/W4 /WX 构建，桥接与拖放两组 CTest 2/2；页面导航 DOM 245 项通过，覆盖选择调用、取消仍可用、失败后按钮禁用。
- 测试修复：DOM 首次因新增计数字段未初始化超时，修正夹具后通过；共享队列路径继续沿用已验证的原生停止／取消测试。
- 安全审查：源路径只存在原生内存，文件选择不打开新写入接口，队列启动异常返回失败；未使用临时明文文件或回收站。
- 限制：本片未自动操纵真实 Windows 选择对话框，视觉与实卡交互留第六片；文件内进度和目录递归仍待继续。
- 提交：依持续授权自主提交，不推送，保留其他工作区记录。

### 2026-09-21 — FAT32 第四片 4e：文件内进度贯通
- 范围与审计：检查上传 helper 到原生队列和页面的链路；保留现有四数字队列状态契约，单独增加有界文件百分比查询。
- 检查与实现：认证 helper 的复制进度经应用／UI 控制器送至原生原子计数，页面将当前文件进度计入队列进度并显示百分比；源路径和文件名不跨桥。
- 完成语义：复制阶段封顶 99%，只在刷新及完整映射核验成功返回后增加完成数；新文件开始清零进度，异常仍停止整队列。
- 验证：真实 USB Vault.exe /W4 /WX 编译通过；会话、UI 控制器、桥接、拖放四组 CTest 4/4，覆盖回调透传、在途 42% 未计完成、越界进度钳制、后台查询放行。
- 页面验证：245 项 DOM 通过，3 文件队列完成 2 个、当前 50% 时显示 83% 总进度；失败恢复与取消既有断言保持通过。
- 构建说明：首次应用构建因未配置格式 DLL 被部署目标拒绝；配置已实测候选 DLL 后通过，未修改正式 DLL 白名单或打包交付，未声称应用启动／实卡验收完成。
- 安全审查：回调不持久化源信息、不更改认证门，百分比是显示值而非写入许可；源／目标句柄仍由传输层掌握，完成仍由存储结果决定。
- 后续：目录递归与新目录元数据安全边界尚待下一子片，第四片不提前标记完成；之后接第五片空间展示。
- 提交：依持续授权自主提交，不推送；其他任务修改保留。


### 2026-09-21 — FAT32 第四片 4f：有界目录上传与句柄回滚
- 范围与审计：核对六片计划及 R-03／8.6，补齐原拖放支持的目录；不扩展为任意管理员文件复制。
- 检查与实现：先按调用用户权限枚举并锁定源，限制 1024 条目、32 层、合法目标路径及 FAT32 单文件大小；超限在创建目标前拒绝。
- 写入与回滚：新目录相对已持有父目录原子创建，子文件复用正式分配／写入引擎；保留本次文件句柄，失败逆序清理本次目录，不按名称重开删除。
- 进度：按目录全部文件字节汇总并即时发送百分比；成功前刷新卷并复核挂载身份，任一错误仍停止队列并要求恢复维护。
- 验证：helper、协议与实机夹具构建通过，相关五组 CTest 5/5；普通客户端完成嵌套目录、空目录、零字节、同名拒绝及 1025 条目拒绝。
- 实机验证：首个子文件完成后取消后续复制，本次目录清理且重挂未复现；正常文件和隐藏区摘要通过，输出见 out/import-tree-final-active.txt。
- 验证修复：首轮目录进度未及时发送导致取消未触发，修正后两次完整运行通过；未将首次失败记为有效验收。
- 安全与风险：拒绝重解析点，源与目标句柄保持所有权，回滚不覆盖原异常；删除失败仍可能留残余，受保护写入／掉电不能保证持久回滚，完整应用实卡验收留第六片。
- 提交：依用户持续授权自主提交，不推送；第四片代码接入完成，保留其他任务未提交记录。


### 2026-09-21 — FAT32 第五片：安全空间显示与失效刷新
- 范围与审计：核对六片计划、既有认证度量与页面三数字契约；第三值改为真实安全空闲量，三个数统一为字节，不再使用尾部空闲量。
- 检查与实现：原生控制器与宿主保留 64 位精度；页面展示向下取整的人类可读容量及精确字节，保留文件系统已用／总量。
- 显示口径：明确已排除保护区域，同时说明目录和文件系统元数据占用，不把空闲簇快照承诺为任意文件负载额度。
- 失效规则：新测量、上传期间先移除旧值；测量失败、非法或超安全整数数据不展示数字，上传失败显示需重新进入维护，保护触发同时清除精确详情。
- 生命周期：沿用身份／卡纪元检查及上传后、文件刷新后的重新测量；恢复和保护锁定优先于测量结果，不因刷新解除。
- 验证：正式应用 /W4 /WX 构建通过，控制器、异步桥、页面契约 3/3；DOM 251 项通过，覆盖 513 字节、1536 字节、零值、在途失效、失败失效及超精度拒绝。
- 验证修复：页面契约发现原集成夹具漏掉第四片选择文件按钮，补齐后通过；未执行完整应用实卡视觉验收。
- 安全与风险：数字只留当前原生／页面内存，未增加文件、日志或注册表记录；空间查询与文件系统事实不是原子快照，不一致保守隐藏，写入仍由正式引擎与驱动核验。
- 提交：依用户持续授权自主提交，不推送；第五片接入完成，完整验收及正式依赖交付留第六片。


### 2026-09-21 — FAT32 第六片 6a：几何 DLL 正式依赖与交付清单
- 范围与审计：检查候选 DLL 来源、固定上游和许可证附件；原打包脚本仍锁旧摘要且漏带几何补丁，不能支持已接入的认证度量。
- 源码核对：985 个上游源码文件与 b48e31f5 固定基线一致；重新应用仓库两份补丁后 SDK 两文件及几何 include 与候选完全一致。
- 检查与实现：正式白名单更新为已验收 DLL 416C9DAB…6363F184；交付携带两份补丁、几何 include、README 及原 License／NOTICE，固定版本和签名检查不放宽。
- 验证：只读几何集成重跑通过真实边界、错误／损坏／交换头、嵌入 NUL、长度边界、失败清零及失败后复用；首次因夹具名称拼写错误未运行，改正路径后通过。
- 构建与打包：package-release 静态运行时正式应用与安装 helper 构建通过；实际暂存载荷生成成功，供应商／格式 DLL 摘要、前置安装器签名和页面资源校验通过。
- 工具：恢复固定 NSIS 3.12，makensis 摘要 B043E554…98EFF 与历史交付一致；未替换已安装应用，暂存产物不是最终安装交付。
- 安全与风险：密钥／几何仍限定 helper，依赖存于 out/local-dependencies 并保存摘要，完整源构建目录保留；完整应用实卡、异常恢复及综合回归尚在后续，不提前标记阶段验收完成。
- 提交：依持续授权自主提交，不推送；其他任务未提交记录保持独立。


### 2026-09-21 — FAT32 第六片 6b：综合回归与 Release 上传边界
- 范围与审计：执行全量构建及不触碰实卡／既有容器的 63 项回归；四项 HID／既有容器集成明确排除，不称完整硬件验收。
- 检查与实现：实机夹具新增稀疏 4 GiB 源文件的单文件／整树拒绝、33 层目录拒绝，以及正常嵌套树重挂后的逐文件内容、空目录和零字节验证。
- 验证：63 项 CTest 中 62 通过，唯一失败为既有 vikey_helper_process_tests 的 development vendor runtime 暂存目录最终路径重定向拒绝；未放宽生产路径校验，未标记已解决。
- Release 验证：普通客户端使用 package-release 正式 helper 在新一次性容器完成全部上传用例，新增超限均在创建目标前拒绝；取消树重挂未复现，成功树完整持久化，隐藏区摘要不变。
- 证据：综合回归见 out/fat32-stage6-ctest.txt；Release 实机结果由 out/release-import-active.txt 指向；前片 251 项 DOM 和容量桥接验证仍有效。
- 安全与风险：仅新建固定测试密码容器及源文件，未认证／写入用户安全卡；不将正常取消恢复推广为保护触发、进程死亡或掉电恢复保证。
- 剩余：实卡 UI、真实文件选择对话框、安装后首次启动及完整故障注入未验收；既有 helper 路径问题保留。下一步生成明确标注限制的测试交付，不自动安装。
- 提交：依持续授权自主提交，不推送，其他任务修改保留。


### 2026-09-21 — FAT32 第六片 6c：测试安装交付与当前口径
- 范围与审计：同步 8.5 和阶段总览，区分旧尾部空闲反例、当前认证安全空间与尚未完成的整体验收，未把剩余问题改写为通过。
- 交付：out/installers/installer-20260921-120245/USB-Vault-Setup.exe，300368711 字节；同根同名 ZIP、测试须知、载荷身份／清单及 SHA256.txt 齐全。
- 验证：安装器摘要 5BCA50F77A0A53C9864AB27DA7F13DA172A0572E297160C103A23D904CDB2DFA 与构建记录一致；目录清单、ZIP 条目及每文件摘要逐项通过，ZIP 摘要 C71453EAA017113BD2F09326DD9ADC51BDC8A7F2236C7514710D8A3A8450407D。
- 构建说明：载荷身份 f3591f6 + dirty，未提交差异仅为其他任务文档；NSIS 保留两个既有未使用变量告警。此交付不自动安装，未改动现有应用或安全卡。
- 安全与风险：说明明确 62/63 回归、既有 helper 路径拒绝及实卡 UI／首次启动未验收；本包仅作测试交付，非完整阶段验收。掉电、进程死亡及保护触发后的残留保证仍未闭合。
- 提交：依持续授权自主提交文档，不推送；六片计划前五片代码接入完成，第六片有已验证测试包但保留上述缺口。


### 2026-09-21 — 最新源码启动：修正应用内格式 DLL 白名单
- 范围与审计：用户要求直接启动最新代码；发现第六片更新打包摘要时漏改 generate-p2-deployment.cmake，应用内仍固定旧格式 DLL 摘要。
- 检查与实现：启动清单固定值同步为已审计的 416C9DAB…6363F184，保留逐文件摘要校验；构建目录补入相同已验证供应商 DLL，无凭据复制。
- 验证：Release 主程序重建通过，生成清单中六个运行文件摘要逐项一致；直接启动 out/build/package-release/USB Vault.exe，窗口有响应且页面导航／就绪完成。
- 当前故障：启动日志显示首次安全卡探测 2005、原因码 1，未声称成功登录；本片只解决启动白名单遗漏，卡探测故障仍待诊断。
- 交付纠正：installer-20260921-120245 的外部清单／ZIP 校验通过不代表应用启动可用，其内置旧摘要会拒绝新 DLL；该包已知不可用于本轮功能验收，尚未重打包。本次启动的是修正后的构建目录程序。
- 安全与风险：未绕过任何安全校验，未安装覆盖旧应用；现有卡仅接受用户请求启动时的正常探测，未执行登录、重置或容器写入。
- 提交：依持续授权自主提交修复，不推送；保留其他任务文档修改。


### 2026-09-21 — 撤回旧测试包、重打及 2005 环境定位
- 范围与审计：用户明确要求删除上一测试包并重打、排查 2005；核实绝对路径及无重解析点后，仅删除 installer-20260921-120245 目录和同名 ZIP。
- 重打：以 87c751e 的启动摘要修复生成 installer-20260921-121828；EXE 300368726 字节，SHA-256 7C7BDCA68816ABEF584B78C28BFB7E6E3F22CDE5A80B17520D4B64891BE7BB8E。
- 验证：应用内六项制品摘要与载荷逐项匹配，EXE 与当前 Release 构建一致，目录清单及 ZIP 每项摘要通过；ZIP SHA-256 7A9E1F8A4C4767EFB45A629BA19B33908D6AFB24E6D9125C73D78BE27BDB2B1C。
- 2005 定位：无实卡测试临时记录预期／实际 SDK 暂存目录，确认 Codex 直接启动环境将普通 LocalAppData 映射到其包 LocalCache，触发严格最终路径比较；父目录本身无重解析点。
- 对照与取舍：官方目录查询 flag 试验无效，已撤回；同一 EXE 通过已有 Explorer 桌面进程启动，日志首次卡探测及页面就绪完成，无 2005。没有修改生产路径／摘要安全规则。
- 回归：恢复无插桩二进制后，普通桌面环境的 vikey_helper_process_tests 与 vikey_artifact_tests 2/2 通过；原 62/63 运行保留为原环境历史结果，不伪称同一轮全过。
- 安全与风险：未输入密码、未执行卡重置或容器写入；新包未自动安装，完整登录／维护实卡与安装完成页启动仍待验收。目录虚拟化环境下直接启动仍可被拒绝；其他历史首次启动失败不以此结论概括。
- 提交：依持续授权自主提交诊断与交付记录，不推送；其他任务未提交文档保持独立。


### 2026-09-21 — 修复扫描后打开外层卷误报会话不可用
- 范围与审计：对照 FAT32 第六片、维护流程及 R-03／8.6；用户实卡页面点击打开外层卷后进入保守卸载页，本片仅修复原生桥工作模式转换。
- 根因：完成扫描后 survey_mode_ 保留；开始挂载没有退役它，工作线程运行时 getVolumeOperationStatus 被扫描查询白名单拒绝，页面提前刷新再遭 E_PENDING。
- 检查与实现：检查 JS 打开／刷新、控制器状态投影及原生桥门禁；新卷操作通过认证／中断检查并 join 旧线程后清除桥扫描快照及模式，业务会话中的维护凭据不变。
- 回归：原生桥测试改走先扫描再打开的真实顺序，暂停挂载，核验进度可读、会话读仍拒绝、完成后状态可刷新；修复前确定失败，修复后通过。
- 验证：会话核心、UI 控制器、原生桥 CTest 3/3 通过；251 项页面 DOM 导航检查通过；git diff --check 通过。
- 安全与风险：未放宽工作线程互斥、卡状态检查或隐藏卷保护；未新增日志敏感数据；没有操作用户当前挂载卷或重启应用。
- 验收限制：未在用户当前实卡会话重试；当前运行 EXE 及 installer-20260921-121828 仍不含本修复，须后续重新构建与交付。
- 提交：依用户此前持续授权自主提交，不推送；其他任务的日志、阶段计划与登录重置文档修改保留。


### 2026-09-21 — 诱饵验证零结果留在当前页（待合并提交）
- 范围与审计：用户要求 UI 小改先不提交；沿用维护扫描及 R-03／8.6 的中性结果，不区分密码错误与不存在可更新容器。
- 检查与实现：检查扫描完成分支、已有弹窗样式及导航夹具；零结果回到输入页，弹窗仅提示“无可更新容器”，不进入结果二级页。
- 交互：确定／Escape／背景点击关闭，焦点回到密码框；Tab 保持弹窗按钮焦点，会话重置时隐藏弹窗，旧轮次不重新弹出。
- 验证：JavaScript 语法检查及现有 251 项 DOM 导航检查通过，包含零结果和未能验证但零匹配两种情形；差异空白检查通过。
- 安全与风险：密码仍在原流程及时清空，不增加凭据或容器状态持久化，不修改挂载／保护逻辑；未重新构建当前运行应用，未做原生 WebView 视觉验收。
- 提交：按本轮明确要求保持未提交，等待后续 UI 改动一起提交；其他任务文档修改保留。


### 2026-09-21 — 上传期间保护查询与切换维护复查（未提交）
- 范围与审计：用户报告上传期间保护未知提示、切换容器后上传要求重新维护；检查 JS 轮询、上传事件、桥工作线程门禁与导入会话生命周期。
- 检查与实现：上传原子进度先于页面通知可见；轮询先检查上传状态，查询失败后再次检查竞态，已确认上传时暂缓保护查询并接管进度；新上传使旧查询失效，不把繁忙判断为安全可写。
- 验证：252 项 DOM 检查通过，新增无窗口通知时后台上传场景；会话核心测试通过，覆盖上传、切换后关闭旧导入会话、再次上传的目标与凭据核验。
- 安全与风险：真实保护触发仍保持锁定，无法确认且非已知上传仍警告；未放宽原生上传身份核验或失败后的维护恢复限制。
- 剩余：核心夹具未复现切换失败，不能替代用户实卡及具体文件验收；用户记不清同名文件／文件系统，要求重启后重试，该问题仍待定位。
- 运行：Release 构建并通过普通 Explorer 桌面启动供重试，未打安装包。
- 提交：遵照用户要求全部保留未提交，其他任务文档不变。


### 2026-09-21 — 受控上传同名自动编号（未提交）
- 范围与审计：用户复测推测切换未成功而上传同名文件，要求添加后缀；确认 CREATE_NEW／FILE_CREATE 的重名拒绝会进入统一维护恢复状态，未断言原切换失败原因。
- 检查与实现：正式 helper 在目标原子创建时，仅对 Windows 名称冲突递增“ (1)”至“ (9999)”；文件保留扩展名，目录只重命名本次导入根目录，不合并已有树。
- 边界：候选名称限 255 UTF-16 单元，必要时缩短主名并避免截断代理对；重试有界，每次仍检查取消及挂载身份，其他错误不重试。
- 验证：正式导入 helper 构建通过；会话核心及新增命名测试 2/2 通过，后续补测实际目录冲突及原文件内容长度保留通过；覆盖大小写冲突、连续编号、无扩展名、点文件、长名和访问拒绝。
- 安全与风险：不使用先判断存在再覆盖，继续以原子创建获得所有权；文件句柄与目录句柄回滚不变，保护触发及真实写入失败仍要求重新维护。
- 验收限制：尚未以新 helper 在完整加密卷内实测重复上传及重名目录整树；当前运行应用尚未替换。
- 提交：依当前用户要求保持未提交，待后续统一提交；不打包，不操作现有挂载卷。


### 2026-09-21 — 维护二级页返回容器管理（未提交）
- 范围与审计：用户反馈维护二级页无法点开容器管理；确认既有共享文件管理区域将该导航强制路由回诱饵页。
- 检查与实现：侧栏前往容器管理先等待普通结束维护，成功后恢复容器列表与共享区域；失败仍留在维护页提示，不进行强制卸载。
- 生命周期：等待期间禁止重复操作，卡会话变化或状态异常不继续跳转，不在后台工作尚未结束时切走共享文件管理区域。
- 验证：253 项 DOM 导航检查通过，覆盖卸载失败与成功导航、布局恢复及重新打开维护；Release 应用构建通过并通过 Explorer 启动。
- 安全与风险：沿用普通卸载和会话检查，未放宽保护规则；用户实际挂载环境中的导航仍待复测，切换会结束当前维护而非保留挂载。
- 提交：按用户要求未提交、未打包；保留之前改动及其他任务文档。


### 2026-09-21 — 维护与容器管理独立导航（取代上一片，未提交）
- 范围与审计：用户明确维护中的容器暂不可操作，其他容器正常使用；撤销上一片导航自动结束维护的行为，保留当前挂载。
- 检查与实现：将维护容器 ID／目录与普通浏览上下文分离；离开诱饵页保存维护目录，容器管理展示完整列表，返回时恢复维护上下文而不重新挂载。
- 操作限制：维护容器行显示“正在更新诱饵内容，暂不可操作”，禁用浏览、行操作和排序；批量选择／重试排除它，备份选择不接纳它，其他容器保持普通操作。
- 异步边界：容量／保护查询绑定独立维护 ID，普通浏览不更改它；离开维护页撤销拖放，迟到授权也撤销，旧目录查询由版本与容器身份检查丢弃。
- 验证：最终 256 项 DOM 导航检查通过，覆盖不卸载页面往返、普通容器浏览、维护行不可浏览、批量只排除维护目标；首次回归发现文件行焦点变量作用域误用，纠正后通过。Release 构建通过并重新启动。
- 安全与风险：保留全局后台操作互斥与上传保护，界面可以切换不代表同时执行磁盘写操作；尚未完成真实卡、多容器全部操作的综合验收。
- 提交：全部保持未提交，不打包；上一片自动结束维护为已撤销的中间行为，其他任务文档保持不变。


### 2026-09-21 — 维护与上传故障修复合并提交
- 范围与审计：用户要求故障修复归入一个 fix 提交，UI 改动保留；本提交不含零结果弹窗、新命名或返回交互，不含另一任务登录重置文档。
- 修复：上传期间根据原子进度暂缓保护查询并丢弃旧结果；维护身份与普通浏览上下文分离，仅锁定维护容器，批量及导出入口同步过滤，离页撤销拖放授权。
- 上传：文件及目录根目标原子创建遇同名时自动编号，保留扩展名，限定长度及重试次数；其他失败不重试，已有目标不覆盖，原有句柄清理与保护限制不变。
- 验证：从候选提交单独生成页面夹具，排除 UI 改动后 256 项 DOM 检查通过；此前核心会话及命名测试 2/2、真实文件／目录冲突补测通过，正式 helper 和 Release 应用构建通过。
- 安全与风险：工作线程互斥、挂载身份、取消与保护失败限制保留；实卡多容器综合验收及完整加密卷重复上传未完成，不宣称已完成。
- 提交：按本轮明确授权提交故障修复，不推送；其余 UI 与独立任务修改继续未提交。


### 2026-09-21 — 高级更新文案与返回列表（UI 未提交）
- 范围与审计：用户指定故障修复单独提交、UI 暂不提交；故障修复最终哈希 a11901b，前述故障条目的“未提交”为历史状态，零结果弹窗继续留在工作区。
- 文案：侧栏、标题与确认框统一“高级更新”；一级列表为“可进行高级更新的容器”，移除指定两处说明，行按钮为“更新”；二级页移除单容器挂载说明，结束按钮改“卸载”。
- 交互：二级标题采用容器管理同款 file-crumb 按钮，点击回一级列表但不结束维护；列表显示状态与维护身份分离，维护目标锁定和保护轮询继续有效。
- 状态：容器管理中的维护提示改为“正在进行高级更新，请卸载后重新挂载”；保护提示中的卸载按钮引用同步更新。
- 验证：257 项 DOM 导航检查通过，包含标题返回列表不卸载及原有零结果弹窗；git diff --check 通过。旧按钮文字断言已同步，无原生 WebView 视觉验收。
- 安全与风险：不修改密码、上传身份与强制卸载规则，返回列表不产生卷操作；尚未重新构建或替换正在运行的应用。
- 提交：本 UI 切片与先前零结果弹窗均未暂存／提交，不推送；另一任务的阶段计划和登录重置文档保持独立。


### 2026-09-21 — 高级更新三块布局与验证页（UI 未提交）
- 范围与审计：按用户设计图调整上半部分，底部文件列表不变；图中示例容量与可直接复制文案不是产品数据或写入规则，不照搬。
- 导航与验证：高级更新移到导出与迁移下方；移除验证说明，验证区域按视口居中，输入宽度由 26rem 收至 23rem；进度只显示“正在验证...”。
- 布局：左侧虚线拖放卡、折角文件 SVG 和带文件夹 SVG 的选择按钮；右侧上方显示外层用量／总量、用量条和剩余可写文字，下方显示新鲜度；窄窗口纵向排列。
- 数据：新增用量主读数与总量，仍用原始空间事实；测量、上传、保护或失败时清除过期值，安全空闲与文件负载不等价的说明保留。
- 验证：257 项 DOM 导航检查通过，更新用量展示断言；差异检查与 Release 构建通过。模拟浏览器预览因工具 sandbox ACL 初始化失败，未完成截图比对；临时本地预览服务已关闭。
- 安全与风险：上传仍走既有受控 helper，不增加资源管理器直接写入引导，不修改保护及回滚；原生窗口实际布局待用户查看。
- 运行与提交：重新通过 Explorer 启动最新应用；UI 改动继续未提交，不打安装包。


### 2026-09-21 — 高级更新 UI 汇总提交
- 范围：按用户要求以单个 UI 提交收束零结果弹窗、高级更新命名／返回列表、导航顺序、验证页居中及二级三块布局；不含另一任务文档。
- 实现：零结果停留输入页；标题可回列表且保留维护；左侧拖放／选择、右侧用量与新鲜度，底部文件列表不变。
- 验证：257 项 DOM 检查通过、Release 构建及页面启动完成；容量异常移除旧值，保护和会话限制保持。浏览器预览工具 ACL 初始化失败，未完成截图比对。
- 风险：实卡完整拖放与多容器验收仍未闭合；本轮用户新报的拖放禁止及新鲜度呈现调整留待下一片，不混入本提交。
- 提交：用户本轮授权单个 UI commit，不推送；其他任务日志／计划保持未提交。




### 2026-09-21 — 高级更新展示与交互汇总提交
- 范围与审计：按用户授权拆分 UI／fix；本片收束内容新鲜度、顶部区域稳定、按钮与图标调整，原生接入及会话修复另提。
- 布局：固定容量卡和拖放卡高度，动态提示占独立区域，避免读取／上传挤动标题；深色下拉选项，底部文件列表不变。
- 展示：内容新鲜度左对齐并按日前／月前／年前着色，今天不显示时刻；移除扫描数量和成功导入数量，替换导出图标。
- 交互：框内双击／键盘选择文件、按钮单击，悬浮与拖入高亮分离；容器管理二级页隐藏导入／新建，一级保留。
- 容量：读取／上传保留当前挂载上次成功读数，校验完成后整体替换；切换、失败或保护不沿用可写数字。
- 验证：独立 UI 候选 267 项 DOM 检查通过，组合版本 268 项通过且 Release 构建成功；未完成真实窗口截图／跨 DPI 验收。
- 安全与风险：显示保留不改变原生写入授权，错误及停止操作保留；原生拖放接入在后续 fix 中完成。
- 提交：用户明确授权单独 UI commit，不推送；其他任务计划与日志不纳入。

### 2026-09-21 — 紧凑高级更新、内容新鲜度及原生拖放区域（未提交）
- 范围与审计：前一轮 UI 已按授权提交 4b80e2a；本片处理用户新增五项反馈，检查 WebView 拖放禁用、原生 IDropTarget 和维护目标授权链。
- UI：内容新鲜度去掉图标与扫描数量说明，左对齐；缩短三块区域高度，容器选择采用深色背景；区域可点击／键盘触发选择文件。
- 新鲜度：显示上次修改时间（今天）或日前／月前／年前；30 天内绿、30–179 天黄、180 天起红，未知与空目录不着色，按现有受限根目录信息计算。
- 拖放根因：旧代码误认为 AllowExternalDrop(FALSE) 会把拖放交给父窗口，实际 WebView 子窗口拒绝并遮挡父接收区域；保留 WebView 禁止外部拖放，增加原生分层子窗口覆盖授权区域。
- 接入：页面仅提供有界屏幕矩形，窗口只在已授权、空闲且卡未中断时显示；仍由 CF_HDROP 提取已有磁盘路径，原生接收器执行上传，页面不接触源路径或明文。
- 生命周期：离页／撤销立即隐藏，上传启动隐藏以保留停止按钮，关闭释放 OLE 注册；越界矩形拒绝，忙碌期间仍允许隐藏区域，不读取可变会话状态。
- 验证：原生拖放与桥测试 2/2，通过命中、边界、撤销、释放及非法参数；初次窗口测试因缺 Windows 兼容清单失败，采用产品同清单后通过；261 项 DOM 检查及 Release 构建通过。
- 安全与风险：不接受虚拟文件、不使用网页路径上传、不放宽挂载身份／保护／取消规则；真实 Explorer 拖放、DPI／缩放／滚动综合交互尚待实机复测，不把分层测试等同完整验收。
- 运行与提交：已通过 Explorer 拉起最新应用供用户复测，本片保持未提交、不打包，其他任务文档保留。


### 2026-09-21 — 拖放框悬浮／双击与导出图标（未提交）
- 范围与审计：按用户要求今日新鲜度只显示“上次修改：今天”，成功上传不再显示项目数量，失败／停止提示保留。
- 交互：选择文件按钮单击、其余框内双击打开选择；保留键盘触发。原生覆盖层通过独立按钮矩形判断，避免页面监听被覆盖层截断。
- 视觉：框内空白悬浮和有效拖入亮起整框，按钮悬浮仅亮按钮，离开或撤销清除；原生仅回传 0–3 状态，不传源路径。
- 图标：文件行和导出导航改为方框向右上伸出的箭头，移除旧文件导出图标的 -90 度旋转，消除置顶观感。
- 验证：265 项 DOM 检查通过，原生测试覆盖按钮／框／拖入／离开高亮，原生桥回归通过；Release 构建及差异检查通过。
- 安全与风险：拖入仍由原生 CF_HDROP 接收器执行身份／互斥检查，按钮坐标有界；真实鼠标视觉、跨 DPI 拖放仍待用户实测，不声称已完成完整原生交互验收。
- 运行与提交：通过 Explorer 拉起新版本；本片及上一片新增调整均未提交，不打包。


### 2026-09-21 — 高级更新读取与导入期间布局稳定（未提交）
- 范围与审计：检查当前高级更新布局、容量刷新／上传状态渲染及相关需求；仅调整展示布局，不改空间口径、R-03 或 8.6 边界。
- 检查与实现：拖放卡采用固定高度，主内容和反馈区分离；提示文本预留行高，进度／停止和错误提示使用独立底部空间，避免挤动图标、标题与选择按钮。
- 容量：右侧两卡使用固定行高；用量、剩余可写与详细说明预留空间，读取占位与实际数值切换不会改变卡片高度。
- 适配：窄窗口保留纵向排列；超长状态在对应区域滚动读取，不通过隐藏错误换取布局稳定，底部文件列表保持不变。
- 验证：现有 265 项 DOM 交互回归通过，integration 应用目标构建成功，差异空白检查通过。
- 安全与风险：未修改原生导入、挂载、取消及保护链，既有 DOM 标识和按钮交互保持；DOM 测试不测量真实像素，实际 WebView 视觉／缩放仍待查看。
- 运行与提交：当前应用仍在运行，本轮未中断它；已验证独立目录构建，未打包、未提交，保留其他任务改动。


### 2026-09-21 — 管理二级页与维护事实刷新（未提交）
- 范围与审计：按用户四项反馈先收束页面修改；检查管理页渲染、容量刷新、上传状态和卸载后原生事实刷新。
- 实现：容器管理二级页隐藏导入／新建入口，一级列表保留；容量读取和上传期间保留当前挂载上次成功快照，结果完整校验后同步替换。
- 异常边界：首次进入与换卷显示空占位，读取失败清除旧数字；保护／上传失败继续显示阻止或恢复提示，不将历史读数用作写入授权。
- 列表：原生状态刷新完成后重新渲染高级更新列表，卸载结果同步更新挂载状态与盘符。
- 验证：268 项 DOM 检查通过，新增待测量／上传保留读数、二级操作入口、卸载后行状态断言；初次测试因旧模拟未真正卸载掩盖问题，完善模拟并隔离后续场景后通过。
- 安全与风险：仅展示层保留容量，原生每次写入的实时容量及保护验证不变；真实窗口视觉仍需复测。
- 提交：保持未提交，不打包、不推送，其他任务文档保留。

### 2026-09-21 — 单容器维护合并 UAC 授权（未提交）
- 范围与审计：检查 N-08 模式 C 提权要求、runtime、测量／上传客户端及 helper 会话；原行为为两个独立 runas 进程，导致打开和首次上传各申请一次。
- 实现：首个维护测量使用明确的新协议操作建立测量＋导入会话，同一挂载后续测量和导入复用管道；主程序仍保持普通权限。
- 协议：版本升为 5，原只读测量会话仍拒绝上传、创建和新维护操作，不把既有只读授权升级；独立上传入口保留原限制。
- 安全：客户端和 helper 均绑定路径、盘符、挂载 ID 和长度；每次操作仍独立认证卷头并核对实时挂载，源／目标继续以调用者身份打开，凭据逐请求清零。
- 生命周期：卸载、切换或结束认证关闭共享进程；helper 空闲超时仍保留。上传失败维持恢复锁定，单纯测量失败关闭通道，后续重试需重新申请授权。
- 验证：协议及挂载会话测试 2/2 通过，覆盖新操作头、缺失凭据、旧版本、跨路径／盘符／挂载身份、无关操作和只读通道拒绝升级；268 项 DOM、Release 构建及差异检查通过。
- 检查修复：首轮构建发现新增管道字符串转义错误，修复后构建通过；安全复核补齐单次执行分派，防止新测量操作落入创建分支。
- 限制：本轮未触碰实卡或容器数据，真实 UAC 次数及实际上传链需在新应用中验收；切换容器或授权失效后仍会重新提示，不承诺整次登录永久免 UAC。
- 运行与提交：构建最新 Release 供 Explorer 启动；保留所有本轮改动未提交，不打包、不推送。


### 2026-09-21 — 原生拖放与维护会话修复提交
- 范围与审计：用户授权 UI／fix 分开提交；UI 已为 af6d365，本片提交原生拖放、卸载列表刷新及受限 UAC 复用，保留前述逐片审计原文。
- 实现：受控原生窗口接收 CF_HDROP，页面同步授权区域坐标；卸载后重绘高级更新列表，清除旧挂载状态和盘符。
- 授权：协议 v5 为同一维护挂载复用测量和上传进程，原只读通道不接受写入或升级；关闭、换卷和失效沿用现有清理规则。
- 验证：组合版本 268 项 DOM、原生拖放及桥测试、协议和会话回归通过，Release 构建和页面启动成功；UI 候选另有 267 项独立检查。
- 安全与风险：路径、盘符、挂载 ID、长度及实时卷身份校验保留；真实 UAC 次数和跨 DPI 拖放仍待实机复测，不把模拟验证视为实机验收。
- 历史状态：上方本轮逐片记录中的“未提交”描述当时状态，本条收束为用户批准提交；不改写历史验证失败或验收限制。
- 提交：只暂存已审查的明确文件清单及本轮日志，不包含其他任务计划；不打包、不推送。

### 2026-09-21 — P3.26 第一片：需求与单次确认契约
- 范围与审计：用户将功能定为 P3.26，明确授权验证审查后逐片自主提交直至全套审查，不推送；同步 N-06／Q-02 的特定破坏性重置增补与六片计划。
- 检查与实现：审阅入口、设置页重置、原生桥及只读卡上下文；新增易失一次性确认票据，取消、重新准备和消费均使旧票据失效；零票据与重放拒绝，序号耗尽不回绕。
- 交互取舍：挂载存在不阻断；提示盘符与红色强制卸载／普通跳过，采用用户确认的末句；仅显式点击才卸载固定目标，查询失败可跳过，不猜测卡与卷的归属。
- 验证：独立 out/build/p326 Release 构建及 card_reset_confirmation_tests 通过，覆盖取消、替换、成功后重放及错误票据失效。原 dev 目录已不存在；Debug 曾因 PDB 写入失败，未算通过。
- 安全与风险：本片无写卡接口、无敏感信息落盘，不降低 SDK 或设置页权限；插卡及路由失效将在第三片接入控制器，不能把独立状态测试称为闭环完成。
- 提交：按用户授权提交本片；仅暂存本功能记录，不纳入此前未提交的其他开发日志；不推送。

### 2026-09-21 — P3.26 第二片：卡操作跨进程互斥
- 范围与审计：复核初始化、登录、改密、设置页重置与 helper 调用链；加入事务级及嵌套调用级同一互斥，不依赖卷挂载状态或其他实例是否已登录。
- 检查与实现：Windows 固定中性 Global mutex，零等待取得；同线程递归允许，其他线程／进程竞争拒绝，弃置互斥保守失败；事务退出后 RAII 释放，未把互斥绑定整个认证会话。
- 验证：独立 Release 构建通过，线程／子进程竞争、嵌套及释放后重入测试通过；入口控制器与 ViKey 初始化目标两组回归通过。首次测试命令正则引号错误已修正，最终命令结果有效。
- 安全与风险：内核名称不含卡身份或秘密，默认访问权限拒绝越权取得；命名对象被占用可导致拒绝服务，不放宽访问控制。旧版本和外部 SDK 客户端不遵守本互斥，第三片仍需同卡与记录复核。
- 提交：按用户授权提交本片，不推送；登录前写入与页面仍未开放。

### 2026-09-21 — P3.26 / 切片 3a：只读快照与条件重置
- 范围与审计：按登录重置计划实现 216 字节快照校验，写入仅限既有 736 字节应用日志。
- 检查与实现：检查日志双槽恢复、helper 协议及硬件 ID 校验；协议升至 13，新增独立快照和条件重置操作。
- 检查与实现：确认前读取不归一化；确认后比对原记录，提交初始化标记并读回完成旧槽清理。
- 验证：Release helper/client 与两组测试编译成功；journal 和 protocol 测试 2/2 通过。
- 验证：覆盖旧快照、重放、只读零写入及四个写入阶段的部分/完整写入异常。
- 安全与风险：失败不伪报成功，仍需一次性上下文接入；断电实卡验证未执行。
- 提交：按用户授权自动提交本切片；禁止推送。

### 2026-09-21 — P3.26 / 切片 3b：同卡绑定与一次性执行
- 范围与审计：复核登录上下文、卡适配器与准备阶段零写要求；修正文档旧版“不卸载”措辞与已批准显式卸载流程的冲突。
- 检查与实现：原生保存同卡目标与锁页快照；准备、取消、确认不暴露硬件 ID 或记录。
- 检查与实现：取消、插拔和其他卡动作销毁确认；执行前一次性消费并退休上下文，helper 在同一 SDK 会话校验硬件与原记录。
- 验证：Release 控制器及适配器编译通过；确认、上下文、初始化适配器 3/3 测试通过。
- 验证：新增无卡、取消、拔卡、重新插卡、替换确认、记录变化、错误票据及重放用例。
- 安全与风险：并发租约覆盖执行；失败不能恢复旧确认；实卡替换/断电和最终 UI 生命周期仍待验。
- 提交：按用户授权自动提交；尚不开放页面入口，禁止推送。

### 2026-09-21 — P3.26 / 切片 4：受限异步桥与挂载选择
- 范围与审计：检查 WebView 后台互斥、卡代次、配置切换、关闭路径和驱动挂载身份。
- 检查与实现：新增准备/状态/取消/确认最小桥接口；票据与盘符之外的身份、路径和记录不交给页面。
- 检查与实现：SDK 操作在独占后台线程运行；参数类型、路由、票据、插卡代次均由原生核验，过期完成不关闭新会话。
- 检查与实现：挂载查询失败允许跳过；显式卸载只用准备时目标，重新核验挂载身份，失败保留跳过入口。
- 检查与实现：可用性不继承设置页挂载限制；重置成功关闭不隐式卸载保留的卷，窗口销毁及配置切换清除确认。
- 验证：Release 主程序编译通过；UI 控制器、桥接和关闭回归 3/3 通过，包含参数错误、直接确认、过期、重放和跳过零卸载。
- 安全与风险：驱动卸载 API 按盘符执行；已在调用前核验挂载唯一身份，但外部软件在最后核验与 IOCTL 间换挂载的极短竞争仍受驱动 API 限制，不能声称原子身份卸载。
- 提交：按用户授权自动提交；实卡及真实驱动异常仍待验，禁止推送。

### 2026-09-21 — P3.26 / 切片 5：登录页忘记密码流程
- 范围与审计：检查登录渲染、卡变化通知与设置页重置；新增独立流程，不复用已认证重置桥。
- 检查与实现：默认不勾选破坏性确认，清空密码输入；后台期间禁用重复提交和关闭，取消/卡变化清空确认。
- 检查与实现：挂载弹窗保留用户指定完整警告、红色强制卸载和普通跳过；查询未知单独说明，禁止未知目标卸载。
- 检查与实现：失败不自动重试；写入结果未知明确要求重新插卡确认真实状态，成功等待原生关闭。
- 验证：新增 8 种 DOM 场景通过；启动 12 场景及既有整页导航 268 项通过，无实际卡或 SDK 操作。
- 验证：隔离 Edge 渲染确认页和挂载页，1000×760 画面无溢出、按钮可辨；仅测试实例绕过 CSP 注入模拟桥，生产 CSP 未改。
- 安全与风险：页面只保留易失票据和盘符，不持久化状态；真实 WebView/卡操作验收在最终切片继续记录。
- 提交：按用户授权自动提交；禁止推送。

### 2026-09-21 — P3.26 / 切片 6a：综合回归与安全复审
- 范围与审计：复核六片实现、双槽写入、敏感材料寿命、挂载选择、关闭与配置切换，未放宽 SDK/硬件/路径白名单。
- 验证：全套 Release 构建通过；首轮 CTest 65/67，修复旧页面契约后定向通过，当前保留 helper 路径拒绝一项失败。
- 验证：旧安装器测试无条件抛错 lambda 触发优化器 C4702；改为零安装调用计数断言后通过，未关闭警告检查。
- 验证：补充区域外 64 字节哨兵、新旧根密钥不同、后台卡代次变化拒绝迟到准备；原生桥定向通过。
- 验证：真实隐藏 WebView 使用模拟卡完成全流程，进程退出 0；同步此前布局遗漏的五个夹具节点和过时文案断言。
- 验证：独立 package-release 静态运行时应用、helper、安装 helper 全部构建通过；生产 CSP 未改。
- 安全与风险：vikey_helper_process_tests 在既有 development vendor 暂存路径重定向校验提前失败，新增进程级快照/重置场景因此尚未执行；底层协议和日志模拟通过不能替代该缺口。
- 安全与风险：实卡断电、换卡和安装后首次启动未验收；驱动身份核验至卸载 IOCTL 的外部竞争仍保留。
- 提交：按用户授权自动提交测试与审查修正，随后交付测试安装包；禁止推送。

### 2026-09-21 — P3.26 / 切片 6b：最终审查与测试安装交付
- 范围与审计：汇总六片软件审查，更新里程碑与最终审查文档；真实卡验收和既有失败保持待处理状态。
- 验证：最终全套 CTest 66/67 通过，76.30 秒；唯一失败仍为既有 helper development vendor 暂存路径重定向拒绝。
- 验证：真实 WebView＋模拟卡退出 0；DOM 8+12 场景、导航 268 项及此前已验证的底层/生命周期边界有效。
- 交付：out/installers/installer-20260921-160759，同根同名 ZIP；来源 4751e4d，dirty 仅来自既有未提交日志，未混入其他任务代码。
- 交付：安装器 300386041 字节，SHA256 6F02ABE5EF6101F733917F8FF0B194430E1D5E431475EFC210B983950E5D0D5C。
- 交付：ZIP 300435574 字节，SHA256 9DAEE41A5ED049D7FFCE9285F1081DAFB7B8C8659432E822D5EEE0BC30FA6B9B；7 文件/7 ZIP 条目、每文件摘要与构建身份逐项通过。
- 安全与风险：固定上游源码版本、依赖摘要/签名均经正式脚本核验；NSIS 两个已有未使用变量告警保留。路径失败、驱动末次核验竞争和真实卡破坏性/断电/首次安装未验均写入测试说明。
- 提交：按用户授权自主提交最终文档，六片软件开发与审查完成；这是测试交付，不称完整硬件验收。未安装、未重置实卡、禁止推送。

### 2026-09-21 — P3.26 / 登录齿轮菜单与确认界面调整
- 范围与审计：按用户最新四项 UI 要求复核登录渲染与重置计划；本轮明确不提交、不打包，覆盖此前自动提交授权。
- 检查与实现：右下角齿轮旋转并向上展开忘记密码/配置凭据；红色忘记密码入口，点击外部或 Escape 收起，忙碌和卡变化收起菜单。
- 检查与实现：确认复选文案改为“我已知晓重置安全卡可能带来的所有影响，并确认重置。”；登录及设置页重新初始化弹窗关闭图标放大。
- 验证：重置 DOM 8 场景、启动 DOM 12 场景与页面契约通过；隔离浏览器核对菜单/弹窗布局，Release 应用重新构建并核验页面摘要一致。
- 安全与风险：只改界面交互，保留原生确认、路由和忙碌限制；未进行重置或实际卸载，未重跑全套原生测试。
- 运行：首次从 Codex 直接启动触发已知路径映射 2005；改由 Explorer 桌面拉起同一更新后应用，首次安全卡探测及页面就绪完成，进程正常响应。使用校验过的既有运行依赖，未覆盖已安装应用。
- 提交：开发时按要求未提交；现获用户明确授权，与齿轮最终样式一并提交，不打包、不推送。

### 2026-09-21 — P3.26 / 齿轮图标样式精简
- 范围与审计：按用户要求复核登录前重置范围及现有齿轮样式，仅调整图标外观。
- 检查与实现：移除齿轮按钮背景和边框，描边由 1.6 加粗至 2.2；保留旋转、菜单交互及键盘焦点提示。
- 验证：既有重置 DOM 8 场景通过；隔离浏览器截图核对外观，Release 应用构建成功。
- 安全与风险：仅 CSS 变更，不涉及原生权限与重置逻辑；未执行实卡重置或全量原生回归。
- 运行：通过 Explorer 启动更新后的应用，首次安全卡探测、页面就绪正常，进程响应正常。
- 提交：用户已明确授权提交本轮 UI 改动；不打包、不推送。

### 2026-09-21 — P3.26 / 最新 UI 安装交付
- 范围与审计：用户要求打包当前最新版，来源 bcf925f；工作区仅历史开发记录未提交，未混入业务代码。
- 检查与实现：正式静态 Release 构建及依赖摘要/签名检查通过，正式 NSIS 生成规定时间戳安装目录。
- 验证：安装器摘要与构建记录一致，页面及可执行载荷与构建逐项一致；7 个交付文件和 ZIP 条目逐项摘要核验通过。
- 安全与风险：保留两个既有 NSIS 未使用变量告警；完整硬件/安装验收与历史 helper 测试失败不因本次打包变为通过，未安装或重置实卡。
- 交付：D:\Desktop\usb-vault\out\installers\installer-20260921-163713.zip；ZIP SHA256 57C1B5E53849200E39B7CEB9E6D0DA07BB2795A03A72C63E49C785021CEE2CEE。
- 提交：仅交付，记录未提交，不推送。

### 2026-09-21 — 主界面离开菜单与安全退出
- 范围与审计：按用户要求检查关闭守卫、单次登录上下文、后台互斥和现有侧栏；需求增补明确仅清理当前应用会话的卷。
- 检查与实现：左下角红色无背景离开图标，向上展开退至登录页／退出应用；支持外部点击和 Escape 收起、键盘聚焦，紧凑侧栏对齐。
- 检查与实现：先走既有异步普通卸载；返回登录请求原生再次检查认证、挂载及后台工作，接受后封禁旧桥并销毁会话，再探卡和重载页面；退出复用 WM_CLOSE 守卫。
- 验证：新增 6 个退出 DOM 场景、重置 8 场景、启动 12 场景、导航 268 项通过；原生桥／关闭守卫／页面契约 3 项通过，Release 应用构建通过。
- 验证：隔离模拟浏览器核对标准及紧凑窗口的菜单；未使用实际安全卡登录／退出，未重跑全套原生测试或安装验收。
- 安全与风险：失败不切页、不自动强制卸载；返回动作不写卡，不保留旧认证上下文，旧桥退休防止迟到访问已销毁对象；已登录实机往返及外部占用仍待验收。
- 提交：本轮未提交、不打包、不推送；此前已交付安装包不包含本轮变更。

### 2026-09-21 — 主界面离开菜单测试安装包
- 范围与审计：用户授权打包当前工作区，基于 bcf925f 加未提交离开菜单与原生退出逻辑，说明明确 dirty 来源。
- 验证：正式静态 Release 主应用/助手构建成功，依赖签名与摘要通过；主程序和页面载荷与本轮构建一致，安装器摘要及 7 个 ZIP 条目逐项核验通过。
- 安全与风险：保留既有 NSIS 两项未使用变量告警及历史 helper 测试失败；实卡往返、占用和安装验收未完成，不自动安装或操作卡。
- 交付：D:\Desktop\usb-vault\out\installers\installer-20260921-170405.zip；ZIP SHA256 CF8A9421876972714A5D2460FD88B73234ED48328538FF59028CECF2C9055184。
- 提交：用户仅要求打包，代码与记录保持未提交，不推送。

### 2026-09-21 — 离开图标水平翻转与重新打包
- 范围与审计：按用户要求将退出 SVG 水平翻转；复核既有离开菜单范围，仅 CSS 变换，不改变权限或卸载逻辑。基于 bcf925f 加未提交退出功能，说明明确 dirty 来源。
- 验证：隔离浏览器检查标准及紧凑窗口外观，静态 Release 主应用/助手构建成功，依赖签名与摘要通过；主程序和页面载荷与本轮构建一致，安装器摘要及 7 个 ZIP 条目逐项核验通过。
- 安全与风险：保留既有 NSIS 两项未使用变量告警及历史 helper 测试失败；实卡往返、占用和安装验收未完成，不自动安装或操作卡。
- 交付：D:\Desktop\usb-vault\out\installers\installer-20260921-170957.zip；ZIP SHA256 52922A0F0F0ABCD263475D591139A677E8B5FC47923FDD8F56C32EF161A1F165。
- 提交：用户仅要求打包，代码与记录保持未提交，不推送。

### 2026-09-21 — 初始化后隐藏盘创建未提权失败定位
- 范围与审计：用户确认忘记密码重置、重新初始化后直接进入主界面，诱饵密码不同且普通容器可提权；已安装六项组件摘要与最新构建一致。
- 定位：submit_initialization 仅通过 entry 建立 authentication，未经过 consume_authenticated_login 保存 session login_password；create_container_b 复制到空密码后在写卡与提权前拒绝。正常登录会保存该密码，普通容器创建不依赖它。
- 验证：补充模拟卡重置→初始化→第二密码设置→双密码解锁同根测试；card_record_v1_tests 与 card_record_service_tests 2/2 通过，87.68 秒。
- 安全与风险：本轮未操作实体卡；记录层模拟通过不替代实卡，但用户路径与会话缺陷一致。通用创建失败文案未揭示实际前置阶段，仍待修复。
- 后续与提交：生产修复尚未实施；临时可退至登录页重新输入密码，再创建隐藏盘；新增回归测试和诊断记录未提交、不打包、不推送。

### 2026-09-21 — 初始化会话密码交接修复
- 范围与审计：用户授权修复初始化直接进入主界面后隐藏盘未提权即失败；复核初始化证明、认证建立、会话密码派生和释放机制，补充需求边界。
- 检查与实现：独立规范化和确认密码后，仅在卡初始化成功时把锁页副本移动到原生卷会话；无额外读写卡或自动挂载，不增加页面接口或持久化。
- 安全与风险：拒绝未认证、空输入和既有会话覆盖；交接失败释放秘密并清除不完整认证，输入和临时副本沿用 RAII 擦除。正常退出释放同一会话密码字段。
- 验证：新增初始化立即创建隐藏盘、NFC 一致、重复/空交接拒绝、失败认证撤销和清理测试；会话核心/UI/桥 3/3，通过入口与生命周期等扩展回归 7/7（桥重复），共 9 个不同原生测试套件。
- 验证：全量默认目标构建及静态 Release 主应用构建通过；真实隐藏 WebView+模拟卡初次退出码 2，定位旧夹具固定返回登录密码且忽略初始化密码；改为真实交接并校验原初始化值后退出码 0。
- 验证限制：此前记录层重置→初始化→第二密码→双密码同根测试通过；未再次重置实体卡或执行实机提权创建，未重跑含既有 helper 路径失败的全套 CTest。
- 提交：修复阶段未提交；后已交付 installer-20260921-173502 测试包。用户现明确授权本修复以单个 [fix] 提交，离开菜单及无关历史记录保留未提交，不推送。

### 2026-09-21 — 初始化会话修复测试安装交付
- 范围与审计：按用户要求打包当前修复版，包含未提交的离开菜单、图标翻转和初始化密码交接，交付说明明确 dirty 来源。
- 验证：静态 Release 主程序和安装助手构建通过，正式脚本验证依赖签名/摘要；载荷与当前构建及页面一致，安装器记录和 7 个 ZIP 条目逐项核验通过。
- 安全与风险：沿用已完成的 9 项原生及真实 WebView 模拟验证；保留既有 NSIS 两项未使用变量告警，未安装或再次操作实体卡，未重跑全套 CTest。
- 交付：D:\Desktop\usb-vault\out\installers\installer-20260921-173502.zip；ZIP SHA256 EF7DDEE67AA511FEABE6B20FC47D3454BFAB83DC883E9DBC379E9D15824E7FA5。
- 提交：用户仅要求打包，修复及记录保持未提交，不推送。


### 2026-09-22 — 启动日志 / 页面就绪前卡诊断补漏
- 范围与审计：复核启动日志增补、R-03、8.6、里程碑及两份用户日志；旧新版首次采集条件未删除，设备事件漏记路径存在，但尚未证明是本次 2011 的实际路径；无范围冲突。
- 检查与实现：检查主入口同步探卡、直接设备通知、定时器延迟事件、页面就绪回调、错误数值净化和现有日志测试。
- 检查与实现：三处共用卡诊断采集；页面就绪前只读复核最终诊断，先补错误再写就绪和结束；页面就绪仅表示界面加载完成。
- 检查与实现：规范化后的主错与清理错按全部字段连续去重；清除后再失败重新记录，延迟拔出与到达合并时先重置去重状态。
- 检查与实现：保留 128 条及小于 64 KiB 上限，预留三条收尾；被上限挡住的事件不更新已记录快照，确保最终错误仍可补记；需求及诊断说明同步。
- 验证：新增七种日志场景，覆盖后到故障、首次失败去重、最终快照兜底、正常无卡、步骤与清理错变化、恢复后复发、条数上限及结束后不追加。
- 验证：startup_log、startup_failure、webview_startup_state、card_entry_context、card_device_lifecycle 五组通过（15.17 秒）；首次构建命令目标名错误，纠正后测试及 Release 主应用构建通过。
- 安全与风险：复核仅保存固定数值、无异常文本／路径／凭据／卡身份／档位信息，不新增卡操作或认证重试；结束后不继续写日志；磁盘写入失败仍沿用 9010。
- 验证限制：未执行真实设备通知注入、用户机器故障复现或全量测试；2011 根因仍待新日志证据；沿用启动期边界，页面就绪之后的新错误不在本日志范围。
- 提交：本片未提交、未打包、未安装、禁止推送；保留工作区既有离开菜单等无关改动，等待用户审阅决定。


### 2026-09-22 — 启动日志 / 2011 全编号诊断模拟验证
- 范围与审计：按用户要求继续验证上片日志补漏；复核需求启动日志边界、R-03／8.6 与固定步骤表，仅增加测试及验证记录。
- 检查与实现：审计 helper client、helper 环境拒绝、诊断捕获、首个清理错误保留及日志输出；55 个直接编号＋4 个属性参数＋30 个属性编号覆盖 1–89。
- 检查与实现：新增独立 CTest 矩阵，覆盖 89 个编号、第 52 步双类别和 helper 无步骤码响应，共 91 类诊断；经过真实卡入口控制器捕获再写真实日志文件。
- 验证：91 类 × 首次／后到重复通知／页面最终复核 3 时点 × 无附错／2015 首附错／2011 独立步骤首附错 3 组合，819 场景全部通过（12.92 秒）。
- 验证：逐项匹配主码、原因、步骤、Win32／HRESULT、首个附码及顺序；二次清理不覆盖首附错，无文本或未分类值泄漏，无额外检查及结束后写入。
- 验证：同轮 startup_failure、startup_log 通过，共 3/3（13.29 秒）；首次 CMake 自动重生成因代码页 936 被拒绝，切换 65001 后编译成功；diff 检查通过。
- 安全与风险：测试源不调用 SDK 或启动真实 helper，不放宽隔离、不改 IPC；仅创建并删除测试自有日志文件，敏感文本哨兵仅用于检查不得输出。
- 验证限制：Windows API 到诊断仅源码审计，未逐 API 实际故障注入；无真实设备事件／WebView 调度或 IPC 传输注入，不能把 819 场景称作实机故障全覆盖。
- 剩余边界：helper 环境拒绝没有步骤字段，仅可记 2011／原因 1／Win32 10；写入失败、强杀和页面就绪后新故障不保证记录，用户 2011 根因仍待复现。
- 提交：本片与上片保持未提交，未打包、未安装、未操作实卡、不推送；完整覆盖报告见 docs/design/process-security-log-verification.md。


### 2026-09-22 — 2011 / helper 环境与复合 SID 诊断细分
- 范围与审计：按用户要求细化所有现有 2011 生成路径；复核需求、里程碑、既有 89 步表、helper IPC 和环境校验，明确允许专用协议步骤字段并记录取代旧实现取舍。
- 检查与实现：新增 90–108 环境步骤，分别定位格式／白名单／大小写／重复、各变量缺失／空值、TEMP 关系、SystemRoot/WINDIR 和获取／释放 API 失败；不输出名称或值。
- 检查与实现：新增 109–114 拆分空 SID、无效 SID 及缺少完整性子授权；原编号不重排，35／43／44／52 收窄到具体条件；源码与表共 114 步核对无缺号。
- 检查与实现：协议 v14／响应 60 字节传递 helper 步骤，父进程保留到日志；只允许 90–108，拒绝零／未知／本地步骤、跨阶段、成功附错、类别不符和旧版；需成套交付。
- 验证：114 步 × 3 时点 × 3 清理组合共 1026 场景通过（22.81 秒）；19 个 helper 步骤真实管道往返及非法响应测试通过。
- 验证：16 种可传入环境由正式 helper 实际拒绝并返回正确步骤；90 畸形格式被 Windows 创建前以 87 拒绝，生产校验器单独注入通过；107／108 共用生产函数 API 注入及两种合法环境通过。
- 验证：真实环境夹具初次错误复用输出与错误管道而被入口拒绝，修正三独立管道后通过；没有放宽产品规则或把初次失败算通过。
- 验证：相关 10 个测试组通过；helper_process 仍因既有 development vendor 暂存路径重定向拒绝失败；默认全目标、主应用与成套静态 Release helper 构建通过，未跑全量测试。
- 安全与风险：借用环境视图不复制敏感值，释放后只留固定数值；重复环境明确拒绝，协议限制来源；首错保留，环境拒绝叠加释放失败的次错不跨 IPC，不新增卡操作或认证重试。
- 验证限制：其他本地 Windows API 未逐项真实故障注入，用户原错未复现；步骤定位首先失败的检查，不能凭拒绝访问推断外部软件责任；写日志失败、强杀和页面就绪后新故障仍保留边界。
- 提交：未提交、未打包、未安装、未操作实卡、不推送；保留无关未提交 UI 改动，等待用户审阅决定。


### 2026-09-22 — 2011 诊断版 / 路径一致环境的集成重测
- 范围与审计：用户要求重跑路径正确的集成测试；复核 helper 暂存 KnownFolder 来源、最终路径比较及既有桌面对照，只调整测试启动环境，不改生产逻辑。
- 检查与实现：先用唯一空目录核验请求路径与实际路径；普通 AppData Local 在当前宿主被重定向至 Codex 包私有 LocalCache，直接指定实际基目录的探针一致，探针均删除。
- 验证：通用 Shell.Application 启动仍继承重定向，预检拒绝且未执行测试；改用 ShellWindows 的 Explorer 桌面对象启动隐藏测试进程，KnownFolder 路径预检一致。
- 验证：同一当前 p326 构建的 vikey_helper_process_tests、vikey_artifact_tests 2/2 通过（2.95 秒），退出 0，无跳过；本轮原阻塞已在普通桌面环境解除。
- 验证证据：out/path-retest-20260922-005829 内保存启动脚本、path-check.json、result.json 和 ctest.txt；诊断覆盖报告同步保留宿主失败及桌面通过两种状态。
- 安全与风险：未改目录选择、未接受任意重定向、未放宽重解析或摘要校验；使用模拟运行库，不访问实体卡；空目录仅精确删除自身，无递归删除。
- 验证限制：Codex 宿主直接启动仍可能失败；用户电脑原 2011 未复现，不据此认定与该路径问题同因；没有重跑全量测试。
- 提交：仅补验证与记录，未提交、未打包、未安装、不推送；生产目录自动适配仍未实施。


### 2026-09-22 — 2011 细分诊断测试安装交付
- 范围与审计：按用户授权打包当前工作区，基于 6b41078 加未提交启动日志补漏、114 步诊断／协议 v14、离开菜单及关联测试文档；交付说明明确 dirty 来源。
- 检查与实现：正式静态 Release 主程序、helper、卷创建工具及安装助手构建通过；正式载荷脚本重新核验第三方摘要／签名及固定 VeraCrypt 源码版本，NSIS 3.12 生成规定时间戳目录。
- 验证：六个自有载荷与当前构建一致，两份页面与源码一致；安装器构建记录摘要一致，8 个交付文件／8 个 ZIP 条目逐项摘要核验通过。
- 验证：沿用 1026 场景诊断矩阵、真实 helper 环境拒绝、IPC 及相关回归；路径一致的 Explorer 环境 helper_process 与 artifact 2/2 通过，无跳过。
- 安全与风险：协议 v14 需要主程序及 helper 成套更新；保留两个既有 NSIS 未使用变量告警；未安装、未实卡验收、未重跑全量测试，用户原 2011 仍待新日志证据。
- 交付：out/installers/installer-20260922-010753；同根 installer-20260922-010753.zip，300432130 字节。
- 校验：安装器 300388350 字节，SHA256 1C0572A35663ADDC0584BE1E5B5E3C59419A88C2DB684917F3A65D8E8682E43A；ZIP SHA256 8A093E1338FBC2CD4E815C045C02F07AA1E4DFF1BBEC73F06EB6DF078EABB38E。
- 提交：用户仅授权打包，代码和记录保持未提交、不推送；没有修改产品路径逻辑或自动安装。

### 2026-09-22 — P4 / 普通数据分区加密规划
- 范围与审计：用户确认普通数据分区加密纳入 P4；审计需求 V-01／V-02、权限边界与既有 P4，发现原定 v2.0 延后且 P4 已用于加固交付。
- 检查与实现：更新需求当前口径与里程碑，保留历史决策；按用户后续决定将分区支持列为 P4.1～P4.6，原有加固交付整体移至 P5，同步验收引用，不删除既有验收。
- 检查与实现：检查 mount_request、volume_creator、container_target_path 与 volume_provider_tests，现有文件目标约束及固定 512 字节扇区假设须在设备适配时复核，不能只放开设备路径。
- 检查与实现：当前限定用户已创建的普通非系统数据分区，分别验收空盘格式化加密和 NTFS 保留数据原地加密；用户同意先提供系统磁盘管理入口及返回刷新，不在应用内创建或调整分区。
- 检查与实现：纳入目标身份、提权侧复核、断电与拔卡恢复、盘符管理、导出边界及实机验收，工期待 P4.1 复估。
- 验证：本片仅文档规划，未运行构建或产品测试，未操作任何分区；检查新增阶段映射与文档差异，产品实现及实机结果均未宣称完成。
- 安全与风险：分区仍可被底层工具看到和破坏；转换中断恢复、设备误选及固定版本能力尚未验证，R-03／8.6 继续约束产品实现。
- 提交：用户已授权提交本规划并开始 P4 第一片；诊断及 UI 已独立提交，历史记录已提交 1fa61ea，不推送。

### 2026-09-22 — 工作区审阅 / 主界面离开菜单提交
- 范围与审计：用户授权盘点并提交可提交改动；独立审阅离开菜单、原生返回登录与关闭守卫，保留诊断和规划为后续独立提交。
- 检查与实现：普通卸载完成后才离开，返回登录要求认证、无挂载且无后台任务；先封禁旧桥并销毁会话，再探卡加载页面；不写卡、不自动强制卸载。
- 验证：当前默认目标构建无待编译内容；原生桥回归通过，六个离开 DOM 场景通过；关闭守卫和页面契约检查见本轮测试结果。
- 安全与风险：权限与参数门禁、繁忙拒绝、失败保留会话和迟到桥调用均有覆盖；未补实卡往返及外部占用验收，沿用历史限制。
- 提交：用户已授权本片提交；提交哈希由 Git 历史追溯，后续汇总登记，不推送。

### 2026-09-22 — 工作区审阅 / 启动诊断提交
- 范围与审计：用户授权提交既有启动诊断闭环；审阅页面就绪前补漏、114 步编号、helper v14 协议与环境检查，不包含分区实现。
- 检查与实现：诊断连续去重并预留收尾，helper 仅传固定 90～108 步；异常／重复环境拒绝，SID 细分不放宽隔离，主程序与 helper 需成套更新。
- 验证：本轮构建无待编译内容；诊断矩阵 1026 场景、日志、错误捕获、WebView 就绪、helper 环境和协议回归通过；入口与生命周期另行回归。
- 验证：沿用已有 Explorer 路径一致环境 helper_process／artifact 2/2 通过证据；未把 Codex 宿主重定向失败改成普遍通过，未重跑实卡或全量测试。
- 安全与风险：只记录固定编号与分类数值，不新增变量值／秘密／身份输出；首次就绪后、强杀与写入失败仍是日志边界，用户原 2011 未复现。
- 提交：用户已授权本片提交；哈希由 Git 历史追溯，后续汇总登记，不推送。

### 2026-09-22 — 工作区审阅 / 历史记录归档提交
- 范围与审计：检查工作区积累的安装交付、登录重置和诊断验证记录，保留当时未提交、失败及未验收状态，不把历史测试改写成本轮执行。
- 检查与实现：仅把此前散留在工作区的记录纳入同一个现有开发日志；未创建其他日志或归档文件，验证报告补齐已交付包状态。
- 验证：本轮当前构建无需重编译，11 个不同原生测试组和 6 个 DOM 场景通过；已有路径一致集成结果文件退出 0，未重跑该集成。
- 安全与风险：离开菜单实卡往返、用户 2011 复现、首次就绪后日志与 Codex 宿主路径差异仍保留，未运行全量测试或安装。
- 提交：本轮离开菜单已提交 ebec3ee，诊断闭环已提交 148ce84；历史记录本片获用户授权提交，不推送。

### 2026-09-22 — P4.1a / 固定源码审计与最小骨架范围
- 范围与审计：用户明确只要求正常暂停可续作，优先最小骨架；本项替代此前断电／强杀恢复验收要求，不自动转入 P5，历史计划记录仍保留当时状态。
- 检查与实现：核验固定 b48e31f5 上游干净源树、现有补丁边界和交付 DLL 摘要；6 个 PE 导出无原地 Begin／Resume，设备格式化分支只是候选能力。
- 检查与实现：审阅 InPlace 的 NTFS 准备、内部文件系统缩小、暂停检查与备份卷头进度；新建文件失败删除语义不可用于设备，原地加密需独立无界面适配。
- 检查与实现：需求、里程碑及 docs/design/partition-encryption-p4-plan.md 同步；最小顺序为只读目标骨架、单空分区加密挂载、NTFS 正常暂停续作，再扩展产品接线。
- 验证：SHA-256 与已审定 FormatDLL 一致，dumpbin 只读导出检查完成；volume_provider／mount_orchestrator／creator_protocol／container_target_path 四组单元基线 4/4 通过（0.77 秒）。
- 安全与风险：保留误选盘与提权目标复验、写入前检查和错误停止；正常暂停须保存成功，失败不得冒称可续作；拔卡停止与突然掉线不等同正常暂停。
- 安全与风险：不照搬上游坏扇区自动处理或明文进度配置，继续遵守 R-03／8.6；断电等非正常中止可能损坏数据，本期不承诺无损恢复。
- 验证限制：本片为源码／二进制审计，未改产品代码、未建测试分区、未加载 DLL 执行写入、未做实卡或暂停续作实测，P4.1 整体仍未完成。
- 提交：此前工作区已提交 ebec3ee、148ce84、1fa61ea、ec0f5b7；用户现已授权提交本片并开始下一代码片，不推送。

### 2026-09-22 — P4.1b / 原生只读分区发现骨架
- 范围与审计：用户授权提交 P4.1a 并继续；a17744e 已提交、不推送。复核最小骨架、卷模块／测试组织与 Windows 查询 API，本片限定原生只读模块。
- 检查与实现：新增无路径输入的卷 GUID 枚举，查询分区、单磁盘区段、几何及文件系统；每次刷新重读，返回候选与不可选原因，不落盘或接提权写入。
- 检查与实现：排除当前 Windows 卷、GPT 非基本数据／保护属性、MBR 活动／未知分区；仅 NTFS／512 逻辑扇区作为发现候选，未知元数据与不一致／溢出／未对齐范围拒绝。
- 验证：MSVC /W4 /WX /sdl /analyze 通过；首次构建命令应用目标名错误，改用正式现有目标后 Release 模块、测试与主应用构建通过。
- 验证：40 个判定场景及四组相关 CTest 4/4 通过（0.19 秒）；原生只读两次刷新通过，5 卷／2 候选／3 不可选，当前 Windows 卷排除 1，不输出路径或硬件身份。
- 安全与风险：零 desired access，只有查询 IOCTL、无磁盘写入／挂载／锁卷／提权；复核返回长度、错误拒绝、4096 项上限、整数范围和 RAII 句柄释放。
- 安全与风险：候选不是写授权，快照可能过期；持久设备身份、分页／启动依赖、BitLocker、独占与写保护须在写入接入前补齐，不宣称通用物理分区全枚举或加密支持。
- 验证限制：未接 UI、会话选择 ID、磁盘管理按钮；未做全量／ASan／API 故障注入、设备建卷或实卡，后续独立接线片继续。
- 提交：用户已授权提交本片并继续选择／刷新接线，不推送；未修改任何分区内容。

### 2026-09-22 — P4.1c / 分区选择与刷新接线
- 范围与审计：用户授权的 P4.1b 已提交 485b078、不推送；复核需求／最小骨架与 R-03／8.6，当前只接候选列表和系统工具入口。
- 检查与实现：检查 WebView 桥接、共享工作线程、会话变化、退出排他和导航测试；新增统一分区页面，进入扫描、手动刷新、单项选择及不可选原因。
- 检查与实现：原生保留目标 GUID，仅投影数字清单；快照 ID 不复用，刷新／清空／会话变化／打开磁盘管理废弃选择，选择不构成写授权。
- 检查与实现：固定系统目录的 mmc.exe 与 diskmgmt.msc，不接收任意路径或参数；失败提供系统菜单手动打开指引，无应用内分区创建或磁盘写入。
- 验证：Release 主应用与测试构建通过；四组相关 CTest 4/4 通过；新增分区 DOM、268 项导航检查、6 项退出、12 项启动、8 项登录重置场景通过。
- 验证：首次构建发现工具路径转义错误，修复后通过；新 DOM 测试初次未等待认证界面就绪，修正等待条件后通过，不以失败运行作为验证证据。
- 安全与风险：复核认证、会话世代、参数／列表边界、XSS、扫描失败默认拒绝、旧 ID 重放与旧操作状态；扫描期间禁止其他操作进度越界读取，换卡迟到结果不能恢复选择。
- 安全与风险：保持普通分区统一入口、不引入密码角色标记；快照仍可能因设备变化过期，写入前设备身份／布局／启动分页／BitLocker／独占复验属于后续必须完成的边界。
- 验证限制：未运行全量、ASan、真实 WebView 视觉／实卡／磁盘管理启动或分区写入验收；本片未修改任何分区内容，不宣称 P4.1 整体完成。
- 提交：用户已授权提交本片，统一使用 `[P4.1]` 前缀；此前两片仅重写提交前缀、文件树不变，a17744e → 3d3585f，485b078 → 67e26a2。本片哈希由 Git 历史追溯，禁止推送。

### 2026-09-22 — P4.1d / 自建隔离 VHD 设备闭环
- 范围与审计：上一片 f9c2834 已提交；复核 P4 最小骨架、文件创建／挂载边界与固定 DLL 设备路径，先建立可复核的单空分区运行证据。
- 检查与实现：新增独立显式构建／运行夹具，只接受自建 VHD 开关、不接已有设备参数，不进默认 CTest，不修改产品写入口或 spike。
- 检查与实现：自建 256 MiB VHD、MBR／512 及 254 MiB 普通分区；持有附加句柄并复核随机签名／位置／长度，使用锁页随机二进制密码格式化 FAT，挂载写入后卸载、重挂载精确读回。
- 验证：Release 构建及 MSVC 静态分析通过；首轮 GUID 链接失败及静态分析的大栈缓冲区／句柄生命周期／导出空指针告警已修复；四类运行门禁返回 77、未创建磁盘，三组相关 CTest 3/3 通过。
- 验证：首轮管理员试验因普通卸载暂时占用退出 1，不记通过；复用现有短时重试后完整闭环通过，补临时挂载身份检查后的最终版本再跑退出 0。
- 验证：DLL SHA-256 与审定版本一致；首轮残留只对已核实的试验挂载正常卸载、镜像确认脱离后删除；最终无驱动挂载或夹具镜像残留。
- 安全与风险：检查无外部目标输入、格式化前复验、DLL 加载路径、锁页密码、作用域清理与失败证据保留；异常强制清理仅限完整临时身份匹配的自建测试卷，未对该分支做故障注入。
- 安全与风险：发现 SDK 默认强制模式可退回共享设备打开且可能用临时盘符，产品接入前必须约束；不能把此隔离证据或现有候选列表当作生产写授权。
- 验证限制：仅本机 MBR／512 VHD＋FAT，未验 GPT／4Kn／实体盘／实卡／原地转换／暂停续作；未跑全量或 ASan，产品分区加密仍未开放。
- 提交：用户已授权提交本片，哈希由 Git 历史追溯，统一 `[P4.1]` 前缀，不推送；下一片接独立设备动作合约和必要安全复验。

### 2026-09-22 — P4.1e / 同句柄分区格式化候选后端
- 范围与审计：P4.1d 按用户授权提交 c4f94a1；复核需求、原 SDK 的强制共享回退／临时盘符／失败删除语义，独立实现设备 I/O 合约，尚不接产品写入口。
- 检查与实现：新增同句柄二进制密码导出及 ABI 镜像，锁前／锁后核验偏移、长度、MBR 普通分区与 512 几何；锁失败即拒绝，不重开目标路径、不强制共享或分配临时盘符。
- 检查与实现：只对已明确识别 RAW 的输入跳过不适用 DASD 控制；NTFS 必须成功取得控制。核心写入前显式归零文件指针，保留并关闭内部副本、不消费调用者句柄。
- 检查与实现：修复新设备分支忽略快速 FAT 最终取消回调的问题，记录中止并限制回调异常跨越 C 清理；密码锁页清零，成功必须包含 flush／解锁。
- 验证：候选 DLL／正式应用构建通过，7 个导出；固定上游加三补丁复现检查通过，夹具静态分析和四组 CTest 4/4 通过。
- 验证：最终管理员自建 VHD 测试退出 0，覆盖参数／非磁盘句柄／占用拒绝与边界扇区不变、独立取消、进度、调用者句柄、设备读回以及普通／隐藏文件容器回归。
- 验证：RAW 控制错误 87、共享文件指针越界、取消结果及取消后句柄复用失败均保留为失败证据；修复或按独立操作合约调整夹具后重跑。补丁 CRLF 应用失败已改 LF；首次构建脚本路径与编码告警已修正。
- 安全与风险：后端不是卡认证或目标写授权；helper 仍需稳定设备身份、系统／启动／分页／BitLocker 排除与清空确认。取消是部分格式化中止，不是暂停续作，失败不自动重建／删除目标。
- 验证限制：仅 MBR／512 RAW VHD；未验 NTFS 输入、真实物理盘、GPT／4Kn、实卡、原地暂停、全量／ASan／完整故障注入，第三方 DLL 未做完整静态分析。
- 提交：用户已授权提交本片，哈希由 Git 历史追溯；候选 SHA-256 94D37A2D8C70707190076680587833CFA3C26ABD3426629EFAE9F31DA0225309，产品 DLL／允许列表不变；残留 9 个失败镜像确认脱离后清理，无驱动挂载，禁止推送。

### 2026-09-22 — P4.1 / 统一新建容器入口
- 范围与审计：候选后端已按用户授权提交 f9883d6、未推送；复核需求真源、P4 设计与现有向导及分区测试，按用户决定默认磁盘、按钮切换文件。
- 检查与实现：新建向导整合已有分区列表、手动刷新和磁盘管理入口，移除独立侧栏页；磁盘指物理硬盘已有数据分区，不提供应用内分区创建。
- 检查与实现：文件分支保留标准／隐藏卷及参数流程；切换清空表单、诱饵密码和分区选择，重新打开恢复磁盘默认值，执行期间禁止切换。
- 安全与风险：所有认证身份使用同样入口和默认值；磁盘创建禁用且明确提示，文件创建执行函数再次检查形态，隐藏分区控件不能触发读取／选择／磁盘管理。
- 安全与风险：复核异步修订号与会话失效、文本渲染和密码清理；候选列表仍不等于写授权，物理设备复验、独立 helper 和卡认证写入接线仍待后续切片。
- 验证：正式 Release 应用构建、相关 CTest 4/4、分区 DOM 场景与 268 项导航检查通过；启动 12、退出 6、登录重置 8、初始化 20+7 项 DOM 检查通过。
- 验证：新增默认值、切换清密、选择失效、重开复位及模拟点击不能误走文件创建检查；修复测试入口迁移遗漏，回归重跑通过。
- 验证限制：DOM 不含真实布局引擎；真实 WebView 夹具已适配但本片未运行，未做实卡／物理分区写入及全量测试；不改变产品 DLL 或允许列表。
- 提交：用户已批准提交本片实现与需求决定，哈希由 Git 历史追溯；禁止推送。

### 2026-09-22 — P4.1 / 选择时目标快照复核
- 范围与审计：上一片统一向导按用户决定提交 08e98c9，未推送；复核需求真源、P4 设计、原生枚举、选择生命周期及 helper 文件目标协议，将本片限定为写入接线前的只读目标复核。
- 检查与实现：原生快照新增布局、GPT 分区标识、文件系统序号；选择时重新枚举并比较完整候选，拒绝目标消失、位置／容量／标识变化、重复身份和未知信息。
- 检查与实现：每次选择先清除旧选项；复核异常与会话变化拒绝，失效列表从页面移除并提示刷新，页面仍只接收会话 ID 和允许展示的数值。
- 验证：Release 正式应用构建、相关 CTest 4/4、分区 DOM 场景及 268 项导航检查通过；原生模块 MSVC /W4 /WX /sdl /analyze 通过。
- 验证：快照测试覆盖字段变更、重复、缺失、上限与布局；桥接覆盖格式序号变化、消失、重复、抛错、复核期间会话更换；实机只读两次扫描 7 卷／2 候选／5 不可选，Windows 排除 1，候选快照一致。
- 验证：初次构建因 DWORD 与 uint32_t 指针类型不同失败，改用 DWORD API 局部值后完整重建通过；不将失败构建计为通过。
- 安全与风险：无写入、提权、口令处理或新增落盘；检查异常收束、互斥释放、会话复验和页面字段投影。卷序号／GPT ID 不是硬件身份，不能防克隆或消除选择后竞态。
- 安全与风险：本片同步复核可能受慢设备影响；持久设备身份、同句柄写前复核、分页／启动／BitLocker 排除、清空确认、卡派生密码与 helper 写动作仍待后续。
- 验证限制：未做实物热拔插、实际重分区／格式化、实卡及磁盘写入或全量测试；磁盘创建保持禁用，产品 DLL／允许列表不变。
- 提交：用户已授权提交本片，哈希由 Git 历史追溯，禁止推送。

### 2026-09-22 — P4.1 / 原生磁盘身份绑定
- 范围与审计：上一片目标复核按用户授权提交 58c5522，未推送；复核需求、阶段计划、现有 helper 协议、枚举与选择测试，先补设备身份来源再进入写接口。
- 检查与实现：复用同一只读卷句柄查询 Windows STORAGE_DEVICE_NUMBER_EX，校验完整响应、版本／尺寸、磁盘类型、盘号／分区号与现有分区区段一致。
- 检查与实现：非零设备 GUID 与来源标志纳入原生快照比较；拒绝随机／冲突／缺失身份、未知标志和不支持的查询，页面显示“无法确认磁盘身份”并禁止选择。
- 验证：Release 正式应用构建、相关 CTest 4/4、54 项分区判定及快照测试通过；分区 DOM（含身份不可用与未知状态拒绝）和 268 项导航检查通过，元数据模块 /W4 /WX /sdl /analyze 通过。
- 验证：只读实机两次扫描 7 卷／2 候选／5 不可选，Windows 排除 1，候选身份一致；桥接测试验证盘号和分区事实不变但设备 GUID 改变时拒绝旧选择。
- 验证：首次回归因编辑脚本误扩展无关登录重置测试循环而失败，已恢复该循环并完整重建重跑；该失败不归因于产品行为，也不计作通过。
- 安全与风险：复核零访问权限、响应长度、未知状态默认拒绝、原生身份不进入页面／日志／持久记录；未新增设备写入、提权或密码处理。
- 安全与风险：驱动提供身份不防伪造标识／恶意驱动，不证明物理盘，未支持身份查询的设备暂不可选；helper 仍需同句柄复验、系统／启动／分页／BitLocker 排除、清空确认和卡认证写入。
- 验证限制：未做真实硬件替换、跨重启、实卡、物理盘写入或全量测试；磁盘创建仍禁用，产品 DLL／允许列表不变，选中后竞态与同步读取延迟仍保留。
- 提交：用户已授权提交本片，哈希由 Git 历史追溯，禁止推送。

### 2026-09-22 — P4.1 / 已打开句柄的身份复核原语
- 范围与审计：上一片磁盘身份绑定按用户授权提交 68c2d3f，未推送；复核需求、helper 的文件创建／失败删除边界、格式化 ABI 与原生查询，先独立实现同句柄复核前置。
- 检查与实现：抽取句柄元数据读取，共用分区／区段／设备身份判定；新增内部函数核对调用者持有的可读卷句柄与原生快照，不使用发现路径重新打开目标。
- 检查与实现：借用句柄不关闭／复制／seek，不锁卷／卸载／写入；所有身份字段均须一致，查询失败或异常返回拒绝；保留普通进程的零权限候选发现行为。
- 验证：最终 Release 正式应用构建、相关 CTest 4/4 和原生模块 /W4 /WX /sdl /analyze 通过；新增无效／事件／普通文件句柄拒绝、句柄所有权与文件游标不变测试。
- 验证：管理员只读 --scan-handles 退出 0，在 2 项候选上完成同句柄成功复核、预期 GUID／偏移／格式序号错误拒绝及句柄保留；实机枚举 7 卷／2 候选／5 不可选，Windows 排除 1。
- 验证：初次将零权限发现改用句柄文件系统查询导致候选归零，首次扫描的通过属于空覆盖，不作为验收；补上零候选失败检查后 NTFS FSCTL 尝试明确失败，最终区分发现权限和管理员可读句柄后通过。
- 安全与风险：复核无任意设备路径输入、无写操作／密码／身份落盘；该原语只证明当前句柄身份，不是写授权，不替代系统／启动／分页／BitLocker 排除、清空确认、卡认证、独占及后端支持矩阵。
- 验证限制：没有接通 helper 写动作或产品创建按钮；未验 RAW、锁卷后调用、实物替换／拔插、实卡与物理盘写入；本片未改页面，不重复 DOM 全套，未执行全量测试。
- 提交：用户已授权提交本片，并授权后续切片在测试与安全审查后自主提交；继续遵守小提交、英文说明与禁止推送，哈希由 Git 历史追溯。

### 2026-09-22 — P4.1 / 分区专用只读检查协议
- 范围与审计：同句柄复核已提交 1d2930a；用户明确授权后续验证／安全审查后自主提交并继续，覆盖逐片询问规则，其他提交准则及禁止推送不变。
- 检查与实现：审计文件 helper 的路径、密码、失败删除与调度；新增 check_partition 独立动作和固定 60 字节目标载荷，不复用文件大小／隐藏卷字段，不携带密码。
- 检查与实现：只接受标准卷 GUID 名、非零设备身份、非随机标志、MBR／512、20 MiB～1 TiB 对齐范围及无溢出偏移；长度、保留字段、跨动作数据均严格拒绝。
- 检查与实现：协议升级 v6，头部 80 字节；先读取 8 字节前缀拒绝旧版本，再读剩余头部及载荷，避免旧短帧等待新头部；应用和 helper 同步重建，未接生产动作。
- 验证：正式 Release 应用／helper 构建、creator_protocol_tests 和协议模块 /W4 /WX /sdl /analyze 通过；覆盖合法往返、伪造载荷、非法路径／长度／标志／范围、混入密码／文件字段和旧版前缀拒绝。
- 安全与风险：管道 nonce／权限机制保持；目标只走原生管道，不落盘／命令行／页面，不能借此进入文件创建清理路径；固定范围为候选后端接线约束，不是全部布局支持承诺。
- 验证限制：本片只落地编码／解析，helper 尚未执行该动作；未测试提权实机请求或设备写入；v5 与 v6 必须成对更新，不宣称跨版本兼容。
- 提交：按用户持续授权在审查后自主提交，哈希由 Git 历史追溯；禁止推送，下一片接只读 helper 调度。

### 2026-09-22 — P4.1 / 只读分区 helper 调度
- 范围与审计：专用协议已提交 3820975；检查 helper 调度、文件失败删除与依赖加载，接入独立只读动作和原生 VolumeCreator::check_partition，不开放写入。
- 检查与实现：调用侧严格验证 MBR 快照，再走既有 nonce／UAC／本地管道；helper 独立枚举排除当前 Windows 卷，按标准卷 GUID 打开 GENERIC_READ 并复核同句柄，返回后关闭，不保留写许可。
- 检查与实现：分区动作在 FormatBackend 前返回，不加载格式 DLL／挂载／删除文件；再次拒绝密码、文件字段或非法目标，非分区动作拒绝夹带目标载荷；调用侧拒绝额外进度及非零结果值。
- 验证：正式 Release 应用／helper 构建和五组相关 CTest 5/5 通过；协议测试新增 8 类直接调度参数拒绝，helper /W4 /WX /sdl /analyze 最终通过。
- 验证：现有设备无受支持 MBR，首次 --probe-helper 明确跳过；随后新建隔离 256 MiB VHD，以镜像对象定位、确认 RAW／非系统／大小一致后准备 MBR NTFS，真实提权管道成功核验并拒绝伪造设备身份，测试退出 0。
- 验证：隔离镜像确认脱离后定点删除；未写现有物理分区。启动脚本末尾读取不存在错误文件使外层 shell 返回非零，夹具退出文件为 0、清理记录成功，二者区分保存。
- 安全与风险：静态分析首次发现既有目录创建代码未检查 ntdll 模块空值，补拒绝后重跑通过；无口令传输、新增身份落盘或任意设备路径，复核结果不能作为后续写授权。
- 验证限制：MBR／512 只读闭环已验，未验取消 UAC、硬件热拔插、实卡和物理盘写入；系统／启动／分页／BitLocker 写前策略、清空确认及卡派生格式化仍未接通。
- 提交：按用户持续授权审查后自主提交，不推送；下一片检查产品向导接入与剩余写前门禁。

### 2026-09-22 — P4.1 / 向导管理员身份检查入口
- 范围与审计：只读 helper 调度已提交 f341a13；审计应用已锁定依赖路径、服务生命周期、分区桥接互斥与向导状态，新增明确用户触发的检查入口。
- 检查与实现：在已有选择旁增加“检查所选分区”；提示管理员确认和 MBR 检查范围，检查成功仍明确加密创建未开放，不将结果转为写许可。
- 检查与实现：应用把已校验的 helper 路径注入原生服务，桥接只使用当前已选原生目标；独占后台工作线程执行 UAC 检查，页面只轮询数字结果，不传路径或设备标识。
- 检查与实现：开始／结束核验卡 epoch，失败和过期清空候选／选择；检查中拒绝刷新、选择、磁盘管理、形态切换及其他冲突操作，不复用旧检查状态。
- 验证：Release 正式应用构建、相关 CTest 3/3、分区 DOM 和 268 项导航检查通过，原生桥接 /W4 /WX /sdl /analyze 通过。
- 验证：覆盖服务收到正确目标、成功、抛错、检查中互斥与换卡；页面覆盖成功仍不可创建、失败移除旧列表、忙时模拟点击不能切换形态。
- 验证：首次编译因测试局部 pending 名称遮蔽失败，改为独立名称后重新构建／回归通过；未把失败构建算作通过。
- 安全与风险：检查服务随应用上下文持有，桥接先解绑且工作线程互斥后才销毁；审核无秘密／设备路径进入页面或日志，旧会话结果不公开，不触发格式化或文件清理。
- 验证限制：未做真实 WebView 布局与实卡点击验收；底层真实 UAC 管道已由上一片隔离 VHD 验证，本片不重复物理或虚拟盘写入。取消 UAC 归通用失败，仍需刷新重选。
- 提交：按用户持续授权审查后自主提交，禁止推送；磁盘写入、清空确认与卡派生格式化仍待后续。

### 2026-09-22 — P4.1 / 提权分区用途与 BitLocker 检查
- 范围与审计：上一片向导管理员检查已提交 d5d1c80；用户再次明确无需逐片确认、自行提交，仍禁止推送。复核需求真源、P4 设计、分区快照、helper 调度及现有测试。
- 检查与实现：新增正式只读策略模块，检查同句柄身份及枚举快照，再查询本机 Win32_Volume 的启动／系统／分页用途，全部明确为否才继续。
- 检查与实现：调用同卷 Win32_EncryptableVolume.GetConversionStatus，仅接受方法成功且 FullyDecrypted；缺失／重复对象、未安装提供程序、状态未知、查询失败均拒绝，不将保护暂停视作未加密。
- 检查与实现：固定本机命名空间与 WQL、严格 CIM／VARIANT 类型和字符串检查、4096 项上限、枚举及方法结果各 5 秒等待；连接采用系统 MAX_WAIT 上限 2 分钟，不能宣称全检查 5 秒超时。
- 检查与实现：查询前后复核同句柄与快照，接入现有只读检查按钮并更新提示；不缓存结果为写许可。清空确认留在实际创建动作前绑定目标／卡会话并一次消费，本片不提前存储同意。
- 验证：Release 正式应用／helper 构建、相关 CTest 5/5、新增 216 组安全策略组合及无效句柄检查通过；分区 DOM 和 268 项导航检查通过，新模块 MSVC /W4 /WX /sdl /analyze 通过。
- 验证：自建 256 MiB MBR NTFS VHD 真实提权管道检查成功、修改设备身份拒绝，夹具退出 0；确认镜像脱离并删除，不写现有物理分区。
- 验证：首次构建漏登记新源文件导致链接失败，静态分析发现 HRESULT 比较类型告警，补登记／改正后通过；DOM 首次因旧文案断言失败，更新断言后重跑通过。
- 安全与风险：审查固定查询／只读方法、COM 资源释放、缺失事实默认拒绝、原生身份不进入页面或日志；系统查询是快照，尚不能替代写句柄独占锁、写保护处理和最终复核。
- 验证限制：未变更真实分页配置或 BitLocker 卷做集成验证，未做实卡／真实 WebView／物理盘写入和全量验收；WMI 不可用的平台暂拒绝，不宣称全部 Windows／多系统启动依赖覆盖。
- 提交：按用户持续授权在测试与安全审查后自主提交，哈希由 Git 历史追溯；禁止推送。磁盘创建未开放，下一片接清空确认和独立格式化动作。

### 2026-09-22 — P4.1 / 独立分区格式化与单次确认握手
- 范围与审计：上一片已提交 c5edfd9；按用户持续授权继续自主提交，禁止推送。审计需求真源、卡会话文件建卷、helper 清理、候选 DLL 和隔离夹具，先建立独立分区写动作。
- 检查与实现：新增 format_partition 和必需的原生清空确认回调，专用目标／64 字节锁页二进制密码经现有私有管道传输；不放宽文件路径，不复用文件分配或删除。
- 检查与实现：协议 v7 新增单次 UCP1 确认帧，绑定本次随机 nonce；助手完成目标／用途复核和导出检查后才询问，30 秒内仅接受明确同意，拒绝／异常／断开／错误帧不进入格式化。
- 检查与实现：确认回调前后检查取消，收到同意后在保留 RW 句柄上重跑策略，再交候选 DLL 锁卷／复核／格式化；取消和失败不删分区、不自动重试、不保存许可。
- 验证：Release 正式应用／helper／显式夹具构建、相关 CTest 6/6 和协议／客户端／助手 MSVC /W4 /WX /sdl /analyze 通过；新增密码长度与二进制往返、旧版本、确认伪造、跨动作字段及 16 类直接调度拒绝测试。
- 验证限制：扩展 --run-owned-vhd-helper 以自身新建镜像准备 NTFS、比对拒绝前后卷头、确认后挂载读回；本轮 Windows UAC 返回用户取消，夹具没有运行，也没有创建测试磁盘，不记作设备验收通过。
- 安全与风险：审查目标原生来源、专用 RW 分支、nonce／版本／长度校验、拒绝默认值、秘密清理及文件删除隔离；协议必须应用与 helper 同步更新。
- 安全与风险：清空提示、当次卡会话／目标绑定及卡派生密码尚未接产品；确认后复核期间拔卡的取消时序仍需补齐，不能宣称首写前撤销已实现；开始后的失败可留下部分密文。
- 验证限制：产品 DLL／允许列表保持旧版，未提供分区导出；页面磁盘创建仍禁用。NTFS 同句柄格式化、真实 helper 写入、实卡与物理盘测试待验，不用此前 RAW 或只读证据替代。
- 提交：按用户授权提交已审阅代码与明确未验收边界，哈希由 Git 历史追溯；未推送。下一步先完成隔离盘闭环再推进产品写入口。

### 2026-09-22 — P4.1 / NTFS helper 实测失败定位与分段报告
- 范围与审计：上一片已提交 ed32bac；用户要求提交后继续，先补上此前被 UAC 取消的隔离盘闭环，不绕过设备验收推进产品入口。
- 检查与实现：复核需求真源、同句柄候选补丁、helper 握手和自建 VHD 夹具；增加拒绝确认阶段的即时结果输出，为同意后的格式化异常标注阶段，避免后段失败掩盖前段结果。
- 验证：本轮首次 UAC 通过，原审阅候选 DLL（94D37A2D…0225309）下实际准备了自建 MBR／512 NTFS VHD；拒绝及抛错确认完成首 64 KiB／NTFS 身份不变检查，随后确认格式化返回 CreationError::io_error，夹具退出 1。
- 验证：此次失败明确不能算 NTFS 格式化／挂载读回通过；未定位到候选 DLL 内具体失败 API，也不能宣称写入从未开始。
- 验证：只在 out 构建了记录源码行号和系统错误码的临时诊断候选，无秘密／设备身份输出；诊断复测 UAC 返回用户取消，未运行，没有诊断结果。未继续重试提权。
- 验证：分段报告修改后的显式夹具构建通过；仅测试报告变化，不重复已有全套应用回归，不把构建通过当作真实设备通过。
- 安全与风险：失败镜像保留作证据，Get-DiskImage 已确认 Attached=false；没有残留 formatter/helper/test 进程。未操作生产分区，产品 DLL／允许列表及禁用的磁盘创建入口未改变。
- 安全与风险：诊断 include 已恢复至仓库版本，隔离运行目录恢复原 SHA-256 候选 DLL；临时诊断代码不进入提交。核心 I/O 错误仍待有管理员确认的定位／修复／完整重测。
- 提交：按持续授权自主提交夹具报告与失败记录，不推送；当前不能推进产品分区写入口，下一步定位 NTFS 同句柄失败点。

### 2026-09-22 — P4.1 / NTFS 锁卷后复核修复与真实 helper 闭环
- 范围与审计：上一片已提交 4bd218e；用户明确要求发起管理员，延续授权定位自建隔离盘 I/O 错误；复核候选同句柄锁卷／查询顺序与前次失败证据。
- 检查与实现：管理员诊断确认锁卷后文件系统信息查询返回 ERROR_NOT_READY（21）；改用 NTFS FSCTL 的第一次候选也在锁卷后返回 21，未作为成功。
- 检查与实现：最终在锁卷前读取 NTFS 元数据、引导扇区并检查类型／512 扇区／范围／序号，申请 extended-DASD；锁卷后通过同一原始句柄读取并比较完整首扇区，再卸载和核对设备范围。
- 安全与风险：不按路径重开、不放松锁卷／失败拒绝，不把查询失败当作 RAW；短读／引导字段不符／锁前后变化全部停止。原密钥锁页清理、取消及文件接口保持。
- 验证：最终候选 DLL 构建通过，SHA-256 878425F4…70F3AA；候选 include 与仓库源码摘要一致，无临时诊断代码。候选构建涉及既有上游文件，未宣称整体第三方静态分析通过。
- 验证：真实 --run-owned-vhd-helper 退出 0：拒绝／抛错确认保持首 64 KiB 和 NTFS 身份，单次同意后完成格式化／挂载／写入／卸载／重挂／读取。
- 验证：同次管理员批次 --run-owned-vhd-handle 退出 0，RAW 无效参数／占用边界不写入、取消／句柄保留、普通及隐藏文件容器格式化与挂载全部通过。
- 验证：成功镜像自动移除；4 个此前失败镜像保留为证据且逐一确认 Attached=false，不改生产分区。此次只改候选 DLL，未重复不受影响的 UI／应用全量测试。
- 验证限制：上述为本机隔离 VHD 后端／helper 验收；未验真实物理盘、GPT／4Kn、实卡、产品清空确认／会话撤销、原地加密与暂停续作。
- 提交：按用户持续授权在验证和安全审查后自主提交，不推送；产品依赖仍为 416C9D…3F184，允许列表及磁盘创建禁用状态不变。


### 2026-09-22 — P4.1 / 卡会话分区格式化接线
- 范围与审计：上一片 3593d80 已提交；复核需求分区增补、设计计划及认证／挂载会话、helper 协议和现有测试，落实卡派生的独立格式化边界。
- 范围与审计：MILESTONES 中早期“候选待提交／helper 待接”属历史滞后，实际进展以本日志及设计证据为准；本片不将 P4.1 标为完成。
- 检查与实现：新增原生会话格式化方法和 production runtime 委派，复用 diskA／A 盐派生 64 字节锁页密码；不走文件分配、发布、索引或挂载。
- 检查与实现：冻结目标与回调，确认只消费一次；新增不持久化的认证代际，清理及同根重新登录使旧操作失效，拒绝格式化重入。
- 安全与风险：强制确认及取消查询，无认证／预取消在派生前停止；派生长度错误、拒绝／抛错确认、helper 失败不自动重试或删除目标，异常由 SecureBuffer 清理秘密。
- 验证：新增 14 个模拟场景，覆盖固定目标、单次同意、取消、会话撤销／重建、缺少确认／取消查询、短派生、helper 失败、重入及失败后新尝试。
- 验证：主程序和相关测试构建通过；认证、卡会话、设备生命周期、UI 会话、WebView 桥、关闭保护六组 CTest 全通过。
- 验证：卡会话／认证源文件严格 /W4 /WX /sdl /analyze 检查通过；差异检查通过。未重新运行未修改的设备后端实测，未执行实卡联合验收。
- 安全与风险：会话操作要求现有串行工作线程，代际不提供跨线程同步；确认后 helper 复核至首次写入的取消窗口仍待处理，取消可能留下部分密文。
- 后续：先完成分区记录、重新识别与挂载，再接产品清空确认及设备撤销验收；产品 DLL／允许列表、页面磁盘创建禁用状态不变。
- 提交：遵循用户持续自主提交授权，验证后提交本片；禁止推送，提交哈希以 Git 记录为准。


### 2026-09-22 — P4.1 / 分区身份加密记录与混合索引
- 范围与审计：上一片 5abb270 已提交；复核 R-03、分区增补、设计记录以及索引 codec／view／store、卡会话与挂载校验，先完成可独立验证的持久格式。
- 检查与实现：槽内新增 v3 分区身份，复用固定 786432 字节索引与共享卡根密钥；仅存稳定设备身份、MBR 布局／512 扇区和分区范围，不存路径、盘号、旧 NTFS 序号或角色。
- 检查与实现：新增分区列举／查重／增删，文件视图只读文件记录；修正新增文件把分区槽视为占用，文件排序／删除保留分区槽原字节。
- 安全与风险：固定容量、随机填充及随机槽位置保持；所有字段在 GCM 内认证，非法范围／标志／布局拒绝，删除先暂存，失败新增和满表不修改索引。
- 验证：新增 768 字节逐字节篡改、独立解密布局、异根拒绝、畸形明文重封、混合操作、重读及满表保护测试；索引 codec／view／store 与卡会话四组 CTest 全通过。
- 验证：首次构建因测试 optional 比较使用有符号字面量触发 /WX；修为无符号后主程序和测试构建通过，最终两处 /W4 /WX /sdl /analyze 静态分析通过。
- 安全与风险：旧程序不认识 v3，不能安全编辑含分区记录的索引；产品写入前需落实降级混用限制，不能承诺旧版向前兼容。
- 后续与限制：本片不自动记录格式化结果，不做设备 I/O；重新发现、句柄复核、认证挂载及 UI 接线仍待完成，产品入口未开放。
- 提交：按用户持续自主提交授权验证后提交，不推送；提交哈希见 Git 记录。


### 2026-09-22 — P4 / 阶段归属纠正与 P4.4／P4.5 顺序互换
- 范围与审计：用户明确要求先完成已有产品接入基础，交换原 P4.4／P4.5；复核 18 笔功能提交、需求与阶段表，纠正此前把实现全部标为 P4.1 的偏差。
- 当前顺序：P4.1 验证与范围锁定 → P4.2 安全选择 → P4.3 空分区加密 → P4.4 产品会话与盘符管理 → P4.5 原地加密与正常暂停续作 → P4.6 专项验收。
- 检查与实现：18 笔提交按主要内容改前缀，原始发生顺序不变；P4.1 两片、P4.2 九片、P4.3 五片、P4.4 两片。已有 P4.4 为统一入口与混合加密索引，不代表记录接线／重新识别／挂载已完成。
- 检查与实现：需求真源、里程碑及设计文档的当前口径已同步；后续完成 P4.4 才开展 P4.5，原地转换与暂停续作未实现，不能漏做或以格式化代替。
- 验证：重写前工作区干净；逐笔确认不在本地 origin/main 祖先历史内，仅修改前缀和必要父提交引用；18 笔文件树逐一一致，作者／日期／正文保留，重写前后最终文件树无差异。
- 验证：本片只改提交元数据和文档，执行差异／阶段映射一致性检查，不重跑产品测试，不新增设备或实卡验收结论。
- 安全与风险：P4.1 原地转换只有源码审计；P4.2 特殊目标实测、P4.3 首写前取消及产品闭环、P4.4 索引降级兼容等历史缺口继续保留，不因改号转为完成。
- 提交：按明确授权重写并提交文档，不推送；旧链备份在 refs/codex/backups/p4-phase-order-20260922。此前日志中的旧哈希和阶段名保留历史含义，下表为当前映射。

| 原提交 | 当前提交 | 正确归属 | 切片 |
|---|---|---|---|
| `3d3585f` | `3d3585f` | P4.1 | Audit partition encryption boundaries and narrow resume scope |
| `67e26a2` | `1e4e7a5` | P4.2 | Add read-only partition candidate discovery |
| `f9c2834` | `555cd7c` | P4.2 | Connect partition selection and refresh UI |
| `c4f94a1` | `253bdff` | P4.1 | Verify partition encryption on an owned virtual disk |
| `f9883d6` | `7355bc7` | P4.3 | Add same-handle partition format backend candidate |
| `08e98c9` | `4af2d0f` | P4.4 | Unify disk and file container creation entry |
| `58c5522` | `2c7625e` | P4.2 | Revalidate partition snapshots before selection |
| `68c2d3f` | `ac1a9f5` | P4.2 | Bind partition candidates to native device identities |
| `1d2930a` | `bbeb755` | P4.2 | Verify partition identity on caller-owned handles |
| `3820975` | `db18a58` | P4.2 | Add a dedicated partition identity check protocol |
| `f341a13` | `8c2afa1` | P4.2 | Route read-only partition checks through the elevated helper |
| `d5d1c80` | `865f54a` | P4.2 | Connect partition identity checks to the creation wizard |
| `c5edfd9` | `8b0231b` | P4.2 | Reject protected and uncertain partition targets |
| `ed32bac` | `165d1b7` | P4.3 | Add a confirmed partition formatting helper boundary |
| `4bd218e` | `3418109` | P4.3 | Separate NTFS helper refusal and format results |
| `3593d80` | `048860c` | P4.3 | Verify locked NTFS targets through the retained handle |
| `5abb270` | `8b6b81f` | P4.3 | Bind partition formatting to authenticated card sessions |
| `3cafcaa` | `8103033` | P4.4 | Store partition identities in the encrypted container index |


### 2026-09-22 — P4.4 / 密文分区重新识别
- 范围与审计：复核交换后的阶段定义、分区需求、原 NTFS 候选模块、索引身份与隔离夹具；本片实现加密后定位能力，不扩大布局支持或开始 P4.5。
- 检查与实现：独立 PartitionLocation 结构枚举跳过文件系统查询，以稳定设备身份和分区范围定位；原 PartitionCandidate／格式化检查继续要求 NTFS，类型不混用。
- 检查与实现：提取共享 PartitionIdentity，索引 v3 编码及有效范围不变；允许当前盘号／分区号／卷路径变化，拒绝身份／范围变化、重复匹配、路径歧义、非法 GUID 路径及超限列表。
- 检查与实现：新增借用句柄的结构身份复核，不重开、关闭或移动游标；匹配只是候选定位，不证明内容或授予写权限。
- 验证：新增结构筛选、无文件系统／NTFS 区分、编号变化、替换、缺失、重复／大小写歧义及句柄所有权测试；最终分区、索引 codec／view、协议、卡会话五组 CTest 全通过。
- 验证：主程序／helper／夹具构建通过；静态分析发现夹具原有 64 KiB 栈缓冲区告警，改堆缓冲区后分区模块与夹具 /W4 /WX /sdl /analyze 均通过。
- 验证：两次管理员自建 VHD 闭环退出 0，最终 out/p44-location-final-20260922-155505 验证同身份找到格式化后的密文分区、同句柄成功／替换拒绝、创建候选仍拒绝密文，以及挂载写读／卸载重挂。
- 验证：最终夹具镜像已移除，无残留测试／helper 进程；结果检查 shell 因查询不存在进程返回 1，与实际夹具 exit.txt=0 区分，不记为设备失败。未操作生产分区。
- 安全与风险：仍需正式挂载边界、会话记录／UI 接线；身份不防伪造或同址重格式化，不消除枚举后竞态，后续必须复核并认证挂载。未验硬件热插拔／跨重启、实卡或完整 P4.4。
- 提交：用户要求撤回的 docs 98fbcc8 已保留，作为本片唯一特例合并；后续独立 docs 改动恢复单独提交。本片按持续授权验证后提交，不推送；产品依赖及禁用的磁盘创建入口不变。
### 2026-09-22 — P4.4 / 正式分区挂载与会话记录
- 范围与审计：用户要求连续完成 P4.4 并自主提交；检查驱动挂载、索引读写、容器采纳、卸载、维护和密文导出路径。
- 检查与实现：新增只接受稳定身份的正式分区挂载，重新枚举唯一定位、保留查询句柄复核并再次检查位置；设备路径由原生盘号／分区号构造，文件接口保持。
- 检查与实现：分区进入统一会话列表，以卡根标准派生挂载；登录读取加密分区记录，支持卸载及忘记；原生拒绝分区隐藏卷维护和复制容器文件。
- 检查与实现：用户确认后、允许 helper 写入前保存身份；索引失败拒绝格式化，后续失败保留未挂载记录，避免已经写入却丢失位置。创建要求启用容器记忆，不默默绕过偏好。
- 验证：新增创建／拒绝／索引失败／格式失败／挂载失败及跨登录恢复、挂载、卸载、忘记与维护／导出拒绝场景；卡会话和 VolumeProvider 两组完整回归通过。
- 验证：主程序／helper／夹具构建、provider 与会话严格静态分析通过；自建 VHD 改用正式 mount_partition，真实格式化、发现、挂载写读、卸载重挂退出 0，夹具清理完成。
- 安全与风险：查询身份不防伪造硬件或同卡克隆，驱动仍独立认证卷头；保留句柄不代表热拔插绝对原子性。记录可能对应被取消／部分格式化目标，不表示加密完成，不自动重试。
- 后续：产品创建 UI、原始挂载点清理、取消等待和最终联合验收继续下一片；目前不开放页面入口。
- 提交：按持续授权提交代码与本片开发记录，不推送；独立 docs 改动仍单独提交。

### 2026-09-22 — P4.4 / 产品创建入口与原始盘符移除
- 范围与审计：复核需求 8.9、阶段表、原生桥接、创建取消、helper 写入及文件导出；按用户授权连续开发并自主提交。
- 检查与实现：默认磁盘入口接到认证会话；明确勾选清空全部数据后提交本次选择 ID，原生消费快照并绑定卡 epoch，错误目标、过期选择、重放均拒绝。
- 检查与实现：UI 展示分区支持范围和记住容器前提；取消／失败不承诺磁盘未修改，也不自动重试；分区密文导出在页面禁用，原生继续拒绝。
- 检查与实现：helper 在确认和二次策略检查后，仅删除保留句柄所绑定分区的盘符／目录挂载入口，逐个复核卷 GUID；不修改分区表或全局自动挂载策略，不恢复可能指向部分密文的入口。
- 检查与实现：创建取消携带卡中断和 epoch；确认后等待 helper 静默策略查询时主动轮询发送取消，helper 在写入及移除访问入口前后检查。
- 验证：首次构建暴露确认回调签名不匹配，修正后主程序、helper 和相关测试构建通过；会话、UI 控制器、桥接三项 CTest 通过。
- 验证：原有分区 DOM、新增创建成功／失败／取消 DOM、268 项页面导航检查通过；helper 与客户端 /W4 /WX /sdl /analyze 通过。
- 验证：out/p44-access-run-path.txt 指向本次管理员自建 MBR/512 VHD 验收；拒绝确认保留原盘符及前 64 KiB，确认后移除全部原始入口，正式 provider 挂载／写入／卸载／重挂／读回通过，退出 0 且测试盘已清理。
- 安全与风险：盘符重新被外部程序分配不在应用控制内；不承诺隐藏分区存在性、断电无损、实体介质兼容或完整 P4.6。候选 DLL 尚待提升为产品固定依赖，后续继续生命周期收尾。
- 提交：用户已授权无需逐片确认；本片验证与安全审查后提交，禁止推送。

### 2026-09-22 — P4.4 / 拔卡竞态与盘符冲突收束
- 范围与审计：创建／盘符片已提交 e821b3f；复核驱动挂载期间拔卡、强制卸载失败、盘符耗尽和重复分配，以及记录移除提示。
- 检查与实现：驱动挂载返回后再次检查创建取消；若已拔卡则按已登记会话卸载，卸载失败仍保留准确挂载状态供生命周期重试，不吞掉残留盘符。
- 检查与实现：分区移除提示明确只删除记录、不解密分区，并说明当前没有通过文件导入找回分区的入口；避免继承文件容器的恢复承诺。
- 验证：新增五种分区会话场景覆盖正常拔卡、挂载中取消、清理失败后重试、无盘符和重复分配；两次失败均为混合夹具未切换密码长度／未填容量，补齐后会话 CTest 全部通过。
- 验证：主程序构建通过；创建 DOM 同时检查分区导出禁用及移除提示，成功／失败／取消用例通过；会话 /W4 /WX /sdl /analyze 通过。
- 安全与风险：取消为协作式，驱动调用期间无法原子阻止瞬时挂载；后置检查和会话清理保证可追踪与重试，不宣称物理拔卡任意时点的无损停止。实卡端到端点击仍属待验硬件边界。
- 提交：依据用户授权自主提交，禁止推送；下一片提升正式格式化依赖并完成组合验收。

### 2026-09-22 — P4.4 / 正式后端接入与组合验收准备
- 范围与审计：生命周期片已提交 bcfe8d3；复核正式 DLL 来源、固定哈希、源码分发清单、页面完成反馈和快格式化边界，继续执行用户自主提交授权。
- 检查与实现：固定 VeraCrypt b48e31f5b47da7d41025e3f0e02751675e15005a；候选 include 与仓库 SHA-256 相同，独立审查后提升已通过隔离试验的 878425F4…70F3AA DLL，旧 DLL 本地备份保留。
- 检查与实现：主程序和打包固定哈希同步，分发清单新增 partition.patch／UsbVaultPartition.inl；生成部署值与构建 DLL 摘要一致，打包脚本语法校验通过，本轮未制作安装包。
- 检查与实现：分区完成反馈发布实际挂载盘符；界面明确快速格式化不保留文件，也不安全擦除历史残留。新建容器不是已有数据的原地加密。
- 检查与实现：增加只接受自建 VHD 的会话组合验收，测试认证秘密＋真实 KDF／加密索引文件／helper／驱动，覆盖重开会话、另一登录输入、文件管理与撤销认证后卸载；增加确认后静默策略查询期间取消检查，不访问用户卡、配置或索引。
- 验证：完整构建通过。全量 CTest 首轮 68/70；页面集成夹具缺少新控件、契约未包含 partition 字段，修复后对应测试通过；会话最终测试通过。
- 验证：另一失败为既有宿主 KnownFolder 重定向；普通 Explorer 桌面先验证请求路径等于最终路径，随后同一构建 helper／artifact 2/2 通过，证据 out/p44-desktop-regression-20260922-164410。不声称初次全量运行全部通过。
- 验证：真实 WebView／COM 假后端集成退出 0；两组分区 DOM 通过，先前 268 项导航检查通过。夹具静态分析发现 64 KiB 栈数组，改为堆缓冲后通过；会话最后变更 /W4 /WX /sdl /analyze 通过。
- 验证限制：最终三组管理员重测在启动前被 Windows 返回“操作已被用户取消”，未执行；待用户同意重发 UAC，准备目录 out/p44-final-20260922-163752。新增真实会话组合和静默取消断言暂不得标记通过。
- 安全与风险：已有管理员隔离格式化、盘符移除及正式 provider 读写证据仍有效；实卡／实体盘、跨重启／热插拔、GPT／4Kn 及 P4.5 原地转换不在已验范围；不承诺断电无损或历史残留安全擦除。
- 提交：本片实现与非管理员验证自主提交，禁止推送。P4.4 产品实现已接齐，最终新增组合验收待 UAC；后续文档按常规单独提交。

### 2026-09-22 — P4.4 / 范围与交付文档回写
- 范围与审计：7864efe 已提交正式后端及收尾变更；按用户规则恢复普通 docs 独立提交，本轮不混入功能变更。
- 检查与实现：阶段表、需求真源和设计记录同步当前单分区 MVP，保留 P4.5 原地转换／正常暂停续作与 P4.6 硬件专项边界；明确快格式化残留、记住容器、移除记录和 v3 索引降级限制。
- 检查与实现：第三方构建说明与 NOTICE 同步全部三份补丁、两个 include 和正式 DLL 哈希；旧候选及几何 DLL 状态标作历史，禁止读者误认为仍需旧允许列表。
- 验证：核对实际提交、固定哈希、构建产物、已执行测试与取消的 UAC；文档不把首轮 68/70 改为全过，也不把新增组合验收写成通过。
- 安全与风险：P4.4 产品实现已接齐，最后新增管理员组合验收仍待确认；没有另起 P4.5，没有推送、实卡操作、生产分区破坏测试或安装包交付。
- 提交：用户授权自主提交；本片为独立 docs 提交，禁止推送。

### 2026-09-22 — P4.4 / 最终管理员验收通过与 MVP 结项
- 范围与审计：用户明确要求“启动管理员 uac”，重新授权此前取消的最终隔离验收；检查准备脚本仅允许程序自建 VHD，无现有磁盘输入。
- 检查与实现：本轮不改产品代码；管理员运行 session／helper／handle 三种模式，核对真实 KDF、加密索引、helper、驱动、文件管理和正常／隐藏文件容器回归。
- 验证：session 退出 0；测试认证根秘密下创建、写入、卸载、重开会话、另一登录输入重挂／读回、文件管理及撤销认证后卸载全部通过，不把测试秘密称为真实安全卡认证。
- 验证：helper 退出 0；拒绝／抛错确认保留原盘符与前 64 KiB，确认后静默策略查询期间取消保持入口及内容，成功创建移除原入口，身份重新识别和正式 provider 读写闭环通过。
- 验证：handle 退出 0；同句柄范围／偏移／占用拒绝、取消及句柄保留、普通／隐藏文件容器创建挂载回归通过。
- 验证：证据 out/p44-final-20260922-163752，三份 exit 均为 0、三份 stderr 均为空、无 partition-fixture-* 目录残留；此前 UAC 取消记录保留为历史，不覆盖或删去。
- 安全与风险：未操作用户生产分区、实卡、个人配置或索引；MBR／512 NTFS 输入快格式化 MVP 已验，不扩展为 GPT／4Kn、实卡／实体盘、跨重启／物理热拔插或断电无损保证。
- 当前结论：P4.4 产品会话与盘符管理的单分区 MVP 完成；P4.5 原地加密与正常暂停续作尚未开展，P4.6 硬件专项验收仍保留。阶段表、需求真源和第三方说明同步结项边界。
- 提交：沿用用户自主提交授权，本轮仅 docs 独立提交，禁止推送；无安装包交付。

### 2026-09-22 — P4.4 / 用户指定三笔提交合并
- 范围与审计：用户指定截图中的 7864efe、b7127e4、7dccb08 合为一笔 P4.4；合并前工作区干净，精确核对三笔提交及父提交 bcfe8d3。
- 检查与实现：三笔合并为 7d60205，原文件树逐字节不变；备份引用 refs/codex/backups/p44-before-squash-20260922 保留原历史。
- 验证：git diff 原备份引用与新 HEAD 无差异；只改提交组织，不重复已通过验收。前文旧哈希为历史引用，以上三者当前共同归属 7d60205。
- 提交：按本次明确授权完成合并；此为指定提交的特例，后续普通 docs 仍独立提交，禁止推送。

### 2026-09-22 — P4.5 / NTFS 原地转换布局与逆向块调度
- 范围与审计：复核需求原地保留数据／正常暂停续作边界及固定上游 InPlace.c 的收缩、备份卷头、逆向转换与进度更新逻辑；不复用 P4.3 的清空格式化操作，不添加任意断电恢复层。
- 检查与实现：新增独立布局规划，验证 MBR／512 范围所需的 NTFS 扇区／簇数、空闲空间、溢出与一致性，计算收缩目标、128 KiB 数据偏移、256 KiB 卷头保留及备份卷头位置。
- 检查与实现：保留可读句柄上查询 NTFS dirty 状态与卷数据，检查前后重新核对分区身份；不打开新路径、不锁卷、不收缩、不移动簇、不改变文件位置或写入分区。
- 检查与实现：逆向块调度上限 2 MiB，拒绝非连续尾部加密范围、未对齐、溢出和越界；计算源／目标偏移和数据单元号，要求读完整块后才写重叠目标。
- 验证：主程序及测试构建通过；partition_inplace_plan／partition_catalog／volume_provider／creator_protocol 四项 CTest 通过；内存搬移模型验证未读原文不被覆盖、续作无缺口、尾部卷头不被触碰和最大尺寸计算。
- 验证：管理员 --run-owned-vhd-inplace-plan 在自建 MBR／512 NTFS VHD 上通过，重复规划相同、前 64 KiB 与调用者文件位置不变、错误设备身份拒绝；退出 0、stderr 空、夹具已清理，证据由 out/p45-plan-run-path.txt 指向。
- 验证：布局模块与扩展隔离夹具 /W4 /WX /sdl /analyze 通过；不重复未受影响的全量 UI 回归。
- 安全与风险：布局可计算不证明文件系统实际可收缩，检查不是写授权。续作范围必须来自已认证且绑定目标的备份卷头；当前调度无 IO、无加密、不保存进度，数据块处理完也不代表卷头最终化完成。
- 后续：接入受限后端的卷头认证、NTFS 准备与逆向转换；正常暂停必须在保存并刷新进度成功后报告。实卡／实体盘及完整 P4.5 尚未验收，页面不开放原地转换。
- 提交：沿用用户验证和安全审查后自主提交授权；本片为 P4.5 首个实现切片，禁止推送。

### 2026-09-22 — P4.5 / 首片范围文档同步
- 范围与审计：首个实现切片 b7beefa 已提交；阶段表中“尚未开展”的当前口径已过期，按 docs 单独提交规则修正。
- 检查与实现：阶段表注明 P4.5 已开始；设计补充只读布局、逆向完整块读取、已认证续作范围及暂停成功的边界，不把算术调度称为实际加密。
- 验证：文档对照本片头文件、固定源码与已完成测试，不改变代码或重跑未受影响测试；差异检查通过。
- 安全与风险：实际转换、卷头认证／进度刷新与暂停续作仍待实现；首片只读 VHD 验证不替代完整 P4.5 验收。
- 提交：用户持续授权，自主独立 docs 提交，禁止推送。

### 2026-09-22 — P4.5 / 转换执行与正常暂停收尾骨架
- 范围与审计：用户指定 b7beefa 与 7d11430 合为 0ffefa8，git diff 验证树不变；原历史保留于 refs/codex/backups/p45-before-squash-20260922，旧哈希仅作历史引用。
- 范围与审计：复核需求正常暂停／卡撤销及上游 InPlace.c 进度卷头行为；本片实现私有同步执行边界，不复用格式化，不接页面写入入口。
- 检查与实现：后端负责同句柄身份、独占、备份卷头认证与算法／标志校验；执行器再次验证连续尾部范围，不接受页面进度，不持有密码或复制上游加密实现。
- 检查与实现：逆向完整读取、调用后端加密、完整写入后才推进内存范围；短读写和异常立即停止，不重试、不发布失败块进度，所有退出路径清理后端。
- 检查与实现：正常暂停保留已观察到的请求，当前块收尾后先刷新数据、保存认证卷头进度、再次刷新，全部成功才返回 paused；完成仍须最终化卷头并刷新。
- 检查与实现：写前再次观察撤销；写后撤销仅尽力保存完整范围并返回 revoked，绝不等同正常暂停；失败不承诺可安全续作。
- 验证：首次新测试因 2 MiB VirtualLock 配额不足失败，诊断确认为系统锁页配额；改为从调度块尾部消费 64 KiB，保持失败关闭而非退回未锁页内存。
- 验证：最终正式应用／helper 构建、五项 CTest 与执行器／测试 /W4 /WX /sdl /analyze 通过；内存磁盘验证暂停续作、尾部不整块、原文一致及卷头范围隔离。
- 验证：逐个调用位置注入完成／暂停路径异常（包括最终刷新），另测短读、部分写入、错误范围和写前／写后／保存期间撤销；失败不报告暂停或完成，清理执行一次。
- 安全与风险：64 KiB 明文缓冲锁页、逐块及析构清零；后端契约不是已完成的设备安全门禁。模拟加密仅为测试标记，不证明真实密码学或设备持久性；未运行管理员磁盘测试，未替换正式 DLL。
- 后续：实现外部分发 DLL 的备份卷头认证、NTFS 准备及变换适配，接 helper／卡会话并验真实重开续作；P4.5 未完成，不保证断电／强杀无损。
- 提交：按持续授权在验证和安全审查后自主提交本片；指定两笔合并是特例，后续普通 docs 仍单独提交，禁止推送。

### 2026-09-22 — P4.5 / 真实 NTFS 转换候选后端
- 范围与审计：继续用户一口气完成 P4.5 的授权；复核固定上游 InPlace.c 的 NTFS 收缩、逆向数据偏移、起始区保护和卷头最终化，算法保留在外部分发 DLL。
- 检查与实现：新增可分发 include／增量补丁及动态适配器；固定 MBR／512、AES／SHA-512／PIM 0 和 64 字节密码，同句柄几何检查、收缩、独占与卸载，无按路径重开和格式化回退。
- 检查与实现：原始 NTFS 起始 8192 字节暂时异或保护，转换该范围时还原再加密；备份卷头认证范围与密码，续作不重生成数据密钥，完成后写主卷头。主备预留区与隐藏卷头槽随机覆盖。
- 检查与实现：执行器在 64 KiB 锁页缓冲中逆向转换，刷新数据／保存进度／刷新卷头后暂停；适配器关闭释放锁与密钥。尚无生产 helper、卡会话或页面入口。
- 验证：两轮管理员自建 NTFS VHD 真加密均退出 0；在已有文件上暂停，错误密码拒绝续作，重新打开同一目标正确续作并挂载，原文件全部字节一致，夹具清理完成，stderr 空。
- 验证：最终证据目录由 out/p45-inplace-final-run-path.txt 指向；正式应用／helper 构建、四项相关 CTest 和适配器严格静态分析通过。
- 安全与风险：候选 SHA-256 6293F2E65D39E3344735F7EE1FFC0587CC8E97731DD9926A26A44386F3A28F22；正式依赖未替换。密码锁页清零，无明文日志／侧车秘密；候选接口本身不提供用户目标授权。
- 安全与风险：收缩失败明确终止，不搬移无法收缩的尾部簇；收缩后后续失败可留下已缩小 NTFS 或部分转换，不能从头覆盖。任意断电／强杀无损及实卡／实体盘仍未验。
- 后续：产品记录须区分正常暂停与不确定失败，未完成禁止挂载；接线须补齐写前撤销观察、提权复验、独立开始／续作和原卡认证。
- 提交：已报告实现、验证及剩余边界，按持续授权自主提交候选后端片，禁止推送。

### 2026-09-22 — P4.5 / 提权开始、续作与独立暂停协议
- 范围与审计：在候选后端 20b9cbe 上接入独立 helper 开始／续作操作；协议 v8，旧版本拒绝，应用／helper 须同步更新。
- 检查与实现：开始转换保留原 NTFS 系统／分页／BitLocker 门禁、同句柄确认后二次复验及原始入口移除；不复用格式化操作，结果分别表示完成与已刷新正常暂停。
- 检查与实现：暂停使用独立 nonce 绑定消息，取消／撤销可在暂停之后继续到达并优先处理；错误 nonce、格式化入口收到暂停、旧协议和畸形参数拒绝。
- 检查与实现：候选 DLL 准备阶段在收缩及写卷头前查询撤销；已有备份卷头写入后的 NTFS 保护收尾仍完成，后续数据块停止，不承诺等同正常暂停。
- 验证与修复：首次真实 helper 续作被拒绝；诊断确认身份／句柄正确，但 RAW 卷 WMI 文件系统相关用途属性为空。未将空值当 false，也未放宽初始 NTFS／BitLocker 门禁。
- 安全与风险：新增仅供续作的独立条件链：结构枚举排除当前 Windows／活动／非普通 MBR，RAW 同句柄复核、驱动已有挂载拒绝、独占锁、保护后的 NTFS 引导签名和卡派生密码认证的原地备份卷头。它不是任意 RAW 写许可；产品还须以加密索引中的正常暂停记录授权，待下一片接入。
- 验证：修复后及最终候选两轮真实 helper 自建 VHD 开始、暂停、独立 helper 重开、错误密码拒绝、续作和原文件逐字节读回通过。最终 exit=0、stderr 空，夹具已清理；证据 out/p45-helper-reviewed-path.txt。
- 验证：正式应用／helper 构建、四项 CTest 与协议／客户端／helper／适配器／策略模块严格静态分析通过；未把此前诊断失败改写为通过。
- 安全与风险：最终候选 SHA-256 242EAB54B82ED682150AE5E7A171A45C4A227753D3F266D9E95791622F0F5382；诊断代码已移除，正式依赖未晋升，页面未开放。
- 提交：已报告实现、验证及风险，按持续授权提交本片后继续产品状态与卡会话；禁止推送，不扩大断电／强杀恢复承诺。

### 2026-09-22 — P4.5 / 加密暂停许可与卡会话闭环
- 范围与审计：P4.5 必须区分正常暂停与异常未完成，并阻止未完成挂载；复核固定槽 GCM 索引与现有卡根 diskA 派生，未引入密码角色或隐藏卷标记。
- 检查与实现：v4 分区槽在原固定尺寸加密文件内保存 uncertain／paused；完成回到 v3 普通分区记录。单记录更新保留身份／排序和无关槽，缺失、重复及非法状态拒绝。
- 检查与实现：原生会话开始前保存 uncertain，续作前核对内存和持久记录均为 paused，再先持久消费为 uncertain；记录失败拒绝本次磁盘写入，不从异常自动续作。
- 检查与实现：根秘密派生固定密码并绑定当前认证代际；设备重新定位必须匹配已保存身份。独立暂停／撤销回调，正常暂停只在 helper 刷新完成后持久保存，失败保留 uncertain。
- 检查与实现：未完成卷单独挂载拒绝、批量挂载跳过、移除记录拒绝；正常完成才恢复普通挂载，挂载期间撤销沿用跟踪后强制清理。两种有效登录输入共享同一卡根视图。
- 验证：三项索引／卡会话 CTest 通过，覆盖记录写失败、拒绝确认、撤销、错误密码长度、错误设备、失去认证、重开续作及全部 768 字节单点篡改拒绝；无关槽逐字节不变，重复身份更新不改文件。
- 验证：正式应用构建及三处严格静态分析通过；管理员自建 VHD 使用真实 KDF／加密索引／helper／驱动完成暂停、销毁重建会话、另一有效登录输入续作、原文件全部字节读回及撤销卸载，exit=0、stderr 空、夹具已清理，证据 out/p45-session-run-path.txt。
- 安全与风险：实卡根认证用合成已认证根替代，未当作实卡／实体盘专项通过；窗口／页面尚未接线。v4 索引不得由不支持 v4 的旧版编辑，否则不能保证记录保留。
- 提交：已报告实现、验证及剩余边界，按持续授权自主提交本片后继续页面与组合验收，禁止推送。

### 2026-09-22 — P4.5 / 页面保留数据、暂停与续作入口
- 范围与审计：接齐已有 NTFS 转换与认证暂停许可；新建仍默认磁盘，保留原清空创建行为，勾选保留数据后改为独立转换操作。
- 检查与实现：切换模式清除旧确认；选中快照与认证代际绑定，原生层消费单次确认，不向页面暴露句柄、路径或派生秘密。
- 检查与实现：正常暂停使用独立结果，不画为 100% 完成；窗口关闭请求暂停并等待收尾，用户收到已保存结果后才可退出。
- 检查与实现：列表显示暂停／异常未完成，暂停可继续；异常不提供自动重试，未完成拒绝挂载／导出／移除，自动与批量挂载跳过。
- 验证与修复：新 DOM 测试最初等待条件早于认证完成；隐藏 WebView 夹具缺少新增控件与进度标签，补齐后重测通过，未将失败算作通过。
- 验证：主应用构建、四项原生界面／关闭契约 CTest、268 项导航、分区选择／清空创建 DOM、新增转换 DOM 和真实隐藏 WebView（exit=0）通过；控制器与 COM 桥严格静态分析通过。
- 安全与风险：复核页面仅发原生 id、卡撤销与暂停分离、失败保留不确定记录；页面不授予绕过后端的写权限。候选晋升和最终组合验收仍待下一片，实卡／实体盘仍未验。
- 提交：已报告实现、验证与边界，沿用持续授权提交；禁止推送。

### 2026-09-22 — P4.5 / 受审后端晋升与最终磁盘组合验证
- 范围与审计：页面片 5cd7b3d 完成后晋升已审候选；复核独立 DLL 分发、固定哈希校验和源码义务，不引入运行时信任可变清单。
- 检查与实现：正式依赖改为 242EAB54B82ED682150AE5E7A171A45C4A227753D3F266D9E95791622F0F5382，保留 P4.4 本地副本；构建与打包白名单同步，分发资产补入第四补丁和第三 include。
- 验证：在固定上游文件上顺序应用四补丁，全部五个修改文件和三个 include 与受审构建输入一致，仅忽略 BOM／换行；首次工作副本 CRLF 补丁应用失败，恢复仓库规定 LF 后通过，非实现差异。
- 验证：正式应用／helper 与全部默认测试目标构建通过；脚本语法、八项源码资产、生成白名单和实际 DLL 摘要一致；本片未制作安装器。
- 验证：最终管理员 inplace-session／inplace-helper／session／handle 四组均 exit=0、stderr 空，自建夹具目录全部清理；证据 out/p45-final-20260922-181852。
- 验证：覆盖真实 KDF／加密索引／重开认证会话／独立 helper 暂停续作、错误密码拒绝、原文件全部字节比对、未完成挂载拒绝、撤销卸载、原有分区及普通／隐藏文件容器回归。
- 安全与风险：复核同句柄界限、认证卷头范围、先刷新后发布进度、锁页清零、RAW 独立续作授权及失败不自动重试；任意断电／强杀不保证无损，实卡／实体盘未验。全量 CTest 正在运行，最终结果写入结项记录。
- 提交：已报告实现、验证与剩余限制，按持续授权提交后单独同步阶段文档；禁止推送。

### 2026-09-22 — P4.5 / MVP 结项与阶段文档同步
- 范围与审计：执行器 06a7110、受限后端 20b9cbe、提权协议 ccc0a2c、持久许可／会话 a6bd964、页面 5cd7b3d、正式晋升 98f1e52 已按独立边界提交；普通文档继续单独提交。
- 检查与实现：需求源、阶段表、设计和第三方边界同步保留数据／清空两种入口、正常暂停许可、RAW 续作复验、v4 索引和 v8 helper 兼容要求。
- 验证：最终全量 CTest 首轮 71/72，唯一失败为既有宿主目录重定向触发 helper 路径保护；其余全部通过，不将首轮写为全通过。
- 验证：首次普通 Shell COM 启动仍继承重定向，路径检查阻止测试；改由已存在的 Explorer 桌面派发，先证实请求路径等于最终路径，再以同一构建通过 helper／artifact 2/2。证据 out/p45-desktop-regression-20260922-182142，不放宽安全门禁。
- 验证：四组最终管理员自建 VHD 组合全部通过；真实 KDF／索引／新会话／独立 helper 正常暂停续作、原文件逐字节一致，旧分区和普通／隐藏文件容器回归通过，成功夹具全部清理。
- 验证：页面新增 DOM、原有分区 DOM、268 项导航、真实隐藏 WebView 和相关严格静态分析通过；四补丁／三 include 源码对照、生成白名单、包源资产及差异检查通过。未制作安装包。
- 安全与风险：完成安全审查；未完成状态失败关闭，不持久化秘密／档位标记，不以百分比代替认证进度。收缩失败、异常 IO、撤销或状态保存失败不承诺自动恢复；任意断电／强杀无损不在范围内。
- 结项：P4.5 单分区最小可行实现完成。卡认证夹具仍用合成已认证根，实卡／实体盘、真实热插拔及兼容矩阵留 P4.6，加固交付留 P5；未把隔离验证宣称为实体硬件专项完成。
- 提交：按持续授权单独 docs 提交结项，禁止推送。

### 2026-09-22 — UI / 分区选择显示盘符与卷标
- 范围与审计：用户反馈磁盘号／分区号不足以识别 D／T 等目标；对照 P4 安全选择要求补充显示信息，不改变加密后端、身份记录或阶段范围。
- 检查与实现：只读查询枚举所得卷的盘符与卷标，保存在当前认证扫描快照；原生按快照 id 返回，失效／未知 id 拒绝，不接受页面路径。
- 检查与实现：列表第一行显示盘符及卷标，第二行保留磁盘／分区号；容量不折行，选中确认和按钮可访问名称包含同一卷名。
- 检查与实现：无盘符、未命名和读取失败分别提示；不暴露目录挂载路径、卷 GUID 或秘密，显示信息不用于设备身份或写入授权，也不持久化。
- 验证：正式应用／helper 构建、三项 CTest、分区选择／清空创建／保留数据暂停续作 DOM 及真实隐藏 WebView（exit=0）通过；新增未知 id 拒绝、特殊标签按纯文本显示断言。
- 验证：本机只读扫描 8 卷／3 候选，确认可读盘符、输出范围及无原生路径；目录模块与 COM 桥 /W4 /WX /sdl /analyze 通过，差异检查通过。未操作磁盘数据或重复全量验收。
- 安全与风险：卷标是可变不可信显示文本，控制／双向格式字符过滤、页面 textContent 渲染；写入继续依赖原有稳定身份、同句柄和提权复验。名称不是身份保证，目标变更需刷新。
- 提交：已报告变更与验证，沿用持续授权自主提交，禁止推送；构建目录的厂商 DLL 保持固定摘要校验。

### 2026-09-22 — 启动会话提权 / 一次授权与拒绝页面
- 范围与审计：用户要求启动申请本次运行提权、业务操作不再逐次 UAC，拒绝后登录页显示需要管理员权限；明确覆盖此前每次建卷提权口径。主界面继续普通权限，不安装服务或持久授权。
- 问题定位：本机两块盘均 GPT／512；P4 后端只支持 MBR，原页面却把 GPT 枚举候选当成可选择，造成 UAC 之前即失败。现在展示不支持布局并禁选，另提前提示记住容器前置条件；本片不增加 GPT 写支持。
- 检查与实现：启动固定摘要校验并持有助手，UAC 建立仅本应用进程可连接的管理员会话；按操作创建独立助手，复用已有 IPC／nonce／身份复验，不把主界面或卡 SDK 提权。
- 检查与实现：会话仅能启动自身固定助手及固定参数，不接收路径／命令／密码；本机用户 ACL、随机管道、双方 PID 校验、父进程句柄绑定、首实例、禁止远端、超时和有限帧；关闭／断开失效，不退回二次 UAC。
- 检查与实现：拒绝或会话建立失败提供仅能返回启动视图的惰性桥，不构建卡会话、不注册卡定时器；页面显示安全组件当前不可用／需要管理员权限，启动日志记录 1212，页面不展示错误数字。
- 验证与修复：拒绝页面初测触发旧页面清理桥调用，置空业务桥后通过；页面契约对英文 admin 子串的历史拦截经中性 permission-required 视图名解决。静态分析发现 64 KiB 栈缓冲，改为堆分配后严格分析通过。
- 验证：真实普通桌面客户端一次 UAC 后启动两个独立助手、拒绝畸形请求、经正式 VolumeCreator 创建 32 MiB 自有临时容器并删除、退出会话后助手退出且禁止回退；exit=0、stderr 空，证据 out/session-elevation-final-20260922-192714。
- 验证：正式应用／helper 构建、四项协议／桥／页面／关闭 CTest、拒绝页面 DOM、三组分区 DOM、268 项导航和真实隐藏 WebView 通过；会话模块／客户端／助手严格分析通过。未做生产分区写入或重复完整硬件验收，真实 UAC 取消未人工复演。
- 安全与风险：管理员授权有效期扩大到进程会话，常驻助手无秘密／业务写接口，仅启动固定子进程；业务助手的卡授权、密码、目标保护及正常暂停规则不变。外部 Windows 磁盘管理是独立系统工具，仍受它自身的权限流程控制。
- 提交：已报告根因、实现、验证和边界，按持续授权自主提交；随后单独同步需求与诊断文档，禁止推送。

### 2026-09-22 — 文档 / 启动权限要求同步
- 范围与审计：实现 634f25e 已提交；用户的新启动授权要求覆盖旧逐次建卷提权交互，按普通 docs 独立提交约定同步。
- 检查与实现：需求源说明普通权限主界面、进程绑定提权助手、拒绝页面无业务桥、失效不回退，以及外部系统工具与应用助手的边界。
- 检查与实现：诊断表补入 1212 和启动次序，保留历史编号统计；明确该码包括拒绝和建立失败，不能推断单一根因。补充 GPT 列表禁选与后端范围一致。
- 验证：文档对照实现和测试证据；差异检查通过，无新磁盘操作或安装包。
- 安全与风险：管理员会话不保存秘密和永久授权；真实 UAC 取消及实体盘写入不以模拟界面或临时文件测试代替。
- 提交：沿用持续授权单独 docs 提交，禁止推送。

### 2026-09-22 — P4.6 / GPT 身份与协议基础
- 范围与审计：按用户“开工，按 4.6”将原专项验收扩为 GPT 支持及验证；计划三笔实现、一笔独立文档提交，需求源与阶段表最终单独同步。
- 检查与实现：审计分区目录、索引、提权协议及后端；现有实际写入仅 MBR，GPT 页面继续禁选，本片只开放结构定位及只读助手核验。
- 检查与实现：稳定身份增加 GPT 分区 GUID；GPT 非零、MBR 必须为零，保留硬件身份、偏移、长度、512 扇区及容量约束。
- 检查与实现：GPT 使用 v5 加密槽认证 GUID 和转换状态；MBR v3/v4 与文件槽格式保持兼容，固定槽大小不变，不持久化明文标识。
- 检查与实现：IPC 升为 v9，目标结构 76 字节；拒绝旧协议及旧结构，助手保留 GPT 写入前置拒绝，防止基础识别被误当作后端支持。
- 验证：新增 GUID 替换、空 GUID、布局混用、状态及版本畸形、逐字节篡改、IPC 往返和 GPT 写入禁用测试；8 项相关 CTest 全通过。
- 验证：初次构建因测试有符号常量触发 /WX，修正为无符号后通过；旧应用占用助手导致原目录链接失败，独立 out/build/p46 助手构建通过，用户随后关闭旧应用。
- 验证：五个变更实现模块 /W4 /WX /sdl /analyze 通过；本片未执行真实 GPT 写入、UAC 集成或完整硬件矩阵。
- 安全与风险：GUID 参与身份等值与认证，不削弱系统分区、受保护类型/属性及扇区门槛；旧客户端不理解 v5，不应混用改写索引。
- 后续：接 GPT DLL 后端与产品身份传递，再做自建 GPT VHD 和 MBR 回归；用户指定最终提示“暂不支持的分区表格式(非GPT/MBR)”随界面开放生效。
- 提交：按持续授权完成审查后自主提交，本片不宣称 GPT 产品功能完成；禁止推送。

### 2026-09-22 — P4.6 / GPT 创建及原地转换后端
- 范围与审计：基础提交 6c09d65；检查 DLL、助手、原地执行器、核心会话及重开目标解析，接入普通 GPT/512 数据分区，页面仍待下一片开放。
- 检查与实现：新增 GPT 专用格式化／原地打开导出，保留旧 MBR ABI；调用方传 GUID，DLL 复制后在锁定前后核验 Basic Data 类型、允许属性、GUID、偏移／长度／扇区。
- 检查与实现：助手在确认／移除盘符前检查 GPT 导出存在；产品创建、加密索引及重开续作完整保留 GUID；原有密码认证、系统保护、锁盘与暂停顺序保持。
- 验证与修复：首组 GPT 测试因夹具沿用 MBR 四表项读取长度而失败，未开始加密；修正 GPT 单表项及磁盘 GUID 校验后重测，失败证据保留 out/p46-backend-20260922-200028。
- 验证：out/p46-backend-20260922-200226 的 handle-gpt、inplace-gpt、inplace-helper-gpt、inplace-session-gpt、session-gpt、inplace-session、handle 七组均 exit=0、stderr 空，成功夹具全部清理。
- 验证：覆盖 GUID 错误／旧导出拒绝且边界扇区不变、繁忙／取消、清空格式化、暂停／错密钥／续作、真实 KDF 与 v5 索引重开、原文件逐字节读回、撤销清理及 MBR／普通和隐藏文件容器回归。
- 验证：四项 CTest 通过，修改重开解析后会话测试再次通过；五个模块严格静态分析通过，最终核心会话再分析通过。
- 验证：out/p46-source-proof 证明固定上游加四补丁及三份仓库 include 与候选源码一致；候选 DLL SHA-256 为 D099EA1D153320E00B16B3A6004231D74639FBB967EDDEFA16D75588DB38500D，尚未提升交付白名单。
- 安全与风险：不修改产品分区表，不放宽 4Kn／动态盘／保护类型和属性；认证根为合成测试根，未替代实卡或实体盘验收，不承诺断电无损。
- 提交：按持续授权审查后提交；下一片提升已验证 DLL、开放 GPT 页面和指定文案并回归，禁止推送。

### 2026-09-22 — P4.6 / GPT 产品开放与已验证后端提升
- 范围与审计：后端提交 7ed87a3 已完成七组管理员 VHD 验证；本片开放普通 GPT 选择并提升同一候选 DLL，保持未知布局拒选。
- 检查与实现：页面按用户原文显示“暂不支持的分区表格式(非GPT/MBR)”；原盘符／卷标展示与双模式确认不变，桥测试覆盖 GPT 可选及未知布局拒选。
- 检查与实现：应用编译期和打包摘要更新为 D099EA1D153320E00B16B3A6004231D74639FBB967EDDEFA16D75588DB38500D；本地依赖、构建 DLL 与生成的摘要一致，旧依赖另留备份。
- 验证：正式应用／助手、真实 WebView 集成构建通过；全量 CTest 首轮 71/72，唯一 vikey_helper_process_tests 因宿主用户目录重定向触发既有安全门禁，不视为全量一次通过。
- 验证：普通桌面环境先确认路径未重定向，再重测助手与 artifact 两项，2/2 通过；证据 out/p46-desktop-regression-20260922-200920，未削弱路径保护。
- 验证：三组分区 DOM、拒绝提权 DOM、268 项导航、JS 语法及真实隐藏 WebView（exit=0）通过；新增未知布局测试再构建／回归通过，桥严格静态分析通过。
- 验证：本机只读枚举通过，8 卷中 3 候选、5 拦截，Windows 卷排除一次；没有实体盘写入。out/build/p326/USB Vault.exe 已重建，未生成安装包。
- 安全与风险：GPT 开放仍受原生身份／写入保护和新 DLL GUID 校验控制；实卡、实体盘、热插拔、跨重启兼容矩阵尚未验收，不以合成根 VHD 测试代替。
- 提交：审查后按持续授权自主提交；下一笔独立 docs 同步最终范围、版本和证据，禁止推送。

### 2026-09-22 — 文档 / P4.6 GPT 范围与验证同步
- 范围与审计：三个实现提交分别为 6c09d65（身份／索引／IPC）、7ed87a3（后端／续作）、b8b893d（产品开放／摘要提升）；按用户约定单独提交 docs。
- 检查与实现：需求源将 P4.6 明确为 GPT 支持与专项验收，当前支持矩阵改为 GPT／MBR 普通 512 数据分区，说明 v5 索引、v9 协议、专用导出与未知布局文案。
- 检查与实现：阶段表、设计记录与第三方边界同步已验范围及最终摘要；保留历史 MBR 限制、首轮失败与修复重测的区别。
- 验证：对照实际七组管理员 VHD、全量首轮 71/72 与普通桌面 2/2 重测、页面／构建及源码复现证据；文档差异检查通过。
- 安全与风险：仅宣布 GPT MVP 完成；实卡／实体盘、热插拔和跨重启矩阵未验收，不宣布整个 P4 验收完成，不承诺断电无损，未生成安装包。
- 提交：沿用持续授权独立 docs 提交；本轮预计四笔提交完成，禁止推送。

### 2026-09-22 — UI / 分区创建失败显示错误码
- 范围与审计：按用户指定，将清空创建失败处的长说明替换为“创建失败。错误码数字”；不改保留文件转换提示及磁盘写入流程。
- 检查与实现：原进度接口仅提供终态，新增认证后可读的创建错误码接口；捕获 system_error 数值，无明确错误时使用 E_FAIL，不透传异常字符串或路径。
- 检查与实现：每次任务清零错误码，任务尚未接受时不读取历史错误；页面在刷新会话前取本次结果，未知情况显示通用失败码。
- 验证：桥与页面契约 CTest 2/2、创建成功／失败／取消 DOM、JS 语法通过；测试初次调用后置辅助函数导致编译失败，改为直接 Invoke 后重新构建测试通过；正式应用和助手重建通过。
- 安全与风险：错误接口仅认证可读，运行中沿用繁忙门禁，不返回密码、身份或异常详情；错误码仍可能是上层通用码，本片不声称已经定位之前实体盘准备失败的底层原因。
- 提交：按持续授权审查后自主提交，禁止推送。

### 2026-09-22 — UI / 补齐原地加密失败错误码
- 范围与审计：用户截图为保留文件转换，前片仅修改清空创建，导致截图路径仍显示旧提示；本片修正遗漏。
- 检查与实现：原地加密及重开续作失败统一显示“创建失败。错误码数字”；三条分区路径复用错误码读取函数，任务未接受不读取历史结果。
- 验证：原地转换失败、续作失败、正常暂停／完成及清空创建 DOM 通过；JS 语法、页面契约测试和正式应用重建通过。
- 安全与风险：保留不确定状态禁止挂载／移除及暂停许可门禁；不改变写入行为，不自动重试，不宣称此次已定位底层准备失败原因。
- 提交：按持续授权审查后独立提交并更新应用，禁止推送。

### 2026-09-22 — P4.7 / 分区 BitLocker 状态查询
- 范围与审计：用户新增 P4.7 应用内确认关闭 BitLocker，当前创建弹窗内确认、解密进度后接回创建；不静默关闭，不绕过现有安全门禁。
- 流程约定：阶段收尾文档并入阶段最后一笔实现提交，替代此前阶段独立 docs 约定；P4.7 预计三笔实现提交，最后同步需求与阶段表。
- 检查与实现：检查需求源、写入策略、助手协议及创建桥；新增 v10 无密码只读 BitLocker 查询，以既有管理员助手执行并在查询前后核对分区身份。
- 检查与实现：仅接受 Windows 0～5 转换状态及 0～100 加密百分比，未知不视为解密；系统／引导／分页分区继续拒绝，不获取或输出恢复密钥。
- 验证：协议与目录 CTest 2/2、助手构建及四模块严格静态分析通过；新增拒绝密码／旧协议／畸形状态测试。
- 验证：初次遍历候选时 D: 查询被拒，后确认它承载分页文件；收窄为 T: 只读探测后 exit=0、stderr 空，正式助手返回完全加密100%，证据 out/p47-inspect-20260922-205358。
- 安全与风险：本片只有只读查询，未对 T: 解密或写入；状态查询不构成后续写入许可，BitLocker 未完全解密仍禁止创建。
- 提交：按持续授权完成审查后自主提交，后续接确认解密与同弹窗流程，禁止推送。

### 2026-09-22 — P4.7 / 显式确认后关闭 BitLocker
- 范围与审计：承接状态查询提交 6f08782；仅关闭已选分区的 BitLocker，不接管恢复密钥，不改变系统策略。
- 检查与实现：助手新增无密码解密操作；准备完成后与客户端进行独立确认握手，调用前再次核对完整身份、卡会话取消状态和系统／引导／分页限制。
- 检查与实现：固定 WMI Decrypt 方法；已完全解密或正在解密不重复发起，实际创建仍沿用完全解密门禁。
- 验证：助手与协议及集成目标构建通过；协议测试通过，四模块 /W4 /WX /sdl /analyze 通过。
- 验证：自建 GPT VHD 完成 BitLocker 加密、拒绝确认零解密、确认后解密状态轮询及原文件逐字节回读；证据 out/p47-decrypt-20260922-205826，exit=0、stderr 空。
- 安全与风险：只操作自建临时盘，未对用户 T: 解密；Windows 接受解密后独立后台运行，取消应用等待不能宣称停止系统解密。
- 提交：按持续授权审查后自主提交，下一片接同弹窗确认及进度并合并阶段收尾文档，禁止推送。

### 2026-09-22 — P4.7 / 同弹窗确认、解密进度与阶段收尾
- 范围与审计：承接 6f08782 状态查询和 db57ffc 确认解密；按最新要求不新建阶段任务文档，收尾需求、阶段表、日志与本片合并。
- 检查与实现：当前创建弹窗内显示盘符／卷标、真实 BitLocker 状态、固定微软帮助、勾选确认及取消／继续；卷标用文本节点呈现，不解析 HTML。
- 检查与实现：独占创建线程先查询、等一次性确认、复用本次管理员助手解密并轮询；只有完全解密状态及再次验证通过才接回原清空创建或保留文件模式。
- 检查与实现：确认绑定当前卡会话与已消费的分区快照，重复确认、取消、拔卡／会话过期不能继续创建；未开启记住容器在解密前拒绝。
- 验证：新增原生两模式十组确认／取消／卡失效／等待门禁测试通过；四组 BitLocker DOM、分区选择／清空／保留转换 DOM、268 项导航检查及 JS 语法通过，浏览器截图已检查确认页及进度页。
- 验证：全量构建通过，CTest 首轮70/72；页面契约旧计数及新增夹具缺项已修复，桥与契约定向重测2/2；卡助手因宿主路径重定向失败，普通桌面路径核验后一并重测2/2，证据 out/p47-desktop-regression。
- 验证：UI 原生两模块 /W4 /WX /sdl /analyze 通过；正式应用与助手已重建。实际 BitLocker 解密及原文件一致性由上一片自建 GPT VHD 提供证据，未操作用户 T:。
- 安全与风险：不传入任意命令／网址，不收集恢复密钥，不绕过系统／引导／分页及稳定身份门禁；未知状态失败关闭，显示100%不等于授权格式化。
- 安全与风险：Windows 接受后的后台解密不由应用取消；取消只停止等待及后续创建，页面明确提示。锁定卷自动解锁、实卡／实体盘和跨重启兼容尚未验收，不宣称任意断电无损。
- 提交：按持续授权审查后自主提交，P4.7 三笔实现切片收尾，文档并入本笔，不推送、不生成安装包。

### 2026-09-22 — UI / 修复占用后强制卸载无法启动
- 范围与审计：用户反馈文件管理器占用时强制卸载提示“操作未能开始”；检查普通失败后显式确认升级需求、页面刷新、桥重试绑定及底层强制卸载路径。
- 检查与实现：页面刷新新增的分区类型／转换状态只读查询误走变更分支，清除最后卸载目标；将两查询归入观察白名单，保留同次失败的重试绑定。
- 验证：先加入页面真实查询序列，复现 single forced cleanup did not start；修复后桥及卷提供者 CTest 2/2，通过单卷／全部目标、重复请求、非占用失败、换卡及无关变更撤销检查。
- 验证：用户正常退出旧应用后，正式应用与助手重新构建成功；git diff --check 通过。未对用户活动卷执行强制卸载，真实文件管理器占用场景仍需用户重试。
- 安全与风险：不放宽忙碌结果、卡 epoch 和目标范围校验，不自动升级强制，不终止文件管理器；强制卸载本身仍可能丢失未保存数据，沿用原确认。
- 提交：按持续授权审查后自主提交，文档并入本片，禁止推送。

### 2026-09-22 — P4.8 / 同句柄分区双卷格式化候选
- 范围与审计：用户授权 P4.8 分区隐藏卷首版，仅清空创建；覆盖历史“分区隐藏卷不做”边界，保留文件转换仍只用于标准卷。预计五笔，逐片验证后自主提交。
- 页面要求：磁盘与文件沿用文件弹窗尺寸；存储切换同行显示指定简短描述；标准／含隐藏卷选项常态固定，下方按四种组合切换。
- 检查与实现：检查固定 VeraCrypt Format／FAT 代码、现有文件双卷、分区格式化／挂载及 R-03／8.6；新增候选双卷导出，一次锁盘、同一句柄创建外层及隐藏卷，支持既定 GPT／MBR 身份。
- 检查与实现：新外层完整初始化，隐藏区域不与残留明文形成可识别差异；按同源 FAT 元数据布局与尾部余量验证隐藏范围，限制容量／对齐／密码长度，不通过路径重开或删除失败分区。
- 验证：候选 DLL 与自建 VHD 夹具构建通过；固定源码加四补丁／三 include 复现与候选一致。初次源码复现因 CRLF／LF 混合未应用，统一换行重测通过。
- 验证：首轮快速候选 MBR 通过、GPT 收尾占用失败；改为完整初始化后 GPT 通过，MBR 收尾占用失败，均保留原证据。最终夹具仅对已刷新、已关闭文件且身份一致的自建挂载允许占用升级，不改变产品策略。
- 安全与风险：本片不提升产品白名单，不接页面；候选摘要 D911EEE4B02177E3BE82970EAC2B8EA6886DE07D23801D6316E5D9E89C2B4CB0。完整初始化耗时随分区大小增长，不宣称闪存历史数据安全擦除或断电无损。
- 验证：最终 out/p48-pair-verified 的 GPT／MBR 均 exit=0、stderr 空，两个卷独立写入／重挂回读及文件双卷回归通过，成功夹具已清理。
- 提交：按持续授权审查后自主提交；不操作用户现有分区，禁止推送，后续接助手与保护。

### 2026-09-22 — P4.8 / 提权助手双卷协议与保护挂载
- 范围与审计：检查原分区确认、身份复验、取消、二进制密钥管道及驱动隐藏区域保护；沿用会话提权与标准分区写入门禁。
- 检查与实现：协议升至 v11，新增分区双卷操作；双方校验两段 64 字节密钥及隐藏容量，导出缺失时在确认和移除盘符之前拒绝；进度合并为有界单调百分比。
- 检查与实现：分区挂载允许携带有效隐藏保护密钥，仍校验物理身份；秘密仅在既有锁页缓冲和私有管道内传递，不新增档位记录。
- 验证：协议与卷提供者 CTest 2/2 通过；四个修改实现文件 /W4 /WX /sdl /analyze 通过，git diff --check 通过。
- 验证：旧应用占用助手导致首次链接失败，用户正常退出后重建成功；out/p48-pair-helper 的 GPT／MBR 自建 VHD 均 exit=0、stderr 空。
- 验证：拒绝确认保持原身份，成功创建两个可独立重挂读写的卷；写入外层触发隐藏区域保护后隐藏文件完整，既有文件双卷回归通过，自建夹具已清理。
- 安全与风险：MBR 夹具正常卸载占用，按既定仅自建目标规则强制收尾；未触碰用户活动卷。产品 DLL 尚未提升，页面与会话接入未完成，不代表最终产品验收。
- 提交：按持续授权审查后自主提交，后续接会话与界面，禁止推送。

### 2026-09-22 — P4.8 / 双卷会话与登录派生
- 范围与审计：检查文件双卷卡密码更新、分区身份持久化、登录派生、维护挂载及取消边界，沿用同一卡的两密码语义。
- 检查与实现：增加分区双卷会话与控制器入口，校验容量与记住容器；先确认卡密码再创建，分区身份仍在破坏性确认前加密保存。
- 检查与实现：创建后挂载登录派生卷；重登从标准派生开始，仅认证失败时尝试登录派生，其他错误不换密钥重试；不新增隐藏存在性持久字段。
- 检查与实现：维护入口可用当前登录派生密钥保护外层分区；卡拔出、部分失败及卸载回收沿用既有会话边界。
- 验证：会话与 UI 控制器 CTest 2/2 通过；新增无效容量、拒绝确认、保存失败、密码输入清零、创建挂载、维护挂载及重登测试。
- 验证：首轮会话测试发现恢复分区错误假定标准派生，改为未知后重测通过；两实现文件 /W4 /WX /sdl /analyze 通过，git diff --check 通过。
- 安全与风险：密码输入沿用锁页及 RAII 清理，角色仅限会话内已证实派生；尚未接 UI，新候选 DLL 未提升。原文件专用维护容量／导入助手不能直接用于物理分区，后续必须明确边界，不降低文件路径校验。
- 提交：按持续授权自主提交；本片模拟测试不替代实卡验收，禁止推送。

### 2026-09-22 — P4.8 / 四组合创建页与安全桥
- 范围与审计：落实用户指定的统一弹窗尺寸、存储类型同行描述和固定标准／含隐藏卷选项；不再增加独立阶段文档。
- 检查与实现：四种组合共用 40rem 宽与有界固定高度，两排选择位置保持一致；表单直接切换，精简分区说明及重复卡片提示，保留清空确认。
- 检查与实现：磁盘隐藏组合显示内层容量和密码，不要求文件名／目录、不提供保留文件转换；复用隐藏密码复核及 BitLocker 确认／等待流程。
- 检查与实现：新增四参数双卷桥入口，绑定当前选择与卡 epoch，消费后不可重放；SAFEARRAY 与工作线程秘密按既有锁页及清零规则释放。
- 验证：安全桥 CTest 1/1 与 /W4 /WX /sdl /analyze 通过；DOM 274 项通过，含四组合、磁盘双卷确认与字节数组清零；正式开发应用构建成功。
- 验证：Edge 本地模拟截图检查四组合，1400×1000 视口下弹窗均 640×768，类型选项坐标一致；初次新增 DOM 用例残留旧 mock pending 导致超时，清理夹具后通过。
- 安全与风险：未降低页面 CSP；截图夹具仅自身上下文绕过 CSP 注入假桥。磁盘卷名按文本呈现，HTML 字符不执行；尚未执行最新产品实卡创建验收，候选 DLL 未提升，不宣称阶段关闭。
- 提交：用户要求创建页提交后评估保留文件转换为隐藏卷，故先提交本片，再做只读可行性审查；禁止推送。

### 2026-09-22 — P4.8 / 保留文件创建隐藏卷可行性审查（未实施）
- 范围与审计：按用户最新要求，先完成创建页提交 9ba3e1e，再只读评估分区隐藏卷保留文件转换；不扩大当前写入实现、不操作用户磁盘。
- 检查与发现：固定上游 Tcformat.c:9834 拒绝 NONSYS_INPLACE_ENC 宿主，Language.xml:724 明确原因为空闲区未随机填充；官方 Hidden Volume 文档脚注及尾部连续空闲区约束一致。
- 检查与发现：本项目 UsbVaultInplace.inl:26、43 保留原地转换标志，partition_inplace_plan.cpp 只为双卷头预留 256 KiB，不包含隐藏卷空间规划；现有新建双卷依赖空白 FAT 几何及完整外层初始化。
- 结论：直接组合现有原地转换和隐藏卷创建不可作为受支持方案；总空闲容量不等于可用尾部连续空间，也不能通过移除上游标志宣称安全。
- 可行方向：若要保留文件，可考虑经另一存储位置加密暂存、校验、重新创建完整双卷、恢复文件并逐项核验；原文件进入外层还是隐藏卷需明确，后者不属于现有原地转换。
- 安全与风险：自研原地方案还需处理空闲区随机化、NTFS 元数据／已删文件线索、连续空间与搬移、暂停进度及不可区分性审查；本次未做破坏性实验，不承诺可行或工期。迁移方案仍需容量、文件系统能力、元数据保真与失败恢复设计。
- 提交：本审查记录留待阶段后续实现／收尾提交合并，不另建任务文档或独立 docs 提交；候选后端提升及 P4.8 最终验收仍未完成，禁止推送。


### 2026-09-22 — P4.8 / 正式依赖提升与 MVP 收尾
- 范围与审计：用户要求先完成 P4.8 收尾；核对四片提交、固定源补丁证据、R-03／8.6、双密钥生命周期及宿主边界，不实施保留文件隐藏转换。
- 检查与实现：提升已核验候选 DLL 为正式本地依赖，部署和打包摘要同时改为 D911EEE4B02177E3BE82970EAC2B8EA6886DE07D23801D6316E5D9E89C2B4CB0；构建目录实际 DLL 与生成的部署清单一致。
- 检查与实现：补充真实 KDF／索引／助手／驱动会话夹具，验证登录派生隐藏文件重开、另一密码外层隔离和保护挂载；显式拒绝分区进入文件宿主专用的维护容量／外部导入助手，不以空路径尝试。
- 验证：out/p48-final-session-verified 的 GPT／MBR 均 exit=0、stderr 空，成功夹具已清理；首轮夹具错误使用全会话清理后再维护，秘密已按设计释放，改为单卷卸载后通过，失败镜像保留证据且由 RAII 脱离。
- 验证：首轮全量构建成功、CTest 70/72；页面旧契约与新设计冲突，更新同行描述和双卷路由检查后，与会话测试定向重跑 2/2 通过，最终 71 个测试已有通过证据。
- 验证限制：vikey_helper_process_tests 因宿主 LocalAppData 路径重定向失败；out/p48-desktop-regression 与 out/p48-admin-regression 的路径复核均不一致，未执行其后集成测试。保留失败，不将历史通过替代本轮，也不移除路径门禁。
- 验证：收尾会话实现 /W4 /WX /sdl /analyze 通过，正式应用及最后夹具重建成功，git diff --check 通过；上一片 274 项 DOM 与四组合浏览器截图证据继续有效。
- 安全与风险：同句柄目标复验、写入前确认和身份持久化、取消／撤销及秘密清零边界保留；无新增持久隐藏标识、不接收任意磁盘测试目标、不触碰用户分区。本片双卷集成用合成认证根，未覆盖真实卡／实体盘和跨重启矩阵。
- 范围收尾：P4.8 五笔完成清空双卷创建 MVP；FAT 限制、全盘初始化耗时、分区高级更新空间估算／外部导入未支持、保留文件隐藏转换未实现和测试环境阻塞均在需求／阶段表明确，不宣称加固交付完成。
- 提交：按用户持续授权审查后自主提交，收尾需求、阶段表和此前可行性记录并入本笔；禁止推送，不生成安装包。

### 2026-09-22 — fix / 磁盘隐藏卷容量误报（未提交）
- 范围与审计：用户反馈磁盘隐藏卷创建提示容量低于最小值，明确修复后不提交；检查 P4.8 容量范围及创建页选择／刷新／提交校验。
- 检查与实现：未选择分区原先按 0 MiB 处理，错误显示最小容量；改为未知容量、隐藏过早报错且禁止继续。统一校验与参数来源为所选分区实际容量，不再读取隐藏的文件总容量输入。
- 验证：新增用例先复现未选分区误报；修复后 DOM 280 项、页面契约 CTest 1/1、JS 语法及 git diff --check 通过，覆盖真实小盘拒绝、刷新失效、超 3/4 拒绝及大于文件默认值的有效隐藏容量。
- 验证：首次应用链接因旧 EXE 占用失败；用户正常退出后重新构建。未在用户分区执行创建或格式化。
- 安全与风险：未放宽原生容量／身份／清空确认门禁，未新增持久记录；本次修复为页面校验，不代表实盘创建验收。
- 提交：遵照用户要求保留工作区修改，不提交、不推送。

### 2026-09-22 — fix / 进入磁盘创建页自动刷新（未提交）
- 范围与审计：用户要求首次打开磁盘容器页及从文件容器切回时自动显示最新磁盘列表；沿用上一条先不提交的要求。
- 检查与实现：页面进入后调用既有异步只读枚举，保留手动刷新；新刷新清除旧选择和清空确认，重复点击当前存储类型不额外扫描。
- 检查与实现：文件容器页不扫描磁盘；恢复已有分区转换仅打开进度流程，不启动无关枚举，避免与续作争用会话。
- 验证：DOM 283 项通过，新增首次自动扫描、文件页不扫描、切回重新扫描且无旧授权断言；页面契约 CTest 1/1、JS 语法与 git diff --check 通过，正式应用构建成功。
- 安全与风险：自动触发只读列表刷新，不自动选择、不复用清空确认、不执行格式化；继续由忙碌状态、页面版本和原生快照身份阻止过期操作，未修改后台写入策略。
- 提交：本片与此前容量修复均保留为未提交修改；未推送、未对用户分区执行创建。

- 2026-09-22 提交决定更新：用户已同意提交上述容量误报与自动刷新两项修复；测试保持有效，合并为一笔 fix，不推送。


### 2026-09-22 — ui / 创建页按交互预览整理
- 范围与审计：用户要求先提交容量与自动刷新修复，再采用既有 HTML 预览中容器类型之外的设计；前两项已提交为 161df48。本片保留 P4.8 四组合入口、描述同行及同尺寸弹窗，不扩大隐藏卷转换范围。
- 检查与实现：检查创建参数、分区枚举、确认／BitLocker／隐藏密码流程及 DOM、页面契约；分区改为整行选择、盘符块、卷标与物理位置、容量和选择标记，不可用项折叠并保留原因。
- 检查与实现：创建方式改为并排卡片，底部显示所选分区摘要；磁盘下一步先展示身份、容量与方式，再勾选原有写入授权；返回清除授权，隐藏卷继续复用密码二次确认。
- 检查与实现：标准文件容器名称和容量同行、路径独立；空名称／路径不在初始页面立即报红，必填门禁仍保留。去除读取成功等重复提示，错误和操作反馈保留。
- 验证：296 项 DOM 检查通过，覆盖自动刷新、容量、不可用分区折叠、恶意卷标文本、返回／方式切换撤销授权、隐藏卷和 BitLocker 独立确认、退出清理；JS 语法及 git diff --check 通过。
- 验证：旧应用占用曾造成链接失败，退出后正式应用构建成功；页面旧契约需允许确认页隐藏选择控件，更新后 CTest 1/1 通过。BitLocker 新用例首次在异步进度更新前断言失败，改为等待后台状态后通过。
- 验证：浏览器夹具四组合均为 640×768，类型区坐标一致；另检查确认页与 600×700 小窗口，无横向溢出，长内容由主体滚动。证据保存在 out/p48-pair-helper/redesign-*.png。
- 安全与风险：卷标和确认摘要均以 textContent 写入，不增加秘密／隐藏形态持久化；不自动选择分区、不复用清空授权，原生身份复验和 BitLocker 同意保留；未做真实卡／实体分区写入验收，未重跑与本片无关的全量集成。
- 提交：本片页面改动保留工作区供用户审阅，尚未提交；未推送，未生成安装包。


### 2026-09-22 — ui / 限定为磁盘创建页改动
- 范围与审计：用户明确只改磁盘容器页面、其余部分及页面尺寸不得调整；对照 161df48 审核当前未提交差异。
- 检查与实现：恢复标准文件容器表单完整原始布局，以及两类文件表单原有字段错误提示；撤销文件网格样式，将新增焦点样式限定在磁盘选择／确认区。类型入口及尺寸规则保持原样。
- 验证：浏览器分别加载 HEAD 和工作区页面，磁盘弹窗尺寸、文件标准／隐藏两表单可见控件坐标、字体、间距及页脚逐项一致；页面契约 1/1、交互检查 296 项通过。
- 验证：首次链接仍遇旧应用占用；用户退出后正式应用重建成功，未运行真实分区写入。
- 安全与风险：只收回越界页面变化，磁盘确认、原生身份复验及秘密清理机制保持；上一条记录中的文件表单整理已撤销。
- 提交：修改仍未提交，未推送。


### 2026-09-23 — ui / 分区列表局部刷新与弹窗交互细化
- 范围与审计：按用户七项 UI 反馈检查创建页、全局 render、分区只读调用和全部 11 个弹窗遮罩；保持现有弹窗尺寸及容器类型布局，仅按明确要求调整文件页按钮和字段标题。
- 检查与实现：分区选择／刷新／打开磁盘管理不再触发全局重绘；保留原生调用串行忙碌门禁，局部更新列表及创建可用性，选择只修改已有行属性，保留节点和滚动位置。
- 检查与实现：可用与默认折叠的不可用分区置于同一 14rem 固定高度框；减少行上下留白，盘符块与文字尺寸不变；工具按钮统一描边，文案改为打开磁盘管理。
- 检查与实现：移除手动检查按钮及页面事件，它仅为预检查；创建器原有写入前身份／策略复验继续保留。选择分区标题与创建方式一致，文件名称／路径／容量标题同样对齐，主按钮为下一步并使用默认鼠标指针。
- 检查与实现：所有弹窗遮罩仅在鼠标左键 pointerdown 落于遮罩本身时取消；内部按下后外部抬起和后续 click 不再取消，保留执行中／忙碌时的取消限制及密码清理。
- 验证：DOM 302 项、页面契约 CTest 1/1、JS 语法通过；浏览器实测按下／拖出／松开、外部按下取消、多条目内部滚动及选中后滚动位置保持；窗口 640×768 未变，列表 224px、条目约 60px、盘符块约 38.4px，字段标题字体与按钮指针符合要求。
- 安全与风险：不新增磁盘写入或持久化，不放宽身份复验／BitLocker／清空确认；重新审查局部异步回调仍使用版本失效和忙碌保护，非可选条目没有选择入口。未做实体分区写入验收。
- 提交：在既有未提交 UI 改动上继续细化；正式应用重新构建，未提交、未推送。


### 2026-09-23 — ui / 紧凑隐藏卷表单与磁盘管理列表恢复
- 范围与审计：按用户五项反馈检查磁盘管理入口、后台快照失效、分区框和隐藏卷表单；保留弹窗尺寸及既有安全确认。
- 检查与实现：确认打开磁盘管理会在原生层 clear_partitions；页面原先只清空未重读，造成暂无可用分区。改为保留禁用的展示行、撤销旧选择／确认，启动工具后自动重新枚举，完成前不得复用旧快照。
- 检查与实现：重新枚举失败清除旧展示行并显示读取错误；工具启动失败也重新枚举，因为后台可能已清除快照。卡变更／页面版本变化仍终止失效结果应用。
- 检查与实现：列表由 14rem 缩为 10rem，不可用分区折叠栏减少上下留白；移除左下角分区摘要和相关状态代码。
- 检查与实现：文件隐藏容器名称／路径按等宽输入框同行布局，浏览按钮单列置右；两种存储类型隐藏容器的诱饵密码／确认密码等宽同行，保持原校验、清零和二次确认逻辑。
- 验证：305 项 DOM、页面契约 1/1、JS 语法、git diff --check 通过，正式应用构建成功；新增工具启动后自动枚举、显示保留但禁止旧授权、重读失败不得恢复旧行用例。
- 验证：浏览器实测分区框 160px、不可用栏低于 40px；名称／路径输入约 250.7px 等长同排，浏览按钮约 72.5px 在右，两密码框同排等宽，弹窗仍为 640×768。未执行实体磁盘写入验收。
- 安全与风险：保留原生快照清理和写入前复验；失败后不允许选择旧目标，无新增持久化或明文秘密记录。
- 提交：延续此前 UI 待审改动，未提交、未推送。


### 2026-09-23 — ui / 文件表单统一与磁盘只读容量
- 范围与审计：按用户四项反馈检查标准／隐藏文件表单、磁盘隐藏卷容量来源、字段提示及选项过渡；不改变弹窗尺寸。
- 检查与实现：标准文件容器复用名称、路径等宽输入框加浏览按钮的同行布局；两类文件表单去掉空名称／路径的填写提示，非法已填值提示和必填禁用门禁保留。
- 检查与实现：磁盘隐藏卷总容量与内层容量同行展示，总容量取自已选分区 MiB，灰色只读、正常鼠标；未选时显示空占位，刷新／关闭清除。展示值不参与后台参数构造，仍使用原生分区实际容量。
- 检查与实现：磁盘／文件切换和磁盘创建方式补充 140ms 背景、边框与颜色过渡，与既有类型选项保持一致；减少动画偏好下禁用新增过渡。
- 验证：307 项 DOM、页面契约 1/1、JS 语法通过，正式应用构建成功；覆盖所选磁盘容量只读展示和空字段不报红但不能继续。
- 验证：浏览器实测标准名称／路径同排等宽，磁盘总容量／内层容量同排，只读输入 cursor=default，过渡为 0.14s；磁盘／文件布局及弹窗 640×768 保持，截图在 out/p48-pair-helper/aligned-*.png。
- 安全与风险：不放宽容量、路径、清空确认和密码校验；新增容量显示随页面状态清除，无新增持久化或磁盘写入；未执行实体盘创建验收。
- 提交：延续未提交 UI 细化，未提交、未推送。


- 2026-09-23 提交决定更新：用户授权提交上述创建页及弹窗 UI 调整；本笔使用 ui 前缀，提权拒绝启动修复单独保留工作区，不推送。

### 2026-09-23 — fix / 拒绝提权后的静态不可用页
- 范围与审计：用户要求 UAC 未同意后只显示“安全组件当前不可用”“需要管理员身份”，启动通信诊断进入 startup log，不弹错误框或显示卸载操作；检查启动宿主、权限专用桥、页面启动 catch 与看门狗。
- 检查与实现：拒绝提权时在导航前注册只减权限的页面标记，页面先进入静态不可用态，不依赖 getStartupView 通信；仍不建立卡会话或产品操作桥，兼容旧权限桥入口。
- 检查与实现：1401–1408 启动页面通信／脚本失败在记录诊断和撤销桥后保留页面，不再弹重复系统框或自动关闭；其他无法建立窗口／渲染环境的原有处置保留。
- 检查与实现：启动握手失败不再假定存在活动卷，撤下页面桥及会话／配置操作；已启动会话运行期的失败卸载保护不变。拒绝提权文案按用户更正为需要管理员身份，不显示错误码。
- 验证：权限桥、预注入标记无桥访问、握手异常三种 DOM 用例通过，307 项交互回归及启动状态／startup log／页面契约 CTest 3/3 通过，正式应用构建成功。
- 验证：页面契约初次因标识符包含旧原型禁用词误报，改用 PermissionRequired 命名后通过；浏览器可见文本实测仅品牌及两句要求文案，无错误码／卸载文字。未进行桌面 UAC 人工取消实测。
- 安全与风险：权限标记只移除能力，不能取得提权或产品桥；诊断数值继续写 startup log，原生会话及退出时磁盘保护仍保留，无真实磁盘操作。
- 提交：此前 UI 调整已提交为 25d23fe；用户现授权提交剩余启动提权修复，既有构建及验证有效，本笔使用 fix 前缀，禁止推送。


### 2026-09-23 — fix / 允许管理员启动兼容卡 helper
- 范围与审计：用户授权单笔实现并提交，接受降低 helper 隔离强度；审计需求、阶段记录、2011 步骤表与客户端／协议／进程测试，当前只调整联网兼容卡后端。
- 需求取舍：增补覆盖历史独立非提权及主界面必须普通权限的绝对要求；严格零网络 AppContainer 策略不变，R-03／8.6 不变。
- 检查与实现：兼容模式启动前与响应后只将 not_elevated 设为可选；继续观察实际提权状态，保留令牌查询错误及其他步骤诊断，不伪造属性或忽略未知位。
- 检查与实现：特权删除、Job／子进程限制、缓解策略、环境／路径／摘要／nonce 检查保留；测试新增真实父进程提权核验及管理员兼容运行入口。
- 验证：逐位验证缺失必需属性／多余属性拒绝、普通／提权属性接受及严格模式拒绝缺失未提权属性；p326 正式应用与相关测试构建、package-release 静态 Release 应用构建通过。
- 验证：普通 Explorer 桌面下 startup_failure、startup_log、helper_protocol、helper_process CTest 4/4 通过（3.14 秒）；真实管理员 helper_process 模拟厂商 DLL 全部兼容链通过，未跳过；两组请求路径与句柄最终路径预检一致。
- 验证边界：普通首轮遭遇全局卡操作锁占用失败，待另一测试结束后串行复跑通过，未放宽锁或路径规则；证据 out/helper-elevation-20260923，保留首轮失败。未进行实卡、客户电脑、关闭 UAC 或完整硬件矩阵验收。
- 安全与风险：审查确认仅兼容模式可选一位，严格模式无放宽；管理员组／完整性级别仍可能保留，管理员主界面及厂商 DLL 漏洞影响扩大。此次消除提权导致的 2011／58，不消除其他安全边界失败。
- 提交：用户已授权本笔提交；仅包含本次实现、测试与文档，既有安装包／out 清理记录保留工作区，不推送；本次未生成安装包。


### 2026-09-23 — fix / 挂载盘符以驱动回报为准
- 范围与审计：用户反馈磁盘容器反复挂卸后打不开，提示"无法读取这个文件夹"，且挂载后资源管理器不出现预期盘符。审计 P4 分区可见性条款、会话挂载记账与卷驱动边界；本片只改盘符记账，不动身份、同意与格式化门禁。
- 检查与实现：确认该提示是页面对 listContainerDirectory 任何异常的兜底，不代表目录丢失。根因为会话记录 reserve_drive_letter() 申请的字母，而驱动实际可能把卷挂到别处；MountResult 原本没有盘符字段，request.drive_number 读后即弃。
- 检查与实现：mount_file／mount_partition 在清空请求缓冲前，按已发送路径在驱动挂载表中反查实际盘符，唯一匹配才采纳，无匹配或多匹配留空；驱动查询失败不得把已成功的挂载变成抛出。shell 通知改用实际盘符。
- 检查与实现：会话新增 settled_drive_letter，创建、普通挂载、双派生探测与维护挂载四处一律记录驱动回报的盘符，驱动无法交代则拒绝，不写入未经确认的字母。allocate_drive_letter 补上驱动已占用盘符的并集，与提权 helper 中 choose_temporary_drive 的既有写法对齐。
- 验证：新增用例令假驱动把卷落到 K 盘，去掉修复即失败（通知盘符不符），修复后记录、通知与释放均指向 K；另覆盖驱动不回报盘符时拒绝挂载且不留已挂载标记。card_login_mount_session 与 mount_orchestrator CTest 2/2 通过，p46 全量目标构建通过，card_product_webview_host_object、card_product_ui_session_controller、partition_catalog CTest 3/3 通过。
- 验证限制：未做实体分区反复挂卸实测，未复现 Windows 侧改派盘符的具体成因；tests/partition_device_integration_tests.cpp 的自建 VHD 循环复现需管理员权限，本次未执行。
- 安全与风险：不放宽身份复验、清空确认或 BitLocker 门禁，无新增持久化。残留风险两项：驱动查询失败时挂载被拒但卷仍在，会话没有盘符可供释放；allocate_drive_letter 现会因驱动不可用而提前抛出。页面兜底文案与 E_FAIL 的拆分留作下一片。
- 提交：实现与测试完成待用户审阅，未提交、未推送。


### 2026-09-23 — fix / 目录读取失败按原因分开报告
- 范围与审计：接上一片，用户要求拆掉页面那句覆盖所有失败的兜底文案，以及 invoke_list_container_directory 的 catch (...) 一律 E_FAIL。只改失败报告，不改列目录本身的行为与越卷防护。
- 检查与实现：宿主对象按异常类型区分未挂载、容器不在会话、卷不可达、目录不存在、权限、IO、内容被拒和未知八类，存入 directory_error_，新增 DISPID 120 与 getDirectoryError 供页面读取；成功列目录会清零。
- 检查与实现：not_found 不再直接归咎于文件夹——先用容器当前盘符探一次卷根，卷根不答则报卷不可达。access_denied 只认 Win32 ERROR_ACCESS_DENIED，不用 std::errc 比较，因为通用映射会把写保护等无关情况折进 permission_denied。
- 检查与实现：页面按原因查表给出对应中文提示，读取失败时退回"原因无法确定"，不再声称文件可能已不在容器里。查询失败原因不清空重试结果。
- 验证：宿主对象新增 8 项分类用例（含成功列目录不留失败痕迹）通过；页面契约新增两条断言，要求文案取自桥的原因且全文不再出现旧兜底句。card_product_webview_host_object、card_product_ui_page_contract、card_product_ui_session_controller、card_login_mount_session、mount_orchestrator CTest 5/5 通过；navigation DOM 307 项通过；exit／login-reset／startup 三组 DOM 通过。
- 验证：上一片的盘符修复补做实机验证——管理员权限下 partition_device_integration_tests 的 --run-owned-vhd-session 与 --run-owned-vhd-handle 均通过，并新增断言要求真实驱动对分区和文件容器都回报请求的盘符，证明回读匹配在真实驱动上成立。
- 已知问题：bitlocker、partition、partition-creation、partition-conversion 四组 DOM 在 HEAD（7f54e65 之前）即已失败，与本片无关，已用 stash 对照确认，尚未定位。
- 安全与风险：新增原因码只描述失败类别，不暴露路径、盘符或容器内容；卷根探测只读属性。未做实机人工界面走查。
- 提交：本片实现与测试待用户审阅，未提交、未推送。


### 2026-09-24 — fix / 挂载盘符随机分配
- 范围与审计：用户要求每次挂载取一个随机的未占用盘符，不再固定从 Z 往 A 扫。审计 8.6 本地状态层要求，其中"盘符历史"属于必须不可区分的痕迹，与本改动同向；否认性真正依赖的是"模式 A/B 盘符数量一致"，本片只改字母不改数量，不触碰该约束。
- 检查与实现：会话侧 allocate_drive_letter 与提权 helper 的 choose_temporary_drive 原本是两段结构相同的 Z→D 扫描，合并为共用的 volume::choose_free_drive_letter；两处仍各自保留原有的占用位图采集和失败异常类型。
- 检查与实现：候选集固定为 D..Z，A／B／C 任何情况下不参与。随机数取自 BCryptGenRandom 的系统首选 RNG，与仓库其他取随机处一致；用拒绝采样消除取模偏置，否则低位字母会被偏向。
- 安全复审自查后加固：拒绝采样原为无界循环，若生成器恒定返回同一值会在挂载路径内自旋，改为最多 64 次后抛出并如实报错。
- 验证：新增 drive_letter_pool_tests 并接入 CTest，覆盖唯一空闲盘符必被返回（D..Z 全量遍历）、A/B/C 永不产出、全占用返回空、4000 次抽样覆盖全部 23 个字母、空闲集下标与计数对应、越界或缺失随机源被拒、随机值恒在界内。临时改成固定取第一个后该用例失败（"a free drive letter was never drawn"），确认能捕捉回归。
- 验证：p46 全量构建通过，全量 CTest 73 项中 72 项通过；管理员权限下 partition_device_integration_tests --run-owned-vhd-session 再次通过，确认随机盘符在真实驱动上可挂载并与上一片的盘符回读一致。
- 已知问题：vikey_helper_hid_compatibility_tests 失败（development HID probe 属性 0xfff 与预期 0xffd 不符），已用 stash 对照确认在 HEAD 即失败，与本片无关，尚未定位。四组分区／BitLocker DOM 测试仍停留在 25d23fe 之前的旧交互，同样未修。
- 安全与风险：不新增持久化，不记录已用盘符，不改变任一模式下挂出的盘符数量。新增失败路径：系统 RNG 不可用时挂载会失败而非退回固定字母。随机化会把 Windows 自身按盘符留下的痕迹（如资源管理器 MountPoints2）分散到更多字母上，而不是集中在一个——相对固定盘符是消除了规律但扩大了面，是否更符合 8.6 需用户确认。
- 提交：实现与测试待用户审阅，未提交、未推送。


### 2026-09-24 — fix / 创建失败按原因给出文案
- 范围与审计：用户反馈文件容器隐藏盘创建失败无从判断原因。审计发现原生侧 creation_error_ 一直有值，但页面只对磁盘容器显示裸数字，文件容器连数字都不显示，只有一句"创建失败"。本片只改失败呈现，不改创建流程与任何门禁。
- 检查与实现：新增 creationFailureMessages 覆盖 CreationError 1–18，配 describeCreationFailure 统一输出"说明（错误码N）"；容器B／配对、分区创建、原地转换三处调用点全部改走它，不再各自拼字符串。取消与清理失败仍用各自更具体的既有文案，文件路径保留"需重新填写名称、路径与诱饵密码"的提示，因为该流程会清空已输入内容。
- 检查与实现：文案按可操作性写，17 明确点出剩余空间、FAT32 单文件 4 GiB 上限与设备连接，10 指向同名文件，11 指向写入权限与安全软件；15 复用既有清理文案，不另造说法。
- 验证：页面契约新增三条断言（三个调用点共用同一描述、全文不再出现"创建失败。错误码"拼接、表中保留 FAT32／同名文件／清理三条可操作原因），CTest 通过；首次因把函数定义误计入调用点数（断言 4 实为 3）失败，修正断言后通过。JS 语法检查通过；navigation DOM 307 项、exit／login-reset／startup 三组 DOM 通过。
- 验证限制：partition-creation 与 partition-conversion 两组 DOM 原本钉死旧字符串"创建失败。错误码17"，已同步改为新文案，但这两组在 25d23fe 之后就因两步式创建流程在更早的断言处失败，本次改动未能实际跑通它们，属未验证修改。
- 安全与风险：文案均为瞬时界面文本，不持久化、不含路径或容量数值。错误码 14 的文案提到隐藏盘容量，但该码只可能由隐藏／配对创建路径产生，且仅显示在用户正在创建隐藏卷的弹窗内，不构成新的存在性泄露。
- 提交：实现与测试待用户审阅，未提交、未推送。目标盘剩余空间预检查为下一片。


### 2026-09-24 — fix / 创建前检查目标位置剩余空间
- 范围与审计：接上一片，用户要求把最可能的创建失败提前成明确提示。审计确认创建前完全不查目标盘，volume_space 只用于已挂载容器的容量显示；容器按完整尺寸落盘，空间不够会在格式化中途失败，留下半成品并浪费等待。
- 检查与实现：检查放进 resolve_target_path——该函数注释本就写明"位置不存在必须报成字段错误，而不是几步之后那个不透明的创建失败"，空间不足属同一类事实。标准与隐藏两条路径都传入所需字节；隐藏容器按外层尺寸计，隐藏卷在该文件内部，不额外占用保存位置。
- 检查与实现：新增 volume::writable_free_bytes，按目录而非盘符询问，使挂在文件夹上的卷也能量对，并取 GetDiskFreeSpaceExW 的调用者可用值而非卷总空闲值，配额卷上才诚实。新增 CreationError::destination_full = 19 与页面文案；该码只由应用侧产生，helper 从不发送。
- 验证：新增控制器用例，按目标目录实际空闲空间请求 free+1 MiB，断言标准与隐藏两条路径都以 destination_full 拒绝、未落文件、未触达卷会话，且最小容量仍能通过该门禁；空闲空间超过 UI 上限时显式跳过并打印原因。临时停用检查后该用例失败（"a container larger than the location was not refused as such"），确认能捕捉回归。本机临时目录空闲 13.7 GB，用例实际执行未走跳过分支。
- 验证：p46 全量构建通过，全量 CTest 73 项中 72 项通过；navigation DOM 307 项及 exit／login-reset／startup 三组 DOM 通过；git diff --check 通过。
- 验证限制：未做真实小容量磁盘上的写满实测；检查与实际写入之间空间仍可能变化，卷层的中途失败路径保留未变。
- 已知问题：vikey_helper_hid_compatibility_tests 仍失败，此前已用 stash 对照确认在 HEAD 即失败，与本片无关。
- 安全与风险：只读查询，不创建目录、不预留空间、不持久化任何位置信息；拒绝发生在任何文件被创建之前，因此不会留下半成品。新增失败路径：目标位置无法查询空闲空间时创建会失败而非继续。
- 提交：实现与测试待用户审阅，未提交、未推送。


### 2026-09-24 — fix / 磁盘容器编号不再与文件容器共用计数器
- 范围与审计：用户反馈磁盘容器序号跳号且每次会话刷新。审计定位三个独立缺陷：编号取自全局容器 id（与文件容器共用）、编号与列表显示顺序无关、分区排序根本未持久化。用户本片只要求修第一个，另两个保留未修。
- 检查与实现：adopt_partition 原本用 entry.id 生成"磁盘容器 N"，而 adopt 每收录一个文件容器也会递增同一计数器，于是磁盘容器编号出现取决于当次会话文件容器数量的跳号。改为会话状态内独立的 next_partition_ordinal，只由磁盘容器递增。
- 检查与实现：容器 id 本身未改动——它是页面寻址容器的句柄，必须跨类型唯一；本片只改展示名的序号来源。名称仍是会话内生成、不持久化。
- 验证：新增用例在同一会话内交错创建磁盘容器、文件容器、磁盘容器、磁盘容器，断言三个磁盘容器名为"磁盘容器 1/2/3"，且全部容器 id 仍互不相同。改回 entry.id 后该用例失败（"disk containers were not numbered one after another among themselves"），确认能捕捉回归。
- 验证：为支持该用例，假运行时两处硬编码的 28 字节口令断言改为随 partition_password_64 取 28 或 64，与该标志下 A 派生的实际长度一致；并为该用例补足盘符池。全量 CTest 73 项通过（vikey_helper_hid_compatibility_tests 本轮为跳过，早前同机曾失败，表现随环境变化）；navigation DOM 307 项通过；git diff --check 通过。
- 已知未修（用户次日核正）：编号与拖动顺序无关是应有行为，不是缺陷。真正的缺陷是编号不跟创建顺序：文件容器的排序本就跟会话持久化（prepend_container_index_path_v1 置顶并写 order），而磁盘容器从未接入该机制：新建后 push_back 到列表底部、索引里 order 为 nullopt、排序退回随机槽位、拖动也不写回。下一片修。
- 安全与风险：仅改展示名的序号来源，不新增持久化、不改身份或门禁，不影响挂载与索引记录。
- 提交：实现与测试待用户审阅，未提交、未推送。


### 2026-09-24 — fix / 磁盘容器接入既有的容器排序机制
- 范围与审计：用户核正后重新定位——排序机制本就跨会话（文件容器经 prepend_container_index_path_v1 置顶并写 order），磁盘容器四条行为全部相反：新建后落列表底部、索引 order 为 nullopt、排序退回随机槽位、拖动不写回。本片把磁盘容器接进该机制。
- 检查与实现：索引层新增 ContainerIndexRef（路径或分区身份）与 reorder_container_index_v1，两类共用一个 order 空间；原 reorder_container_index_paths_v1 保留并与新函数共用实现，靠 include_partitions 区分，维持"文件专用操作不改分区槽 byte-for-byte"这条既有保证。新增 list_container_index_refs_v1 与 prepend_container_index_partition。
- 检查与实现：重排分区时按分区重新密封，只改 order，conversion 状态原样带回。会话侧创建改用 prepend 并让 adopt_partition rotate 到列表首位；登录按统一 ref 列表排序，分区按索引倒序收录使展示序号跟随创建顺序；reorder_containers 同时提交两类，并对索引中找不到的分区跳过而不是让整次拖动失败。随机槽位选择未改动。
- 验证：索引层新增用例覆盖新分区置顶、混排两类后重载一致、文件视图仍自洽、重排保留 conversion 状态、未知／重复／两者皆空的引用在写入前被拒且缓冲区不变。会话层新增用例断言新建置顶且带下一个编号、拖动后的顺序按身份写进索引。
- 验证：两处负向对照均确认能捕捉回归——去掉 rotate 后置顶断言失败；把排序改回 file-only 后索引断言 10/10 次失败。既有"文件插入／排序／删除不得改写分区槽"用例继续通过。全量 CTest 73 项中 72 项通过（vikey_helper_hid_compatibility_tests 与本片无关，同机曾失败亦曾跳过）；navigation DOM 307 项通过；git diff --check 通过。
- 已知限制（重要）：展示序号仍由收录顺序推导，而收录顺序现在跟随持久化位置。因此用户拖动列表后磁盘容器的编号会跟着变——这与用户"编号应与拖动无关"的期望不符。要让编号成为容器自身的固定属性（真正的创建序号），需在记录里另存一个创建计数，属索引格式改动，未做。
- 验证限制：ContainerStatus 对未挂载容器只报 size 0 且不含分区身份，跨会话无法在会话层区分具体分区，故"重启后位置不变"只在索引层验证，未做实机重启走查。
- 安全与风险：不改随机槽位策略、不新增明文、不升索引版本；order 与 conversion 均在既有密封记录内。分区记录现在会因重排被重新密封，写入仍是先全部加密再整体落盘，失败不改调用方缓冲区。
- 提交：实现与测试待用户审阅，未提交、未推送。


### 2026-09-25 — fix / 磁盘容器名改为记录中的创建序号（索引升 v6）
- 范围与审计：上一片把位置持久化后，展示序号仍由收录顺序推导，拖动列表会改名，用户明确这不对。按用户选定的 B 方案：把创建序号存进记录，接受索引版本升级的兼容代价。
- 审计与取舍：分区载荷的空闲区不可直接使用——明文写字段前整块 fill_random，读出来与随机数不可分，故必须靠版本号声明字段存在。序号放进注释中早已写明用途的保留尾部 [528,532)，槽大小与文件大小不变（文件大小是观察者唯一能测到的量，不得改动）。
- 检查与实现：新增载荷版本 6，取代 3/4/5 用于此后所有写入：layout 改由自身字段声明而非由版本号隐含，恒带 conversion 状态，并携带创建序号。读端继续接受 3/4/5 为无序号旧记录；v6 中序号为 0 视为畸形，与不支持的槽一样按填充处理。
- 检查与实现：新记录取"当前已记录的最大序号 + 1"，中间空档保留不复用。每条重新密封的路径都带序号：重排（位置变而序号不变）与 conversion 状态更新均已覆盖。新增 number_container_index_partitions_v1，在登录时一次性给旧记录补号并写回；旧记录没有任何创建顺序可恢复，按当前位置自下而上补号，此后不再变动。
- 检查与实现：会话侧名字改为读记录中的序号。清空创建在 prepend 之后回读序号，名字当场正确；原地转换的记录在 persist 时才落盘，故先按会话计数命名、记录落盘后立即改名。
- 验证：索引层新增用例覆盖序号按序发放、重排后序号不动而位置改变、conversion 更新不丢序号、中间空档不复用、旧记录可读且补号一次后第二次为 no-op。会话层新增验收用例：创建后为 3/2/1 且最新在顶、拖成 1/3/2 后名字不变、重启后名字与顺序完全一致、删掉中间号后新建取 4。
- 验证：负向对照把名字改回按位置推导后，"a restart changed the disk container names or their order" 失败，确认用例能捕捉回归。全量 CTest 73 项中 72 项通过（vikey_helper_hid_compatibility_tests 与本片无关）；navigation DOM 307 项通过——首次运行紧接 215 秒全量测试之后撞上 30 秒 deadline 超时，单独重跑两次均通过，本片未改任何页面文件。
- 修正的自身错误：最初注释与用例声称"序号永不复用"，实测发现取剩余最大值 +1 在删掉最高号后会复用该号。记录槽删除后被随机字节覆盖，"历史最高"无处可查，故改为如实描述：中间空档保留，最高号在被遗忘后会重新可用。
- 安全与风险：序号在 AES-GCM 载荷内，只有持根派生密钥者可见；但这引入一项新推断——持密钥者可由"最高序号 > 记录数"看出曾删除过磁盘容器，而此前删除不留痕。该信息不指示密码档位或隐藏卷存在性，且持密钥者本就能看到全部记录，判断为可接受，但属本片新增。
- 兼容性（重要）：本机写出或补号过的索引，已交付安装包中的应用无法读取，且登录一次即不可逆完成升级。需求原已规定不支持不认识相应版本的旧客户端改写同一索引，本片属该规则内，但现场影响是真实的。
- 提交：实现与测试待用户审阅，未提交、未推送。


### 2026-09-25 — fix / 创建失败码只接受创建错误类别（修回归）
- 范围与审计：/simplify 四路审查发现 9c90d3e 引入的回归。begin_creation 的 catch 用 error.code().value() 存码，丢弃 category，而 CreationError 取值 1..19 与 errno／Win32 密集撞号。
- 已核实的撞号（UCRT errno.h 实测）：errc::file_exists=17 撞 io_error=17，errc::no_such_file_or_directory=2 撞 protocol_error=2，EACCES=13 撞 bitmap_scan_failed=13。会话在 1469／1564 行正是以 file_exists 抛出"同名文件已存在"，于是页面把它报成"读写失败……FAT32 单个文件不能超过 4 GiB"，而表中第 10 条本就有正确文案。
- 性质：9c90d3e 之前页面只印裸数字，印错不了；加了文案表之后才把撞号变成自信的错误诊断，故该片比改动前更糟，属回归而非既有缺陷。
- 检查与实现：新增 creation_failure_code，把该通道定义为"一个 CreationError 或零"。本类别权威直接取值；唯一有精确对应的通用条件 errc::file_exists 译为 target_exists；其余留零，由既有的 status 3 兜底转成 E_FAIL，页面照原样显示"原因无法确定"。状态分类（取消／清理失败）本就按完整 error_code 比较，不受影响。
- 对称性记录：目录读取那条通道当初处理了 category（并刻意收紧 access_denied 只认 Win32 值，不吃 std::errc 折叠），创建这条却没处理。同一个坑一处防了一处没防。
- 验证：既有用例本就有一例"以通用类别抛出 operation_cancelled 的数值"，把断言收紧为"状态 3 必须读回 E_FAIL"后，该例同时证明撞号码不会被当成创建错误交给页面；另新增一例以 errc::file_exists 抛出并要求读回 target_exists。负向对照把存码改回原样后失败于"a failure outside the creation vocabulary was reported as one"。
- 验证：全量 CTest 73 项中 72 项通过（vikey_helper_hid_compatibility_tests 与本片无关）。页面未改动，契约测试通过。
- 未修（审查同批发现，已向用户报告并排期）：D2 原地转换路径仍用 add_container_index_partition，order 为 nullopt；且文件专用重排会让文件与磁盘容器落在同一 rank，新建文件容器后磁盘位置再由随机槽位决定——5c4eadd 的修复不完整。D3 settled_drive_letter 抛异常会留下无人持有的挂载，且装载管理器异步发布可能使回读暂态为零而误伤正常挂载。
- 安全与风险：只收窄页面可读到的码的取值域，不新增持久化、不改门禁；诊断信息未扩大，非本类别的原始 Win32 值不再送到页面显示。
- 提交：实现与测试待用户审阅，未提交、未推送。


### 2026-09-25 — fix / 原地转换的分区也记录位置
- 范围与审计：/simplify 审查指出 5c4eadd 的排序修复不完整。已核实 convert_partition 在 persist 时用 add_container_index_partition 插入记录，order 为 nullopt，于是原地加密出来的磁盘容器仍然并列排在末尾、由随机槽位决定先后；只有清空创建走了 prepend。
- 检查与实现：转换路径改用 prepend_container_index_partition，与清空创建一致。会话侧 adopt_partition 本就 rotate 到列表首位，改后索引与内存顺序一致。转换状态照原样随插入一起写入，未改其他门禁。
- 验证：在既有 test_partition_conversion_lifecycle 的成功场景后新增断言，要求记录同时具备 order 与非零 sequence。断言最初写在 during_conversion 回调里，抛出被转换路径吞成笼统的 outcome mismatch，已挪到转换完成之后，负向对照即报出本条消息。
- 验证：改回 add_container_index_partition 后该断言失败（"a converted partition was recorded without a position or a number"）；全量 CTest 73 项中 72 项通过（vikey_helper_hid_compatibility_tests 与本片无关）。
- 未修并已上报：文件专用重排（reorder_container_index_paths_v1）把文件条目重编为 0..n-1 而不动分区 rank，两类因此可能落在同一 rank，ordered_entries 的并列再退回随机槽位——新建文件容器后磁盘容器位置仍可能变。该修复取决于下一条的决定，未动。
- 🔴 待用户决定的否认性问题（审查发现，先于本次改动存在，但 5c4eadd 扩大了范围）：reorder_entries 会把当前密钥能打开的每一条记录全部重新密封，无论其 rank 是否变化。需求 2328-2331 要求"只动要动的那一槽，未触碰的槽必须逐字节原样写回"，理由是两份快照一比，没变的槽就是被占用的槽。全量重封是同一泄漏的镜像：变了的槽就是被占用的槽，条数与位置同样一次交出。原实现只重封文件记录，5c4eadd 之后连分区记录一起重封，范围因此扩大。三种处置（只重封 rank 真正变化的槽／每次写入整份重随机使差异无信息／保留现状并接受）代价与合规解释各不相同，属 R-03／8.6 范畴，未自行决定。
- 安全与风险：本片只改一处插入调用，不新增持久化、不改身份与同意门禁、不触碰随机槽位选择。
- 提交：实现与测试待用户审阅，未提交、未推送。

### 2026-09-25 — fix / D2 统一混合排序并保留未变记录密文
- 范围与审计：用户确认先统一排序、只改实际变化记录；整槽换位另片处理。核对需求排序增补、8.7 索引重写约束、R-03／8.6 及 MILESTONES，未发现本片需求冲突。
- 检查与实现：检查索引读写、文件置顶、分区置顶、会话转换调用和既有测试；保留进入本片前的转换置顶及测试修改，不提交其他历史工作。
- 检查与实现：移除文件重排排除分区的分支，两个入口统一对当前密钥可见记录编号；指定项置前，未指定文件与分区保持原相对顺序，离线记录不删除。
- 检查与实现：排序编号相同即跳过重新密封；编号变化时保留分区身份、转换状态和创建序号；头文件同步修正文件排序绝不改分区槽的旧保证。
- 验证：新增确定槽位的混合交换、未变记录／填充／异密钥逐字节保留、重复排序零字节变化测试，并补文件置顶后完整混合顺序断言。
- 验证：首次构建因新增测试 optional 与有符号零比较触发 /WX 失败，改为 0u 后 p46 全量构建通过；该失败后启动的旧二进制测试不计作本片证据。新构建索引测试单独运行通过；全量 CTest 73 项中 71 项通过，索引三组、登录挂载会话和页面契约通过，git diff --check 通过。
- 验证限制：vikey_helper_process_tests 报暂存目录经过重定向，未做基线对照、尚未定位；vikey_helper_hid_compatibility_tests 报属性 0xfff／0xffd 不符，与历史记录一致。不能将本轮称为全绿。
- 安全与风险：输入校验及加密全部在复制缓冲区中完成后才回写，未新增明文元数据、密钥副本或落盘渠道；不移动槽，不改原子落盘与文件尺寸。实际排序值变化仍会暴露更新集合，不声称消除快照差异。
- 后续边界：用户决定切片 2 暂不实施，整槽换位未做；旧记录无编号／重号的跨密钥显示兼容及需求换位表述留待以后评估。此前日志“条数与位置一次交出”仅应理解为特定操作知识下推断更新集合，不能等同读取路径、类型或隐藏卷存在性。
- 提交：用户已授权提交 D2 排序修复（含此前转换置顶），本笔提交包含实现、测试和相关日志；历史交付记录留在工作区，禁止推送。未进行实卡／实体盘或界面手工验收。

### 2026-09-25 — fix / D3 成功挂载必须交出可管理的盘符
- 范围与审计：用户授权修 D3。核对需求 924（精确残留卷）、2576（只清理会话卷）、R-03／8.6、MILESTONES 会话回滚条款及 provider／会话／编排测试；无本片需求冲突。
- 审计纠正：最初预计生命周期与查询重试两片；本地 out/p326-veracrypt-upstream/src/Common/Tcdefs.h 确认版本 1.26.29，Driver/Ntdriver.c 的 TCCreateDeviceObject／MountDevice／MountManagerMount 均使用同一个 nDosDriveNo，盘符冲突拒绝而不另选号，挂载清单直接枚举驱动对象，不依赖异步 shell 发布。
- 检查与实现：此前“挂载管理器自动改盘符”及因此需要清单重试的前提不适用于该固定 ABI；收敛为一片。移除挂载成功后的按路径清单回查，避免查询失败、路径表示差异或多重匹配产生零盘符后被会话抛弃。
- 检查与实现：文件／分区挂载共用 complete_mount，调用驱动前校验并保存盘符编号，仅在事务成功且驱动返回码为零后返回 A..Z；返回前无额外查询／内存分配，既有 shell 通知保持 noexcept；成功与异常均清零锁页请求。
- 检查与实现：同步 MountResult／运行时契约与会话注释；会话继续使用 provider 返回值。未新增猜测性卸载、强制卸载或认证重试，不修改驱动、分区身份门禁、挂载参数或持久化格式。
- 验证：新增 A..Z 全范围成功接管、单次事务、结果标志、传输异常／认证失败／占用错误类别、非法盘符调用前拒绝与双密码缓冲清零测试；分区假运行时复用完成路径，旧零盘符测试改为拒绝挂载不留附件检查。
- 验证：provider、mount_orchestrator、card_login_mount_session 三组构建及 CTest 3/3 通过；p46 全量构建通过；全量 CTest 71/73 通过，helper_process 暂存目录重定向与 helper_hid 属性 0xfff／0xffd 两项失败均与本轮修改前 D2 验证相同；git diff --check 通过。
- 安全与风险：解决成功后的附加查询导致失去挂载管理这一具体缺陷；不承诺驱动违约／进程崩溃／其他进程并发卸挂时的原子所有权，也不将驱动设备已存在等同文件系统或资源管理器已就绪。未做实卡／实体盘、干净机或新的 VHD 验收。
- 提交：用户已授权提交本片实现、测试及相关日志，禁止推送。D2 的整槽换位仍暂不实施；身份匹配去重重构作为下一独立切片。

### 2026-09-25 — refactor / 索引与会话共用容器身份匹配
- 范围与审计：用户要求提交 D3 后推进下一片；D3 已提交 6e01d59，整槽换位继续暂缓。本片落实已讨论的 rank／names 重复规则隐患，核对排序增补、索引离线记录约束和 MILESTONES，无冲突。
- 检查与实现：检查索引 names、same_path、会话恢复 rank 与现有混合排序／跨登录测试；两处目前合法输入行为一致，本片消除后续规则分叉风险，不宣称修复已复现错排。
- 检查与实现：提取 container_index_ref_matches_v1，索引重排与会话恢复共同调用；文件按大小写不敏感的完整 ordinal 路径，分区按完整身份，分区会话显示路径不参与身份判断。
- 检查与实现：畸形引用不匹配，跨类型不匹配；未记忆会话项仍排在已匹配项后，保持其相对顺序，不因索引正常缺失报错。不扩展为文件系统别名／短路径解析。
- 验证：相关核心及两个测试目标构建通过，container_index_view_v1_tests 与 card_login_mount_session_tests CTest 2/2 通过；覆盖大小写、路径视图长度、前缀误匹配、分区显示路径、同设备不同分区和畸形引用，既有混排／跨登录用例通过。
- 验证限制：本片未重跑全量 CTest 或实机验收；上一片 D3 全量 71/73 及两项 helper 失败仍属此前结果，不能当成本片全量通过。git diff --check 通过。
- 安全与风险：不新增持久化、不改变加密格式或槽位写入，不新增路径副本、文件系统访问或身份弱化；未记忆项排后仍是明确的兼容行为。
- 提交：用户已授权提交本片实现、测试与相关日志，禁止推送；历史交付日志保持工作区未提交。

### 2026-09-25 — refactor / 第一组索引扫描复用与空号补用
- 范围与审计：按用户最终决定推进第一组；新编号补用最小未用正整数，保存排序继续跳过索引缺失分区、不新增“未知容器”错误。核对需求排序／索引重写条款与里程碑，行为调整补入需求原文。
- 检查与实现：登录 prepare_container_index_v1 一次解析同时完成旧记录补号、返回更新后的排序列表；已有编号不变，无旧记录不复制索引缓冲，不保留跨操作缓存。
- 检查与实现：插入和补号统一使用有界空号分配器，高于槽位上界的已有编号无需递增，避免 uint32 回绕；旧记录继续由列表末尾向前补号，未变槽逐字节保留。
- 检查与实现：add／prepend／状态更新可回传记录编号，删除 recorded_sequence 再扫描；满表、缺失或异常不发布输出编号，置顶成功后才发布。转换续作的持久化暂停许可检查保留。
- 检查与实现：拖动只建 refs，用同一解密列表查文件与分区，保留缺失分区跳过与离线项；补录仍经既有安全插入，最终重排重新解析修改后缓冲，不向重排传旧快照。
- 扫描核对：按代码调用路径普通登录 3→1（旧记录迁移 4→1），创建分区 3→2，拖动无补录 3→2；每个待补录文件仍增加一次插入扫描。未重新测量毫秒耗时，不声称等比例用户体感加速。
- 验证：初轮索引／会话 CTest 2/2 通过；新增用例覆盖最小空号、高编号、多个旧记录补号及状态保留、异密钥／填充逐字节不变、幂等、重复／满表／缺失的输出编号语义，以及缺失分区拖动不失败不补录。新索引用例单独运行通过。
- 验证：p46 全量构建通过，全量 CTest 71/73 通过；helper_process 暂存目录重定向及 helper_hid 属性 0xfff／0xffd 两项失败与此前一致，未修复、不计通过；git diff --check 通过。未执行实卡／实体盘或重启人工验收。
- 安全与风险：不改加密槽格式／尺寸、随机落槽、密钥派生或写盘原子性；补号先在副本完成全部加密和结果构造才回写。现存编号保留，不修复历史重复编号；补空号后名称不再表示创建先后。
- 提交：用户已授权复查后提交本片，最终复查未发现新增阻塞问题；提交实现、测试及相关需求／日志，禁止推送。历史交付日志保留，整槽换位和其他分组重构未实施。


### 2026-09-25 — refactor / 会话恢复排序位置预计算
- 范围与审计：核对排序增补、P3.23 与 R-03／8.6 边界；仅优化登录恢复的会话排序，不调整拖动持久化、编号或整槽换位，无需求冲突。
- 检查与实现：检查 adopt_remembered_containers、共享身份匹配函数、SessionContainer 字段及既有混合排序／重启测试。
- 检查与实现：每个容器查找一次首个匹配位置，稳定排序临时原位置与 rank；未匹配条目继续稳定排在底部，比较器仅比较数字。
- 检查与实现：复制完整容器对象到临时列表后统一 swap，准备阶段异常不会部分搬空现有容器；既有接纳流程及异常吞并策略不变。
- 验证：新增混合文件／暂停分区恢复用例，覆盖两个未匹配布局条目相对顺序、ID／名称／分区标记／转换状态／未挂载状态，以及一次读取、零写入、密文不变。
- 验证：首次构建因新测试误将公开状态的分区布尔标记当身份对象比较而失败，修正断言后全量构建通过；card_login_mount_session_tests 与 container_index_view_v1_tests 两项均通过（11.85 秒）。git diff --check 通过。
- 安全与风险：复查未新增密钥副本、持久化元数据或 I/O；全对象复制保留挂载与维护字段。复制带来临时对象内存开销，不引入跨操作缓存。
- 安全与风险：本片未做耗时基准、全量 CTest、实卡／实体盘验收或分配失败注入，不声称登录可感知提速；历史全量测试中的两项 helper 失败状态不因本次定向测试改变。
- 提交：2026-09-26 用户授权复查后提交，最终差异检查通过，沿用本片构建与定向测试结果；仅提交本片实现、测试及日志，禁止推送，历史交付日志保留工作区。
