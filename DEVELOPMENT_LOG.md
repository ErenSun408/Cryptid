# 开发日志

> 本文件用于长期记录实施过程、验证结果、安全审计、遗留风险和提交决定。
> 阶段范围以 `MILESTONES.md` 为准，技术决策以 `需求梳理清单_v2.md` 为准。

## 条目模板

每个开发阶段的日志应包含：

- 日期与阶段
- 当前状态
- 计划与需求审计
- 已检查的现有代码
- 实现内容与设计决策
- 验证项目与结果
- 安全审计与发现
- 遗留风险或后续事项
- 提交决定，以及提交哈希（如有）

---

## 2026-08-21 — P1.0 安全基础设施

**状态：** 已完成并获准提交

### 计划与需求审计

- `MILESTONES.md` 将 P1.0 定为首个正式代码阶段，要求完成 `SecureBuffer`、
  编译期 ABI 断言框架、`/W4 /WX` 和 AddressSanitizer 接入。
- P1.0 的出口条件为：复制敏感缓冲区的代码在编译期失败；AddressSanitizer
  无异常；所有传递给驱动的结构体在引入时均带有 `static_assert`。
- `需求梳理清单_v2.md` 的 R-03 要求明文驻留时间最小化、使用
  `SecureZeroMemory` 清零、禁止使用会静默重分配的容器，并通过
  `VirtualLock` 锁页。
- 按 P1.0 实施时的阶段编号，P1.0 至 P1.3 必须能在 `ICardStore` 桩实现下
  完成测试；真实 USB 硬件不是 P1.0 的依赖。该编号已于 2026-08-22 重排，
  新边界见后文「P1 子阶段重排」。

### 已检查的现有代码

- 仓库原有内容包括需求文档、HTML UI 原型和 `spike/` 下的技术可行性代码。
- 此前不存在正式产品工程、正式源码目录或正式测试套件。
- `spike/` 仅用于验证技术行为，不能演变为正式产品代码库。

### 实现内容与设计决策

- 建立首个正式 Windows x64 C++20 工程，使用 CMake 和 Ninja 预设。
- 所有正式目标均启用 MSVC `/W4 /WX`、`/permissive-`、`/sdl` 和 UTF-8
  源码处理；构建系统拒绝非 Windows 或非 x64 工具链。
- 新增相互独立的 `dev`、`asan` 和 `analyze` 配置、构建与测试预设。
- 将 `SecureBuffer` 实现为固定大小、禁止复制、允许移动的 RAII 类型。
- 非空缓冲区使用独立的 `VirtualAlloc` 分配，并要求 `VirtualLock` 必须成功。
  锁页失败时按失败关闭原则处理，在缓冲区接收秘密数据前抛出带 Win32
  错误码的 `std::system_error`。
- `clear()` 使用 `SecureZeroMemory`，不会重新分配或解除锁页。析构和移动
  赋值均先清零原有字节，再执行 `VirtualUnlock` 和 `VirtualFree`。
- 新增可复用的标准布局、结构体大小和成员偏移 ABI 断言宏。
- 新增预期编译失败目标，主动尝试复制 `SecureBuffer`；只有编译器拒绝该
  目标时，CTest 才将测试判定为通过。

### 验证项目与结果

- 已确认 `D:\BuildTools\VS2022` 下安装 Visual Studio Build Tools 2022、
  MSVC x64、CMake、Ninja 和 MSBuild。
- `dev`：在 `/W4 /WX` 下构建通过，CTest 2/2 通过。
- `asan`：优化且带插桩的构建通过，CTest 2/2 通过，AddressSanitizer 无异常。
  构建命令包含 `/fsanitize=address`，测试程序依赖
  `clang_rt.asan_dynamic-x86_64.dll`。
- `analyze`：MSVC `/analyze` 构建无发现，CTest 2/2 通过。
- 运行测试覆盖空缓冲区、零初始化、写入、显式清零、移动构造、移动赋值、
  自移动和锁页状态。
- `QueryWorkingSetEx` 确认内存页在分配后以及显式清零后仍处于锁定状态。
- 负向编译测试按预期失败：MSVC C2280 报告调用了已删除的复制构造函数。
- 优化目标文件的反汇编中存在清零用的 `rep stos` 指令，并确认释放顺序为
  清零、`VirtualUnlock`、`VirtualFree`。
- 测试程序启用了高熵 ASLR、动态基址和 NX 兼容标志。
- `git diff --check` 未发现空白字符错误。

### 安全审计与发现

- P1.0 正式产品代码中未发现尚未解决的漏洞。
- 审计期间已修复：最初的 `VirtualLock` 失败路径会重复清零一块刚分配且本来
  就为零的内存。对于超大请求，这可能无意义地触碰大量内存并放大拒绝服务
  影响。现已移除该冗余清零；构造函数返回之前不可能已有秘密数据写入。
- 已确认正式产品代码未使用 `std::string`、`std::vector`、`memset` 或
  `memcpy` 存储或清除秘密数据。
- `VirtualLock` 只能降低换页暴露，无法抵御进程已被攻陷、特权内存检查、
  崩溃转储或休眠镜像捕获；API 注释已明确该安全边界。
- ASan 不能证明安全清零，因此另通过运行时回读和优化后机器码检查验证清零。

### 遗留风险或后续事项

- 当前尚无正式的驱动通信结构体。按当时编号，原 P1.1 引入任何发送给
  VeraCrypt 驱动或从驱动接收的结构体时，必须应用新的 ABI 断言；该工作在
  重排后的 P1.2 落地。
- P1.1 必须让口令输入和派生密钥始终处于 `SecureBuffer` 作用域内，不得为
  这些数据引入普通字符串或动态数组容器。
- USB 硬件按当时计划不属于 P1.0 范围；重排后在 P1.6 前不构成阻塞。

### 提交决定

- 已于 2026-08-22 批准提交；本条记录随 P1.0 提交落库。
- 提交：`e0909a2 [P1.0]add secure memory foundation and build validation`

---

## 2026-08-22 — P1.1 密钥派生

**状态：** 已完成并获准提交

### 计划与需求审计

- 重排后的 P1.1 只覆盖密码学供应链、KDF v1 合约、敏感输入输出和 Argon2
  大工作区生命周期；驱动 ABI、挂载与卸载属于 P1.2，不进入本阶段提交。
- `需求梳理清单_v2.md` 要求 Argon2id 参数固定为 m=512 MiB、t=2、p=1，
  派生耗时须纳入 P-002 五秒预算，且 R-03 的秘密驻留与可靠清零要求继续适用。
- KDF v1 已确认：用户输入是 password，32 字节卡材料是 Argon2 secret `K`，
  每容器使用 16 字节公开随机 salt，固定域标识作为 associated data，输出
  64 字节原始 VeraCrypt 卷密码。
- 已确认 UTF-8 输入不做 Unicode 规范化；小秘密强制锁页，512 MiB 工作区
  使用可分页私有内存并在释放前清零，不因资源不足静默降低参数。

### 已检查的现有代码与上游材料

- 已检查 P1.0 的 `SecureBuffer`、构建预设、负向复制测试和 ABI 断言设施。
- 已检查 RFC 9106 对 password、salt、secret 与 associated data 的输入定义。
- 官方 `phc-winner-argon2` tag `20190702` ZIP 由离线方式提供；标签对应提交
  `62358ba2123abd17fccf2a108a301d4b52c01a7c`，归档 SHA-256 为
  `506A80B90AC3CA8407636F3B26B7FED87A55F2F1275CDE850CD6698903C4F008`。
- 归档 82 个条目均位于预期顶层目录，不存在路径穿越；许可证允许 Apache-2.0
  或 CC0，项目按已确认方案选择 Apache-2.0。
- 已确认 VeraCrypt 本地源码中的 Argon2 是其自行修改版本，未将其冒充或替代
  已确认的官方 P-H-C 参考实现。

### 实现内容与设计决策

- 将官方 reference 单线程构建所需的最小源码集原样导入 `third_party/argon2`，
  保留完整许可证和来源说明；导入文件逐个与离线归档校验一致。
- 新增固定 KDF v1 接口：Argon2id v19、m=524288 KiB、t=2、p=1、32 字节
  card secret、16 字节 salt、64 字节原始输出，associated data 固定为
  `usb-vault:volume-password:v1`，调用方不能覆盖或调低参数。
- KDF 只接受 `SecureBuffer` 中的非空 UTF-8 密码和恰好 32 字节卡材料；
  使用 `MultiByteToWideChar` 与 `MB_ERR_INVALID_CHARS` 验证 UTF-8，
  不产生普通堆上的转换结果，也不进行 Unicode 规范化。
- 512 MiB 工作区由 Argon2 allocator callback 通过 `VirtualAlloc` 分配。
  上游释放前清零一次，回调再调用 `SecureZeroMemory`，最后 `VirtualFree`。
- 密码、卡材料与 64 字节派生输出保持在强制锁页的 `SecureBuffer` 中；原始
  输出直接写入锁页缓冲区，不经过字符串、十六进制或 Base64。
- 进程内派生由互斥锁串行化，将单进程峰值 Argon2 工作区限制为 512 MiB。
- CMake 启用 C 语言并固定 C/C++ 编译器为 MSVC，避免系统 PATH 将新增 C
  源码误交给旧 MinGW；官方参考实现以 `ARGON2_NO_THREADS` 构建。
- ASan 可执行目标会复制匹配当前 MSVC 配置的 runtime DLL，避免多个插桩测试
  连续启动时因临时 PATH 不稳定出现 `0xc0000135`。

### 验证项目与结果

- 独立生产参数向量由 Python `cryptography 50.0.0` Argon2id 生成，正式
  P-H-C reference 实现的 64 字节结果逐字节一致：
  `AB9C82025115EBFEA6BAE133D9FC7348AE11CAA6042ABDCD7ED353A8F698167ED89C0F13F5B49AF00CCB8CA7020DD961342D4E65DA1F39386135E374B1127472`。
- 测试覆盖非 ASCII UTF-8、错误编码、空密码、错误 card secret 长度、固定参数
  向量与派生输出锁页状态；前置错误不会分配 512 MiB 工作区。
- 重排前的完整工作区验证中，`dev`、`asan`、`analyze` 均通过；优化版 KDF
  实测 788–847 ms，低于 Spike 6 的目标机 1.04 秒基准。
- 已在只含 P1.0+P1.1 的独立快照上重新配置、构建并验证：`dev` 3/3、
  `asan` 3/3、`analyze` 3/3 全部通过；AddressSanitizer 无异常，MSVC
  代码分析构建无发现。
- `git diff --check` 未发现空白字符错误。

### 安全审计与发现

- 正式 KDF 代码未发现秘密进入普通 `std::string`、`std::vector`、日志或异常文本。
- KDF context 显式固定 v19、Argon2id、成本、secret 与 associated data；
  未启用会清空调用方缓冲区的 flags，秘密生命周期仍由 `SecureBuffer` 所有者控制。
- 内部分配失败返回固定内存错误，不降参重试，也不切换到较弱 KDF。
- 参考实现内部清零保持开启；固定单线程路径不存在成功分配后绕过 `finalize()`
  的运行时错误分支，释放回调在成功派生中执行第二次清零。
- 并发序列化避免不受限的 512 MiB 并行分配形成进程内拒绝服务放大器。
- 未发现尚未解决的 P1.1 代码漏洞。

### 遗留风险或后续事项

- 可分页工作区的中间状态可能进入分页文件；进程崩溃、崩溃转储或休眠镜像也可能
  在清零回调执行前捕获内容。当前清零不能追溯擦除这些外部副本，这是已接受并
  必须持续记录的威胁模型残余风险。
- 官方 P-H-C 仓库没有安全策略且实现基本停止维护。`20190702` 仍是最新 release，
  但“没有已发布公告”不能证明不存在漏洞；发布前必须重新复核供应链状态。
- 当前使用固定测试卡材料；真实 USB 卡读取属于 P1.6。
- KDF 元数据层尚未实现。后续只允许持久化 KDF 版本、固定参数与 salt，不得记录
  档位、隐藏卷存在性或可推导这些信息的字段。

### 提交决定

- 已于 2026-08-22 批准按重排后的阶段边界继续拆分提交。
- 本条随 P1.1 提交落库。

---

## 2026-08-22 — P1 子阶段重排

**状态：** 已完成并提交计划文档

### 计划与实现边界审计

- 原 P1.0~P1.4 总范围合理，但原 P1.1 同时覆盖第三方密码学、KDF、安全内存、
  驱动 ABI、挂载、卸载和性能闭环；原 P1.4 同时覆盖卡通信与插拔安全响应；
  最小 UI 没有独立出口条件。
- 已按依赖重排为 P1.0~P1.7：安全基础、密钥派生、单容器驱动边界、双容器
  应用编排、建卷工具、最小 UI、真卡存储、插拔监听与真机闭环。
- P1.0~P1.5 必须在 `ICardStore` 桩下完成，只有 P1.6~P1.7 依赖真卡。
- 当前工作区文件边界确认 Argon2+KDF 可独立形成 P1.1，驱动 ABI、provider、
  卸载重试和真实驱动测试可独立形成 P1.2。
- 重排不增减 P1 范围，也不改变技术需求；P1.5 不吸收 P2 的注册登录、三模式
  策略、完整建卷向导或全量 UI。

### 验证、安全审计与提交决定

- 已同步 `MILESTONES.md` 与技术主文档中的编号、出口条件和硬件边界。
- 新边界将密码学供应链、驱动 ABI、失败回滚、提权、卡秘密和设备移除响应拆开，
  降低单次审计与回退的耦合风险。
- 计划修改未触碰运行时代码；编号一致性和 `git diff --check` 均通过。
- 提交：`f691745 [docs]split P1 into auditable development stages`

---

## 2026-08-22 — P1.2 单容器驱动边界

**状态：** 已完成安全审计并获准按拆分边界提交

### 计划与需求审计

- P1.2 只覆盖 clean-room VeraCrypt 驱动 ABI、单文件容器挂载、查询、普通卸载
  与有界 busy 重试；双容器顺序和失败回滚属于 P1.3。
- `MILESTONES.md` 要求仅接受已验证 ABI，用 P1.1 KDF 输出挂出可读写容器，
  卸载后 Shell、盘符与驱动列表均无残留，认证到挂载须满足 P-002 五秒预算。
- `需求梳理清单_v2.md` 要求显式指定 SHA-512 PRF，禁止自动探测；密码/PIM
  缓存须关闭，任何本地状态不得记录档位或隐藏卷存在性。
- R-03 继续覆盖传入驱动的卷密码副本：固定缓冲区、锁页、最短驻留和可靠清零。

### 已检查的现有代码与验证材料

- 已检查 P1.0 `SecureBuffer`、ABI 断言宏和 P1.1 KDF 接口。
- 已检查 `spike/README.md`、`spike/SPIKE6-双容器挂载性能.md`、
  `spike/vcspike.c` 与只读 VeraCrypt 参考头；正式代码没有包含或扩展 spike。
- 已核对驱动版本 `0x126`，以及 Password 136、MountRequest 982、
  UnmountRequest 16、MountedVolumes 17424 字节的既有实测 ABI。
- 已核对文件路径传递、SHA-512 PRF 编号 1、关闭 `bCache`/`bCachePim`
  和启用 mount manager 的既有实测结论。

### 实现内容与设计决策

- 新增正式 `VolumeProvider`，覆盖版本查询、单文件容器挂载、卸载和挂载列表。
- 以 clean-room 方式声明 VeraCrypt 1.26 驱动 ABI；四类结构逐字段断言标准布局、
  总大小与关键成员偏移，不包含 VeraCrypt 或 spike 头文件。
- 每次通信前查询驱动版本且只接受 `0x126`；每个 IOCTL 要求返回字节数与预期
  结构大小完全相同，未知版本或畸形响应均失败关闭。
- 挂载路径必须是无内嵌 NUL、长度合法的绝对路径，并规范为 Windows 反斜杠；
  盘符、PIM、密码长度与 PRF 在打开驱动前完成验证。
- 挂载只允许显式 SHA-512 `pkcs5_prf=1`；密码缓存、PIM 缓存、隐藏卷保护、
  恢复模式和可移动盘模式保持关闭。
- 驱动请求使用锁页 `SecureBuffer`。IOCTL 返回后先复制非敏感结果，再立即清空
  含密码的完整请求；错误路径也在抛出前清空。
- Shell 通知改用 `SHChangeNotify` 的盘符路径事件；未沿用 spike 的异步窗口广播，
  避免消息携带栈指针形成潜在释放后使用。
- `VolumeProvider::unmount()` 保持单次驱动边界语义；上层 `unmount_with_retry()`
  在两秒窗口内每 100 ms 只重试精确匹配的 busy 错误，不自动升级为强制卸载。
- 新增单元测试、真实驱动集成测试，以及 Debug/Release 两套本地 integration 预设。
  真实 fixture 与一次性改密工具位于被忽略的 `out/vendor`，不进入仓库。

### 验证项目与结果

- 重排前完整验证：`dev` 4/4、`asan` 4/4、`analyze` 4/4、
  `integration-release` 5/5 全部通过。
- 单元测试覆盖合法请求，以及相对路径、NUL、超长路径、空/超长密码、非法盘符、
  自动 PRF 与越界 PIM 的前置拒绝。
- 真实驱动集成测试覆盖挂载、固定磁盘可见性、文件写入/回读/删除、驱动列表，
  以及卸载后的盘符和驱动记录清理。
- busy 重试策略加入后连续真实集成 5/5 通过；曾有一次首轮 busy、第二次普通
  卸载成功，累计 147 ms，没有使用强制卸载。
- KDF 输出驱动真实 VeraCrypt fixture 的 Debug 闭环为 KDF 3471 ms、挂载
  826 ms、认证到挂载 4299 ms、卸载 87 ms，低于 P-002 五秒上限。
- 优化版真实闭环连续 5/5 通过：KDF 788–847 ms、挂载 718–870 ms、
  认证到挂载 1528–1692 ms、卸载 82–96 ms；每次均一次普通卸载成功。
- 拆分后的 P1.2 快照已重新配置、构建并验证：`dev` 4/4、`asan` 4/4、
  `analyze` 4/4、Debug `integration` 5/5、Release `integration-release`
  5/5 全部通过；AddressSanitizer 无异常，MSVC 代码分析构建无发现。

### 安全审计与发现

- 已修复路径分隔符未规范化、异步通知携带栈指针、IOCTL 返回长度未严格验证
  三项问题；修复后重新通过单元与真实驱动测试。
- 驱动边界未发现越界复制、自动 PRF、密码/PIM 缓存或未知 ABI 继续执行问题。
- 异常文本不包含容器路径或密码；挂载请求不会把密码复制到普通堆容器。
- 密码请求已从“函数结束时析构清零”缩短为“驱动使用结束后立即清零”，不会等待
  Shell 通知完成。
- `mounted_volumes()` 返回易失驱动状态，不写配置、注册表或日志；P1.4 建卷
  验收仍需用其核对 `type=1/0/2`，因此保留查询能力。
- 重试只识别公开数值 busy 错误；认证失败、驱动故障与未知错误不会被误重试，
  正常路径没有隐式强制卸载导致未写完数据损坏的风险。
- 未发现尚未解决的 P1.2 代码漏洞。

### 遗留风险或后续事项

- 本阶段固定支持 VeraCrypt 驱动 ABI `0x126`；升级驱动前必须重新核对全部结构、
  IOCTL 和响应长度，未知版本当前会失败关闭。
- 测试使用离线 fixture 与固定卡材料；真实卡属于 P1.6。
- Windows Shell 或第三方进程短暂持有卷句柄时，普通卸载可能在两秒窗口后仍失败；
  当前策略会明确报告而不会强制卸载，后续 UI 必须把该状态呈现给用户。

### 提交决定

- 已于 2026-08-22 批准继续按重排后的边界拆分提交。
- P1.2 经独立快照复验后提交。

---

## 2026-08-22 — P1.3 双容器应用编排

**状态：** 实现、测试与安全审计完成，等待提交决定

### 计划与需求审计

- P1.3 只负责容器存在性分支、B→A 串行挂载、认证 gate 与失败回滚；建卷、
  UI、真卡和插拔监听分别属于后续阶段。
- `MILESTONES.md` 的出口条件是：真实密码与胁迫密码在 A/B 均存在时挂出相同
  数量的盘；错误密码一个都不挂；任何应用可控失败不得遗留半挂载状态。
- N-05 要求 A 为承载日常数据的普通卷，B 承载普通卷或外层+隐藏卷；两文件
  独立存在，不允许把同一容器同时挂为外层和隐藏卷。
- N-06 要求 B 存在时先以 `KDF(S, 用户输入)` 挂 B；B 失败立即认证失败，
  不得派生或挂载 A；B 成功且 A 存在时，A 才使用 `KDF(S, "diskA")`。
- 8.3/8.6 要求只观察 A/B 文件是否存在，覆盖 6 格状态空间；代码中不得出现
  档位、模式或隐藏卷存在性枚举，也不得通过“两种派生都试一遍”判断形态。
- B 不存在而 A 存在时，A 使用 `KDF(S, 用户输入)` 作为档一认证锚点；A/B
  均不存在时必须报告未建库，不执行 KDF 或驱动调用。

### 已检查的现有代码与测试

- P1.1 `derive_volume_password_v1()` 接受锁页 `SecureBuffer` 的用户输入和
  32 字节卡材料，按每容器 salt 返回 64 字节锁页卷密码；适合在每次 mount
  返回后立即清空派生输出。
- P1.2 `VolumeProvider` 是 concrete final 类型，提供 mount/query/unmount；
  `unmount_with_retry()` 只对 busy 做两秒普通卸载重试，明确不自动强制。
- 当前 provider 在每次成功挂载后立即通知 Shell。若 B 已成功而 A 随后失败，
  B 会短暂成为系统可见盘，因此补偿卸载是安全边界而非普通错误处理。
- 现有单容器单元测试无法注入“B 成功、A 失败、回滚又 busy”等顺序故障；
  P1.3 需要内部可替换 runtime，以 fake 验证调用顺序和所有失败分支，同时保持
  生产公开 API 使用真实 KDF、文件探测与 `VolumeProvider`。
- 本地已有普通卷 KDF fixture 和 Spike 3b 的外层+隐藏卷 fixture，可在被忽略的
  `out/vendor` 中制作 P1.3 专用副本；正式测试不得修改或提交 spike 原件。

### 初步实现方案

- 新增中性的 A/B 容器描述和 session 挂载结果，不引入 tier/mode/volume-shape
  类型或字段。
- 在任何 KDF 前验证两盘盘符不同、路径合法；文件探测仅将“文件/路径不存在”
  解释为 absent，拒绝目录、重解析点和其他访问错误，避免错误降级到另一派生分支。
- 生产路径按 8.6 的四行规则只执行一次存在性分支；派生结果在对应 mount 返回后
  立即 `clear()`，不进入日志、异常或普通堆容器。
- 单元测试将覆盖 A/B 六格存在性、B→A 次序、错误 B 不触碰 A、A/KDF/mount
  失败后的回滚，以及回滚失败的异常传播。
- 真实驱动测试计划覆盖：A+B 下 B 隐藏密码与外层密码均挂出两个盘、两条路径
  数量相同、错误密码零挂载、A-only/B-only 分支和结束后零残留。

### 待确认的设计取舍

- P1.2 已确认正常流程不得自动强制卸载；但 P1.3 新增了“B 已挂、A 失败”的事务
  回滚。若两秒普通卸载仍 busy，有两个互斥选择：
  1. 不强制：保护可能尚未落盘的数据，但违反 P1.3 的零半挂载安全目标；
  2. 仅在未成功发布的事务回滚中，普通重试失败后强制卸载一次：优先消除认证
     失败后的暴露，但极端情况下可能丢失系统刚产生的写入。
- 当前建议选择第 2 项。它不改变正常卸载策略；若驱动连强制卸载也拒绝，则返回
  含残留盘符的致命清理错误，不能声称软件能对驱动/系统故障提供绝对保证。
- 2026-08-22 已同意第 2 项，并同意同时加入延迟 Shell 通知：B 与 A 先以
  静默方式挂载，只有整个 session 成功后才连续发布盘符到 Shell；该缓解能减少
  资源管理器或后台程序在 A 完成前占用 B 的概率，但不能替代失败回滚。

### 验证、安全审计与提交决定

- 新增公开的中性 A/B 容器描述与 `mount_session()`，只按文件存在性执行四行
  分支；没有引入档位、模式或卷形态字段，也没有把挂载类型写入产品状态。
- 全部输入在首次 KDF/驱动调用前验证：A/B 盘符必须不同，路径必须绝对且不同；
  安全审查中发现初版比较可被 `.` 路径分量绕过，现已改为纯词法规范化后进行
  Windows 大小写不敏感比较，并加入回归测试。
- 生产文件探测只把 `FILE_NOT_FOUND`/`PATH_NOT_FOUND` 视为不存在；目录、最终
  路径重解析点和其他访问错误全部失败关闭，避免错误地降级到另一派生分支。
- B 存在时严格执行“探测 B → 用户输入派生 → 静默挂 B → 探测 A → `diskA`
  派生 → 静默挂 A”；B 认证失败不会探测、派生或挂载 A。B 不存在时只执行
  A 的用户输入派生；两者都不存在时不触发 KDF 或驱动。
- `VolumeProvider` 新增显式 Shell 通知开关与单独发布接口。事务内挂载不立即通知
  Shell，全部成功后才发布 A、B；不改变既有单容器调用的默认通知行为。
- A 的探测、派生或挂载在 B 成功后失败时，先执行既有两秒普通 busy 重试；失败
  后仅在该未发布事务回滚中强制卸载一次。若强制清理仍失败，抛出只包含残留盘符
  与异常状态的 `MountRollbackError`，不保留路径、用户输入或派生密码。
- 派生出的 64 字节卷密码在每次驱动 mount 返回后立即清零，异常路径同样清零；
  用户输入和卡材料仍由调用方的 `SecureBuffer` 管理，没有复制到普通堆容器。
- fake runtime 单元测试覆盖 B→A 顺序、延迟通知、错误 B 不触碰 A、A-only、
  B-only、未建库、A 各阶段失败回滚、普通回滚失败后只强制一次、强制失败残留
  盘符，以及重复盘符/路径在 runtime 调用前拒绝。
- 在 `out/vendor` 制作了两个不入库的一次性真实 fixture 副本；原 `spike` 文件未
  修改。夹具派生值先检查 VeraCrypt `char*` 接口的 NUL 兼容性，改密只对副本执行。
  受限环境最初拒绝 SDK 原生写入，获准仅写这两个副本后 A、B 外层、B 隐藏层
  三次改密均返回成功。
- Release 真实驱动测试验证：真实输入挂出 A 普通卷+B 隐藏卷（`type=1`，
  4089 ms），胁迫输入挂出 A 普通卷+B 外层卷（`type=0`，3225 ms），盘数均为
  2 且低于 P-002 五秒；错误输入在 B 处认证失败、A 未挂载。每轮卸载后盘符和
  驱动列表均零残留。
- 最终验证结果：`dev` 5/5、`asan` 5/5、`analyze` 5/5、Debug `integration`
  7/7、Release `integration-release` 7/7 全部通过；`/W4 /WX`、AddressSanitizer
  与 MSVC 代码分析均无发现，`git diff --check` 无空白错误。
- 安全审查未发现未解决的越界访问、敏感值持久化、二次派生探测、错误分支越权
  挂载或正常卸载隐式强制问题。剩余边界是驱动 ABI 只接收路径而非已打开句柄，
  因而文件探测与驱动打开之间无法彻底消除本地文件系统竞态；攻击者需已具备修改
  容器目录的权限，认证门和事务回滚把其影响限制为拒绝服务。硬链接同一文件也不能
  仅靠词法比较识别；P1.4 必须继续保证 A/B 由产品创建为两个独立文件。
- 2026-08-23 审阅阶段报告后已批准提交 P1.3。

---

## 2026-08-23 — P1.4 独立建卷工具

**状态：** 已完成安全审查并获准按拆分边界提交

### 计划与需求审计

- `MILESTONES.md` 将 P1.4 固定为独立 exe、IPC 协议、错误码映射与 UAC 提权；
  出口条件直接复用 Spike 3b：无界面创建普通容器 A 和带隐藏卷容器 B，并由真实
  驱动验证挂载 `type=1/0/2`。
- N-01 与 Spike 3b 已证明 VeraCrypt 1.26.29 FormatDLL 只需透传既有
  `hiddenVol`/`hiddenVolHostSize` 字段即可创建隐藏卷；正式实现不得扩展 spike harness，
  只复用结论并在产品目录重写编排与边界。
- N-08 要求所有建卷操作无条件进入 UAC，不能只在带隐藏卷的路径提权，否则 UAC
  行为本身会泄露创建选择。helper 另嵌入 `requireAdministrator` manifest 并在运行时
  复核 token elevation。
- 8.6 要求创建选择用完即焚。正式 API/IPC 只有单次 `create_standard` 与
  `create_with_hidden` 操作，没有档位、模式或持久形态字段，不写配置、注册表、日志、
  最近使用记录或临时状态。
- L-01 要求 VeraCrypt 派生代码与主产品保持进程/许可证边界；主程序仅与独立 helper
  通信，只有 helper 动态加载同目录的 patched FormatDLL。补丁与构建说明存入
  `third_party/veracrypt_format/`，最终分发仍需既定法律审查。

### 已检查的现有实现与 Spike 证据

- 检查了 P1.2 的 `VolumeProvider`、驱动 ABI、静默挂载/卸载及 busy 重试，可直接用于
  helper 内部临时挂载外层卷，不需要复制 spike 的 `vcspike` 进程编排。
- 检查了 Spike 3b 的两阶段流程、`FSCTL_GET_VOLUME_BITMAP` 分块扫描、0.5%/10MB
  自留余量、碎片化反例及 `type=1/0/2` 验收脚本；正式代码只使用真实尾部连续空闲簇，
  任何位图失败均关闭，不退化为空闲簇总数。
- 审查 FormatDLL 后发现 Spike 的 `const char*` 接口内部使用 `strlen`/`strcpy_s`。
  产品 KDF 输出是任意 64 字节，可能含 NUL；若照搬会静默截断并建出无法按既定密码
  挂载的卷。因此正式补丁新增显式长度的 `VeraCryptFormatBinary`，保留原字符串 API
  兼容路径，并由含 NUL 的集成夹具专门验收。

### 实现决策

- 新增 `VolumeCreator` 客户端 API。每次调用都用 `ShellExecuteExW(..., "runas")`
  启动独立 `usb_vault_volume_creator.exe`，不允许条件提权；UAC 取消映射为稳定的
  `CreationError::elevation_cancelled`。
- IPC 是版本化的固定宽度二进制协议：严格校验 magic/version/header size、操作、长度、
  保留字段和进度 stage；路径上限 32767 个 UTF-16 字符，密码上限 128 字节。
  密码不进入命令行、环境变量或普通堆序列化缓冲，而是从调用方锁页 `SecureBuffer`
  直接写入本地命名管道，在 helper 中直接读入新的 `SecureBuffer`。
- 每次调用使用 BCrypt 生成的 128 位随机管道名/nonce；helper 以 first-instance、
  reject-remote 和仅 SYSTEM/对象所有者可访问的 DACL 创建单实例管道，并要求 nonce
  与命令行随机值一致，未知协议版本、操作、stage 或错误码全部失败关闭。
- 为避免 elevated helper 成为可覆盖管理员文件的 confused deputy，所有目标路径创建、
  VeraCrypt 格式化和按路径临时挂载都通过 `ImpersonateNamedPipeClient` 降回原调用者
  token；只有无需目标路径的卷设备簇位图扫描和清理卸载使用提升后的 token。
  目标必须以调用者权限 `CREATE_NEW`，既有文件绝不覆盖。
- 普通卷和带隐藏卷创建均为失败事务：失败时删除本次新建容器；隐藏卷流程若临时挂载
  仍在，先普通 retry、再仅对未发布事务强制卸载。卸载或删除失败映射为
  `cleanup_failed`，不得把残留误报为普通格式错误。
- helper 固定 AES、SHA-512、FAT、PIM=0、快速格式化；隐藏卷前静默挂载外层卷，
  用管理员句柄读取真实 `FSCTL_GET_VOLUME_BITMAP`，按 Spike 3b 公式得到最大安全尺寸，
  用户请求值须簇对齐且不超过该上限。
- CMake 可通过 `USB_VAULT_FORMAT_DLL` 把按固定补丁重建的 DLL 复制到 helper 同目录；
  普通构建不依赖或链接第三方源码，缺失或未含 `VeraCryptFormatBinary` 导出时明确失败。

### 测试与验证

- `creator_protocol_tests` 覆盖含 NUL/0xff 的二进制密码逐字节往返、标准/隐藏请求字段
  约束、nonce 不匹配、未知协议版本、未知进度 stage、响应往返和 FormatDLL 错误映射。
- `volume_creator_integration_tests` 已编译并接入串行真实测试：创建 20MB 普通容器 A
  （64 字节密码首字节为 NUL）和 100MB 外层+隐藏容器 B，检查位图扫描进度，再分别
  验证普通 A `type=0`、B 隐藏层 `type=1`、B 外层未保护 `type=0`、保护挂载
  `type=2`；析构清理测试盘符与两个容器。
- patched VeraCrypt 1.26.29 FormatDLL x64 Release 已成功重建；正式补丁对本地固定源码
  执行 `git apply --check --reverse` 通过，证明补丁文件与实测二进制对应。
- 最终自动化结果：`dev` 6/6、`asan` 6/6、`analyze` 6/6；Debug `integration`
  （排除需前台 UAC 的新建卷验收）8/8，Release `integration-release` 同范围 8/8。
  AddressSanitizer 无异常，MSVC 静态分析修正一次 `wWinMain` SAL 签名后无发现，
  `/W4 /WX` 与 `git diff --check` 通过。
- 提取 Release helper 的嵌入 manifest，确认 `requestedExecutionLevel` 为
  `requireAdministrator`。两次后台真实验收尝试均在创建前由 Windows 安全桌面取消，
  正确返回 `elevation_cancelled`；未生成 `p1_4_*.hc`，驱动挂载位图为零。

### 安全审查与遗留风险

- 未发现密码进入命令行、环境变量、日志、异常文本或普通堆聚合缓冲；IPC request 中的
  密码在 helper 异常路径也由 `SecureBuffer` 析构清零。测试固定明文密码只存在于
  不入产品的真实验收代码，用于调用既有 Spike `mount-outer` 验证 `type=2`。
- helper DLL 使用绝对的同目录路径和 `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | SYSTEM32`，
  不从当前目录或 PATH 搜索；客户端拒绝 reparse helper 文件，目标拒绝 UNC、ADS、
  相对路径、内嵌 NUL 和既有文件。
- 位图返回长度、空响应、簇大小和乘法溢出均失败关闭；不会使用会在碎片场景高估
  40.9% 的空闲簇降级算法。临时挂载不通知 Shell，成功和失败均要求零残留。
- 2026-08-23 在前台接受 UAC 后，Release `volume_creator_integration_tests`
  退出码为 0：真实创建并验证了普通容器 A、容器 B 隐藏卷、未保护外层卷和受保护
  外层卷，对应 `type=0/1/0/2`，满足阶段要求的 `type=1/0/2` 全路径出口。
- 验收完成后复核无 `p1_4_*.hc` 临时容器，驱动挂载位图为 0；真实建卷路径没有遗留
  文件、盘符或驱动挂载。
- 许可证边界是工程设计而非法律意见；patched FormatDLL/helper 的最终分发义务仍须律师
  确认。P2 若要在隐藏卷创建前引导用户填充诱饵，需要在本协议上增加“对既有外层卷
  添加隐藏卷”的独立瞬时操作，不能把创建选择持久化；该向导能力不属于 P1.4 出口。

### 提交决定

- 2026-08-23 审阅阶段报告后已批准提交 P1.4；实现按协议与客户端、提权 helper 与
  FormatDLL 边界、验证分开提交，阶段记录另行提交。
- 提交：`a1bde93 [P1.4]add volume creation client protocol`、
  `0b964d0 [P1.4]add elevated volume creation helper`、
  `d951ba8 [P1.4]add volume creation verification`

---

## 2026-08-23 — P1.5 最小 UI 与应用闭环

**状态：** ✅ 已完成；小切片 1~15 均已按独立可审查边界提交

### 计划与需求审计

- `MILESTONES.md` 将 P1.5 的出口限制为：在桩卡下完成“单一密码输入 → 挂载现有
  容器 → 主动卸载”的最小闭环。P2 的注册、忘记密码、口令强度、风险确认、建卷向导、
  三模式策略、备份与全量 UI 均不进入本阶段。
- 技术主文档要求登录界面恒为一个密码框，无用户名、自动填充或模式选择器；认证以
  容器 B 成功挂载为锚点，不另设登录密码校验。界面和本地产物不得记录或暗示档位、
  卷类型、隐藏卷容量、口令、派生结果或隐藏卷事件。
- `usb-vault_ui-disign.html` 只能作为视觉参考。其用户名、注册/忘记密码页、“密码存储
  于 USB 密钥”、按隐藏卷分类展示和包含隐藏卷事件的历史日志与最新需求冲突，P1.5
  不得直接复用这些交互和文案。
- 现有 `mount_session` 已提供固定 B-before-A、失败回滚、成功后统一 Shell 通知及
  `SecureBuffer` 密码/卡材料边界，可以作为最小应用会话的下层，不需要改写 P1.3。
- 仓库此前没有 `ICardStore`；P1.5 又必须在返回固定 blob 的桩实现下完成，因此先固化
  单 blob 接口，真卡读写与硬件差异留给 P1.6。
- 仓库和本机 Build Tools 中尚未发现 WebView2 SDK 头文件。该依赖只影响后续 UI 外壳
  切片，不阻塞本次 UI 无关的卡存储合约。
- R-03 对 WebView2 口令桥接形成后续审计重点：浏览器输入和 COM/消息桥可能产生不可锁页
  的临时副本。后续切片必须避免 JSON/普通 C++ 字符串承载口令，复制进
  `SecureBuffer` 后立即清空 DOM 与桥接缓冲，并明确记录浏览器进程内存无法完全清零的
  剩余边界，不能把 WebView2 便利性当成豁免。

### 小切片 1：`ICardStore` 单 blob 合约

- 新增 `ICardStore`，只暴露 `get_blob`、`put_blob` 与 `capabilities`。blob 通过
  `SecureBuffer` 返回或借用，不允许接口层引入 `std::string`、`std::vector` 等会复制、
  重分配秘密的所有权容器。
- `CardCapabilities` 只表达可选硬件能力；当前首个位是 `multiple_pins`。能力差异不会
  改变单 blob get/put 合约，也不携带 PIN、档位、模式或容器形态。
- 本切片不加入生产固定密钥。契约测试中的桩按调用即时生成 32 字节固定 blob，含首字节
  NUL，用后由 `SecureBuffer` 清零；真卡实现、写入策略和硬件错误映射仍属于 P1.6。

### 测试、安全审查与风险

- 独立 MSVC Debug 构建在 `/W4 /WX` 下通过；测试 7/7 通过，其中新增契约测试覆盖
  二进制 NUL 保留、get/put 所有权语义及 capability 不改变单 blob 形态。
- 审查未发现普通堆容器持有 blob、日志/异常输出秘密、档位/模式字段或本地持久化行为。
  接口故意不规定 blob 长度；P1.5 应用会话在交给固定参数 KDF 前仍须强制验证 32 字节，
  P1.6 真卡实现也应在设备边界校验自身容量与返回长度。
- 提交：`e0fd8e8 [P1.5]define single-blob card store contract`

### 小切片 2：UI 无关的应用会话控制器

- 新增 `ApplicationSession`，单次挂载调用按值接收不可复制的 `SecureBuffer` 口令，从
  `ICardStore` 读取一次 blob，先强制校验为 KDF 所需的 32 字节，再调用既有
  `mount_session`。成功、普通失败、非法卡 blob 和残留挂载异常路径都会立即显式清空
  口令与卡 blob；对象成员不保存两者或派生结果。
- 会话只保留容器 A/B 的固定盘符是否仍由本次会话挂载这两个易失布尔量，用于精确主动
  卸载；公开状态只有统一的 `mounted()`，不暴露数量、容器形态、卷类型或模式，也不写入
  配置、注册表、日志或临时文件。
- 主动卸载固定先 B 后 A，优先缩短潜在隐藏卷的运行期暴露。卸载只走既有普通 busy
  retry，绝不擅自 force；B 失败时两者状态均保留，B 成功但 A 失败时只保留 A，调用方
  可安全重试而不会重复卸载已经成功移除的盘。
- 若 P1.3 的事务回滚连普通和强制清理都失败，控制器从 `MountRollbackError` 接回残留
  盘符并将其保留为活动状态，使 UI 仍可提供主动卸载，不会把真实残留误报为“未登录”。

### 小切片 2 测试、安全审查与风险

- 全新 MSVC Debug `/W4 /WX` 构建通过，CTest 8/8；新增状态机测试覆盖成功挂载、非法
  卡 blob、普通认证失败、P1.3 回滚残留、重复挂载拒绝、B→A 卸载顺序，以及 B/A
  分别失败后的精确重试状态。
- MSVC `/analyze` 构建无发现。Visual Studio 多配置生成器会让 `link.exe` 忽略直接传入
  的 `/fsanitize=address`，因此该次运行未计为 ASan 证据；随后在仓库正式 Ninja
  `asan` 预设下重新插桩构建，`application_session_tests` 1/1 通过且无 ASan 发现。
- 安全审查未发现敏感值进入成员、普通堆所有权容器、日志或异常文本，也未引入档位、
  模式、卷类型探测或持久化。测试事件使用的 `std::string` 只记录固定动作名，不接触秘密。
- `ApplicationSession` 析构函数不会静默强制卸载，因为析构路径无法可靠报告 busy/失败，
  强制卸载又可能损坏未写完数据。P1.5 后续宿主必须在窗口关闭路径显式调用
  `unmount_active()` 并展示失败；进程崩溃或被强杀后的自动卸载仍属于 P1.7 插拔/异常闭环。
- 尚未实现 WebView2 宿主、口令桥或真实 UI 闭环。
- 提交：`4e4a1ef [P1.5]add retryable application session lifecycle`

### 小切片 3：可擦除的 UI 口令 UTF-8 入口

- 新增 `consume_password_utf8`，只接收调用方拥有的可变字节跨度，不接收
  `std::string`、UTF-16 `BSTR` 或 JSON 文本。入口把原始 UTF-8 字节复制到锁页
  `SecureBuffer` 后立即用 `SecureZeroMemory` 擦除来源，再以
  `MultiByteToWideChar(..., MB_ERR_INVALID_CHARS, ...)` 严格校验编码。
- RAII 擦除守卫覆盖空输入、长度拒绝、`SecureBuffer` 分配失败及其他异常路径；非 ASCII、
  内嵌 NUL 均按原始字节保留，不依赖 C 字符串终止语义。错误信息只描述输入类别，
  不包含长度、内容或派生结果。
- 该边界为后续 WebView2 桥约定前端使用 `TextEncoder` 产生可变 UTF-8 字节数组，并把
  COM 传入数组作为一次性缓冲消费；本切片尚未引入 WebView2 SDK 或宿主，也不会声称
  能可靠清除浏览器 DOM、JavaScript 引擎或跨进程封送产生的全部副本。

### 小切片 3 测试、安全审查与风险

- MSVC Debug `/W4 /WX` 完整构建通过，CTest 9/9；新增测试覆盖 ASCII 成功复制、中文与
  内嵌 NUL 保留、过长编码/截断编码/代理项编码拒绝、空输入拒绝，以及各可观察来源
  缓冲的清零。
- MSVC `/analyze` 的 `password_ingress_tests` 目标构建无发现；正式 Ninja `asan`
  预设重新插桩构建，新增测试 1/1 通过且无 AddressSanitizer 发现。
- 人工复核未发现口令进入普通所有权字符串、日志、异常文本、配置、注册表或临时文件；
  成功与异常路径均由入口来源守卫及 `SecureBuffer` 析构形成清零闭环。
- 剩余风险位于尚未实现的 WebView2 边界：密码在 DOM、JavaScript 字符串、`TextEncoder`
  内部及 COM 跨进程封送中可能有不可锁页副本。下一切片必须最小化存活时间、避免
  JSON/字符串桥并及时清空可控缓冲，但浏览器进程内存无法作绝对清零保证。
- 提交：`8e68661 [P1.5]add wipeable UI password ingress`

### 小切片 4：UI 会话适配器

- 抽出只包含 `authenticate_and_mount`、`unmount_active` 与统一 `mounted()` 的
  `IApplicationSessionActions`，既有 `ApplicationSession` 实现该接口；接口不暴露盘符、
  容器数量、卷类型、档位或任何敏感材料。
- 新增 `UiSessionController`：提交时直接把调用方的一次性可变 UTF-8 缓冲交给
  `consume_password_utf8`，并将返回的 `SecureBuffer` 按值转交应用会话；控制器自身不
  保存口令、卡 blob、派生结果或错误历史。主动卸载和统一挂载状态只透传到会话层。
- 本切片刻意不引入 WebView2 SDK、HTML 或窗口生命周期，使“安全桥接到应用会话”的
  责任可以独立验证和回退；下一切片才处理浏览器宿主与实际页面。

### 小切片 4 测试、安全审查与风险

- MSVC Debug `/W4 /WX` 完整构建与 CTest 10/10 通过；新增测试覆盖二进制安全的口令
  转交及来源擦除、挂载失败仍擦除、统一 mounted 状态，以及卸载失败不得隐藏活动状态。
- 首次链接新增测试时遇到一次 `LNK1201` 写 PDB 失败；未修改代码原样重试即成功，确认
  为瞬时工具链/文件占用问题，不计为产品缺陷。
- MSVC `/analyze` 目标构建无发现；正式 Ninja `asan` 预设下新增测试 1/1 通过且无
  AddressSanitizer 发现；`git diff --check` 通过。
- 安全复核未发现适配器持久化或分类状态、复制秘密到普通所有权容器、记录秘密或吞掉
  卸载失败。WebView2/COM 侧不可控副本风险仍未改变，必须由后续宿主切片继续约束。
- 提交：`9c09fd5 [P1.5]add UI session action boundary`

### 小切片 5：WebView2 本地页面宿主骨架

- 依据微软原生 Win32 集成方式，引入固定版本 `Microsoft.Web.WebView2 1.0.4129.50`；
  CMake `FetchContent` 固定官方 NuGet URL 与 SHA-256
  `D3934F482D484B89FB4825DF720C710664E1143A1E90F7B3A60794EF33F473D2`，静态链接
  x64 Loader。SDK 包只缓存于已忽略的 `out/`，不提交第三方二进制包。
- 新增最小 `usb_vault_app` Win32 宿主，异步创建 WebView2 environment/controller，随窗口
  调整边界，并从 exe 同目录只加载构建时复制的 `index.html`。导航开始事件严格比较完整
  本地 URI，任何其他导航均取消。
- 初始页面只有中性产品标题和“正在初始化”状态，不含用户名、注册/忘记密码、模式选择、
  卷分类、日志或“密码存卡”等冲突文案；CSP 使用 `default-src 'none'`，不允许脚本、
  网络资源、表单提交或 base URI。宿主关闭开发者工具、默认右键菜单和状态栏。
- 本切片不添加密码框、JavaScript、COM host object、卡桩或挂载动作；其唯一目标是独立
  验证可复现的 SDK 接入、本地页面边界与 Runtime 生命周期。

### 小切片 5 测试、安全审查与风险

- 本机已有 WebView2 Runtime `151.0.4129.101`。Debug `/W4 /WX` 宿主构建成功，隐藏启动
  后主进程保持运行并产生 6 个新的 `msedgewebview2.exe` 进程，测试结束后已清理；证明
  environment/controller 能在真实 Runtime 上建立。
- 首次运行因错误地以空输出探测 `UrlCreateFromPathW` 缓冲长度而停在初始化错误路径；改为
  有界输出缓冲后真实运行通过。MSVC `/analyze` 又发现 192KB 栈缓冲 C6262 及 `wWinMain`
  SAL C28251，分别改为普通堆路径缓冲并补齐入口注解后分析无发现。
- Debug 既有 CTest 10/10 通过；正式 Ninja `asan` 预设下宿主完成插桩编译和链接。GUI
  ASan 二进制未自动启动，因此该结果只证明宿主可插桩构建，不作为运行时内存检查证据。
- 安全复核未发现外部导航、脚本执行、敏感状态、日志或持久化业务字段。WebView2 Runtime
  必然维护用户数据目录；当前页面不写业务状态，但该目录仍须纳入 P3 档二/档三逐字节
  不可区分性验收，后续桥接不得使用 localStorage、自动填充或浏览器日志保存秘密。
- 提交：`47cb861 [P1.5]add local-only WebView2 host`

### 小切片 6：单密码框页面契约

- 将初始化占位页扩展为唯一一个 `type=password` 的会话输入控件、一个中性动作按钮与统一
  `aria-live` 状态区；没有用户名、注册/忘记密码、模式选择、卷分类、日志入口或任何可
  推断档位的页面字段及文案。
- 密码控件明确关闭 `autocomplete`、自动大小写与拼写检查，不设置 `name`，不放入
  `<form>`；页面仍无脚本且 CSP 保持 `default-src 'none'`/`form-action 'none'`。
- 在原生桥尚未接入的本切片中，密码控件与动作按钮都固定 `disabled`。这避免用户把真实
  口令输入一个尚不能立即消费并清零的 DOM；下一切片只能在安全桥就绪后主动启用。
- 新增静态页面契约测试，锁定单输入框、密码类型、自动填充/拼写检查关闭、桥前禁用、
  无脚本以及禁止用户名/模式/卷类型/localStorage/sessionStorage 标记等约束。

### 小切片 6 测试、安全审查与风险

- MSVC Debug `/W4 /WX` 完整构建与 CTest 11/11 通过；MSVC `/analyze` 的页面契约测试
  目标构建无发现；正式 Ninja `asan` 预设下该测试 1/1 通过且无 AddressSanitizer 发现。
- 人工复核未发现提交、网络、脚本、浏览器存储、自动填充或页面分类状态；本切片没有
  DOM 到原生的口令复制路径，禁用控件也不会产生用户输入的秘密副本。
- 视觉样式只提供中性登录卡片，不复用设计稿中与技术主文档冲突的用户名、密码存卡、
  隐藏卷分类和历史日志内容。完整视觉与无障碍验收仍属于后续 UI 工作。
- 提交：`482bd5d [P1.5]define inert single-password UI`

### 小切片 7：WebView2 字节数组适配器

- 新增 `submit_webview_password_array`，边界只接受一维 `VT_UI1` SAFEARRAY；拒绝
  字符串、JSON、其他元素类型及多维数组，避免把密码转换成普通所有权字符串。
- 通过 `SafeArrayAccessData` 直接把调用方数组暴露为可变字节跨度，并交给既有
  `UiSessionController` 消费；不创建额外普通堆密码副本。既有口令入口会在调用应用
  会话前擦除该跨度，RAII 保证成功和异常路径都解除 SAFEARRAY 锁定。
- 边界计算支持非零下界，并使用有符号宽运算避免上下界相减时溢出。本切片仅实现可独立
  测试的 COM 数据适配器，尚未注册 `IDispatch` host object，也未启用页面控件。

### 小切片 7 测试、安全审查与风险

- MSVC Debug `/W4 /WX` 完整构建与 CTest 12/12 通过；专项测试覆盖非零下界、非 ASCII
  与内嵌 NUL 的逐字节转交、成功及会话异常后的来源清零，以及错误 VARIANT 类型拒绝。
- MSVC `/analyze` 首轮在测试夹具中报告 `C6387`：自定义断言无法向分析器证明
  `SafeArrayCreateVector` 非空；改为显式空指针分支后，正式代码及专项测试目标分析无发现。
- 正式 Ninja `asan` 预设在最终边界计算版本上重建，专项测试 1/1 通过且无
  AddressSanitizer 发现；有效数组在会话成功或失败时均被擦除。
- 人工复核未发现字符串/JSON 密码副本、日志、持久化或分类状态。错误类型在无法证明其
  所有权和字节布局时只拒绝、不擅自改写；浏览器 DOM、JavaScript 引擎及 COM 跨进程
  封送仍可能保留不可控副本，后续桥接只能缩短可控副本寿命，不能声称绝对清零。
- 提交：`29a00cf [P1.5]add WebView password array adapter`

### 小切片 8：窄化原生 WebView host object

- 新增无类型库的最小 `IDispatch` 实现，只解析一个区分大小写的
  `submitPasswordBytes` 方法名；未知名称、未知成员、属性读取、错误参数数量和非字节数组
  参数均拒绝。对象不提供 mounted、盘符、容器、卷类型、日志、档位或其他状态查询。
- 方法调用直接复用已提交的 SAFEARRAY 适配器和 `UiSessionController`；host object 只持有
  控制器引用，不保存密码、返回值或错误历史。应用层异常统一映射为不含秘密的 `E_FAIL`，
  参数类型错误映射为 `DISP_E_TYPEMISMATCH`。
- 工厂显式转移一个 COM 引用，引用计数使用原子量；本切片尚未调用 WebView2
  `AddHostObjectToScript`，也没有添加页面脚本或启用密码框。

### 小切片 8 测试、安全审查与风险

- MSVC Debug `/W4 /WX` 完整构建与 CTest 12/12 通过；新增测试覆盖唯一方法名、无类型信息、
  未知状态名拒绝、属性访问拒绝、有效数组成功转交、应用失败映射，以及两条路径的来源清零。
- MSVC `/analyze` 的正式代码和专项测试目标构建无发现；正式 Ninja `asan` 预设下专项测试
  1/1 通过且无 AddressSanitizer 发现；`git diff --check` 待提交前复核。
- 人工复核未发现 host object 暴露分类状态、记录秘密、把口令变成字符串，或在异常文本中
  回显输入。其生命周期依赖后续宿主确保控制器活得更久；真正注册到页面后仍须限制仅本地
  源可访问，并验证 typed array 以 SAFEARRAY 而非字符串/JSON 方式封送。
- 提交：`4d5be60 [P1.5]add narrow WebView host object`

### 小切片 9：受限本地 WebView2 的 host object 注册

- WebView2 controller 建立后、首次本地导航前创建窄化 `IDispatch`，以中性名称
  `nativeBridge` 调用 `AddHostObjectToScript` 注册；任何创建或注册失败都会中止导航，
  不会降级为没有安全桥的可输入页面。
- COM `VARIANT` 显式持有并释放临时引用，宿主持有另一引用；窗口销毁时先释放 WebView，
  再释放 controller 和宿主持有的 host object，避免对象引用已销毁的 UI 控制器。
- 本切片使用只会清空口令并拒绝调用的临时会话后端。页面仍无脚本且输入控件继续禁用，
  因而注册成功不等于可登录；桩卡与应用会话组合、页面 typed array 调用分别留待后续切片。

### 小切片 9 测试、安全审查与风险

- MSVC Debug `/W4 /WX` 完整构建与 CTest 13/13 通过；`usb_vault_app` 的 MSVC
  `/analyze` 构建无发现；正式 Ninja `asan` 预设下宿主完成插桩编译和链接。
- 在已安装的 WebView2 Runtime `151.0.4129.101` 上隐藏启动真实 Debug 宿主 3 秒：主进程
  保持运行并产生 6 个新的 `msedgewebview2.exe` 进程；测试结束后只清理本次进程。
  这证明本地导航前的 host object 注册与真实 Runtime 生命周期可建立。
- 人工复核未发现新增外部导航、脚本、持久化、日志或分类状态。临时后端即使被意外调用也
  只清空并拒绝口令；但 host object 一旦注册就会对该 WebView 的文档可见，因此现有严格
  导航白名单仍是安全边界，后续不得放宽为任意本地或网络页面。
- GUI ASan 二进制未自动运行，本切片的 ASan 结果只证明可插桩构建；真实 Debug Runtime
  冒烟不等同于 ASan 运行时检查。
- 提交：`23c2a7c [P1.5]register local WebView host object`

### 小切片 10：固定 blob 桩卡与正式应用会话组合

- 新增 P1.5 专用 `FixedBlobCardStore`：每次读取直接在锁页 `SecureBuffer` 中生成固定的
  32 字节测试 blob，不把 blob 保存为成员或普通堆对象；桩明确只读，写入会拒绝，也不
  冒充任何真实硬件能力。
- 宿主移除“始终失败”的临时会话，组合正式 `VolumeProvider`、`ApplicationSession`、
  `UiSessionController` 和已注册的 host object。容器路径固定解析为 exe 同目录的
  `container_a.hc` / `container_b.hc`，盐为与 P1.3 验证夹具一致的公开固定参数。
- 每次启动只从 Z 向下选择两个当时空闲盘符，不写入配置、注册表或其他本地产物；应用
  会话、UI 控制器和 provider 按引用依赖的逆序释放。页面仍禁用，启动不会访问驱动或挂载。

### 小切片 10 测试、安全审查与风险

- MSVC Debug `/W4 /WX` 完整构建与 CTest 14/14 通过；新增测试覆盖固定 blob 的独立锁页
  分配、确定性内容、无虚假能力和只读拒绝。MSVC `/analyze` 的桩、测试和宿主无发现。
- 针对轻量模型质量方面的担忧，在全新的 `out/build/review-clean` 目录从零配置并编译
  全部 53 个构建步骤，随后 CTest 14/14 再次通过；证明结果不依赖原有增量对象缓存。
- 正式 Ninja `asan` 预设下宿主完成插桩构建，桩专项测试 1/1 通过且无
  AddressSanitizer 发现。真实 Debug 宿主隐藏启动 3 秒后保持运行并产生 6 个新的
  WebView2 子进程，测试结束后均清理；未复制容器文件，也未产生挂载。
- 首次正式链接暴露旧 `application_session.cpp.obj` 未随先前虚接口头文件变化重编译，
  缺少虚析构链接符号；符号检查后强制重编该源文件即恢复。用于触发重编的临时代码变更
  已撤回，最终代码没有 ABI 补丁；随后再次从最终源码重编和链接通过。
- 安全复核未发现 blob 常驻普通内存、秘密日志、持久化状态或档位实体。固定 blob 仅为
  P1.5 桩，不能进入 P1.6 真卡实现；固定容器文件名和盐不是秘密，但发行配置与建卷流程
  后续仍需形成明确契约。页面启用前还必须补齐关闭主动卸载，并依据微软 WebView2 契约
  设置 `shouldPassTypedArraysAsArrays=true` 后做真实封送验证；两项均未被当前禁用页面触发，
  记录为后续边界而非本切片阻塞。
- 提交：`16f8037 [P1.5]compose fixed-card application session`

### 小切片 11：窗口关闭主动卸载守卫

- 新增 `prepare_ui_session_for_close`：无挂载时直接允许关闭；存在活动挂载时先调用既有
  主动卸载，只有统一 mounted 状态确实清空后才允许销毁窗口。该函数为 `noexcept`，避免
  C++ 异常穿越 Win32 窗口过程。
- `WM_CLOSE` 接入关闭守卫；卸载失败时保留窗口和 `ApplicationSession` 中的可重试残留
  状态，只显示不含盘符、卷类型或异常细节的中性提示。再次关闭会精确重试仍活动的挂载。
- 本切片不添加页面脚本，也不改变密码输入和 host object；强制结束进程、断电等系统外部
  终止无法由进程内关闭守卫保证，P1.7 拔卡与异常退出仍需独立处理。

### 小切片 11 测试、安全审查与风险

- MSVC Debug `/W4 /WX` 完整构建与 CTest 15/15 通过；专项测试覆盖无挂载不调用卸载、
  活动挂载成功后允许关闭、失败阻止关闭并保留状态，以及随后重试成功。
- MSVC `/analyze` 的守卫、测试和宿主无发现；正式 Ninja `asan` 预设下专项测试 1/1
  通过且无 AddressSanitizer 发现；`git diff --check` 待提交前复核。
- 真实 Runtime 首次以隐藏窗口启动时 `MainWindowHandle=0`，无法由测试工具发送
  `WM_CLOSE`，因此不计产品失败；清理后改为普通窗口启动，获得有效句柄并成功发送正常
  关闭，主进程在 5 秒内自行退出。500ms 后尚有 2 个本次 WebView2 子进程处于退出尾声，
  测试随后按 PID 清理，未触碰既有 WebView2 进程。
- 安全复核未发现卸载异常泄露分类状态、窗口在失败时错误退出或活动状态被吞掉。系统强杀
  仍可绕过 `WM_CLOSE`，这是桌面进程无法完全消除的剩余风险。
- 提交：`a7d328f [P1.5]guard window close with unmount`

### 小切片 12：WebView2 typed array 真实 Runtime 验证

- 依据微软 WebView2 host objects 契约，测试页面在调用前显式设置
  `chrome.webview.hostObjects.options.shouldPassTypedArraysAsArrays = true`，再把固定的
  `Uint8Array` 直接传给既有 `submitPasswordBytes`；没有改动产品页面，密码框和按钮仍禁用。
- 新增隐藏 Win32 集成测试目标，在严格的单一本地页面 URI 导航白名单内注册既有
  `nativeBridge`。原生探针验证包含内嵌 NUL 和多字节 UTF-8 的 4 个固定非秘密字节逐字节
  到达；页面在 `finally` 中清零 typed array，只有原生收到原值且 JavaScript 数组确认全零
  才报告成功。
- 测试目标标记为 `EXCLUDE_FROM_ALL` 且不注册到 CTest，避免日常构建和单元测试依赖本机
  WebView2 Runtime；测试 HTML 只复制到该测试可执行文件目录，不进入产品资源。

### 小切片 12 测试、安全审查与风险

- 首次专项链接因测试目标仍是 console subsystem、源码入口为 `wWinMain` 而失败；把目标明确
  声明为 `WIN32` 后恢复，该问题只涉及测试目标类型，与 typed array 封送行为无关。
- MSVC Debug `/W4 /WX` 完整构建与 CTest 15/15 通过；专项目标在 Debug、`/analyze` 和
  AddressSanitizer 配置下均构建通过，`/analyze` 无发现。Debug 与 ASan 专项可执行文件均在
  本机 WebView2 Runtime `151.0.4129.101` 上退出 0，证明当前 Runtime 会把该 `Uint8Array`
  投影为既有适配器要求的一维 `VT_UI1` SAFEARRAY，并保持字节值不变；ASan 无发现。
- 测试页面 CSP 的 `unsafe-inline` 仅用于固定的测试内联脚本，页面没有输入、网络能力或产品
  打包路径；宿主同时拒绝除该测试文件外的导航。测试字节不是口令，不写日志、不持久化、
  不触发真实挂载。既有单元测试继续覆盖 COM SAFEARRAY 在成功和失败路径的清零。
- JavaScript 引擎、WebView2 和 COM 内部仍可能制造测试无法逐字节追踪或主动擦除的短期副本；
  本切片只验证可控 typed array 与原生消费链路，不把它表述为浏览器进程全内存清零保证。
  `git diff --check` 通过。
- 提交：`7276726 [P1.5]verify WebView typed array bridge`

### 小切片 13：WebView2 主动卸载方法

- P1.5 出口需要用户可主动卸载，而既有 host object 只有密码提交方法；本切片只新增无参数
  `unmount`，把调用转交既有 `UiSessionController::unmount()`，仍不启用产品页面。
- `GetIDsOfNames` 只识别大小写精确的 `submitPasswordBytes` 与 `unmount`；卸载只允许
  `DISPATCH_METHOD` 且参数数量必须为零。成功返回 `S_OK`，底层卸载失败统一返回 `E_FAIL`，
  不把异常文字、盘符、卷类型、挂载数量或任何分类状态传入 WebView。
- host object 仍没有 `mounted` 属性或其他状态查询面；失败时由既有应用会话保留活动挂载，
  之后可安全重试，不会把卸载失败误报成成功。

### 小切片 13 测试、安全审查与风险

- MSVC Debug `/W4 /WX` 完整构建与 CTest 15/15 通过；专项单元测试覆盖卸载成功、卸载失败、
  带参数调用拒绝、属性访问拒绝，以及 `mounted` 仍不可发现。
- MSVC `/analyze` 专项目标构建无发现；正式 Ninja `asan` 预设下专项测试 1/1 通过且无
  AddressSanitizer 发现。
- 真实 Runtime 验证页面扩展为先提交固定非秘密 `Uint8Array`、再调用零参数 `unmount`；
  Debug 与 ASan 可执行文件均在本机 WebView2 Runtime `151.0.4129.101` 上退出 0，原生探针
  确认会话先挂载后卸载，且既有 JavaScript 数组清零断言仍通过。
- 安全复核未发现新口令副本、日志、持久化状态、档位/模式实体或分类反馈。产品页面仍无
  脚本且控件保持禁用；下一页面切片必须在异步调用期间禁用操作，防止重复提交或卸载重入。
  `git diff --check` 通过。
- 提交：`7f8070f [P1.5]expose active-session unmount`

### 小切片 14：WebView2 统一活动会话查询

- 页面启用前复核发现：挂载失败回滚可能留下仍需处理的活动挂载，WebView 渲染进程也可能
  重载；若页面只有 `submitPasswordBytes` 与 `unmount`，重载后无法安全恢复“打开/卸载”
  按钮状态，回滚残留也可能暂时失去可见恢复入口。
- host object 新增零参数方法 `hasActiveSession`，只把既有
  `UiSessionController::mounted()` 投影为 `VT_BOOL`。它是当前进程内的易失统一状态，不返回
  挂载数量、盘符、容器路径、卷类型或任何档位/模式信息，也不写入本地产物。
- 查询只允许 `DISPATCH_METHOD`，带参数调用与属性访问均拒绝；原始 `mounted` 名称仍不可
  发现。调用方不接收会话对象、异常文字或其他原生接口。

### 小切片 14 测试、安全审查与风险

- MSVC Debug `/W4 /WX` 完整构建与 CTest 15/15 通过；专项单元测试覆盖活动/非活动布尔值、
  `VT_BOOL` 返回类型、带参数拒绝、属性访问拒绝，以及原始 `mounted` 名称保持隐藏。
- MSVC `/analyze` 专项目标构建无发现；正式 Ninja `asan` 预设下专项测试 1/1 通过且无
  AddressSanitizer 发现。
- 真实 Runtime 验证在固定非秘密 typed array 提交后调用查询并得到 JavaScript `true`，
  主动卸载后再次查询得到 `false`；Debug 与 ASan 可执行文件均在本机 WebView2 Runtime
  `151.0.4129.101` 上退出 0，既有数组清零断言继续通过。
- 该布尔值会向严格本地页面表明当前应用会话是否仍有活动挂载，这是实现状态反馈和恢复
  所需的最小可观察面；真实与胁迫路径、一个与两个容器均只得到同一个 `true`，不能据此
  分类。未发现口令副本、日志或持久化行为。产品页面仍无脚本且控件禁用。
  `git diff --check` 通过。
- 提交：`c37db3d [P1.5]expose unified session activity`

### 小切片 15（2026-08-24）：产品页面最小会话状态机

- 复核 `MILESTONES.md` 的 P1.5 出口、`需求梳理清单_v2.md` 的 R-03 与 8.6，以及已提交的
  `UiSessionController`、窄化 host object、本地页面、导航白名单和关闭守卫后，本切片只启用
  页面侧“输入密码—打开—统一状态反馈—主动卸载”状态机，不扩大到真卡、插拔监听或新模式。
- 新增独立打包的 `ui/session.js`；产品 CSP 只允许同目录 `self` 脚本且继续禁止表单提交和
  其他默认资源，不使用内联脚本。CMake 同时复制 `index.html` 与 `session.js`，并把两者纳入
  链接依赖，避免只修改资源后正式包仍保留旧文件。
- 页面初始化时显式启用 `shouldPassTypedArraysAsArrays`，取得异步 `nativeBridge` 并查询统一
  活动状态，以便渲染进程重载后恢复。打开、卸载和状态查询期间用单一 `busy` 标志串行化，
  防止重复点击、回车重入或打开/卸载交错。
- 密码只从 DOM 编码为 `Uint8Array`：编码后立即清空输入框。真实 Runtime 调查确认异步
  host-object 代理可能在返回 Promise 后才编组 typed array；若在 `await` 前清零，原生端会收到
  已清零内容。因此数组保留到 `await bridge.submitPasswordBytes(...)` 结算，并在其紧邻的
  `finally` 中立即清零，外层 `finally` 再次兜底清零。这是既保证参数完整性又不无故延长
  JavaScript 可控副本寿命的最早安全边界。
- 打开失败后重新查询统一活动状态；若回滚留下活动挂载，页面保留“安全卸载”入口。查询本身
  失败时按失败关闭原则视为可能仍活动，不重新开放密码输入。所有提示保持中性，不显示密码、
  盘符、容器数量、真实/胁迫路径、卷类型或底层异常。
- 正式宿主逐项检查所有 WebView2 安全设置的 HRESULT；关闭 DevTools、默认右键菜单、脚本
  对话框、WebMessage、状态栏和浏览器快捷键。通过 `ICoreWebView2Settings4` 关闭密码自动保存
  与通用自动填充；任一设置或接口查询失败都会在注册 host object 和首次导航前停止初始化。

### 小切片 15 测试、安全审查与风险

- 当前 Runtime `151.0.4129.101` 的逐层诊断确认：即使启用
  `shouldPassTypedArraysAsArrays`，同一个 `Uint8Array` 也可能投影为一维
  `SAFEARRAY(VARIANT)`，每项为 `VT_I4`，而不是先前观察到的 `SAFEARRAY(UI1)`。适配器现仅
  接受这两种窄格式；variant 路径逐项要求 `VT_I4` 且值域为 0~255，使用锁页
  `SecureBuffer` 归一化，并在转交既有密码入口前清零 COM 整数槽。混合类型、越界整数、直接
  `SAFEARRAY(I4)`、非数组和会话失败均有拒绝/清零测试，不接受字符串或任意自动类型转换。
- 集成页与产品页一致，只允许 `script-src 'self'`，测试驱动也拆为外部脚本；隐藏 WebView 的
  状态等待改用 `MutationObserver`，避免 25ms 轮询定时器被后台限流后超过原生 15 秒超时。
  测试退出码只区分初始化、提交、卸载、负载不匹配等非秘密阶段，不输出密码内容或异常文本。
- MSVC Debug `/W4 /WX` 完整构建与 CTest 15/15 通过；真实 Runtime 闭环连续三次退出 0：
  含内嵌 NUL 和多字节 UTF-8 的固定非秘密输入逐字节到达原生探针，DOM 输入已清空，页面进入
  统一活动状态，随后主动卸载并恢复 ready。加固后的正式宿主经 UAC 隐藏启动 4 秒保持运行并
  创建 6 个 WebView2 子进程，随后只停止本次主进程；未点击输入、未访问容器、未产生挂载，
  子进程已退出。
- MSVC `/analyze` 全量构建无发现；新增 SAFEARRAY 测试夹具使用
  `SafeArrayPutElement/GetElement` 后同样通过分析。正式 RelWithDebInfo ASan 预设下页面契约、
  SAFEARRAY 适配器、host-object 单元测试和真实 WebView2 闭环均退出 0，未报告
  AddressSanitizer 错误。调查中临时 ASan-Debug 组合曾因复制 `_dbg_` 运行库而缺少程序实际
  导入的非 `_dbg_` DLL；该非预设组合未用于结论，正式 `asan` 预设运行库与程序匹配。
- 安全复核未发现页面日志、持久化、用户名、模式/档位实体或分类反馈。产品禁用 WebMessage，
  测试页的 WebMessage 仅存在于不打包、不可日常运行的专项测试宿主，测试 CSP 不允许内联脚本。
  WebView2、JavaScript 引擎和 COM 内部仍可能产生应用无法主动擦除的短期秘密副本；当前实现
  会立即清空 DOM，在异步编组结算后清空 typed array，并清空已识别的 COM/原生受控缓冲区，
  但不能承诺浏览器全内存清零。异步代理挂起期间 typed array 必须保留原值，否则会破坏提交；
  本地 `session.js` 的替换防护仍依赖后续安装包与签名完整性阶段。
- `git diff --check` 通过。
- 提交：`072f79b [P1.5]accept WebView variant byte arrays`、
  `42000bf [P1.5]activate secure session controls`

### 阶段收口（2026-08-24）

- P1.5 出口条件已满足：固定 blob `ICardStore` 桩下能够完成“输入单一密码 → 挂载两个盘 →
  统一状态反馈 → 主动卸载”，且窗口关闭会先尝试卸载并在失败时保留可重试入口。
- 最终验证为 MSVC Debug CTest 15/15、Debug 真实 WebView2 Runtime 连续三次退出 0、正式
  ASan 相关测试与真实 Runtime 闭环退出 0、MSVC `/analyze` 全量通过；安全复核未发现秘密
  日志、本地持久化、模式/档位实体或分类反馈。浏览器与 COM 内部不可控短期副本风险按本阶段
  已知边界保留，后续由安装包完整性和签名阶段继续收口。
- USB 安全卡已经确认到货。P1.6 可开始真卡型号、接口、驱动/SDK、单 blob 读写及可选
  持久计数器能力的实机审计；到货不等同于能力验收通过，结论须在 P1.6 记录。

## 需求补充：明文导出与密文换机迁移（2026-08-24）

- 已确认保留既有明文导出，并补充密文导出供个人用户换机迁移；其余需求不变。
- 为避免把“密文导出”误实现成新的密钥体系，技术口径定为逐字节复制既有 VeraCrypt 容器：
  不解密、不重新加密、不修改容器，不导出卡内根密钥 `S`、口令、PIN、派生结果、档位或
  恢复材料。换机后仍须使用原安全卡与原口令，卡丢失/损坏仍不可恢复，也不提供自动迁移、
  新卡换卡或恢复码。
- 明文导出的原有风险确认、隐藏盘二次确认和零本地痕迹要求全部保留；密文迁移导出同样不得
  持久化目标路径、历史或校验值。复制失败、空间不足、取消和目标冲突不得留下被误认成成功
  结果的不完整副本。
- 已同步 `需求梳理清单_v2.md` 与 `MILESTONES.md`。F-037 继续删除：明文导出仍存在，新增
  容器原样复制不等于恢复“所有备份均随机无特征”的旧要求。P1.6 单 blob 真卡存储范围不变，
  新功能留在 P2；初估增加 1-2 天，待 P2 切片时复核。
- 验证：`git diff --check` 通过；既有 MSVC Debug CTest 15/15 通过。本切片只改文档，未触碰
  生产代码或安全卡。安全复核确认未新增根密钥导出、恢复码、新卡迁移或档位元数据；P2 实现
  时仍须专项防范目标覆盖、部分副本、重解析点/路径替换、Shell 最近记录和持久化复制校验值。
- 提交：`f48cca9 [docs]define encrypted migration export boundary`

## P1.6 真卡存储（2026-08-24 开始）

### 计划、需求与既有实现审计

- `MILESTONES.md` 的 P1.6 出口为：真实 `ICardStore` 完成单 blob 读写、以能力位报告硬件差异，
  卡材料只进入 `SecureBuffer`，且不得在本地持久化秘密；P1.7 的插拔监听和拔卡卸载不提前混入
  本阶段。
- `需求梳理清单_v2.md` 要求卡至少保存 32 字节密钥材料；N-07 还要求审计可写持久计数器，
  但不支持时允许降级为仅保留完整性校验。N-02 的单 blob 和本地不可区分边界继续成立。
- 已检查 `ICardStore`、`FixedBlobCardStore` 及其契约测试：现有应用边界只有 `get_blob`、
  `put_blob` 和能力位，读结果为 `SecureBuffer`；P1.5 桩明确只读且不保存可变秘密。正式适配器
  尚不存在，现阶段没有需要迁移的真卡代码。

### 真卡首次接入只读审计

- 插卡后未出现格式化、初始化或厂商软件提示。Windows 将新到达设备识别为
  `LiViKey ViKeyPRO`，硬件 ID 为 `VID_1987&PID_0331`、设备版本 `0x0500`；首次安装和本次
  到达时间均为 2026-08-24 04:52:12（系统记录时间）。设备状态为 Started、问题码为 0，
  使用微软内置 `HidUsb` / `input.inf`，不需要专用内核驱动。
- 设备只公开一个厂商自定义 HID 集合：Usage Page `0xFF00`、Usage `0x0001`；Input/Output
  Report 长度均为 0，Feature Report 长度为 257 字节。USB 字符串没有公开序列号。探测仅调用
  PnP 属性、HID attributes/string/preparsed-data/caps 读取接口，没有发送 Feature Report、
  PIN、初始化或写入命令。
- 厂商产品页称 ViKeyPRO 提供 4KB 数据区、用户/管理员 PIN 权限与 64 个计数器模块，纸面能力
  覆盖 32 字节 blob 和 N-07 计数器需求；但厂商 SDK 当前仅通过客服渠道提供。本次尚未取得
  官方头文件、库、示例、二进制校验信息和协议说明，故这些能力尚未完成实机验收。
- 网络上可找到非官方 API 转录，显示数据写入和计数器初始化需要管理员权限、运行期计数器为
  递减模型；该资料只作为索引线索，不作为生产 ABI 或安全结论。取得与本卡匹配的官方 SDK
  并审计其签名、位数、调用约定、缓冲区长度、错误码、PIN 重试/锁卡语义前，不进行登录或
  任何写卡测试。
- 提交：无（仅文档审计）

### 官方 SDK 静态审计（小切片 1）

- SDK 已从仓库移到仓库外的 `D:\Desktop\usb-vault-local-deps`；Git 当前只看到本次既有的
  三份文档修改，没有 SDK 文件进入工作区。收到的压缩包版本为
  `ViKeySDK_V4.1.2.37-7.4.7-7`，大小 498,898,534 字节，SHA-256 为
  `AA50C5BF2AB071D90E965DD9B984B36E5F76B854307E41FAB27FA4D638D93555`；解压后共
  1,051 个文件、1,156,017,632 字节。厂商未随包提供独立校验签名或可核对的发布哈希。
- 已完整提取并渲染复核官方《产品说明书》和《API 开发手册》的相关页，交叉检查头文件、x64
  导出表及 .NET/C# 示例。主头文件 SHA-256 为
  `739A3527E25942BA29F643BAFACDA37B33915820341896A1089DD1164FE93953`；正式 x64
  `ViKey.dll` 为 4.1.2.37，SHA-256 为
  `735A2347F66F10C649C9B38ED0232B839812277C8E178F0C5084C676CF1DE48E`。SDK 内 47 份
  同名 DLL 只有三个不同哈希，除当前 x86/x64 版本外只有一份旧 x64 4.1.0.16，全部未签名，
  不存在可替换的已签名版本。
- 头文件使用 `extern "C"` + `__stdcall`，x64 DLL 的未修饰导出与所需的发现、登录、容量查询、
  数据读写、次数模块和清理函数一致。头文件和各语言示例均把数据地址/长度声明为 32 位
  `DWORD`/`uint`，但 PDF 的单函数页把两者写成 16 位 `WORD`；权限汇总表又与
  `VikeyGetType` 单函数页冲突。生产适配器须以头文件、导出和示例的交集建立显式 ABI 断言，
  并要求供应商书面确认这两处文档冲突，不能直接照抄 PDF 原型。
- 7.4.6 及以后硬件必须先调用 `VikeySetUserSN` 才能发现设备；7.4.5 及以后还须在登录前设置
  API Key。文档要求从加密管理工具“帮助中心”复制 UserSN，并用管理工具根据 API Key 种子生成
  API Key。SDK 示例内含一组通用演示常量，不能据此认定为本采购批次凭据。当前尚未取得本卡的
  UserSN、API Key/种子及其供应链交付说明，因此不能进行有意义的匿名 API 探测。
- 官方资料说明用户/管理员 PIN 均固定为 8 字符，默认值分别为八个 `1` 和八个 `0`，默认尝试
  次数各 200；失败登录会递减次数，用户次数归零后需管理员恢复，管理员次数归零后只能用管理
  工具恢复出厂。出于防锁卡原则，本次没有尝试任何默认 PIN、示例凭据、登录、初始化、读写或
  计数器调用。
- 当前手册称卡数据区最大可达 16KB，产品页对 ViKeyPRO 的描述为 4KB，实际型号容量须通过
  `VikeyGetUserDataSize` / `VikeyGetAdminDataSize` 实机查询，不能预设。数据区分为用户区与
  管理员区；当前权限表显示用户登录可读写用户区、只读管理员区，足以承载一个 32 字节 blob。
  计数器为 16 位递减模块，提供初始化、读取、递减与校验 API，纸面上覆盖 N-07，但持久性、
  断电行为、原子性、溢出/耗尽和权限仍须真卡验证。

### SDK 供应链与集成安全审查

- x64 `ViKey.dll` 未签名，PE 头没有 ASLR、NX_COMPAT、CFG 或高熵地址标志，包含 `.boot`、
  `.vikey` 等保护/加壳节；导入依赖含 WinHTTP、Winsock、Crypt32、DbgHelp 等。静态导入只能
  证明它具备相应能力，不能证明会联网，但也无法从受保护二进制审计其真实控制流。约 188MiB
  的官方管理工具同样未签名、加壳且缺少 ASLR/NX，并声明 WinINet/WinHTTP 网络依赖。本次没有
  运行 DLL、管理工具、示例或远程授权工具。
- SDK PDF 内的《软件开发协议》允许为保护应用而合并/链接接口软件，并允许合理数量的归档副本，
  同时禁止逆向、反汇编、反编译和修改；包内没有独立 `LICENSE`。协议没有清楚写明可把
  `ViKey.dll` 随消费级产品再分发，故发行前必须取得供应商对 DLL 再分发、更新与漏洞响应的
  书面授权。自行逆向 HID Feature Report 协议既违反当前协议边界，也会把未经验证的私有协议
  变成生产 ABI，不采用。
- 在当前证据下，不允许把该 DLL 直接加载进持有根密钥、用户口令和派生结果的主进程。下一代码
  切片应先建立可替换的 SDK 边界、严格错误映射和失败关闭测试；若供应商不能提供签名且带现代
  缓解标志的库，还需把供应商组件放进受限、禁网的独立 helper，并把 DLL 哈希列为显式部署
  白名单。即使隔离，DLL 仍会看到其返回的卡内 blob，因此隔离只能缩小主进程攻击面，不能把
  未知供应链风险描述成已消除。
- P1.6 还暴露一个需求契约缺口：产品登录界面只有一个任意长度的用户口令，但设备另需一个固定
  8 字符用户 PIN；同时还需要批次 UserSN 和 API Key。实现前必须明确配卡/初始化流程、三类凭据
  的来源与生命周期、是否允许相同或默认 PIN、blob 地址/长度/版本及首次写入的失败恢复规则。
  任何方案都不得把卡 PIN、根密钥或可推导档位的信息持久化到本地；未明确前不能把默认 PIN
  硬编码进产品，也不能把用户口令静默截断成 8 字符充当卡 PIN。
- 本切片结论：设备枚举与 SDK ABI 的静态可行性成立，但 P1.6 的“真卡写入并读回”出口尚未
  通过。需先向供应商取得本批 UserSN、API Key 生成/交付方式、ABI 冲突答复、签名/现代构建和
  DLL 再分发授权，并由项目负责人确认配卡认证契约；随后才能在受控环境执行首次非破坏查询和
  单 blob 读写。
- 验证：MSVC Debug CTest 15/15 通过，`git diff --check` 通过。由于本切片没有生产代码或构建
  产物变更，未重复运行 ASan、`/analyze` 或真实 WebView2 闭环；安全审查集中于供应链、PIN
  锁定、秘密生命周期、DLL 进程边界、联网能力、许可和本地不可区分性。PDF 审计产生的 12 张
  临时渲染页已从专用临时目录删除。
- 提交：无（仅文档审计）

### UserSN 获取与匿名真卡探测（小切片 1 补验）

- 已通过官方管理工具的“帮助中心”取得当前账户 UserSN。完整 UserSN 不写入仓库、日志或
  产品配置；本次仅经标准输入交给一次性独立探测进程。探测前再次核对 x64 DLL SHA-256 为
  `735A2347F66F10C649C9B38ED0232B839812277C8E178F0C5084C676CF1DE48E`，只调用
  `VikeyGetLibraryVersion`、`VikeySetUserSN`、`VikeyFindEx`、硬件 ID/类型/固件/容量/权限/PIN
  次数查询及 `VikeyUninitialization`；没有设置 API Key、激活、登录、尝试 PIN、读写数据区或
  调用计数器。一次性探测脚本执行后已删除，未进入 Git 工作区。
- 匿名调用成功发现 1 张实体卡：接口库 4.1.2.37、硬件类型值 3（ViKeyPRO）、固件 7.4.7，
  硬件 ID 与管理工具界面一致。用户数据区为 8,192 字节，管理员数据区为 8,192 字节，总容量
  16KB；这解决了产品页 4KB 与新版 API 手册“最高 16KB”的冲突，并远超 32 字节单 blob 需求。
- `VikeyGetLevel`、用户 PIN 次数与管理员 PIN 次数查询均返回 `0x80000024`
  （`VIKEY_ERROR_NOT_ACTIVE`），与管理工具显示“未激活”一致。这些查询不执行登录，未消耗
  PIN 尝试次数。SDK 的公开头文件和示例没有激活 API，激活只暴露在管理工具中，当前尚不清楚
  它会绑定哪些账户/批次信息、是否联网、是否可逆及是否同时初始化密码/数据区。
- 因此下一安全边界从“取得 UserSN”推进为“确认并执行首次激活”：在供应商说明激活副作用、
  API Key 交付方式、默认/初始化 PIN、恢复出厂影响和离线/联网要求前，不点击“激活”。卡仍未
  写入项目根密钥，P1.6 单 blob 写入/读回与计数器实测尚未开始。

### 首次激活与激活后匿名复核

- 已明确决定激活这张新卡，并在官方管理工具内完成操作。界面确认卡已绑定当前 UserSN，
  出厂日期为 2026-08-13；软件 ID 仍是厂商默认 `1234ABCD`，设备名为空。激活前后均未向
  数据区写入项目根密钥，也未设置产品设备名或修改软件 ID。
- 使用同一经哈希核对的 x64 DLL 再次执行独立匿名只读探测：`VikeyFindEx` 发现 1 张卡，
  `VikeyGetLevel` 成功返回 0（匿名权限）；用户 PIN 与管理员 PIN 的当前/最大尝试次数均为
  200/200，说明激活未消耗 PIN 次数。`VikeyUninitialization` 成功，未执行登录、PIN 尝试、
  数据读写或计数器操作。一次性复核脚本随后删除，完整 UserSN 仍未写入仓库。
- 激活阻塞已解除。下一步转为只读确认管理工具“密钥设置”页的 API Key 种子/生成流程，再制定
  正式配卡值；在该流程确认前不使用 SDK 示例 API Key，不尝试默认用户/管理员 PIN，也不覆盖
  卡的密钥设置。

### 开发 API Key 生成与匹配验证

- SDK 批量配置注释确认 API Key 种子最长 32 字符且可自定义；示例生成结果为 128 个十六进制
  字符。管理工具页面同时包含 PIN 修改、尝试次数、API Key 生成和“外壳独立密钥”写卡四类
  操作；本次只使用“API 密钥种子 → 生成 API 密钥”，没有触碰其他三类状态。
- 使用系统 CSPRNG 生成 16 字节开发专用随机种子并编码为 32 个十六进制字符。种子 SHA-256
  短指纹为 `7532E145A07A`；生成的 128 字符 API Key 经格式校验，SHA-256 短指纹为
  `ED580A9B5298`。两个明文均未进入聊天、仓库或开发日志，只保存在主机 Windows 凭据管理器的
  `usb-vault-vikey-dev-api-seed` 和 `usb-vault-vikey-dev-api-key` 通用凭据中；剪贴板已清空。
- Codex 文件/进程沙箱与主机凭据库隔离，沙箱内 `cmdkey /list` 不会显示上述主机凭据；在同一
  主机用户上下文中读取开发 API Key 后执行独立验证，`VikeySetUserSN`、`VikeyFindEx`、
  `VikeySetApiKey`、`VikeyGetLevel` 和 `VikeyUninitialization` 均返回成功。权限级别仍为 0，
  证明 API Key 匹配且该验证没有登录或消耗 PIN 次数。一次性验证脚本随后删除。
- 该凭据仅用于当前开发卡和 P1.6；生产种子/API Key 必须另行生成并进入正式构建秘密管理，
  不能复用开发值。厂商资料仍未说明同一 UserSN + 种子生成的 API Key 是账户/批次通用还是
  单卡独立，量产流程定案前须向供应商确认或用第二张卡交叉实测。

### 硬件交付边界确认

- 客户决定经转述确认：量产阶段由客户硬件方自行烧录安全卡，当前 ViKeyPRO 只用于开发阶段跑通，
  最终卡型/供应商可能替换。因此 P1.6 不实现量产烧录工具、生产卡初始化后台或生产凭据托管，
  也不把 ViKey 选型写死为产品架构。
- 现有 `ICardStore` 单 blob 合约正是正式交付边界：业务层只接触 `get_blob`、`put_blob` 和能力位，
  卡材料只以 `SecureBuffer` 进入/离开。ViKey 的 DLL、ABI、UserSN、API Key、8 字符设备 PIN、
  数据区偏移和错误码必须封闭在厂商适配实现及部署注入层，不得泄漏到 KDF、挂载编排、UI 或
  本地持久化格式。
- 当前 P1.6 仍要用这张卡完成真实 32 字节 blob 写入/读回与持久计数器验证，以证明抽象可实现；
  但验收结论只证明 ViKey 开发适配可用，不构成量产硬件锁定。以后换卡时只替换适配器并重跑
  `ICardStore` 契约、秘密生命周期、错误映射、插拔和能力降级测试，不修改上层安全设计。
- 下一代码切片建议先实现“厂商无关函数表/错误域 + ViKey 动态加载边界”的单元测试骨架，
  不立即把未签名 DLL 加载进主进程；该边界评审并提交后，再接真实登录、blob 和计数器调用。
- 提交：`d665955 [P1.6]record ViKey audit and replaceable hardware boundary`

### 小切片 2：通用错误域与 ViKey 动态加载边界

#### 计划、需求与既有实现复核

- 重新核对 `MILESTONES.md` 的 P1.6 出口、`需求梳理清单_v2.md` 的 R-03、N-02 和 8.6，
  并检查既有 `ICardStore`、固定 blob 桩、错误类别实现、CMake 目标和卡契约测试。结论是
  `ICardStore` 已经是正式的厂商无关边界，不应再公开一套照搬 ViKey 登录/地址/计数器语义的
  “通用”函数表；供应商切换只需替换适配实现。
- 本切片只建立 SDK 装载和错误映射边界，不实现 `ICardStore` 真卡读写，不登录、不尝试 PIN、
  不接收 UserSN/API Key，不读写数据区或计数器，也不加载真实 `ViKey.dll`。插拔监听仍属于
  P1.7，不混入本切片。

#### 实现

- 新增公共 `CardError` 错误域，覆盖后端不可用、配置无效、无卡、认证失败/锁定、权限不足、
  不支持、通信失败、非法操作和设备内部失败。ViKey 数值状态在内部映射后才可离开适配层；
  未知状态失败关闭为通用设备故障，不把厂商错误码扩散到 KDF、UI 或应用编排。
- 新增私有 `vikey_sdk_abi.h`，用固定宽度类型和 `__stdcall` 定义 15 个产品运行期所需导出；
  包含 `VikeyGetHID`，避免未来默认使用索引 0 选错卡。管理员登录和 `ViKeySetModule` 未进入
  运行期表，量产/初始化继续由客户硬件方或独立受控验证流程负责；运行期只保留计数器读取、
  递减和能力检查入口。
- 新增 `VikeyModule`：只接受绝对 DLL 路径，使用
  `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32` 限制依赖搜索，要求全部导出
  一次性解析成功；任一导出缺失即卸载并失败。模块对象不可复制、不可移动，函数表不会脱离
  唯一模块生命周期成为悬空指针。
- ViKey 代码单独构建为 `usb_vault_vikey_sdk` 静态库。构建图和最终 PE 导入表均确认
  `usb_vault_app.exe` 不链接或导入该库/`ViKey.dll`；单元测试通过注入的假模块操作验证装载
  边界，不执行供应商代码。

#### 验证与安全复核

- MSVC Debug 全量 CTest **16/16** 通过；新增测试覆盖相对路径和不完整装载接口拒绝、DLL 装载
  失败、缺少导出时卸载、完整 15 项函数表的单一生命周期，以及已知/未知 ViKey 状态到
  `CardError` 的失败关闭映射。
- 新测试的 MSVC AddressSanitizer 运行通过；`usb_vault_vikey_sdk` 与测试目标在 MSVC
  `/analyze` 下无告警构建通过；`git diff --check` 通过。
- 对仓库外 x64 DLL 重新计算 SHA-256，仍为
  `735A2347F66F10C649C9B38ED0232B839812277C8E178F0C5084C676CF1DE48E`；使用 `dumpbin /exports`
  静态确认 15 个必需导出全部存在。该操作只读取 PE，不加载 DLL，也不访问安全卡。安全扫描
  同时确认运行期 ABI 中没有管理员登录、计数器初始化或档位/模式实体，代码与测试中未加入
  UserSN、API Key、PIN 或根密钥明文。

#### 剩余风险与下一步

- 供应商 DLL 仍未签名且缺少现代 PE 缓解标志；当前代码只限制加载路径和依赖搜索，尚未在
  装载前强制校验 DLL 哈希，也尚未建立禁网、低权限的独立 helper。因此本切片不能授权把真实
  DLL 接入主进程；下一切片应先固化 helper 进程边界、固定部署位置/哈希白名单和最小 IPC，
  再让真实卡会话进入其中。
- `VikeyUninitialization` 已纳入函数表，但模块对象本身不会假设 SDK 已初始化并擅自调用它；
  下一会话 RAII 必须保证一旦执行 `FindEx`，所有成功/失败路径都先注销权限并执行
  `VikeyUninitialization`，之后才卸载模块。
- 真卡 `ICardStore`、8 字符设备 PIN 的独立凭据注入、32 字节 blob 地址/版本/首次写入恢复、
  持久计数器实测和拔卡处理均未实现。本切片没有改变卡片状态。
- 提交：`00b3319 [P1.6]add isolated ViKey SDK loading boundary`

### 小切片 3：ViKey DLL 固定身份与加载期防替换门

#### 计划、资料与既有实现复核

- 重新核对 P1.6 的真卡出口、R-03 与 8.6，并复查小切片 2 的 `VikeyModule`、构建隔离及测试。
  本切片仍不实现 helper、IPC、卡登录或 `ICardStore`，只完成任何供应商代码执行之前都必须经过
  的本地二进制身份门；产品主程序继续不链接 ViKey 目标。
- 依据 Microsoft `CreateFileW` 文档确认：不授予 `FILE_SHARE_WRITE` 可在句柄存续期拒绝后续写入，
  不授予 `FILE_SHARE_DELETE` 可拒绝后续删除/改名；依据 `GetFileInformationByHandleEx` /
  `FileAttributeTagInfo` 检查目录、普通文件与重解析点；SHA-256 使用 Windows CNG 的
  `BCryptCreateHash` → `BCryptHashData` → `BCryptFinishHash` 生命周期。对应官方资料：
  <https://learn.microsoft.com/th-th/windows/win32/api/fileapi/nf-fileapi-createfilew>、
  <https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getfileinformationbyhandleex>、
  <https://learn.microsoft.com/en-us/windows/win32/api/Bcrypt/nf-bcrypt-bcryptfinishhash>。

#### 实现

- 新增私有 `VerifiedVikeyDll` 移动令牌与 `verify_vikey_dll`。调用方只提供绝对、固定本地盘上的
  部署目录，文件名由实现固定拼为 `ViKey.dll`，不能向模块装载器传任意文件路径。目录和文件
  都以 `FILE_FLAG_OPEN_REPARSE_POINT` 打开，拒绝末级重解析点，并用句柄解析出的最终 DOS 路径
  反查原配置，防止既有 junction/symlink 把验证落到其他位置。
- 校验器把 DLL 大小限制为 1 字节至 16 MiB，通过持有的只读文件句柄分块计算 SHA-256；读取
  总量、文件大小或白名单摘要不一致均失败关闭为通用 `backend_unavailable`，不向上泄漏底层
  文件或 CNG 错误细节。64 KiB 读取缓冲改为有界堆内存，避免受限 helper 小栈压力。
- 校验成功后令牌继续持有部署目录和 DLL 句柄：文件句柄只共享读，目录句柄不共享删除；
  `VikeyModule::load_from` 已删除裸路径入口，只接受该令牌，并把它持有到 `FreeLibrary` 完成后
  才释放。哈希失败、模块装载失败或导出解析失败的所有路径都由 RAII 释放句柄。
- `usb_vault_vikey_sdk` 显式纳入校验实现并私有链接 `Bcrypt`。生产 exe 仍不链接该静态库；
  测试只使用固定 ASCII fixture 与假模块操作，不加载真实 `ViKey.dll`。

#### 验证与安全复核

- 新增 `vikey_artifact_tests`，覆盖固定目录/文件名构造、相对目录拒绝、正确摘要、摘要不匹配、
  空文件、超过 16 MiB、失败后句柄释放，以及令牌存续期间文件写入/删除和部署目录/上一层测试
  目录改名被拒绝。模块测试进一步证明锁句柄随模块生命周期持有，并为两支测试增加顶层异常
  输出，避免 Debug 失败被 Windows 错误报告对话框掩盖。
- MSVC Debug 全量 CTest **17/17** 通过；两支 ViKey 测试的 MSVC AddressSanitizer 运行通过；
  `usb_vault_vikey_sdk` 与两支测试在 MSVC `/analyze` 下无诊断构建通过；`git diff --check` 通过。
- 对仓库外 x64 `ViKey.dll` 再做静态复核：既有 SHA-256 白名单匹配，15 个运行期必需导出仍
  全部存在；`usb_vault_app.exe` 导入表仍不含 ViKey。该复核没有加载供应商 DLL、没有访问安全
  卡，也没有读取 UserSN、API Key、PIN 或根密钥明文。

#### 剩余风险与下一步

- 该门只证明“准备加载的文件与显式摘要一致”，不能证明未签名供应商 DLL 本身安全。独立
  helper、最小 IPC、禁网/低权限进程令牌、显式继承句柄清单和进程缓解策略仍未实现，所以真实
  DLL 仍不得加载进产品主进程；下一小切片先建立 helper 进程与 IPC 外壳，并由 ViKey 适配构建
  注入不可由运行时用户配置改写的当前版本摘要。
- 句柄锁在当前 NTFS 实测中同时阻止了部署目录及其上一层改名，但量产仍必须把部署目录放在
  安装器保护的父目录并设置只读 ACL；开发机用户可写的 SDK 目录只允许用于受控跑通，不能作为
  量产可信部署。供应商升级还必须显式更新摘要白名单并重跑 ABI、导出和安全审计。
- 真卡登录、独立 8 字符设备 PIN 注入、32 字节 blob 读写、计数器和拔卡处理均未开始，本切片
  未改变安全卡状态。
- 提交：`357b751 [P1.6]require verified ViKey DLL identity`

### 小切片 4：受限 ViKey helper 与最小 IPC 空外壳

#### 计划、资料与既有实现复核

- 重新核对 P1.6 出口、R-03、N-02、8.6 以及前三个小切片的 `ICardStore`、ViKey 私有 ABI、
  动态加载和 DLL 固定身份门。结论是供应商代码必须停留在独立非提权进程中，主程序和公共卡
  接口均不能链接 helper 客户端或 ViKey SDK；本切片只验证进程/IPC 边界，不加载 SDK、不登录、
  不读取 UserSN/API Key/PIN、不读写卡片、计数器或根密钥。
- 依据 Microsoft 官方文档复核 `CreateRestrictedToken`、`CreateProcessAsUserW`、
  `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`、进程缓解策略和 job object：显式句柄清单要求目标句柄可继承
  且 `bInheritHandles=TRUE`；受限主令牌可直接用于 `CreateProcessAsUserW`；`KILL_ON_JOB_CLOSE` 可
  收口异常/超时路径，`ACTIVE_PROCESS=1` 与子进程策略共同阻止 helper 扩张进程树。参考资料：
  <https://learn.microsoft.com/en-us/windows/win32/secauthz/restricted-tokens>、
  <https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessasuserw>、
  <https://learn.microsoft.com/en-us/windows/desktop/api/processthreadsapi/nf-processthreadsapi-updateprocthreadattribute>、
  <https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects>、
  <https://learn.microsoft.com/en-us/windows/win32/api/jobapi2/nf-jobapi2-setinformationjobobject>。

#### 实现

- 新增固定布局的 IPC v1：32 字节 `Request`、36 字节 `Response`、固定 magic/版本/头长、唯一
  `probe` 操作、全零保留字段和 16 字节 CSPRNG nonce。未知操作、状态码、安全属性、版本、
  nonce、短消息和尾随字节均失败关闭；协议目前不承载 PIN、API Key、blob 或任何秘密。
- 新增独立 `usb_vault_vikey_helper` GUI 子系统空外壳和未接入产品目标的 helper client。client
  只接受绝对非 UNC、末级非重解析普通文件路径；用 `CreateRestrictedToken(DISABLE_MAX_PRIVILEGE)`
  创建主令牌，并枚举令牌特权确认除 `SeChangeNotifyPrivilege` 外没有启用项，同时要求非提升令牌。
  已带 restricting SID 的父令牌会失败关闭，避免在不确定的嵌套沙箱中误判边界。
- helper 以 `CREATE_SUSPENDED` 启动；只有请求/响应两根匿名管道进入
  `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`，stdin 读取请求，stdout/stderr 共用响应管道，父端句柄显式
  不可继承。命令行不携带句柄或秘密，Unicode 环境块为空。父进程先把 helper 加入
  `ACTIVE_PROCESS=1 | KILL_ON_JOB_CLOSE` 的 job，再核验 job 成员关系及所有必需缓解策略，全部
  通过后才恢复线程。
- 启动策略强制 DEP、SEHOP、堆损坏终止、bottom-up/high-entropy ASLR、严格句柄检查、禁用扩展点、
  禁止远程/低完整性映像加载和创建子进程；父进程逐项读取实际进程策略并要求完整匹配。helper
  自检环境为空、核对标准句柄均为预期管道并回显 nonce；失败、协议异常、非零退出或截止时间
  到期时均由 RAII 关闭 job 并终止残留进程。
- 受限令牌下，控制台子系统或动态 CRT 产物在进入程序前即以 `0xC0000142` 失败；最小对照夹具
  证明 GUI 子系统 + 静态 CRT 可在相同令牌下启动，因此 helper 与卡死测试夹具固定采用该组合。
  这只是运行期兼容选择，不改变产品主程序 CRT，也没有引入厂商依赖。

#### 验证与安全复核

- 新增协议测试，覆盖请求/响应往返、未知版本/操作/安全属性/状态、非零保留字段、错误响应约束
  和 nonce 不匹配拒绝；新增进程测试覆盖完整安全属性、相对路径拒绝及 150 ms 卡死超时。普通
  桌面令牌下真实 helper/fixture 测试通过；Codex/CI 这类父令牌已受限的环境返回 77 并由 CTest
  明确标记跳过，不能把沙箱假失败冒充真实进程验证。
- MSVC Debug 全量 CTest **18 项通过、1 项因已受限父令牌跳过**；同一进程测试在普通桌面令牌下
  单独通过。MSVC AddressSanitizer 全量同为 **18 项通过、1 项明确跳过**，无内存错误；helper、
  协议、客户端和测试目标在 MSVC `/analyze` 下无诊断构建通过。
- PE 复核确认 helper 为 Windows GUI，启用 high-entropy ASLR、动态基址和 NX，嵌入清单为
  `asInvoker`/`uiAccess=false`；helper 仅导入 Windows `ADVAPI32.dll` 与 `KERNEL32.dll`，不导入
  `ViKey.dll`。`usb_vault_app.exe` 导入表同样不含 ViKey。敏感信息扫描和 `git diff --check`
  在本切片最终复核中执行；本测试过程没有加载厂商 DLL、访问安全卡或改变卡片状态。

#### 剩余风险与下一步

- 当前 helper 路径门只拒绝相对路径、UNC 语法和末级重解析点，尚未像供应商 DLL 一样持有并
  校验 helper 自身身份，也尚未依赖安装器保护目录 ACL；开发构建目录可由当前用户写入，不能
  作为量产可信部署。接入根密钥前必须固化 helper 摘要/签名或等价身份门，并验证安装 ACL。
- 受限令牌和映像加载策略不等于禁网。当前 helper 没有网络代码、没有 SDK、也没有秘密，所以
  本切片可安全验收；在装载未签名供应商 DLL 前必须增加可验证的网络隔离，否则不能把卡内
  根密钥交给该进程。
- 真卡会话 RAII、独立 8 字符设备 PIN 注入、32 字节 blob get/put、首次写入恢复、持久计数器和
  P1.7 拔卡处理仍未实现。
- 提交：`9603b38 [P1.6]add restricted ViKey helper boundary`

### 小切片 5：ViKey helper 固定身份与启动期防替换门

#### 计划、需求与既有实现复核

- 重新核对 P1.6 出口、R-03、8.6、小切片 3 的 DLL 身份令牌和小切片 4 的受限 helper 启动
  路径。本切片只解决 helper 可执行文件自身的身份与启动期替换风险；不实现网络隔离、SDK
  装载、卡登录、PIN/API Key/UserSN 注入、blob/计数器操作，也不接触根密钥或安全卡。
- 复核确认 DLL 与 helper 的文件验证需求一致，但两者的部署名称和消费者不同。因此提取厂商
  无关的本地制品令牌，而不是让 helper client 依赖 ViKey DLL 封装。生产摘要仍必须由不可被
  普通用户修改的构建/部署配置注入；测试中按运行时测试产物计算摘要只用于测试。

#### 实现

- 新增移动独占的 `VerifiedLocalArtifact` 与 `verify_local_artifact`：只接受固定本地盘的绝对
  普通文件，显式拒绝相对路径、空文件名、NTFS 备用数据流、末级重解析点和最终路径重定向；
  同时锁住包含目录与文件，文件只共享读、两者都不共享删除，令牌销毁前不能写入、删除或替换
  已验证制品。文件大小由调用方给出上限，当前 DLL/helper 均限制为 1 字节至 16 MiB，并通过
  持有的同一文件句柄分块计算 SHA-256。
- `VerifiedVikeyDll` 改为组合通用令牌，保留既有固定名 `ViKey.dll` 和模块生命周期合约；原有
  DLL 锁定、摘要、失败释放和目录改名测试继续覆盖重构后的通用实现。
- `probe_vikey_helper` 删除仅按路径启动的弱入口，强制调用方同时传入期望 SHA-256；在创建任何
  管道、令牌或进程前先验证 helper，并把制品令牌持有到握手、退出码和尾随数据检查全部完成。
  摘要不匹配统一失败关闭为 `backend_unavailable`，不会启动目标进程。
- 安全测试发现优化版静态 CRT 在 helper 返回后仍会处理标准流。启动端因此为 stdin、stdout
  和 stderr 提供三个值不同且都位于显式继承清单中的句柄；stderr 是响应写端的独立重复句柄，
  非协议输出仍会污染响应并失败关闭。helper 校验三者均为不同的管道句柄，并让进程终止统一
  回收标准句柄，避免在 CRT 收尾前提前关闭导致 Release 崩溃。非零退出码现在带十六进制码供
  内部诊断，不改变对上层的通用通信错误。

#### 验证与安全复核

- 新增/扩展测试覆盖通用令牌不可复制、正确摘要、摘要不匹配、相对路径、备用数据流、空/超限
  文件、文件写入/删除锁、目录及上一层改名锁、helper 正确摘要启动、错误摘要启动前拒绝和
  150 ms 卡死终止。MSVC Debug 与 AddressSanitizer 全量 CTest 均为 **18 项通过、1 项因父
  令牌已受限而明确跳过**；两套进程测试在普通桌面令牌下分别单独通过。
- 额外建立独立未插桩 Release helper 诊断构建，确认优化版在 5 秒门限内通过；该验证直接发现
  并修复标准句柄提前关闭问题。相关制品、helper client、SDK 包装和测试目标在 MSVC
  `/analyze` 下无诊断构建通过，`git diff --check` 通过。
- PE 复核确认产品 `usb_vault_app.exe` 不导入 ViKey 或网络库；helper 仍为 Windows GUI、
  `asInvoker`/`uiAccess=false`，启用 high-entropy ASLR、动态基址和 NX，且只依赖
  `ADVAPI32.dll` 与 `KERNEL32.dll`。仓库扫描未发现当前卡的 UserSN 或硬件 ID；既有文档中的
  厂商默认软件 ID 不是秘密。本切片没有加载 SDK、访问安全卡或改变卡片状态。

#### 剩余风险与下一步

- 产品接线时必须把发布 helper 的固定 SHA-256 作为不可由运行时用户修改的部署常量注入；
  当前测试的动态摘要不能进入生产。安装器还需把 helper 与供应商组件置于受保护目录并验证
  ACL，供应商/程序升级则必须显式更新摘要并重跑 ABI 与安全审计。
- 受限令牌、身份门和映像加载策略仍不等于禁网。下一独立小切片只建立可验证的 helper 网络
  隔离；在该门通过前继续禁止装载真实 `ViKey.dll`、注入卡凭据或读取卡内 blob。之后才可接入
  真卡会话 RAII、独立 8 字符设备 PIN、32 字节 blob 和持久计数器。
- 提交：`db7bef1 [P1.6]verify ViKey helper identity before launch`

### 小切片 6：ViKey helper 可验证网络隔离门

#### 计划、需求与既有实现复核

- 重新核对 P1.6 出口、R-03、N-02、8.6 以及小切片 4~5 的受限进程、IPC 和 helper 身份门。
  本切片只建立供应商代码接触卡内 blob 前的独立禁网门，不装载 `ViKey.dll`，不调用 SDK，
  不访问安全卡，也不读取或注入设备 PIN、API Key、UserSN、根密钥或其他凭据。
- 对照 Windows 官方 AppContainer 启动与 capability 文档后，选择不声明任何 capability 的
  AppContainer：网络能力默认拒绝，且可与现有受限主令牌、显式句柄清单、单进程 job 和进程
  缓解同时使用。未选择 Windows 防火墙/WFP 作为运行时证明，因为其规则创建通常需要管理员、
  修改全局状态并依赖部署配置，不能替代每次启动前可回读的子进程令牌属性。参考：
  <https://learn.microsoft.com/en-us/windows/win32/secauthz/implementing-an-appcontainer>、
  <https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/app-capability-declarations>。
- AppContainer 同时默认隔离文件和设备，因此“禁网成立”不等于“供应商 DLL 及 ViKey USB/HID
  仍可用”。该兼容性不属于本切片，必须在任何卡材料进入 helper 前作为下一独立安全门验证。

#### 实现

- helper client 每次用 BCrypt CSPRNG 生成 128 位随机后缀，创建仅在本次调用期间存在的
  AppContainer profile，并通过 `PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES` 传入该 SID，
  capability 数组严格为空；原受限令牌、句柄白名单、缓解策略和禁止子进程属性继续同时生效。
- 子进程保持 suspended 时，父端打开**实际子进程令牌**并要求 `TokenIsAppContainer=true`、
  `TokenAppContainerSid` 与本次随机 SID 完全相等、`TokenCapabilities.GroupCount=0`、完整性不高于
  Low；这些属性与既有非提权、特权过滤、job 归属及所有缓解位全部满足后才允许恢复线程。
  IPC 安全属性新增 `app_container`、`no_capabilities`、`low_integrity` 三个强制位。
- 空 Unicode 环境会使 AppContainer 创建进程返回 Windows 错误 203，因此改为只传
  `LOCALAPPDATA`、`TEMP`、`TMP` 三项非秘密随机 profile 路径，且 helper 自检环境恰好只有三项、
  `TEMP=TMP=LOCALAPPDATA\\Temp`。父端同时把工作目录显式固定到该随机 profile 目录，不再继承
  产品工作目录；helper 不接受宿主环境中的 PATH、代理、用户配置或凭据。
- profile 在标准 AppContainer 生命周期内保留到 helper 退出；正常退出后显式删除。创建进程
  失败、令牌/策略校验失败、协议错误和超时路径都会先终止并有界等待子进程，再显式删除 profile；
  清理失败会覆盖原错误并向上可见，析构只作为最后的尽力兜底。该顺序避免使用行为不稳定的
  “profile-less AppContainer”，也为下一步文件/设备兼容性审计保留标准 Windows 语义。
- 新增独立静态 CRT GUI 网络夹具及共享 probe。probe 对 TCP/UDP 分别测试 IPv4/IPv6 的回环
  地址和 RFC 文档地址，共八条路径；只发送固定单 NUL 测试字节，不含秘密。非隔离父进程先作
  正向控制，证明测试主机网络栈并未整体失效；隔离子进程只有在所有受支持路径均返回拒绝时才
  成功。测试目标单独链接 `Ws2_32`，产品与正式 helper 不包含网络代码或网络库。

#### 验证与安全复核

- 扩展进程测试覆盖：完整安全属性精确匹配、非隔离网络正向控制、八条隔离网络路径、相对路径
  拒绝、错误摘要启动前拒绝、非 PE 制品创建进程失败、150 ms 卡死超时，以及正常 helper、网络
  夹具、创建失败和超时四类路径的 profile 前后计数不变。所有普通桌面测试结束后再次枚举，
  `usb-vault-vikey-helper-*` 残留目录为 0。
- 最终 MSVC Debug、AddressSanitizer 与 `/analyze` 三套全量 CTest 均为 **18 项通过、1 项因
  Codex 沙箱父令牌已经受限而明确跳过**。普通桌面令牌下 Debug 与独立 Release 优化版进程测试
  均通过；ASan 插桩父测试进程启动未插桩 helper/fixture 也通过，未报告内存错误。MSVC
  `/analyze` 全量构建零项目诊断；仅对 Windows SDK `WS2tcpip.h` 自身的 C6101 误报作头文件范围
  内的窄抑制，项目代码仍保持 `/WX`。
- 完全 ASan 插桩的子 helper 需要额外动态 ASan 运行库，在 AppContainer 内握手前退出；改用
  ASan 父进程 + 未插桩静态 CRT 子进程可验证 launcher 生命周期。该现象不是产品构建失败，
  但它提供了明确证据：未来 `ViKey.dll` 及其依赖的文件访问必须单独证明或设计安全复制/ACL，
  不能因网络门通过就直接假定可加载。
- PE 复核确认 Release 正式 helper 仍为 `asInvoker`/`uiAccess=false`，启用 high-entropy ASLR、
  dynamic base 和 NX，只导入 `KERNEL32.dll`，不导入 `Ws2_32` 或供应商库；产品 exe 也未出现
  ViKey、`Ws2_32`、WinHTTP、WinINet 或 Userenv 导入。本切片改动扫描未发现卡标识、API Key 或
  其他凭据，`git diff --check` 通过。本切片没有加载 SDK、访问安全卡或改变卡片状态。

#### 剩余风险与下一步

- 下一小切片必须先验证零 capability AppContainer 对供应商 DLL/依赖文件和 ViKeyPRO USB/HID
  设备的最小访问方案。候选方案需要保持禁网、低完整性、固定摘要和启动期防替换属性，且不得
  让主程序提权或依赖用户临时配置；在该门通过前禁止注入设备 PIN/API Key 或读取卡内 blob。
- 父进程异常崩溃或被强杀时无法执行用户态清理，可能留下随机命名、零 capability、无秘密的
  空 profile；正常、已观察错误和超时路径均已验证清理。若后续采用固定 AppContainer 身份以
  配合部署 ACL，必须另行审计 profile 防篡改、升级和卸载清理，不能直接沿用本切片结论。
- 发布时仍须把 helper/DLL 固定 SHA-256 注入不可由普通用户修改的配置，并由安装器落实受保护
  目录 ACL。本切片未改变该部署门，也未完成真卡会话 RAII、独立设备 PIN、32 字节 blob、首次
  写入恢复、持久计数器或 P1.7 拔卡处理。
- 提交：`7ab0237 [P1.6]add verifiable ViKey helper network isolation`

### 小切片 7：ViKey helper 供应商运行库与零能力 HID 兼容性门

#### 计划、需求与既有实现复核

- 重新核对 P1.6 出口、R-03、8.6、小切片 3 的供应商 DLL 身份门，以及小切片 4~6 的受限
  helper、启动期防替换和零 capability AppContainer 禁网门。本切片只验证供应商运行库和
  ViKeyPRO HID 能否在既有边界内工作；不注入 UserSN、API Key 或设备 PIN，不建立 SDK 卡会话，
  不发送 HID input/output/feature report，也不读取或写入卡内 blob。
- 对照 Windows 官方 AppContainer、HID device capability、custom capability 和设备安装安全
  文档。普通 HID 的受管应用访问要求 `humanInterfaceDevice` 设备 capability；Windows 10 1809
  起清单中的 VID/PID/Function 限制会被忽略。更精确的 custom capability 需要 capability 所有者、
  应用包/SCCD，并由驱动 INF 或驱动代码把 capability 绑定到设备接口；设备 ACL 同样属于驱动/
  部署安全描述符，不是无管理员、每次随机 profile 的运行时开关。参考：
  <https://learn.microsoft.com/en-us/windows/win32/secauthz/implementing-an-appcontainer>、
  <https://learn.microsoft.com/en-us/uwp/api/windows.devices.humaninterfacedevice>、
  <https://learn.microsoft.com/en-us/windows-hardware/drivers/devapps/hardware-support-app--hsa--steps-for-driver-developers>、
  <https://learn.microsoft.com/en-us/windows-hardware/drivers/install/creating-secure-device-installations>。

#### 实现

- IPC v1 增加 `vendor_runtime_probe` 与 `hid_access_probe` 两个固定操作；响应原保留字段改为仅承载
  非秘密 Win32/供应商状态的 `detail`。成功响应要求 `detail=0`，错误状态仍必须位于统一
  `CardError` 范围，字段不允许承载路径、设备标识、凭据或卡数据。
- helper client 在创建任何 AppContainer 前同时验证并锁定固定名 `ViKey.dll` 的来源摘要；随后
  复制到随机 profile，设置 Medium + No-Write-Up 强制完整性标签，再对暂存字节重算同一摘要并
  持锁到子进程结束。摘要不匹配在创建 profile/进程前失败；正常、启动失败、协议错误、超时和
  操作拒绝路径均先释放锁、删除暂存 DLL，再删除 profile，任何清理失败保持可见。
- AppContainer 文件虚拟化实测表明，子进程直接使用宿主 `GetAppContainerFolderPath` 绝对路径会
  被再次重定向并得到 `ERROR_PATH_NOT_FOUND`。启动环境因此只携带由
  `SHGetKnownFolderPath(FOLDERID_LocalAppData)` 取得的常规虚拟路径，宿主仍把工作目录和暂存文件
  绑定到随机 profile；子进程通过虚拟 `LOCALAPPDATA\\ViKey.dll` 访问同一文件。该路径不是继承
  用户环境变量，环境仍恰好只有 `LOCALAPPDATA/TEMP/TMP` 三项且不包含 PATH、代理或凭据。
- helper 在加载前先确认固定文件可读，再用 `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)`
  将依赖搜索锁到 System32，加载已验证暂存文件，并只解析、调用无卡参数的
  `VikeyGetLibraryVersion`。产品目标继续不链接 helper client 或供应商 SDK；供应商 DLL 不进入
  仓库，测试默认使用只导出同名版本函数的静态 CRT 无害夹具。
- 新增固定 ViKeyPRO HID probe，只枚举 HID class 中路径匹配 `VID_1987/PID_0331` 的接口；打开后
  还要用 `HidD_GetAttributes` 与 `HidP_GetCaps` 核验 Usage `FF00/0001`。probe 只申请并关闭
  `GENERIC_READ|GENERIC_WRITE` 句柄，不调用任何报告 I/O。硬件测试先要求宿主阳性对照成功，再
  运行相同 AppContainer helper；精确的 `ERROR_ACCESS_DENIED` 被记录为兼容门 skipped，其他设备
  错误仍失败，将来隔离句柄真正成功才可标记 passed。

#### 验证与安全复核

- 无害 `ViKey.dll` 夹具在普通桌面令牌下通过完整进程测试：来源/暂存双重摘要、启动期文件锁、
  固定导出调用、完整安全属性、禁网回归、错误摘要启动前拒绝、超时和 profile 清理均成立。
  官方 x64 `ViKey.dll` 的 SHA-256 复核为
  `735A2347F66F10C649C9B38ED0232B839812277C8E178F0C5084C676CF1DE48E`，在相同零网络
  AppContainer 内加载并完成版本查询；未关闭 Defender，也未调用任何卡 API。
- 取得明确授权后执行真实 HID 门：宿主对同一 ViKeyPRO 的读写句柄阳性对照成功，零 capability
  AppContainer 返回 Windows `ERROR_ACCESS_DENIED (5)`。测试确认拒绝路径的 profile 计数前后
  不变，并由 CTest 明确标记 skipped；该结果是未满足的门，不计作兼容通过。
- 最终 MSVC Debug 与 AddressSanitizer 全量 CTest 均为 **18 项通过、2 项在 Codex 受限父令牌下
  明确跳过**，无失败、无 ASan 报告；普通桌面令牌下供应商运行库进程测试单独通过，真实 HID
  测试单独得到上述精确 skipped。MSVC `/analyze` 首轮指出 vendor 摘要指针的不变量无法静态
  推导，补充显式非空条件后全量零诊断构建通过；Analyze CTest 同为 18 通过、2 跳过。
- 安全复核确认：没有把 `humanInterfaceDevice`、网络或其他 capability 加入 helper；没有删除
  AppContainer、低完整性、受限令牌、job、句柄白名单、映像限制、摘要或防替换门；没有向 IPC、
  环境或日志写入卡标识/凭据。`VikeyGetLibraryVersion` 运行在无秘密、无网络的临时进程内；真实
  HID 测试没有发送报告，卡片状态和存储未改变。

#### 剩余风险与下一步

- 零 capability AppContainer 已被实机证明确实无法打开 ViKeyPRO HID，因此 P1.6 不能直接进入
  真卡会话。下一小切片应先做“设备授权边界选择”：评估打包 Win32 helper +
  `humanInterfaceDevice` 后是否仍能逐项证明禁网和既有进程属性，并接受或拒绝其在 Windows 10
  1809+ 上无法按 VID/PID/Usage 收窄的攻击面。
- 更窄的优先方案是由硬件/驱动方提供 custom capability 或设备接口 ACL，但这要求驱动 INF/
  代码、包身份/SCCD、签名和部署协作；当前通用 HID 设备与随机临时 AppContainer 没有这条绑定。
  运行时修改设备 ACL、防火墙/WFP 全局规则或让产品提权都不属于本切片可接受的替代方案。
- 在设备授权门通过前继续禁止注入设备 PIN/API Key、建立真实 SDK 卡会话或访问 blob。之后仍需
  完成真卡会话 RAII、32 字节单 blob get/put、首次写入恢复、持久计数器和 P1.7 拔卡处理。

#### 决策：开发阶段采用明确联网的兼容模式

- 2026-08-25 已决定不让零 capability HID 不兼容继续阻塞 P1.6。下一小切片建立独立的
  **开发兼容模式**：ViKey helper 不使用 AppContainer，因而允许系统默认网络访问并可直接打开
  测试卡；这表示“没有出站隔离保证”，不表示产品或 SDK 会主动建立网络连接，也不新增网络代码。
- 开发兼容模式仍必须保留 helper 独立进程、非提权受限令牌、固定 SHA-256、启动期防替换、净化
  环境、显式继承句柄清单、单进程 job、现有进程缓解、版本化 nonce IPC、截止时间和失败关闭。
  该模式只使用当前开发卡、开发凭据和非生产 blob；不得承载量产根材料、生产 API Key/PIN 或被
  记作生产安全验收结果。
- 生产/交付阶段继续保留独立安全门：供应商代码接触真实根材料前必须重新证明出站隔离。候选为
  安装期防火墙/WFP、经威胁审计且与 HID 兼容的 capability，或驱动方提供的 custom capability/
  设备 ACL。若最终决定长期允许供应商 SDK 联网，必须显式降低安全定位并重新记录该决策，
  不能沿用小切片 6 的“可验证禁网”结论。
- 因当前小切片 7 尚未提交，以上决策先并入同一审阅边界；下一代码切片须在本切片提交决定后
  开始，不把历史零网络启动函数静默改义，应为开发兼容启动提供显式、不可误认的入口与测试。
- 提交：`da88f0f [P1.6]verify ViKey helper runtime compatibility`

### 小切片 8：ViKey helper 明确联网的开发兼容启动边界

#### 计划、需求与既有实现复核

- 重新核对 P1.6 出口、R-03、N-02、8.6，以及需求清单 2026-08-25 新增的开发/生产运行
  边界。本切片决策为：开发阶段不让零 capability AppContainer 的 HID 拒绝继续阻塞真实 SDK
  适配，允许 helper 使用系统默认网络与开发卡；生产材料进入前仍须重新通过可靠的出站隔离门。
- 复核小切片 3~7 后确认：原零网络入口的语义和测试必须原样保留，开发兼容模式应使用名称中
  明示 `development` 与 `network_access` 的独立入口，不能靠布尔参数或配置默认值静默降级。
  本切片只建立该启动边界并验证 SDK 版本查询、普通 Winsock 路径和 HID 句柄兼容性；不注入
  UserSN、API Key 或设备 PIN，不建立真实卡会话，不发送 HID report，也不读写 blob/计数器。
- 开发日志已统一采用无人物视角的客观记录口径；决策、授权、批准和交付动作只记录事实与状态，
  客户、供应商、产品用户等确有区分意义的主体仍按其真实身份表述。后续日志继续沿用该口径。

#### 实现

- 新增三个显式开发入口，分别用于 helper 空握手、供应商运行库版本查询和 HID 打开测试；原
  `probe_vikey_helper`、`probe_vikey_vendor_runtime`、`probe_vikey_hid_access` 继续强制进入零
  capability AppContainer，既有禁网合约没有改义。IPC 安全位为精确集合：开发模式只删除
  `app_container`、`no_capabilities`、`low_integrity` 三位，其余边界位不得缺失或多报。
- 开发进程用当前非提权用户创建并保持挂起，在任何 helper/供应商指令执行前打开**实际子进程
  令牌**，永久移除 `SeChangeNotifyPrivilege` 以外的全部特权，再逐项确认非提权、特权过滤、
  单进程 kill-on-close job、禁止子进程、DEP、ASLR、严格句柄、禁扩展点及远程/低标签映像
  阻断；全部成立后才恢复主线程。这里是“挂起后对子令牌做等价的最大特权过滤”，不是伪称
  `CreateRestrictedToken` 标记；测试同时证明过滤子令牌不会改变父测试进程的特权集合。
- 每次调用在可信 LocalAppData 下用 BCrypt CSPRNG 创建 128 位随机私有暂存目录，工作目录与
  `LOCALAPPDATA/TEMP/TMP` 全部固定到本次目录。实测只保留这三项时普通 Winsock provider 也会
  被系统拒绝，因此开发环境额外从 `GetWindowsDirectoryW` 注入同值的 `SystemRoot/WINDIR`；
  helper 自检环境必须恰好是隔离模式三项或开发模式五项，PATH、代理、用户配置和凭据仍不继承。
- 正式 helper/DLL 的固定盘普通文件、最终路径、大小与 SHA-256 身份门保持在所有临时资源创建
  之前；开发 SDK 仍复制为固定名 `ViKey.dll`，重新核对摘要并持有文件/目录锁到子进程结束，
  依赖搜索继续只允许 System32。正常、摘要错误、不可执行、协议错误与超时路径都会删除暂存
  DLL、Temp 和随机根目录；清理失败向上可见，不能被无声忽略。
- 显式继承句柄仍只有 stdin/stdout/stderr 三个固定管道；版本化 nonce IPC、截止时间、失败
  关闭和 helper 身份持锁保持不变。产品 `usb_vault_app.exe` 尚未接入这些开发 API，也没有新增
  网络客户端或供应商库链接。

#### 验证与安全复核

- MSVC Debug 与 `/analyze` 两套完整 CTest 均为 **20/20 通过**，其中进程测试要求宿主和开发
  helper 至少成功创建一条普通 TCP/UDP Winsock 路径；Winsock 初始化失败、地址族不可用或全部
  被拒绝都不能冒充联网阳性。探针只面向回环和 RFC 文档保留地址发送固定单 NUL 字节，不包含
  业务数据；它证明的是“没有运行时零网络隔离”，不声称已经连通公网或 SDK 会主动联网。
- 新增回归覆盖开发安全位精确匹配、父令牌不变、供应商来源/暂存双摘要、错误 helper/DLL 摘要
  在暂存前失败、不可执行制品的启动失败、150 ms 卡死终止，以及所有成功/失败路径的随机目录
  计数前后不变。原 AppContainer 的安全位、八条禁网路径、profile 清理和官方 DLL 隔离加载测试
  同时继续通过。
- 仓库外官方 x64 `ViKey.dll` 的 SHA-256 仍为
  `735A2347F66F10C649C9B38ED0232B839812277C8E178F0C5084C676CF1DE48E`；普通桌面下通过完整
  开发边界加载并只调用 `VikeyGetLibraryVersion`。真实 ViKeyPRO 测试先确认零 capability
  AppContainer 仍返回 `ERROR_ACCESS_DENIED`，再由开发模式成功打开/关闭同一设备的读写句柄；
  未发送任何 HID report，未调用卡 API，未读取凭据或 blob，卡状态未改变。
- AddressSanitizer 构建及 18 个普通测试通过；Codex 受限父令牌下两个跨进程测试按既有约束明确
  skipped。普通桌面下完全 ASan 插桩的两个跨进程父测试会在收到完整 IPC 前失败，而 Debug 与
  `/analyze` 的相同进程/HID 路径均通过；因此本切片只能记录“ASan 构建和普通逻辑已覆盖，强
  进程边界的 ASan 运行期未覆盖”，不能把它写成全量 ASan 通过。
- Debug 与 `/analyze` 两套 CTest 若被外部同时启动，会因共用
  `usb-vault-vikey-helper-*` profile 前缀而在临时计数窗口互相误报“残留”；并行试跑后只读枚举
  证实两类临时对象实际均为 0，随后串行复跑两套各 20/20 通过。这是测试夹具的并发限制，不是
  launcher 清理失败；当前 CI/本地验收须按构建串行运行该进程测试。
- `dumpbin /dependents` 复核确认产品 exe 不依赖 ViKey、`Ws2_32`、WinHTTP 或 WinINet；正式
  helper 只依赖 `HID.DLL`、`SETUPAPI.dll` 与 `KERNEL32.dll`，也未静态链接供应商或网络库。
  `Ws2_32` 只存在于两个专用网络测试夹具中。

#### 剩余风险与下一步

- 开发兼容模式明确**没有出站隔离保证**。若供应商 DLL 或其依赖被攻陷，它可以使用当前用户
  可用的普通网络路径；因此该入口只能接触开发卡、开发凭据和非生产 blob，不能承载量产根材料
  或生产 API Key/PIN，也不能作为 P1.6 的生产安全验收结果。
- 随机暂存目录与摘要/锁能阻止制品静默替换，但它仍位于当前用户可写的 LocalAppData，尚未由
  安装器落实受保护 ACL。当前无秘密的版本/HID 兼容测试可接受这一开发风险；真实卡会话接入时
  仍须确认只使用测试材料，并把安装目录 ACL、生产摘要注入和出站隔离保留为交付门。
- 下一独立小切片可在这个明确的开发边界内实现 ViKey 卡会话 RAII 和错误映射，随后再接 32 字节
  单 blob get/put、首次写入恢复与持久计数器。生产阶段另行落实防火墙/WFP、兼容 capability、
  驱动 custom capability/设备 ACL 或经审计的等价方案后，才能让真实根材料进入 helper。
- 提交：`905cb64 [P1.6]add network-enabled ViKey development boundary`

### 小切片 9：ViKey 开发卡会话 RAII 与错误映射核心

#### 计划、需求与既有实现复核

- 重新核对 P1.6 出口、R-03、`ICardStore` 可替换适配边界及现有开发 helper 约束。本切片只实现
  可由 helper 后续调用的会话核心与伪 SDK 测试，不修改 IPC，不加载真实 `ViKey.dll`，不访问
  安全卡，也不读取或注入实际 UserSN、API Key、设备 PIN、blob 或计数器。
- 官方头文件与示例确认调用顺序为 `VikeySetUserSN` → `VikeyFindEx` → `VikeySetApiKey` →
  `VikeyUserLogin`，退出时执行 `VikeyLogoff` 与 `VikeyUninitialization`；用户 PIN 明确为 8 个
  字母或数字。官方示例一致使用 32 个十六进制字符的 UserSN 与 128 个十六进制字符的 API Key，
  但头文件没有单独声明这两个长度，因此把精确长度保留为 ViKey 私有适配约束，不上移到
  `ICardStore`。
- `VikeyGetUserPassWordAttempt` 注释把首个输出描述为“当前错误尝试次数”，但激活后、从未尝试
  PIN 的实体开发卡曾实测返回 `200/200`，与该措辞矛盾。实现按运行证据将首值解释为当前剩余
  预算：最大值为 0 表示不限次；最大值非零且剩余为 0 时在登录前失败关闭；剩余大于最大值视为
  设备状态异常。真实会话接线时仍须用测试 PIN 做一次受控前后读数复核。

#### 实现

- 新增不可拷贝、不可移动的 `VikeySession`。`open` 只接受注入的私有 SDK 函数表和三块已锁页的
  `SecureBuffer`；在任何厂商调用前检查 UserSN/API Key/PIN 的固定长度和 ASCII 字符集，并检查
  会话所需函数指针完整性。产品 executable 仍不链接 ViKey SDK 适配库。
- 会话严格要求只发现一张卡并固定使用索引 0；API Key 配置后先读取 PIN 尝试预算，预算未锁定
  才执行用户登录，登录后再读取权限级别并只接受用户级别 1。SDK 的 API-key 计算错误状态
  `0x80000026/27` 新增到私有 ABI，并统一映射为通用 `invalid_configuration`，厂商状态不会越过
  适配边界。
- 三项凭据不会转换为 `std::string` 或可重分配容器。每次厂商 C ABI 调用前分配一块恰好多一个
  字节的锁页 `SecureBuffer`，复制凭据并补 NUL；调用结束立即清零、解锁和释放。测试夹具主动
  修改 ABI 参数后，原始三块凭据仍保持逐字节不变。
- 初始化失败由待定会话守卫负责清理：真正进入首个厂商调用后始终执行
  `VikeyUninitialization`，只有登录成功后才先执行 `VikeyLogoff`。成功会话由 RAII 析构持有同一
  顺序；显式 `close` 幂等，并在注销失败时仍继续反初始化、返回首个通用清理错误。

#### 验证与安全复核

- 新增伪 SDK 单元测试，覆盖成功调用顺序、显式关闭、析构关闭、二次关闭、凭据临时副本隔离、
  凭据形状与函数表的调用前拒绝，以及 UserSN、发现零卡/多卡、API Key、尝试次数查询、锁定、
  不限次、尝试次数矛盾、PIN 登录、权限查询/级别和清理错误等失败路径。所有路径都逐项断言
  是否应注销及必须反初始化的精确次序。
- MSVC Debug 与 `/analyze` 构建最终均为 **21/21 通过**：受限沙箱内先得到 19 项通过、2 项因
  父令牌已受限而明确跳过，随后在普通桌面令牌下串行补跑同一构建的两项 helper 跨进程测试并
  全部通过。`/analyze` 零项目诊断。AddressSanitizer 构建为 19 项普通测试通过、2 项按既有强
  进程运行时约束跳过；新增 `vikey_session_tests` 与 `vikey_module_tests` 均在 ASan 下通过，
  未报告内存错误。
- 安全复核确认凭据只存在于锁页固定缓冲及短期锁页 C ABI 副本，不进入异常文本、日志、环境、
  IPC 或普通容器；函数表与凭据错误在首个厂商调用前失败，SDK 清理义务只在实际进入 SDK 后
  建立。本切片未加载供应商 DLL、未访问真实卡、未尝试 PIN，也未改变卡片或 blob 状态。

#### 剩余风险与下一步

- 当前只完成进程内会话核心，尚未把凭据安全送入开发 helper，也未在 helper 内装载正式函数表
  或建立真实会话；因此不能记作真卡认证通过。下一小切片应设计固定长度、版本化、单次消费的
  凭据 IPC，在开发 helper 内落入 `SecureBuffer` 后立即清除传输缓冲，再接入本会话核心。
- `open` 失败时以原始业务错误为主，栈展开中的注销/反初始化错误只能尽力清理而不能覆盖主错误；
  后续 helper 仍须在请求结束后退出，避免失败后的供应商全局状态被复用。正常关闭路径会显式
  返回清理错误。
- 精确 UserSN/API Key 长度来自官方示例的一致行为而非头文件常量，PIN 尝试首值语义来自既有
  实体卡运行证据而非可靠文档。下一真实会话切片必须在只使用测试材料的前提下复核这两项，并
  保留开发联网风险、生产出站隔离、受保护安装目录 ACL 与固定发布摘要作为独立交付门。
- 提交：`7de1701 [P1.6]add ViKey development session lifecycle`

### 小切片 10：开发凭据受控 IPC 与 helper 会话接线

#### 计划、需求与既有实现复核

- 重新核对 P1.6 真卡出口、R-03、`ICardStore` 可替换边界、2026-08-25 开发/生产运行决策，
  并复查版本化 nonce IPC、开发 helper 进程边界、DLL 双重摘要/防替换门以及小切片 9 的
  `VikeySession`。本切片只打通开发凭据传输与 helper 内一次真实函数表会话调用，不实现
  `ICardStore`、blob 读写或计数器，不引入档位状态。
- 继续执行已确认的阶段边界：开发 helper 允许系统默认网络访问，但必须保留非提权、子进程
  特权过滤、显式句柄清单、单进程 job、进程缓解、净化环境、固定摘要、启动期防替换、版本化
  IPC 与超时终止。生产材料进入前仍须重新通过可验证出站隔离门。
- 本切片验证只使用固定 ASCII 的无害 `ViKey.dll` fixture。未读取 Windows 凭据管理器，未取得
  实际 UserSN、开发 API Key 或测试 PIN，未调用官方 DLL、未登录实体卡，也未读写卡数据。

#### 实现

- helper IPC 协议由版本 1 升至版本 2；32 字节请求头大小保持不变，原保留字段改为精确
  `payload_bytes`。新增的开发会话操作只接受固定 200 字节请求体：32 字节 DLL SHA-256、
  32 字节 UserSN、128 字节 API Key 和 8 字节用户 PIN。旧版/未知操作、缺失/多余载荷和普通
  操作携带载荷均失败关闭。
- 父端入口只接受三块精确长度的 `SecureBuffer`。凭据不会组装进 `std::string`、`std::vector`
  或普通线协议结构，而是从调用方锁页缓冲依次直接写入匿名继承管道；尺寸错误在制品验证、
  暂存或进程创建前拒绝。helper 恢复执行前仍完成摘要/防替换、令牌、job、进程缓解和句柄边界
  验证，只有恢复后才发送请求。
- helper 只在明确识别为联网开发环境时接受开发会话操作，并将三项凭据直接读入三块固定长度
  的锁页 `SecureBuffer`。随后重新验证工作目录内固定名 `ViKey.dll` 的摘要并持有文件锁，使用
  `VikeyModule` 解析完整 15 项函数表，调用 `VikeySession::open`，显式注销与反初始化；请求作用域
  在响应写回前结束并擦除全部 helper 侧凭据。每个 helper 进程仍只处理一个请求并退出，失败后
  不复用供应商全局状态。
- helper 目标直接编译既有制品验证、模块、会话和安全缓冲实现，保持静态 CRT 边界；产品主程序
  不链接 helper client 或 ViKey SDK。网络 fixture 同步接入安全缓冲实现，并增加分阶段退出码；
  进程测试为每个场景补充名称，helper 无完整响应时现在报告非秘密退出码，便于区分协议与启动
  故障。

#### 验证与安全复核

- MSVC Debug 全量构建通过，CTest **21/21** 通过：受限沙箱内 19 项执行通过、2 项 helper
  跨进程测试因父令牌已受限按既有规则跳过；普通桌面令牌下补跑
  `vikey_helper_protocol_tests`、`vikey_helper_process_tests` 和
  `vikey_helper_hid_compatibility_tests`，**3/3** 通过。
- 协议测试覆盖版本 2、操作/载荷精确匹配、200 字节请求体往返、错误尺寸拒绝及调用方凭据保持；
  进程测试覆盖开发会话的完整 SDK 顺序、精确开发安全属性、调用方锁页缓冲保持、错误凭据的
  通用错误映射、错误尺寸的暂存前拒绝，以及成功/失败后的暂存目录清理。扩展后的无害 DLL
  fixture 强制验证 `SetUserSN → FindEx → SetApiKey → 尝试预算 → UserLogin → GetLevel →
  Logoff → Uninitialization` 顺序。
- MSVC `/analyze` 定向构建通过且无项目诊断；MSVC AddressSanitizer 下新增协议测试通过。
  `git diff --check` 通过。PE 依赖复核确认产品仍不依赖 ViKey 或网络库；helper 只静态导入
  `bcrypt/HID/SetupAPI/Kernel32`，`ViKey.dll` 仍仅在 helper 内经固定路径和摘要门动态装载。
- 安全复核确认三项凭据不进入环境、命令行、异常文本、日志或普通可重分配容器，helper 返回值
  只保留通用 `CardError`，不回传厂商状态或凭据内容。匿名管道只通过显式句柄清单交给已验证的
  单一 helper，nonce、定长载荷、超时终止和进程退出限制重放、尾随数据及长生命周期暴露。

#### 剩余风险与下一步

- 明文凭据传输时会短暂存在于 Windows 内核管道缓冲，该缓冲不能由应用显式锁页或清零；应用
  自身不建立普通中间副本，helper 接收后立即进入锁页缓冲。结合开发 helper 可联网的既定风险，
  此入口只允许开发卡和开发凭据，不能承载量产根材料或生产凭据。
- 尚未使用实际开发凭据调用官方 SDK 或实体卡。本切片只证明受控 IPC、真实装载/函数解析代码和
  会话生命周期可由无害 fixture 贯通，不能记作实体卡认证通过。下一小切片需在明确使用测试 PIN
  的前提下建立一次实体开发卡会话，并在登录前后受控复核 PIN 尝试首值确为剩余预算；任何错误
  尝试都必须停止，不能自动重试。
- `ICardStore` 的 32 字节单 blob get/put、首次写入恢复、拔卡中断和持久计数器仍未开始。生产
  出站隔离、受保护安装目录 ACL、固定发布摘要注入和量产凭据管理仍为独立交付门。
- 提交：`ac41e8e [P1.6]wire development credentials into ViKey helper session`

### 小切片 11：实体开发卡会话与 PIN 尝试预算复核

#### 计划、需求与既有实现复核

- 重新核对 P1.6 真卡出口、R-03、开发联网兼容边界、小切片 9 的会话生命周期和小切片 10 的
  固定长度凭据 IPC。本切片只建立并关闭一次实体开发卡用户会话，读取登录前后 PIN 尝试预算；
  不实现或调用 blob/计数器接口，不注入生产材料，不改变档位状态。
- 官方管理工具和 SDK 资料确认当前开发卡 UserSN 为 32 个十六进制字符、用户 PIN 为 8 个
  字母或数字。开发阶段决定把 UserSN、API Key 和用户 PIN 保存在当前主机用户的 Windows
  凭据管理器；项目仓库、Git 历史、开发日志、helper 环境和运行探针命令行均不保存凭据值。
  对应目标为
  `usb-vault-vikey-dev-user-sn`、`usb-vault-vikey-dev-api-key` 和
  `usb-vault-vikey-dev-user-pin`。

#### 实现

- `VikeySession` 保留登录前读取的当前/最大 PIN 尝试预算并提供只读访问。helper 在用户权限级别
  验证成功后再次调用 `VikeyGetUserPassWordAttempt`，校验当前值不超过最大值，随后显式注销和
  反初始化；任一步失败仍由会话析构执行失败关闭清理。
- helper IPC 协议由版本 2 升至版本 3。成功的开发会话响应在既有非秘密 `detail` 字段中打包
  登录前后四个 8 位尝试次数；其他操作成功时仍强制 `detail == 0`。父端按请求操作验证响应，
  独立拒绝当前值大于最大值的矛盾观测，并返回专用开发会话结果类型。
- 新增默认构建不包含的 `usb_vault_vikey_development_session_probe` 手工验收目标。探针从固定
  Windows 凭据目标读取三项开发凭据，兼容原始 ASCII 与 UTF-16 blob，复制到锁页
  `SecureBuffer` 后在 `CredFree` 前清零系统返回的 blob；凭据不转换为普通字符串。helper 与
  官方 DLL 路径及 SHA-256 仍由非秘密命令行参数明确传入并执行原有身份门。

#### 验证与安全复核

- 无害供应商 fixture 扩展为强制验证
  `SetUserSN → FindEx → SetApiKey → 登录前预算 → UserLogin → GetLevel → 登录后预算 →`
  `Logoff → Uninitialization`。协议测试覆盖四字段打包往返、非会话成功响应禁止非零 detail、
  会话观测上限校验和 nonce；进程测试断言开发安全属性及 `200/200 → 200/200` 观测。
- MSVC Debug 全量构建通过；受限沙箱内 CTest 为 19 项执行通过、2 项跨进程测试按既有父令牌
  约束跳过，普通桌面令牌下三项 helper 测试为 **3/3 通过**。MSVC `/analyze` 聚焦构建完成且
  会话/协议测试 **2/2 通过**；AddressSanitizer 下同两项测试 **2/2 通过**，未报告内存错误。
- 官方 x64 `ViKey.dll` SHA-256 再次核对为
  `735A2347F66F10C649C9B38ED0232B839812277C8E178F0C5084C676CF1DE48E`。在普通用户、非提权、
  允许联网的开发 helper 中使用凭据管理器内开发凭据建立实体 ViKeyPRO 用户会话成功；登录前
  和登录后均返回 **200/200**，确认首值表示剩余尝试预算，成功登录不消耗预算，随后注销和
  反初始化成功。测试未读取或写入用户区、管理员区、blob 或计数器。
- 安全复核确认凭据值未进入仓库或构建产物，IPC 响应只新增四个非秘密计数值；失败响应仍只
  暴露通用错误域。Windows 凭据管理器只能防止普通落盘明文，当前用户上下文中的其他软件仍可
  读取这些开发凭据，因此三项值只能用于开发卡和非生产材料，不能升级为生产凭据管理方案。

#### 剩余风险与下一步

- 实体认证与 PIN 预算语义已验证，下一独立小切片可接入固定 32 字节单 blob 的受控写入、读回
  和恢复测试。首次真卡写入必须只使用可丢弃的非生产随机材料，并先明确地址范围、原值备份及
  中途拔卡后的恢复判据。
- 开发 helper 仍允许普通网络访问；供应商 DLL、开发凭据和当前用户网络能力属于同一信任域。
  生产根材料进入前仍须落实可验证出站隔离、受保护安装目录 ACL、固定发布摘要和生产凭据注入。
- 提交：`4774f7c [P1.6]verify real ViKey development session`

### 小切片 12：ViKey 固定 32 字节用户区原语

#### 计划、需求与既有实现复核

- 重新核对 P1.6 真卡出口、R-03、N-02 的 `ICardStore` 单 blob get/put 边界及开发联网兼容
  决策。本切片只在已认证的 ViKey 私有会话内实现固定用户区读写原语，不修改公共
  `ICardStore`，不增加档位状态、计数器或真实卡事务。
- 官方 SDK 头文件确认用户区接口为 `VikeyGetUserDataSize`、`VikeyReadData` 和
  `VikeyWriteData`；官方示例一致使用地址 0、长度 32。实体开发卡此前已只读确认用户区容量为
  8192 字节。固定地址和长度保留在 ViKey 私有适配层，不上移为通用卡接口约束。
- 既有 `VikeyModule` 已解析三项用户区函数，正式 helper 也已具备锁页安全缓冲实现。本切片的
  验证只使用进程内伪 SDK 和本地无害 DLL fixture，不加载官方 SDK、不读取凭据、不连接或改写
  实体卡。

#### 实现

- `VikeySession` 新增固定 32 字节、地址 0 的 `read_user_blob` 与 `write_user_blob`。每次操作先
  要求会话仍处于认证状态，再查询用户区容量；容量不足时在读写前返回
  `invalid_configuration`，SDK 查询和数据操作错误继续映射到通用 `CardError`。
- 读取结果直接落入锁页 `SecureBuffer`，异常路径由缓冲析构清零、解锁并释放。写入只接受精确
  32 字节的锁页输入，并在调用供应商可变指针 ABI 前建立短期锁页副本，避免供应商修改逃逸到
  调用方缓冲；副本在调用结束或异常展开时自动擦除。
- 会话函数表完整性检查同步要求容量、读取和写入函数存在。缺失函数、错误输入长度和已关闭
  会话均在进入对应供应商调用前失败。

#### 验证与安全复核

- 扩展伪 SDK 单元测试，覆盖含零字节的精确 32 字节读取、固定地址/长度、写入内容、供应商
  修改参数时的调用方隔离、容量查询错误、容量不足、读写错误映射、短输入、关闭后读取以及
  缺失数据函数。成功与失败路径均断言精确调用边界和顺序。
- MSVC Debug 全量构建通过。受限沙箱内 CTest 为 19 项执行通过、2 项按既有父令牌约束跳过；
  普通桌面令牌下补跑三项 helper 测试为 **3/3 通过**。MSVC `/analyze` 聚焦构建与
  `vikey_session_tests` 通过且无新增项目诊断；AddressSanitizer 下同一测试通过，未报告内存
  错误。
- 安全复核确认固定范围不能由调用方扩大，所有数据缓冲均使用锁页并自动擦除，输入形状、会话
  状态和卡容量在数据调用前验证，供应商状态不会越过适配边界。本切片未读取 Windows 凭据管理
  器、未加载官方 DLL、未连接实体卡，也未读取或写入真实用户区。

#### 剩余风险与下一步

- 当前原语只存在于认证会话内部，尚未接入 helper 单次事务或公共 `ICardStore`，因此不能记作
  P1.6 的实体 blob 写入/读回出口完成。下一小切片应先以无害 fixture 实现并验证 helper 事务：
  备份原 32 字节、写入非生产测试值、读回比对、恢复原值、再次读回确认恢复。
- 真卡验收必须在事务夹具与中断错误路径通过后单独执行；测试前先读取并锁页保存原值，任何
  写入失败或读回不符都应尽力恢复，只有恢复后再次读回完全一致才算未改变卡片最终状态。断电
  或拔卡仍可能使自动恢复失败，因此真卡执行前需要再次明确这一残余风险。
- 开发 helper 仍允许普通网络访问，真实读写只能使用开发卡和非生产 blob。生产根材料进入前的
  可验证出站隔离、安装目录 ACL、发布摘要及生产凭据管理仍为独立交付门。
- 提交：`aea344d [P1.6]add fixed ViKey user blob operations`

### 小切片 13：helper 可恢复 blob 往返事务

#### 计划、需求与既有实现复核

- 重新核对 P1.6 真卡出口、R-03、N-02 单 blob 边界、小切片 12 的固定用户区原语及开发联网
  约束。本切片只建立开发 helper 内的单次可恢复事务和无害夹具验证，不调用正式 SDK、不读取
  Windows 凭据管理器、不连接实体卡，也不修改公共 `ICardStore` 或档位状态。
- 事务恢复责任确定为：原值备份成功后，测试写入即使报告失败也可能已发生部分修改，因此之后
  的测试写入错误、测试读回错误或内容不一致都必须进入恢复；只有原值写回成功且再次读回逐字节
  一致，才可报告恢复已验证。恢复写入或恢复读回无法证明时统一返回 `device_failure`，不保留可能
  产生“卡片未改变”误解的较轻错误。
- helper 仍沿用非提权、特权过滤、显式句柄清单、单进程 job、进程缓解、净化环境、制品摘要与
  防替换、nonce、超时终止和单请求退出边界。新增事务只允许显式联网的开发边界，不开放给零
  capability AppContainer 或生产材料。

#### 实现

- 新增开发专用 `verify_development_blob_round_trip`：先把原 32 字节读入锁页 `SecureBuffer`，再
  写入测试值并读回验证，随后无条件写回原值并再次读回验证。测试失败在恢复成功后重新抛出原
  通用错误；恢复无法验证时提升为 `device_failure`。三次读回、测试写入参数和原值写回参数均由
  锁页缓冲承载并在作用域退出时清零，内容比较遍历完整固定缓冲。
- helper IPC 协议由版本 3 升至版本 4，新增开发 blob 往返操作及精确 232 字节载荷：32 字节 DLL
  摘要、32 字节 UserSN、128 字节 API Key、8 字节用户 PIN 和 32 字节测试 blob。新请求体中的
  凭据与测试 blob 均直接进入固定大小锁页缓冲；错误操作、错误载荷长度和短测试 blob 在启动或
  数据调用前失败关闭。
- helper 新操作重新验证并锁定暂存 DLL、建立一次认证会话、执行完整往返事务、显式注销与反初
  始化后退出。恢复未验证的 `device_failure` 优先于会话关闭错误；其他事务错误在恢复成功后仍需
  完成显式关闭，关闭错误保持可观察。父端只返回安全属性和通用错误，不回传原 blob、测试 blob
  或任何逐步状态。
- 无害供应商 DLL fixture 增加严格状态机，强制验证
  `备份读 → 测试写 → 测试读 → 原值写回 → 恢复读` 的固定地址、固定长度、内容和顺序；两个
  网络夹具补充分阶段响应校验，便于区分协议响应与网络探针故障。

#### 验证与安全复核

- 伪 SDK 单元测试覆盖成功后恢复原值、测试 blob 调用方保持、首次写入已改变数据但返回通信错
  误、测试读回失败、测试内容不一致、恢复写入失败、恢复验证失败、备份读取失败和短测试值。
  测试逐项断言读写次数、最终伪卡内容及错误映射；备份失败路径确认不会尝试无原值写回。
- 协议测试覆盖版本 4、两种开发载荷的精确区分、232 字节请求体往返、调用方测试值保持、短值
  拒绝和成功响应禁止携带 blob 明细。无害 DLL 跨进程测试验证完整 helper 安全边界、事务顺序、
  调用方缓冲保持及暂存目录清理。
- MSVC Debug 全量构建通过；受限沙箱内 CTest 为 19 项执行通过、2 项按既有父令牌约束跳过，
  普通桌面令牌下三项 helper 测试为 **3/3 通过**。MSVC `/analyze` 聚焦构建完成且会话/协议测试
  **2/2 通过**，无新增项目诊断；AddressSanitizer 下同两项测试 **2/2 通过**，未报告内存错误。
- 安全复核确认原 blob、测试 blob 和全部读回值不会进入日志、异常文本、响应、普通容器或持久
  文件；恢复失败不会被原始通信错误遮蔽。协议仍会让明文数据短暂存在于 Windows 内核匿名管道
  缓冲，因此此入口只适用于已接受普通网络能力的开发模式和非生产材料。

#### 剩余风险与下一步

- 进程被强制终止、系统断电或卡片在测试写入后拔出时，helper 无法保证执行恢复；软件事务只能
  对进程仍运行且设备仍可通信的路径提供尽力恢复和读回证明。实体卡验收前仍须明确接受这项残
  余风险，并确保测试值不承载生产材料。
- 当前只证明伪 SDK 失败路径和无害 DLL 跨进程链，尚未调用官方 DLL 执行实体用户区往返，不能
  记作 P1.6 真卡 blob 出口完成。下一小切片应新增默认构建不包含的手工验收入口，从凭据管理器
  读取开发凭据、在锁页缓冲生成一次非生产测试值，并复用本事务；入口实现与测试完成后再单独
  决定是否对实体开发卡执行。
- 公共 `ICardStore` 的 ViKey 适配、首次正式 put 语义、持久计数器以及 P1.7 拔卡处理仍未开始。
  生产材料进入前的可验证出站隔离、安装目录 ACL、发布摘要和生产凭据管理仍为独立交付门。
- 提交：`27b7abe [P1.6]add recoverable ViKey blob round trip`

### 小切片 14：实体 blob 往返手工验收入口

#### 计划、需求与既有实现复核

- 重新核对 P1.6 真卡出口、R-03、N-02 单 blob 边界、开发/生产运行决策及小切片 13 的可恢复
  helper 事务。按既有评审结论，本切片只建立默认构建不包含的手工验收入口，不执行实体卡
  写入；真卡执行继续作为下一独立决策点。
- 复查既有开发会话 probe、Windows 凭据管理器读取、helper client v4 blob 操作及跨进程事务
  测试。三项开发凭据仍只从固定凭据目标读取，helper/DLL 路径和 SHA-256 仍由非秘密命令行参数
  提供；本切片不修改 IPC、helper、SDK 会话、公共 `ICardStore` 或卡上地址布局。

#### 实现

- 新增 `usb_vault_vikey_development_blob_round_trip_probe`，以 `EXCLUDE_FROM_ALL` 独立目标复用
  既有凭据读取和 v4 helper client。默认产品/测试构建的 `all` 目标不依赖该可执行文件，原只读
  开发会话 probe 保持原行为。
- 写卡入口要求精确的 `--i-accept-card-interruption-risk` 参数；缺失或错误时在路径摘要解析、
  随机数生成、凭据读取、helper 启动和设备访问之前退出。该参数只表示调用者明确接受测试写入
  至恢复之间发生强制终止、断电或拔卡时可能无法恢复的风险，不会被解释为生产授权。
- 通过系统首选 CNG 随机源把一次性 32 字节非生产测试值直接写入 `SecureBuffer`。随机数实现与
  Windows 凭据读取拆分为不同翻译单元，测试值不经过普通字符串、向量、日志或命令行；调用
  完成或异常退出时由锁页缓冲清零并释放。
- 成功输出只说明 helper 已完成原值恢复并再次验证，不打印测试值、原值、凭据、厂商状态或
  事务中间步骤。失败继续只经既有通用错误边界报告。

#### 验证与安全复核

- MSVC Debug `/W4 /WX` 全量构建通过，CTest **22/22**：20 项执行通过，2 项 helper 跨进程
  测试因当前父令牌已受限按既有规则跳过。新增测试两次直接生成锁页随机 blob，验证固定 32
  字节形状、有效分配及独立调用结果不相同；不读取凭据、不启动 helper、不访问设备。
- 新旧两个手工 probe 在 Debug、MSVC `/analyze` 和 AddressSanitizer 配置下均构建通过；新增
  随机 blob 测试在 `/analyze` 与 ASan 下分别 **1/1** 通过。防误触运行验证确认无确认参数时
  返回失败和用法信息，退出码为 1。
- `git diff --check` 通过。Debug 二进制导入复核只发现预期的 Windows/CNG/凭据及既有 helper
  client 系统依赖，不静态导入 `ViKey.dll` 或网络客户端库；Ninja `all` 依赖图确认手工 blob
  probe 不进入默认构建。
- 安全复核未发现测试值或凭据进入普通可重分配容器、环境、命令行、日志或持久文件。官方 SDK、
  Windows 凭据和实体卡在本切片均未被读取或调用；新增自动测试不会产生卡片副作用。

#### 剩余风险与下一步

- 软件恢复保证仍只覆盖 helper 存活且卡片持续可通信的路径；强制终止、系统断电或拔卡发生在
  测试写入与恢复之间时，实体卡可能保留测试值或部分写入。本入口的显式确认只能防误触，不能
  消除此风险。
- 手工入口已经具备执行条件，但尚未用官方 DLL、开发凭据和实体开发卡运行，因此 P1.6 的真卡
  blob 写入/读回出口仍未完成。评审并提交本切片后，应由用户单独决定是否在供电稳定、卡片
  固定且接受中断窗口风险的前提下执行一次实体往返。
- 公共 `ICardStore` ViKey 适配、首次正式 put 语义、持久计数器、P1.7 拔卡处理以及生产出站
  隔离/安装 ACL/发布摘要/生产凭据管理仍未开始，不属于本切片。
- 提交：`866a6e4 [P1.6]add ViKey blob acceptance probe`

### 小切片 15：实体开发卡 blob 往返验收

#### 计划、需求与既有实现复核

- 小切片 14 已按批准创建本地提交 `866a6e4 [P1.6]add ViKey blob acceptance probe`。
  本切片不修改产品代码，只按独立风险决策执行一次实体开发卡验收并记录结果。
- 执行前重新核对 P1.6 真卡出口、开发/生产运行边界、小切片 13 的恢复判据及小切片 14 的防误
  触入口。用户已明确理解并接受测试写入至恢复之间发生断电、拔卡或强制终止时可能无法恢复卡内
  原 32 字节的残余风险；本次目标仅为开发卡和非生产用户区，不承载真实根材料。

#### 执行前只读核对

- 工作区在 `866a6e4` 后干净。已连接设备枚举为 `HID/USB VID_1987&PID_0331`，两个设备节点均为
  `Started`；未发现目标混淆。
- Debug helper SHA-256 为
  `581BEC042F98EEFF173841B3D15BBBA52A8207ABE0222A66758AED5F05D7DC22`。仓库外官方 x64
  `ViKey.dll` SHA-256 为既有白名单
  `735A2347F66F10C649C9B38ED0232B839812277C8E178F0C5084C676CF1DE48E`。手工入口和 helper
  均使用上述精确摘要执行原有制品身份门。

#### 实体执行、验证与安全复核

- 在普通用户、非提权、允许联网的开发 helper 边界中，从固定 Windows 凭据目标读取开发
  UserSN、API Key 和测试 PIN；命令行只携带非秘密路径、摘要及显式风险确认参数。执行一次且仅
  一次实体 blob 往返，程序返回：测试写入和读回成功，原值写回成功，恢复后 32 字节逐字节验证
  成功；进程退出码为 0。
- 往返完成后另启一次只读开发会话复核，登录前后 PIN 尝试预算均为 **200/200**，注销和反初始
  化成功，确认卡片仍可正常重新认证；该复核不再读取或写入用户 blob。
- helper 的恢复成功响应不包含原 blob、测试 blob 或中间状态；控制台仅输出恢复已验证和非秘密
  PIN 预算。凭据与 blob 未写入仓库、开发日志、环境、命令行或持久文件，测试随机值和全部
  helper 缓冲在作用域退出时清零。本次未观察到断电、拔卡、超时、写入错误或恢复失败。

#### 结论、剩余风险与下一步

- P1.6 的实体 ViKeyPRO 固定地址 32 字节原语、helper 事务及真实写入/读回/恢复链已经获得硬件
  证据；原用户区最终状态已由同一会话再次读回并证明与测试前备份一致。
- 这次成功不改变中断风险：未来任何写操作若在写入与恢复/确认之间遭遇断电、拔卡或强制终止，
  仍不能由软件单 blob 事务保证原子恢复。生产卡不得运行该破坏性验收入口。
- P1.6 总出口尚未完成：公共 `ICardStore` 仍未接入 ViKey 适配，持久计数器尚未实测，生产出站
  隔离/安装 ACL/发布摘要/生产凭据管理仍是交付门。下一最小代码切片应先审计并设计正式
  `ICardStore` get/put 与开发 helper IPC 的边界，避免把 ViKey 私有凭据、地址或错误域上移到
  公共接口；计数器继续作为之后的独立切片。
- 提交：`cac2d45 [P1.6]record real ViKey blob acceptance`

### 小切片 16：正式 ViKey `ICardStore` 适配边界

#### 计划、需求与既有实现复核

- 小切片 15 已按批准创建本地提交
  `cac2d45 [P1.6]record real ViKey blob acceptance`。本切片重新核对 P1.6、
  R-03、N-02、公共 `ICardStore`、P1.5 固定桩、产品装配及 helper IPC v4；范围仅为建立正式
  ViKey 卡存储适配边界，不同时升级 IPC、读取凭据、加载官方 DLL、访问实体卡或替换产品桩。
- 公共接口继续只表达单 blob 的 `get_blob`、`put_blob` 和能力位。ViKey 的凭据、DLL 路径、
  用户区地址、供应商错误与 helper 传输细节不得进入公共接口；实际 helper 后端留作下一独立
  切片，以便单独审阅明文响应和凭据生命周期。

#### 实现

- 新增独立静态库 `usb_vault_vikey_card_store` 及正式 `VikeyCardStore`，通过私有
  `IVikeyBlobBackend` 注入传输实现。适配器只保存后端引用，不保存 blob、凭据、路径、地址或
  供应商状态，也不直接依赖或加载厂商 DLL。
- `get_blob` 在返回应用层前强制验证后端结果恰为 32 字节；任何短读或超长结果统一失败关闭为
  通用 `device_failure`。`put_blob` 在触达后端前强制验证调用方输入恰为 32 字节，错误形状返回
  `invalid_configuration`；现阶段不宣称尚未实测的硬件能力位。
- 编译期断言 ViKey 固定 blob 长度与 KDF v1 卡秘密长度一致。后端返回和调用方输入均保持
  `SecureBuffer` 类型，不引入普通可重分配字节容器、日志、序列化或本地持久化。

#### 验证与安全复核

- 新增伪后端单元测试，覆盖二进制数据原样返回、写入不改变调用方缓冲、能力位为空、31/33
  字节写入在后端调用前拒绝、31/33 字节后端结果失败关闭，以及通用读取/写入错误不被适配器
  改写。测试伪后端不保存传入 blob。
- MSVC Debug `/W4 /WX` 全量测试 **23/23**：21 项执行通过，2 项 helper 跨进程测试按既有
  受限父进程令牌约束跳过。新测试在 MSVC `/analyze` 与 AddressSanitizer 下分别 **1/1**
  通过，无新增项目诊断或内存错误。初次目标构建暴露库的公共头依赖未传播；将核心接口依赖改为
  `PUBLIC` 后修复，随后三套配置全部通过。
- `git diff --check` 通过。装配复核确认 `tools/app/main.cpp` 仍实例化 `FixedBlobCardStore`，新库
  仅由新单测链接；现有产品可执行文件导入表没有 ViKey 项。本切片未读取 Windows 凭据管理器、
  未加载官方 SDK、未启动 helper、未连接或改写实体卡。

#### 剩余风险与下一步

- 当前只有正式应用侧适配边界，`IVikeyBlobBackend` 尚无 helper 实现，因此产品仍使用 P1.5
  固定桩，不能把正式 `ICardStore` 读写链记为完成。下一最小切片应审计并实现 IPC v5 的固定
  32 字节 get/put 操作、父端锁页响应承载、凭据提供者及最短生命周期，同时保留 helper 单请求
  退出和通用错误边界。
- 下一切片必须专项检查秘密 blob 在匿名管道内核缓冲和父进程响应缓冲中的短期副本、异常/超时
  清零、错误响应禁止携带 blob、凭据不得缓存，以及 put 失败时“可能已部分写入”的语义。首次
  正式 put、持久计数器、P1.7 拔卡处理和生产出站隔离仍应继续拆分审阅。
- 提交：`bfc15e2 [P1.6]add formal ViKey card-store adapter`

### 小切片 17：helper IPC v5 正式 blob 后端

#### 计划、需求与既有实现复核

- 在 `bfc15e2` 基础上重新核对 P1.6、R-03、N-02、IPC v4、正式
  `VikeyCardStore` 适配边界、helper 单请求退出模型和现有开发凭据读取代码。本切片只把固定
  32 字节 get/put 接到可注入的正式后端；不替换产品固定桩、不新增生产凭据实现、不加载官方
  SDK、不读取凭据，也不连接或改写实体卡。
- 明确 put 失败语义：进入供应商写调用后发生的任何错误都可能表示部分写入，必须统一映射为
  `device_failure`、禁止自动重试，也不得暗示旧值仍完整。每次 get/put 都重新向凭据提供者获取
  一组锁页凭据，操作结束即销毁，不在后端对象中缓存。

#### 实现

- helper IPC 由版本 4 升至版本 5，新增独立的开发 blob read/write 操作。请求继续携带精确
  DLL 摘要和固定长度锁页凭据；write 另携带精确 32 字节锁页 blob。响应头新增显式
  `payload_bytes`，只有 read 成功允许声明并紧随 32 字节 blob，错误响应和其他成功响应必须为
  零载荷；协议层拒绝声明载荷却走普通无载荷响应函数的情况。
- 父端在同一个绝对超时期限内先验证响应头，再把精确 32 字节直接读入 `SecureBuffer`，最后
  要求 helper 正常退出且管道无尾随字节。输入凭据和写入 blob 在暂存与启动前先验证固定长度；
  调用方缓冲不被修改。
- helper read 在制品复验、模块加载、认证、固定用户区读取及显式关闭全部成功后才回传 blob。
  helper write 不重试；一旦进入写调用，写异常或关闭失败统一返回 `device_failure`。请求作用域在
  发响应前销毁凭据与写入 blob，读取结果则只在锁页响应缓冲中存活至单次响应完成。
- 新增 `VikeyHelperBlobBackend` 和 `IVikeyCredentialProvider`。配置对象只保存非秘密路径、摘要和
  超时；每次操作获取新的 `VikeyCredentialSet`，保留既有通用卡错误，凭据提供者或非卡异常统一
  收敛为 `backend_unavailable`。该组合层单独成库，仍明确标记为允许联网的开发边界。

#### 验证与安全复核

- 协议测试覆盖 v5 操作与精确载荷、read/write 请求往返、调用方缓冲保持、锁页 read 响应、
  缺失/错误长度/错误状态携带载荷的失败关闭。跨进程 fixture 覆盖正式
  `VikeyCardStore -> VikeyHelperBlobBackend -> helper` 的 get、put、模拟已部分写入后报错、每次
  重新加载凭据、凭据源失败不重试且不暂存，以及暂存目录清理。
- MSVC Debug 全量 CTest **23/23**：21 项执行通过，2 项按受限沙箱父令牌约束跳过；普通桌面
  helper 测试 **3/3 通过**。首次普通桌面补跑时，网络夹具在完整响应前以诊断码退出；增强
  非秘密分阶段诊断并显式重建两种网络夹具后，同一测试和随后完整三项均通过，未能复现。
- MSVC `/analyze` 与 AddressSanitizer 聚焦构建均成功，协议和正式卡适配测试分别 **2/2
  通过**，无新增项目诊断或内存错误。`git diff --check` 通过，新文件无行尾空白。
- 安全复核未发现凭据或 blob 进入日志、异常文本、普通可重分配容器或持久文件。产品入口仍为
  `FixedBlobCardStore`，新后端只由跨进程测试链接；产品导入表无 ViKey/helper 项。本切片未读取
  Windows 凭据、未加载官方 SDK、未连接或改写实体卡。

#### 剩余风险与下一步

- 开发 helper 仍具有普通网络访问能力，凭据和 blob 也会在 Windows 匿名管道的内核缓冲中短暂
  留下不可由进程显式锁页或擦除的副本，因此当前后端不得接收生产凭据或生产根材料。可验证的
  生产出站隔离、安装目录 ACL、发布摘要和生产凭据管理仍是产品装配前置门。
- 单次 put 不是断电原子事务：供应商写调用返回失败、进程被强制终止、卡片拔出或系统断电时，
  卡内 32 字节可能是旧值、新值或部分值。本切片只保守报告 `device_failure`，没有也不能证明
  自动恢复；首次正式写入/初始化语义仍需独立设计。
- 产品仍使用固定桩，`IVikeyCredentialProvider` 也只有抽象边界和测试实现。下一最小切片可新增
  开发专用 Windows 凭据提供者及默认构建不包含的正式后端只读手工验收入口，先用实体开发卡
  证明 v5 get 链；正式 put、产品装配、持久计数器和 P1.7 拔卡处理继续分开审阅。
- 提交：`07a1e28 [P1.6]add helper-backed ViKey blob operations`

### 小切片 18：开发凭据提供者与正式后端只读入口

#### 计划、需求与既有实现复核

- 在 `07a1e28` 基础上重新核对 P1.6、R-03、N-02、开发/生产运行边界、既有 Windows 凭据
  读取代码及三个手工 probe。本切片只提供开发专用凭据来源和 IPC v5 正式后端的实体卡只读
  入口；不替换产品固定桩、不定义生产凭据部署方案，也不调用 `put_blob`。
- Windows 凭据管理器只是当前开发机上三项开发凭据的临时来源。正式产品仍只依赖
  `IVikeyCredentialProvider` 抽象；量产凭据是否使用 DPAPI、受限服务或最终硬件自身能力须由
  后续部署威胁模型决定，不能把本实现默认为生产方案。

#### 实现

- 新增默认构建排除的 `usb_vault_vikey_development_credentials` 组件及
  `WindowsCredentialProvider`，只读取当前用户下三个固定开发目标。每次加载建立新的
  `VikeyCredentialSet`；`CredReadW` 返回的原始 blob 在复制到锁页 `SecureBuffer` 后先用
  `SecureZeroMemory` 擦除，再交给 `CredFree` 释放。异常发生在任意一项时，已构造的锁页凭据
  随栈展开自动清零。
- 抽出可确定性测试的凭据 blob 解码器，兼容既有原始 ASCII、带 NUL 的 ASCII、UTF-16LE 和
  带 NUL 的 UTF-16LE 四种 Windows 存储形状；错误长度、零预期长度和非 ASCII UTF-16 高字节
  均失败关闭。目标名、错误文本和测试不包含凭据值。
- 既有只读会话 probe 和可恢复写入 probe 改为复用该提供者，删除重复的凭据管理器读取实现。
  新增 `usb_vault_vikey_formal_blob_read_probe`，继续为 `EXCLUDE_FROM_ALL` 手工目标，只接受
  helper/DLL 路径与精确摘要，通过
  `VikeyCardStore -> VikeyHelperBlobBackend -> IPC v5 -> helper` 调用一次 `get_blob`。成功只输出
  已取得锁页 32 字节结果，不打印 blob、凭据、路径或供应商状态。

#### 验证与安全复核

- 新增单元测试覆盖四种有效凭据形状、短值、零预期长度、非 ASCII UTF-16 和提供者禁拷贝属性；
  测试只使用固定无害字节，不访问 Windows 凭据管理器。MSVC Debug `/W4 /WX` 下三个既有/新增
  手工 probe 与新测试均构建通过。
- Debug 全量 CTest **24/24**：22 项执行通过，2 项按受限父令牌约束跳过；普通桌面 helper
  测试 **3/3 通过**。MSVC `/analyze` 与 AddressSanitizer 下三个聚焦测试分别 **3/3 通过**，
  无新增项目诊断或内存错误。只读 probe 无参数运行在读取路径、摘要、凭据或设备前以用法错误
  退出，退出码为 1。
- `git diff --check` 通过，新文件无行尾空白。默认产品入口仍实例化 `FixedBlobCardStore`；产品
  导入表没有 ViKey 或开发凭据组件，手工只读 probe 不静态导入 `ViKey.dll`。安全审阅未发现
  凭据或 blob 进入日志、异常文本、普通可重分配容器、环境、命令行或持久文件。

#### 实体验收、风险与下一步

- 官方 x64 DLL 仍匹配既有 SHA-256 白名单
  `735A2347F66F10C649C9B38ED0232B839812277C8E178F0C5084C676CF1DE48E`；当前 Debug v5 helper
  SHA-256 为 `C270D3E25CF8D62974D6832D4889F7A9065A26584570AE251749CE2CAA0C2E25`。安全审批首次拒绝
  在隐含授权下读取三项开发凭据；用户随后明确授权把这些凭据交给本机具普通网络权限的开发
  helper 和官方 SDK，用于一次实体开发卡只读 `get_blob`，未扩大为写入或生产材料授权。
- 在上述明确授权下执行一次正式链路实体读取，probe 返回精确 32 字节锁页 blob，进程退出码为
  0。输出只说明读取通过，未打印凭据、blob、路径或供应商状态；本次未调用写接口，卡内容没有
  被修改，也没有产生写入至恢复之间的中断窗口。执行后未发现残留的随机 helper 暂存目录。
- 即使只读验收通过，Windows 提供者和联网 helper 仍只允许开发凭据、开发卡和非生产 blob。
  下一代码切片才考虑显式启用的真卡开发版产品装配；生产材料进入前仍须恢复可验证出站隔离。
- 提交：`714e371 [P1.6]add development ViKey credential provider`

### 小切片 19：显式启用的真卡开发版应用装配

#### 计划、需求与既有实现复核

- 在 `714e371` 基础上重新核对 P1.6、R-03、8.6 及开发/生产运行边界。默认产品入口仍硬编码
  `FixedBlobCardStore`，此前的正式 ViKey 适配器、helper 后端和开发凭据提供者均尚未进入产品
  应用；本切片只增加一个可单独构建、显式确认联网风险的开发版 EXE，不替换默认产品，也不把
  Windows 凭据管理器确定为正式产品凭据方案。
- 审计现有 CMake 配置后确认 `out/build/dev` 关闭 UI，`review-clean`、`analyze` 和 `asan` 才启用
  UI。首次在 `out/build/dev` 请求应用目标因此得到“未知目标”；切换到正确的 UI 构建配置后，
  默认应用与开发应用均正常编译。这是配置不匹配，不是代码编译失败。

#### 实现

- 新增默认构建排除的 `usb_vault_vikey_development_app`。它复用正式 UI 和
  `ApplicationSession`，但把 `ICardStore` 装配为
  `VikeyCardStore -> VikeyHelperBlobBackend -> WindowsCredentialProvider`；默认
  `usb_vault_app` 继续装配 `FixedBlobCardStore`，没有改变既有演示/离线路径。
- 开发 EXE 必须收到精确确认开关
  `--i-understand-vikey-development-mode-is-network-enabled`、helper 绝对路径及 SHA-256、供应商
  目录绝对路径及 DLL SHA-256。解析器拒绝缺失/错误确认、额外参数、相对或形状无效的路径、
  非 64 位十六进制摘要，并只保存词法规范化后的非秘密部署数据。
- 开发窗口标题固定显示“ViKey 联网开发模式”。应用启动只完成非秘密参数解析和对象装配，不读取
  Windows 凭据；只有用户发起需要卡 blob 的会话操作时，后端才按既有边界加载一组新凭据并
  启动单请求 helper。清理顺序先销毁 UI/应用会话和卡适配器，再销毁后端及凭据提供者，避免悬空
  引用；命令行不承载 UserSN、API Key、PIN 或 blob。

#### 验证与安全复核

- 新增配置单测覆盖有效路径规范化与摘要解码、确认开关必需、helper/供应商相对路径拒绝、摘要
  长度及非十六进制拒绝和额外参数拒绝。MSVC Debug `/W4 /WX` 全量 CTest **25/25**：23 项
  执行通过，2 项按既有受限父进程令牌约束跳过；普通桌面 helper 测试 **3/3 通过**。
- 默认应用和开发应用在 Debug、MSVC `/analyze`、AddressSanitizer 三套 UI 配置下均构建成功。
  三个聚焦测试在 `/analyze` 和 AddressSanitizer 下分别 **3/3 通过**，无新增项目诊断或内存
  错误。`git diff --check` 通过。
- Ninja 默认 `all` 目标不包含开发应用。二进制导入表复核确认默认 EXE 不导入 `CredReadW` 或
  `ViKey.dll`；开发 EXE 按设计导入 `CredReadW`，但不静态导入 `ViKey.dll`，官方 SDK 仍只在
  独立 helper 内经摘要验证后动态加载。代码与测试未发现凭据或 blob 进入参数、日志、异常文本、
  普通可重分配字节容器或持久文件。
- 本切片没有启动开发 GUI、没有读取 Windows 凭据、没有加载官方 SDK、没有连接或改写实体卡；
  既有的一次只读实体授权没有被扩大为新的访问或写入授权。

#### 剩余风险与下一步

- 开发 EXE 会把开发凭据和非生产 blob 交给具普通网络访问权限的 helper，匿名管道内核缓冲仍有
  进程无法显式擦除的短期副本；它只能用于开发卡和开发材料，不能作为生产出站隔离或生产凭据
  方案。命令行中的路径和摘要虽非秘密，仍需由可复现启动方式提供，避免人工抄错。
- 真实应用闭环尚未验收：开发 EXE 尚未在连接的开发卡上完成“启动界面 -> 输入密码 -> 读取卡
  blob -> 挂载/失败提示 -> 正常退出”的手工流程，WebView2、建卷工具、容器状态和卡内现有 blob
  的兼容性仍可能暴露集成问题。下一最小切片应先生成或记录当前制品摘要，按用户对凭据和实体卡
  的明确授权启动一次开发 EXE，执行只读应用级验收；不得在该验收中初始化或覆写卡 blob。
- 提交：`9195800 [P1.6]add opt-in ViKey development app`

### 小切片 20：真卡开发版应用只读界面冒烟

#### 执行前审计与制品核对

- 在 `9195800` 后重新复核开发应用调用顺序：用户提交非空输入后，
  `ApplicationSession` 先调用 `ICardStore::get_blob`，随后才检查并挂载容器；该路径没有
  `put_blob`、初始化卡或其他卡写入调用。开发 EXE 目录当前只有页面资源，没有
  `container_a.hc` 或 `container_b.hc`，因此即使读卡成功也预期在后续“无可用容器”处失败。
- 当前开发 EXE SHA-256 为
  `C210DB44BBBD628ED0914D47A2FE5437EC7AD5B620E86F322C6AB19FC6F4D735`，helper SHA-256 为
  `EFC38B2C5D44466C0C5EC170BC0E7726BA212FC5F3EA8C598AC5D4E7AC64E034`；仓库外官方 x64
  `ViKey.dll` 仍匹配白名单
  `735A2347F66F10C649C9B38ED0232B839812277C8E178F0C5084C676CF1DE48E`。
- 执行前 `USB\\VID_1987&PID_0331` 和 `HID\\VID_1987&PID_0331` 两个节点均为 `Started`，
  没有旧开发应用或 helper 进程，也没有 helper 暂存目录。用户在已知下一步是应用级实体卡只读
  验收后要求继续，并在启动审批中再次确认：允许三项开发凭据进入本机具普通网络访问权限的
  helper，用于当前开发卡的一次只读 `get_blob`，不授权写卡。

#### 界面执行与清理复核

- 使用精确确认开关、绝对制品路径及上述摘要启动
  `usb_vault_vikey_development_app.exe`。窗口标题明确显示“ViKey 联网开发模式”，页面进入
  “安全组件已就绪”状态；启动阶段没有读取凭据或访问卡。
- 按人工接管要求，用户在密码框输入无害测试文本 `test` 并提交。页面清空输入，随后显示
  “无法打开安全会话，请检查密码后重试”；没有进入活动会话或显示卸载操作。由于目录内没有
  容器，该结果与预期的后续挂载失败一致。
- 关闭无活动会话的窗口后，开发应用、helper 及其 WebView2 子进程均已退出；未发现
  `usb-vault-vikey-helper-*` 暂存目录，文件系统盘符仍只有执行前的 C、D、E，未产生挂载残留。
  工作区在记录本节前保持干净。本次没有调用 `put_blob`，也没有创建、复制或修改容器文件。

#### 结论、可观察性限制与下一步

- 本次证明了真实开发版 EXE 可用精确制品参数启动，WebView2 页面和 host object 可就绪，用户
  提交能进入统一应用会话链，失败后输入会清空且进程/暂存/盘符清理闭环成立。
- 不能仅凭当前界面文案把这次操作记为“本次 `get_blob` 已独立证明成功”：host object 把读卡、
  KDF、容器检查和驱动错误统一收敛为同一个 HRESULT，页面又统一显示同一句失败提示。静态调用
  顺序、此前正式后端实体只读 probe 的成功证据以及本次无容器前置条件共同支持“很可能在读卡后
  到达容器检查”的推断，但不是本次运行自身的可区分证据；开发记录不得把推断写成实测事实。
- P1.6 的正式后端实体 `get_blob` 已由小切片 18 的独立 probe 证明，实体 blob 写入/读回/恢复也已
  由小切片 15 证明，因此本次 UI 可观察性缺口不推翻既有硬件证据。若要把应用级读卡也做成独立
  验收，应另加仅开发版可用、不会泄露 blob/凭据的阶段结果；完整容器挂载仍属于后续带匹配测试
  容器的集成验收/P1.7，不应通过临时复制不匹配容器冒充成功。
- 用户复核后明确指出最终应用必须允许首次使用者建立容器。当前开发 EXE 只复用了 P1.5 的
  “挂载已有容器”最小 UI，不能从零初始化产品；P1.4 虽已有独立建卷工具，但主界面尚未接入
  “无容器 -> 建卷向导 -> 分别创建 A/B -> 返回登录”的 P2 流程。因此它只能称为真卡装配开发
  预览，不能称为可供新用户完整试用的 EXE。该范围限定不改变本次只读冒烟结论，也不得以手工
  复制容器代替正式建卷向导验收。
- 下一项 P1.6 硬件能力工作仍是可选持久计数器的 ABI、失败语义和实体能力验证；任何会永久消耗、
  递增或不可恢复改变计数器的测试必须先查清供应商语义，并作为独立风险决策点。
- 提交：`f2480df [P1.6]record ViKey development app smoke test`

### 小切片 21：P1.6 完结与计数器暂缓决策

#### 完结审计

- 重新核对 `需求梳理清单_v2.md` 的单 blob 合约、P1.6 开发/生产边界、N-07 和
  `MILESTONES.md` 的 P1 子阶段出口。P1.6 的必需出口已有独立证据：正式 `ICardStore` 适配只
  暴露 get/put/能力位；IPC v5 和 helper 后端以锁页缓冲传递精确 32 字节 blob 与开发凭据；真实
  开发卡完成 get、受控测试写入、读回、原值恢复及恢复后逐字节验证；ViKey 私有类型、凭据、
  地址和错误域没有越过适配边界；默认产品构建仍不包含联网开发实现。
- 开发版应用已完成制品参数、真实 WebView2 和提交/清理冒烟，但它只挂载已有容器，不包含 P2
  的首次建卷向导；该限制不属于 P1.6 真卡存储出口，也不得把开发预览描述为完整用户试用版。

#### 计数器决策

- 用户复核 N-07 机制 2 后决定暂缓卡上单调计数器，不继续进行 ABI 实现、实体初始化或消耗测试。
  该计数器只能证明中间发生过其他挂载，无法区分用户自行测试胁迫密码、维护模式或真实胁迫，
  因而告警解释力有限；对手绕过本软件直接使用卡材料时又不会触发计数。ViKey 的有限递减计数器
  还会引入初始化、耗尽、回绕比较、断电同步和不可恢复实体变化风险。
- N-07 机制 1“隐藏卷完整性校验”继续保留在 P2，可独立判断数据是否真正受损。计数器只作为未来
  可重新评估的增强项，不计入当前工期，不再是 P1.6、P1.7、P2 或量产卡选型的阻塞门。

#### 文档同步、结论与后续边界

- `需求梳理清单_v2.md` 已把计数器从硬件必需项改为暂缓的可选增强，并保留未来重新启用时必须
  审计的硬件与 UX 风险。`MILESTONES.md` 已将 P1.6 标记为 2026-08-27 完成，将 P1 总览推进为
  P1.0~P1.6 已完成、仅剩 P1.7，并更新外部依赖、风险项和当前状态摘要。
- P1.6 正式完结。残余风险不被本状态变更掩盖：联网开发 helper 不可承载生产材料；生产出站隔离、
  安装目录 ACL、发布摘要和生产凭据部署仍是交付门；单 blob 写入在断电、拔卡或强制终止时不具
  原子恢复保证；完整首次建卷体验属于 P2。
- 下一阶段为 P1.7：插卡发现、拔卡触发卸载、忙碌/异常反馈和真卡最小闭环。该阶段不得顺带加入
  P2 建卷向导、计数器或生产凭据部署。
- 本切片只修改文档，没有编译或执行代码，没有读取 Windows 凭据、加载官方 SDK、连接实体卡或
  修改容器。
- 提交：`124933a [docs]close P1.6 real card storage`

## P1.7 插拔监听与真机闭环（2026-08-27 开始）

### 小切片 1：厂商无关的卡存在状态与移除卸载核心

#### 计划、需求与既有实现复核

- 重新核对 `需求梳理清单_v2.md`、`MILESTONES.md` 和 R-03/8.6。P1.7 的必需范围是插卡发现、
  拔卡触发卸载、忙碌/异常反馈和真卡最小闭环；已取消的自动登录不得因插卡监听重新引入，
  也不得在此阶段顺带加入 P2 建卷向导或暂缓的计数器。
- 代码检索确认正式应用尚无 `WM_DEVICECHANGE`、`RegisterDeviceNotification` 或 `DBT_*` 处理。
  现有 `ApplicationSession::unmount_active()` 已按 B 后 A 的顺序卸载，并会在失败时保留仍挂载
  状态以供重试；`prepare_ui_session_for_close()` 也已复用同一卸载边界。因此本切片先固定不依赖
  Win32 或 ViKey VID/PID 的状态/卸载语义，Windows 设备通知装配留给下一独立切片。

#### 实现

- 新增 `CardPresenceController` 及非秘密的 `CardPresenceTransition`。控制器只保存进程内瞬时
  `card_present` 布尔值，不保存设备路径、供应商状态、用户模式、卷类型、凭据或卡 blob。
- 首次到卡只把状态改为可用并返回 `card_available`，不会调用认证或挂载；重复到卡返回
  `no_change`。首次离卡先无条件标记卡已离场：无活动会话时返回 `card_unavailable`，有活动会话
  时调用既有 `unmount_active()`。
- 卸载抛异常或调用返回后仍显示已挂载时统一返回 `active_session_remains`，不把失败伪装成成功；
  会话对象继续保存可重试状态。一次物理离卡只触发一次卸载，重复离卡通知被忽略，避免通知抖动
  导致重复执行破坏性会话操作。后续人工卸载/重试由 Windows/UI 装配切片提供。

#### 验证与安全复核

- 新增测试覆盖：到卡不认证、重复到卡幂等、无会话离卡、有会话离卡只卸载一次、卸载异常保持
  可重试会话、非抛异常但仍挂载时失败关闭。MSVC Debug `/W4 /WX` 聚焦测试通过；Debug 全量
  CTest **26/26**：24 项执行通过，2 项按既有受限父进程令牌约束跳过。
- MSVC `/analyze` 与 AddressSanitizer 下聚焦测试分别 **1/1 通过**，无新增项目诊断或内存错误；
  `git diff --check` 通过。首次直接构建未加载 Visual Studio 开发环境，因标准库路径缺失而失败；
  经既有 `VsDevCmd.bat` 环境入口重跑后正常通过，确认不是源码错误。
- 安全复核确认到卡路径没有自动登录、卡访问、凭据读取或持久化；离卡路径复用正式会话卸载，
  不读取卡即可执行。失败结果继续区分“会话仍在”，避免 UI 将仍暴露的卷误报为安全关闭。
  本切片没有加载官方 SDK、读取 Windows 凭据、连接实体卡、挂载/卸载真实卷或修改容器。

#### 剩余风险与下一步

- 当前只是可测试的应用层核心，尚未接收真实 Windows 设备事件；应用启动时已插入卡的初值、
  精确 ViKey 设备接口识别、通知注册/注销和窗口生命周期均未实现。卸载失败后重复设备通知不会
  自动重试，后续 UI 必须明确显示仍有活动会话并提供人工重试/退出保护。
- 下一最小切片应实现 Windows 设备通知适配器：枚举启动时在场状态、只接受目标 ViKey 接口的
  到达/移除事件、把事件送入本控制器并妥善注销通知；仍不访问凭据、不读写卡、不自动登录。
- 提交：`90a1c45 [P1.7]add card removal session controller`

### 小切片 2：Windows HID 设备通知适配与开发版接线

#### 计划、需求与既有实现复核

- 在 `90a1c45` 基础上重新核对 P1.7、基本需求第 3 条、R-03/8.6 和既有窗口生命周期。正式应用
  尚未处理 `WM_DEVICECHANGE`，但 P1.6 的 HID 兼容性证据已确认当前开发卡是 HID 类接口，硬件
  身份为 `VID_1987/PID_0331`，打开时还可核验 Usage `FF00/0001`。移除完成后设备已不可打开，
  因此离卡通知只能在 HID 类 GUID 之外按设备接口路径中的精确 VID/PID 令牌筛选。
- 为保持量产卡可替换，Windows 适配器只接收通用 `HidDeviceIdentity{vendor_id, product_id}`；
  `1987:0331` 只出现在显式 ViKey 开发版应用装配中。默认桩卡应用不链接该适配器，也不因当前
  开发硬件写死产品架构。

#### 实现

- 新增独立的 `usb_vault_windows_hid_notifications` 静态库。RAII 注册对象通过
  `RegisterDeviceNotificationW` 为窗口注册 HID 设备接口类，析构和窗口销毁时注销；注册失败或
  启动枚举失败会中止开发版应用初始化，不允许在没有拔卡保障的情况下静默继续。
- 通知先注册、后用 SetupAPI 枚举启动时已存在的 HID 接口，关闭“应用启动前卡已插入”的漏判
  窗口。枚举只读接口元数据，不打开设备句柄、不发送 HID 报告。路径匹配按 `#`/`&` 分隔的精确
  8 字符 `vid_XXXX`、`pid_XXXX` 令牌执行，大小写不敏感；子串、超长 token 和错误 ID 均拒绝。
- `WM_DEVICECHANGE` 路由只接受 `DBT_DEVICEARRIVAL` 与 `DBT_DEVICEREMOVECOMPLETE`，并核对
  `DBT_DEVTYP_DEVICEINTERFACE`、HID class GUID、结构长度、UTF-16 偶数字节、32768 字符上限和
  边界内 NUL。目标到卡只更新瞬时在场状态；目标离卡进入上一切片的卸载控制器。若卸载后会话
  仍在，开发窗口立即显示“卡已拔出但磁盘仍无法安全卸载”的统一占用提示。
- 开发应用在会话初始化后创建在场控制器与通知注册，窗口销毁时先注销通知，最终清理时再按
  通知、在场控制器、UI/应用会话、卡后端顺序释放。默认应用编译路径不含这些全局对象和
  `WM_DEVICECHANGE` 分支。

#### 验证与安全复核

- 新增测试覆盖大小写无关的精确路径、错误 VID/PID、嵌入子串、超长 PID token、目标到达不认证、
  目标移除卸载，以及错误 class GUID、过小/奇数/超限/无 NUL 结构和无关事件全部忽略。首次测试
  编译因测试代码使用未由当前头文件集声明的 `GUID_NULL` 失败，改为显式零 `GUID{}` 后在严格
  `/W4 /WX` 下通过，不涉及生产逻辑降级。
- Debug 全量 CTest **27/27**：25 项执行通过，2 项按既有受限父进程令牌约束跳过；开发版 EXE
  在普通 Debug、MSVC `/analyze` 和 AddressSanitizer 三套 UI 构建中均成功链接。聚焦通知测试在
  Debug、`/analyze` 和 AddressSanitizer 下分别 **1/1 通过**，无新增项目诊断或内存错误。
- PE 依赖复核确认默认 `usb_vault_app.exe` 没有新增 `HID.DLL` 或 `SETUPAPI.dll`；只有显式
  ViKey 开发版按设计新增两者，仍不静态导入 `ViKey.dll`。`git diff --check` 通过。代码没有记录
  设备路径、卡标识、卡在场历史、档位、卷类型、凭据或 blob，也没有自动登录或持久化。
- 本切片只编译和运行合成消息单测，没有启动开发 GUI、读取 Windows 凭据、加载官方 SDK、打开
  HID 设备、连接/改写实体卡或挂载/卸载真实卷。

#### 剩余风险与下一步

- 当前真实 Windows 注册、启动枚举和物理插拔事件尚未做实体界面验收；合成消息只能证明解析和
  路由语义。设备移除成功后底层会话已关闭，但 WebView 页面仍会保持上一次按钮/文案，直到用户
  下一次操作触发状态查询；忙碌失败已有原生提示，但页面状态同步仍是下一切片的必需工作。
- VID/PID 路径身份可被另一只仿冒 HID 复制，因此同身份到达/移除可造成可用状态抖动或提前安全
  卸载；由于到达绝不认证，这不能读取卡或打开会话，影响限于拒绝服务。当前策略对任何匹配移除
  都立即卸载，宁可在多卡/仿冒场景提前关闭，也不因另一只同型号设备仍存在而漏掉真实会话卡。
- 下一最小切片应增加不泄露模式的 WebView 会话状态刷新事件，分别反馈“拔卡后已安全关闭”和
  “拔卡但卸载失败、仍需人工重试”，并用窗口级合成/集成测试验证；之后才进行需用户物理配合的
  真卡插拔与实际挂载清理验收。
- 提交：`968cddc [P1.7]wire Windows HID card notifications`

### 小切片 3：拔卡后 WebView 会话状态刷新

#### 计划、需求与既有实现复核

- 在 `968cddc` 基础上重新核对 P1.7、R-03/8.6、页面契约和 WebView2 安全设置。
  拔卡控制器已能关闭底层会话，但页面只在初始化、提交密码或手工卸载后查询
  `hasActiveSession()`，因此外部移除后会留下过期按钮和文案。本切片仅修复这个
  窗口到页面的状态同步边界，不引入自动登录、设备详情、模式分类或 P2 建卷。
- 安全设置复核确认产品宿主仍显式关闭 `IsWebMessageEnabled`。因此原生窗口通过
  `ExecuteScript` 只分发一个固定、无数据的 DOM 事件，页面再复用现有统一布尔查询；
  不新增可变脚本拼接或页面到原生的消息通道。

#### 实现

- 新增共享的固定脚本常量，只分发 `usb-vault-session-changed` 事件，不携带设备路径、
  VID/PID、卡身份、桃拨状态、卷类型、盘符、凭据或 blob。目标移除后，无论卸载成功
  还是会话仍在，宿主都先通知页面；若会话仍在，再显示原生占用失败对话框，
  避免模态对话框延迟页面刷新。脚本分发的即时失败和异步失败都有统一本机提示。
- 页面监听该事件并复用 `hasActiveSession()`。事件在密码提交、手工卸载或 bridge
  初始化期间到达时只记录待刷新标志，当前操作收敛后合并查询，不并发进入 host
  object。查询成功时只显示“会话已关闭”或“会话仍在，请尝试安全卸载”；
  查询异常时按仍有活动会话失败关闭，不把不可知状态误报为安全。
- 扩展页面契约和真实 WebView2 闭环验证：先在活动会话下注入外部刷新事件，
  证明页面仍显示需要卸载；再由原生探针完成外部卸载并注入同一事件，证明页面
  回到就绪态。测试回调内的卸载异常也被截留，不允许 C++ 异常越过 COM 回调边界。

#### 验证与安全复核

- MSVC Debug 全量 CTest **27/27**：25 项执行通过，2 项按既有受限父进程令牌约束跳过。
  Debug 真实 WebView2 Runtime 闭环退出 0，覆盖“会话仍在”和“外部卸载后已关闭”
  两条路径。首次在受限沙箱内直接启动隐藏 GUI 时 15 秒超时；使用允许桌面组件的
  普通本机环境复跑后通过，确认为 GUI/WebView2 沙箱约束，不是产品逻辑回归。
- 开发应用、页面契约和真实 WebView2 闭环目标在 MSVC `/analyze` 和 AddressSanitizer
  下均成功构建，无新增项目诊断。页面契约在两种配置下均 **1/1 通过**；ASan
  插桩的真实 WebView2 闭环退出 0，未报告内存错误。`git diff --check` 通过。
- 安全复核确认：事件是固定且无数据的，不拼接外部输入；产品 WebMessage 仍关闭；
  页面只能重查统一活动会话布尔值，不能区分模式、卷类型或安全卡身份；查询失败
  时保留人工卸载路径。本切片没有读取 Windows 凭据、加载官方 SDK、打开或改写
  实体卡、挂载/卸载真实卷或修改容器。

#### 剩余风险与下一步

- 代码层闭环已完成，但尚未使用真实开发卡做“应用启动时卡已插入 / 运行中插卡 / 活动
  挂载中拔卡”验收。真实卸载闭环需要与当前卡 blob 匹配的测试容器；在匹配容器就绪前，
  只能证明设备通知和无活动会话路径，不能把“密码后挂载失败”冒充为拔卡卸载验收。
- 下一最小切片应先核对卡 blob 中已有容器标识与本机测试容器，必要时仅在用户明确
  批准后准备可丢弃的匹配测试容器；然后启动开发版 EXE，进行需用户物理插拔配合的
  真卡在场、移除卸载与页面刷新验收。
- 提交：`528fd63 [P1.7]refresh UI after external unmount`

### 小切片 4：P1.7 真卡验收夹具（可丢弃匹配容器）

#### 计划、需求与既有实现复核

- 在 `528fd63` 基础上复核 P1.7 出口条件与阻塞项。代码层的插卡发现、拔卡卸载和页面刷新已
  齐备，但真卡闭环缺少可挂载对象：开发版 EXE 只认自己目录旁的 `container_a.hc` 与
  `container_b.hc`，两者当前都不存在；仓库内 P1.3 遗留容器由合成卡材料建成，卷密码与真卡
  blob 的派生结果不一致，不得复制冒充。
- 手工用 P1.4 建卷工具无法补齐：容器卷密码由「用户输入 + 卡 blob + 固定盐」派生，而卡 blob
  从不离开锁页缓冲，人无法抄写。因此需要一个复用产品自身常量与 KDF 的一次性夹具，而不是把
  建卷能力混入正式应用（首次建卷向导仍属 P2）。

#### 实现

- 新增 `include/usb_vault/application/container_layout_v1.h`，把容器文件名、容器 A/B 的固定盐
  与容器 A 的固定派生输入 `diskA` 从 `mount_orchestrator.cpp` 与 `tools/app/main.cpp` 中提取为
  共享常量。这些值原本就分别硬编码在这两处，抽出后夹具与产品挂载路径共用同一份定义，避免
  夹具与产品出现静默漂移。常量均为非秘密，未新增任何落盘、日志或注册表产物。
- 新增默认排除构建的 `usb_vault_p1_7_development_fixture` 库与
  `usb_vault_p1_7_development_container_fixture` 可执行文件。库负责语义：读卡恰好一次且从不
  写卡，blob 非 32 字节时在派生前失败关闭，三组卷密码（容器 A 固定输入、容器 B 外层=胁迫
  测试口令、容器 B 隐藏=真实测试口令）全程在锁页 `SecureBuffer` 中派生并在最后一次使用后
  显式清零。
- 破坏性保证同样落在库内而不是可执行文件内：`require_absent` 对既有文件、目录以及无法分类的
  目标一律拒绝，绝不打开或覆盖既有容器；`require_regular_file` 校验三个必需制品；
  `FixtureCleanup` 在**调用建卷之前**登记暂存路径所有权，因此建卷写到一半再抛异常时暂存文件
  仍会被删除。发布采用「先建暂存名、两只都成功后再改名为正式名」，`publish_b` 后若
  `publish_a` 失败，已发布的容器 B 会被回滚，不留半发布布局。
- 可执行文件退化为纯装配：解析确认开关与制品参数、组装
  `VikeyCardStore -> VikeyHelperBlobBackend -> WindowsCredentialProvider`、提供基于 P1.4
  `VolumeCreator` 的建卷适配器。缺少精确确认开关
  `--create-disposable-p1.7-containers` 或参数不全时，在读取凭据、访问卡和创建任何文件之前
  即以用法错误退出。

#### 验证与安全复核

- 单元测试从 2 项扩到 8 项，新增覆盖：建卷失败时半成品被清理、`require_absent` 拒绝既有文件
  与既有目录、`require_regular_file` 拒绝缺失与目录、已登记/未登记暂存文件的差别清理、
  第二次发布失败时回滚第一次发布、`release()` 后完整布局保留。测试使用独立临时目录，只写入
  1 字节占位内容，不接触真实卡、凭据或容器。
- 变异验证确认新测试确有鉴别力：删除「建卷前登记所有权」后报
  `a partially written container survived the failure`；删除 blob 尺寸门后报
  `fixture changed card blob size`；删除发布回滚的所有权转移后报
  `a half-published layout survived the failure`；三者恢复后全部通过。
- MSVC Debug `/W4 /WX` 全量 CTest **28/28 通过，0 失败**（本次环境下此前受限父令牌跳过的
  2 项也已执行通过）。`/analyze` 与 AddressSanitizer 两套配置下，夹具库、夹具 EXE、默认应用与
  开发版应用均构建成功且无新增诊断，夹具测试在两套配置下分别 **1/1 通过**。
  首次测试编译因自定义 `exists` 与 `std::filesystem::exists` 经 ADL 产生重载歧义而失败，改名为
  `path_exists` 后通过，不涉及产品逻辑降级。
- Ninja 默认 `all` 目标不含夹具 EXE 与开发版应用，只含链接夹具库的单元测试。默认
  `usb_vault_app.exe` 导入表复核仍无 `CredReadW`、`ViKey.dll`、`HID.DLL` 或 `SETUPAPI.dll`。
  `git diff --check` 通过。异常文本不含容器路径、卡标识、档位、凭据或 blob。
- 本切片只编译并运行合成单元测试，**没有**读取 Windows 凭据、加载官方 SDK、连接或改写实体卡、
  创建任何容器，也没有挂载或卸载真实卷。

#### 剩余风险与下一步

- 夹具会在磁盘上产生一个含隐藏卷的容器 B，即 8.3 意义上的档三布局。这是容器文件本身的属性，
  夹具不写配置、注册表或日志，不构成 8.6 禁止的「本地产物记录档位」；但这两只容器是一次性
  测试材料，验收结束后应删除，不得用于任何生产或演示数据。
- 单次建卷不是断电原子事务。夹具只保证异常路径下删除暂存文件与回滚半发布布局；进程被强制
  终止或系统断电时，暂存文件可能残留，需人工删除 `*.p1-7-preparing`。
- 下一步是需要用户物理配合的真卡验收，且必须先取得明确授权：读取三项开发凭据、启动具普通
  网络权限的 helper 对开发卡执行一次只读 `get_blob`、两次 UAC 提权建卷、生成约 120MB 可丢弃
  容器；随后启动开发版 EXE，完成「卡在场 → 输入测试口令 → 挂出两个盘 → 物理拔卡 → 自动卸载 →
  页面刷新 → 盘符与驱动无残留」。真实口令与胁迫口令都应验证挂出**数量一致**的两个盘。
- 附带发现（不属本切片范围）：`integration` 预设未设置
  `FETCHCONTENT_SOURCE_DIR_WEBVIEW2_SDK`，因而尝试联网重新下载 WebView2 SDK 并在受限网络下
  失败。本地 `out/webview2-sdk-1.0.4129.50/` 已完整可用，`dev`／`analyze`／`asan`／`review-clean`
  均已指向它，故不影响当前构建与验收；如需修复，只是给预设补一个 cache 变量，不需要网络。
- 提交：`3deed4c [P1.7]add disposable real-card container fixture`

### 小切片 5：真卡验收前的 UAC 创建器管道访问修复

#### 计划、需求与既有实现复核

- 在启动真卡验收前复核 P1.4 创建器权限边界、P1.7 夹具调用链及昨日失败现场。普通权限的夹具
  通过 `runas` 启动提权创建器，再连接由创建器建立的单实例、拒绝远程命名管道；问题发生在
  UAC 之后：提权令牌的默认对象属主是 Administrators，而原 DACL 使用 `OWNER RIGHTS`，因此
  获得访问权的是提权对象属主，不是发起提权的普通权限 Windows 用户，客户端会在连接管道时
  被拒绝。
- 修复仍须保持 P1.4 的最小权限、随机管道名、nonce 校验、首实例及拒绝远程约束；不得把管道
  放宽给 Users、Authenticated Users、Administrators 或 Everyone，也不得把密码转移到命令行、
  环境变量或日志。

#### 实现

- 新增独立的创建器管道安全描述符构造器。它从当前提权进程令牌读取 `TokenUser` SID，而不是
  使用可能已变为 Administrators 的对象属主，生成受保护 DACL：只给 LocalSystem 和该明确用户
  SID 完全访问。SID 由 Windows API 转为规范字符串后再组成 SDDL；令牌查询、SID 转换或分配
  任一步失败都返回空描述符并使创建器失败关闭。
- 安全描述符、令牌句柄及 SID 字符串分别由 RAII 对象释放；创建器只替换管道 ACL 的来源，
  其余协议、随机名称、nonce、客户端模拟和建卷逻辑不变。
- 新增回归测试直接解析生成的安全描述符，要求 DACL 受继承保护且恰好只有两条 allow ACE，
  两个主体必须分别是 LocalSystem 与当前 `TokenUser`，出现重复或任何额外主体均失败。

#### 验证与安全复核

- MSVC Debug `/W4 /WX` 全量构建成功；CTest **28/28**：26 项执行通过，2 项按受限父进程令牌
  约束跳过。创建器协议聚焦测试单独执行通过。
- MSVC `/analyze` 下创建器协议测试、提权创建器和 P1.7 夹具均构建成功，无新增项目诊断，
  聚焦测试通过；AddressSanitizer 下相同三个目标构建成功，聚焦测试通过且未报告内存错误。
- `git diff --check` 通过。安全复核确认没有新增可被低权限其他用户访问的主体，没有改变密码
  传递、客户端模拟、目标文件操作或卡访问边界；故障仍为失败关闭。
- 本切片只执行合成测试和构建，**没有**读取 Windows 凭据、加载官方 ViKey SDK、连接或改写
  实体卡、创建容器、请求 UAC、挂载或卸载真实卷。

#### 剩余风险与下一步

- 单元测试证明 ACL 结构和主体正确，但完整的「普通权限夹具 → UAC 创建器 → 普通权限客户端
  回连」仍需在交互桌面上由真实 UAC 流程验证；这正是下一真卡验收切片首先覆盖的边界。
- 下一步在本修复获准提交后运行已提交的 P1.7 夹具，按既有授权读取开发凭据并对开发卡执行
  一次只读 `get_blob`，接受两次 UAC 后生成约 120MB 可丢弃容器。随后分别输入准确测试口令
  `p1.7-real-test` 与 `p1.7-duress-test`，验证两者都挂出两个盘，并配合物理拔卡验证自动卸载、
  页面刷新及盘符/驱动无残留。
- 提交：`8039f09 [P1.7]fix elevated creator pipe access`

### 小切片 6：P1.7 真卡双路径挂载验收与 WebView2 数组封送修复

#### 验收过程与故障定位

- 用户在普通 PowerShell 中运行一次性容器夹具，两次 UAC 创建流程均成功，生成 20 MiB 的
  `container_a.hc` 与 100 MiB 的 `container_b.hc`；这证明小切片 5 的显式用户 SID 管道
  DACL 已在真实「普通权限客户端—提权创建器」边界生效。
- 正式只读 blob 探针从开发卡取得 32 字节 blob；新增的只读挂载诊断分别验证 B 容器的
  real-test 派生口令、A 容器的固定派生口令及与正式应用相同的双容器事务，全部挂载、卸载
  成功。诊断不写卡、不输出秘密，并只接受显式的一次性 P1.7 确认参数。
- 正式 GUI 最初对手工输入和粘贴输入均返回通用失败。临时 GUI 诊断先确认原生层收到 14 个
  非零字节，但从位置 2 开始共有 12 个字节不符；随后由原生宿主在文档创建时自动填入固定
  口令，仍得到相同结果，排除了键盘、剪贴板、输入法及用户操作。
- 同一固定口令改用普通 JavaScript 数组传输后逐字节匹配。由此把根因锁定为当前 WebView2
  Runtime 的 `Uint8Array` host-object 投影发生确定性重排，而不是卡、凭据、KDF、容器、
  VeraCrypt 驱动或应用事务。用于显示字节分类的临时 GUI 诊断目标和产品宿主改动已在提交前
  删除，不保留密码字节观察面。

#### 正式修复与回归覆盖

- 页面仍先用 `TextEncoder` 生成 UTF-8 typed array，但立即复制到普通可变数字数组；随后
  清空 DOM 并擦除 typed array，再把普通数组交给异步 host object。调用结算后立即擦除普通
  数组，外层 `finally` 为两个数组保留兜底清零。原生侧继续只接受既有窄化的一维
  `SAFEARRAY(UI1)` / `SAFEARRAY(VARIANT/I4)`，不接受字符串或 JSON。
- 产品页和真实 Runtime 集成页为脚本 URL 增加本次内容版本标记，避免既有 WebView2 用户
  数据目录继续命中旧脚本缓存。页面契约测试新增普通数组复制顺序、typed array 提前清零、
  bridge 数组结算后清零、双重兜底及禁用 Runtime 相关 typed-array 投影选项的断言。
- 真实 Runtime 集成覆盖扩展为两次提交：先验证与真卡验收相同的 14 字节公开测试口令，再
  验证原有含 NUL 和多字节 UTF-8 的 4 字节负载；两次都必须逐字节匹配、进入统一活动状态并
  完成主动/外部卸载闭环。

#### 真卡结果、验证与安全复核

- 用户使用重新编译的正式 ViKey 开发应用输入 `p1.7-real-test`，X:、Y: 两个盘符正常出现；
  随后输入 `p1.7-duress-test`，同样只呈现统一的双盘挂载结果。文件资源管理器浏览卷时，
  非强制安全卸载按设计拒绝并保留可重试会话；关闭占用后可继续安全卸载，不采用强制卸载。
- Debug 全量 CTest **28/28**：26 项通过，2 项按沙箱父进程约束跳过。无卡 WebView2 真实
  Runtime 集成退出 0；MSVC `/analyze` 聚焦构建通过且无新增项目诊断；ASan 下 4 项相关
  单元测试通过，真实 WebView2 集成退出 0，未报告内存错误。
- 安全复核确认新增普通 JavaScript 数组是异步 COM 封送所需的短期可控副本，typed array 在
  复制后立即清零，普通数组在调用结算后立即清零；没有字符串化、日志、持久化、档位实体或
  分类反馈。浏览器/WebView2 内部仍可能产生应用无法主动擦除的短期副本，此风险与既有宿主
  对象边界相同。此次真卡流程只读 `get_blob`，未执行 `put_blob`，也未改写卡内容。

#### 剩余边界

- 本切片完成普通与胁迫口令的真卡挂载和主动卸载验收；物理拔卡触发的自动卸载、页面刷新及
  盘符/驱动无残留仍作为下一独立验收切片，不在本提交中宣称完成。
- 提交：`7c486b1 [P1.7]verify real-card mount and fix password array transport`

### 小切片 7：拔卡强制卸载与 MSVC/Ninja 头文件依赖修复

#### 计划、需求与既有实现复核

- 重新核对 P1.7 的出口条件、基本需求第 3 条、R-03/8.6、既有普通卸载重试、窗口关闭守卫和
  手工安全卸载路径。物理拔卡后卡材料已不可再信任，若卷因资源管理器等进程占用而拒绝普通
  卸载，应优先消除仍公开可见的解密盘符；但用户主动卸载与关窗仍须保持非强制，避免把普通
  busy 状态转换成可能丢失未写入数据的破坏性操作。
- 审阅未提交实现时发现构建证据污染：`dev`、`analyze`、`asan`、`integration`、
  `review-clean`、`asan-debug-diagnostic`、`release-diagnostic`、
  `release-helper-diagnostic`、`vikey-release-diagnostic` 共 9 个 Ninja 目录的
  `msvc_deps_prefix` 被写成乱码，`application_session.cpp.obj` 的 Ninja 依赖数为 0；正常的
  `p1_7_acceptance` 同一目标有 128 条依赖。纯头文件变更因此不会触发重编，曾造成新增纯虚
  函数后旧对象仍参与链接、vtable 槽位错位并在 `noexcept` 路径进入 `std::terminate`。
- 新配置实验确认根因是 CMake 配置时的 Windows 控制台代码页。代码页 936 会生成无法与
  `cl /showIncludes` 输出匹配的 Ninja 前缀并泄露数千行包含信息；配置前切换到 65001 后前缀
  和依赖数据库均正常。`VSLANG=1033` 不能修复该问题。

#### 构建系统修复

- `CMakeLists.txt` 在 MSVC + Ninja 配置期读取活动 Windows 代码页，非 65001 时立即失败并
  指向仓库入口；同时要求 C 与 C++ 的 `CMAKE_*_CL_SHOWINCLUDES_PREFIX` 均存在，避免生成
  头文件依赖状态不可验证的构建目录。该保护不影响 Visual Studio 等其他生成器。
- 新增 `tools/configure-preset.cmd`：先执行 `chcp 65001`，失败则关闭配置，再原样转发 preset
  和附加 CMake 参数。代码页 936 的负向配置探针已确认在生成 Ninja 文件前失败，不再产生
  “配置成功但头文件不重编”的目录。
- 删除并按原有构建类型、测试、UI、ASan、`/analyze`、集成测试和本地格式化 DLL 参数重建
  上述 9 个仅含生成产物的目录；所有 `rules.ninja` 现均记录正确的
  `注意: 包含文件:` 前缀。UI 配置显式复用已存在且此前通过验收的本地 WebView2 SDK，未联网
  获取新依赖。
- 这次缺陷只使“受影响目录中的纯头文件增量构建”证据失效，不等于全部历史验证作废：发生
  `.cpp` 重编、执行 clean-first 或使用正常 `p1_7_acceptance` 目录的证据仍然成立。后续若需
  依赖历史纯头文件验证，应在已修复目录重新执行。

#### 拔卡强制卸载实现与审阅修正

- 新增 `unmount_with_forced_escalation()`：先执行一次普通卸载，使空闲卷仍能正常刷写和关闭；
  只有精确收到 `DriverError::volume_busy` 才立即执行一次强制卸载，其他驱动、版本或传输错误
  原样传播。原有 `unmount_with_retry()` 未改动，继续只在两秒窗口内重试 busy 且永不强制。
- `IApplicationSessionActions` 新增仅供物理移除控制器调用的 `force_unmount_active()`；会话层
  保持 B 后 A 的清理顺序，每个盘只在卸载成功后清除状态。B 失败时保留 A/B，B 成功而 A
  失败时只保留 A，调用方仍可通过手工安全卸载重试准确残留。
- 物理移除新增 `active_session_force_unmounted` 结果。真正使用强制卸载时原生宿主显示警告，
  明确说明未写入数据可能丢失；连强制卸载也失败时才报告会话仍在。WebView 只刷新统一活动
  会话状态，不新增档位、卷类型、盘符或卡身份观察面。
- 手工“安全卸载”、WebView host object 入口与窗口关闭守卫仍只调用原有非强制路径；相关测试
  替身在意外触达强制入口时显式抛错，防止以后无意扩大破坏性策略。

#### 验证与安全复核

- 重新配置后的 Debug 从零执行 135 步构建，未再泄露 `/showIncludes` 行；CTest **28/28**：
  26 项通过，2 项按沙箱父令牌约束跳过。新增测试覆盖强制路径 B→A 顺序、是否实际使用 force
  的返回值、强制失败保留可重试状态、物理移除结果分类，以及非拔卡路径不得调用强制卸载。
- 将 `application_session.h` 时间戳临时推进后执行 Ninja dry-run，准确调度 **43 步**，包括
  `application_session.cpp`、控制器、UI 与 7 个测试消费者；恢复时间戳后工作区内容不变。
  `application_session.cpp.obj` 的依赖数据库由原来的 0 恢复为完整头文件集合。
- MSVC `/analyze` 从零完成 137 步构建，无新增项目诊断；CTest **28/28**，其中 2 项按环境
  约束跳过。AddressSanitizer 从零完成 137 步构建；CTest **28/28**，其中 2 项按相同约束跳过，
  未报告内存错误。`git diff --check` 通过。
- `p1_7_acceptance` 已在 UTF-8 配置保护下重新生成，核心库、挂载诊断及 CTest **28/28** 均已
  刷新通过；但旧的 `usb_vault_vikey_development_app.exe` 仍被既有运行进程占用，链接器无法
  覆盖该单一制品。本轮未擅自终止用户进程；相同开发应用目标已在 Analyze 与 ASan 目录从零
  构建成功。用户随后关闭旧窗口，验收目录中的开发版应用已重新链接成功，现可执行真卡测试。
- 安全复核确认强制策略只由已匹配的物理卡移除事件触发，只对精确 busy 升级，且失败不会吞掉
  活动状态；没有读取 Windows 凭据、加载官方 SDK、连接或改写实体卡、修改容器内容，也没有
  新增秘密、档位或身份持久化。

#### 剩余风险与下一步

- 软件无法保证强制卸载时尚未写入磁盘的数据不丢失；这是物理拔卡后消除持续暴露盘符与数据
  完整性之间的明确取舍，因此 UI 必须保留当前警告，正常卸载路径不得复用此策略。
- 真卡最终验收已完成：real/duress 均能挂载 X/Y；real 会话在资源管理器占用时物理拔卡，
  强制卸载警告、双盘符消失和页面回到统一关闭状态均符合预期；重开后两条口令再次挂载成功，
  排除了卡 blob、容器或驱动状态残留。强制清理与派生口令无关，无需重复一次破坏性拔卡。
- 提交：`b0198d7 [build]fix MSVC Ninja header dependencies`；
  `d852f87 [P1.7]force unmount busy volumes after card removal`。

#### 真卡强制卸载首轮验收与重开故障排除

- 用户从普通 PowerShell 启动重新链接的开发版应用，输入 `p1.7-real-test` 后成功挂出 X/Y；
  保持资源管理器占用并物理拔卡时，应用显示强制卸载及未写入数据可能丢失的警告，两个盘符
  均立即消失，证明 busy → force 的真实 VeraCrypt 驱动路径生效。
- 随后重开应用时 real/duress 均统一失败。排查确认卡和容器没有损坏：此前提供的启动命令把
  实际目录 `ViKeySDK_V4.1.2.37-7.4.7-7` 误写为不存在的
  `ViKeySDK\_V4.1.2.37-7.4.7-7`，会在读取卡之前以统一 backend failure 关闭；诊断期间重新
  链接 helper 后，其内容固定哈希也相应变化，继续传旧哈希会被本地制品身份门拒绝。
- Codex 沙箱内的只读探针另观察到 `LocalAppData` 被 Windows 重定向至 Codex 包的
  `LocalCache`，因路径身份门而按设计拒绝；这只影响包沙箱内的诊断进程，不影响用户从普通
  PowerShell 启动的开发应用。用于显示期望/最终路径的临时诊断改动已撤销，正式源码无残留。
- 使用实际 SDK 目录和重新计算的 helper SHA-256 后，用户重新验证 `p1.7-real-test` 与
  `p1.7-duress-test` 均能正常登录和挂载。这证明首次强制卸载没有改写 32 字节卡 blob，也没有
  破坏两只可丢弃测试容器。

## P2：功能全量

### 小切片 0：取消注册后的卡初始化与双信封认证需求基线

#### 计划、需求与既有实现复核

- 用户于 2026-08-28 明确 P2 新入口：不再需要注册；产品未初始化卡首次插入时要求设置日常
  登录密码，已初始化卡直接进入登录；只有创建带隐藏卷的隐藏盘时才产生诱饵密码，此后两个密码
  均可从同一入口登录；首次建卷向导暂缓，后续单独补齐。
- 重新审计 `需求梳理清单_v2.md` 的 N-02、N-05、N-06、Q-02、Q-06、Q-07、8.3~8.6，及
  `MILESTONES.md` 的 P1 历史任务、P2 出口和任务清单。旧基线把 B 挂载当认证锚点，导致无 B 时
  必须让 A 绑定用户输入并在以后改写卷头；它仍无法覆盖“卡已初始化但 A/B 都不存在”的新状态。
- 检查现有 `ICardStore`、`FixedBlobCardStore`、ViKey 会话、`kdf_v1`、`ApplicationSession`、
  `mount_orchestrator`、`UiSessionController` 和 WebView 页面。当前实现把固定 32 字节 blob 直接
  当 S，页面只有登录/卸载两态；没有产品初始化标记、记录版本、密码认证信封、已认证无卷态或
  损坏记录分类。现有 `mount_orchestrator` 仍按“B 存在则 B 用输入、A 用固定域；只有 A 则 A 用
  输入”分支，不能直接承载新协议。

#### 基线决策与文档修改

- `需求梳理清单_v2.md` 将 2026-08-28 决策声明为优先于旧冲突表述：取消注册，区分厂商设备
  激活与产品初始化，允许初始化后无卷登录，首次建卷向导不自动启动且暂缓实现。
- N-06 改为“登录先认证卡记录、挂载为后续可选动作”。容器 A 从首次创建起统一使用
  `KDF(S,"diskA")`；取消档一派生分支和升档 `ChangePwd()`，保留后者的 Spike 可行性证据但
  不再作为产品依赖。A/B 均存在时仍保持 B→A，作为挂载事务与短暂盘符防护顺序。
- 为避免“卡上有第二密码槽”本身证明隐藏结构，P2 卡记录基线定为固定长度双信封：普通形态的
  主信封由日常密码解封、第二信封为随机占位；带隐藏卷形态的主信封由诱饵密码解封、第二信封由
  日常密码解封。两者等长且密文外观一致，无 `has_duress_password`、档位、模式或信封计数字段；
  只知道诱饵密码时只能证明主信封有效，不能证明第二信封不是随机数。
- P1 的 32 字节裸 S 明确为开发基线而非 P2 持久格式。`ICardStore` 仍保持单一不透明 blob 抽象，
  但后续须设计固定长度版本化记录、认证加密、KDF 域分离、明确未初始化标记、损坏/未知版本失败
  关闭及卡写入崩溃恢复。未知或损坏记录不得当新卡覆盖。
- Q-02 的“持卡即可重置登录密码”重新打开并暂停实现。若诱饵密码持有者可借卡重置日常信封，
  会形成隐藏数据探测/恢复通道；安全协议未独立评审前不提供忘记密码入口。
- `MILESTONES.md` 将 P2 标为已进入 P2.0，首批实现顺序改为卡记录格式与状态判定、卡初始化、
  统一登录；诱饵密码与双信封切换留到带隐藏卷创建流程。首次建卷向导后移但未从最终范围删除。

#### 验证与安全复核

- 本切片仅修改需求、里程碑和开发记录，不改源码、构建配置、实体卡或容器。使用冲突关键词扫描
  复核注册、旧 N-06、B 即认证、32 字节 blob、忘记密码与首次建卷表述；当前有效段均已指向
  2026-08-28 基线，保留的旧语句均标为历史、取消或被替代。
- `git diff --check` 通过；Markdown 表格、标题与新增代码块由文本检查确认无尾随空白。本切片不
  运行 C++ 测试：没有产品代码变化，现有测试也尚无双信封协议可执行；下一实现切片必须先以纯
  内存 codec/state-machine 单元测试覆盖，再接 `ICardStore` 与 UI。
- 安全复核确认新基线不在本地或卡上保存档位/模式/诱饵启用位，主/第二信封对只持诱饵密码者
  保持“有效主记录 + 不可判定随机第二记录”的否认性；认证先于挂载，错误密码不会短暂挂出 A；
  损坏记录失败关闭，避免误初始化覆盖。双信封切换的原子提交仍未设计，因此明确禁止在首次建卷
  向导暂缓期间先落地半套切换。

#### 风险、后续切片与提交状态

- 精确卡记录大小、版本头、AEAD、一次慢 KDF 后尝试两个信封的域分离、随机占位生成、旧 32 字节
  开发卡处置及断电恢复协议尚未定稿；这些属于下一小切片的设计与测试范围，不得由 UI 先行决定。
- 当前真卡内仍是 P1 32 字节测试 blob。后续协议未评审、未提供可恢复备份与明确迁移策略前，
  不得让开发应用把它误判为未初始化并覆盖。
- 用户已于 2026-08-29 决定提交本切片；本次提交只包含需求基线、里程碑与开发记录，不包含
  产品代码。提交完成后进入双信封卡记录格式与纯内存状态机小切片。

### 小切片 1：Card Record v1 固定格式与纯内存双信封 codec

#### 计划、需求与既有实现复核

- P2.0 需求基线已提交为 `2efe7cb [docs]rebaseline P2 card initialization and login`。本切片遵守
  用户“首次建卷向导暂缓”的顺序，只锁定持久格式与纯内存密码操作，不接 UI、`ICardStore`、
  ViKey helper、实体卡写入或建卷事务。
- 重新检查 `SecureBuffer`、`kdf_v1`、Argon2 静态库、Windows CNG 链接及 card/test 目录。现有
  `kdf_v1.cpp` 自己持有 Argon2 mutex 和 512MiB `VirtualAlloc` 回调；若卡记录再复制一套 mutex，
  两条路径可并发占用 1GiB，违反 P-002 的资源边界，因此先抽出进程内共享运行器。
- 当前 `ICardStore` 与 ViKey helper 仍固定传输 32 字节 P1 blob。直接把 216 字节 codec 接进去会
  同时扩大 IPC、真卡地址范围和失败恢复范围，不是同一可逆边界；本切片明确不修改这些接口。

#### 固定格式与实现

- 新增公开 `card_record_v1` 纯内存 API，记录固定 **216 字节**：64 字节 little-endian 公开头，
  后接两个各 76 字节的信封。头含 magic/version/length、16 字节 Argon2 盐、16 字节 record id、
  固定 m=512MiB/t=2/p=1 与 Argon2 v1.3 参数；信封均为 12 字节 nonce、48 字节 ciphertext、
  16 字节 GCM tag，布局不存在档位差异。
- 有效信封的明文为 32 字节 S + 16 字节 marker。密码先经域
  `usb-vault:card-record-master:v1` 的 Argon2id 得到一个 32 字节 master；再用 HMAC-SHA-256、
  record id、槽位号及域 `usb-vault:card-envelope-key:v1` 派生两个 AES-256-GCM key。完整 64 字节
  头和槽位号作为 AAD，阻止参数降级、头篡改或信封换位。
- 初始化生成随机 S、盐、record id、nonce，主信封有效，第二信封由 CNG 随机数完整填充。通用
  `rewrap_card_record_v1()` 只接受互不相同的主/第二密码，把原记录解出的同一 S 封入两边；API
  不出现日常/诱饵、档位、模式或“第二槽启用”状态。登录结果只返回 S，不返回命中的槽位。
- `unlock_card_record_v1()` 对一次输入只运行一次 512MiB Argon2id，再无条件认证两个信封，不因
  主信封成功提前返回；两边统一分配 32 字节结果缓冲，并以掩码选择唯一成功的 S，不按槽位分支。
  错误密码与 GCM tag/密文损坏统一抛 `CardCredentialRejected`；若同一密码异常地同时解开两个
  信封则按歧义记录失败关闭，不猜测使用哪一边。
- 产品未初始化改用受控配卡流程预写的精确 216 字节 marker：`UVBLNK2\0` + 208 个零字节。
  只有逐字节完全匹配才返回 `provisioned_uninitialized`；全零、修改过的 marker、截断或未知内容
  返回 malformed，32 字节 P1 blob 返回独立 legacy 状态，均不能进入覆盖式初始化。
- 抽出内部 `argon2id_runtime`，统一 UTF-8 校验、参数/长度检查、512MiB 工作区分配与重复清零，
  并用一个进程级 mutex 串行所有域。`derive_volume_password_v1()` 改为调用该运行器，公开 API、
  参数、域、输出和锁页语义不变，原 KDF 向量测试保持通过。

#### 测试、静态分析与安全复核

- 新增 `card_record_v1_tests`，真实使用生产 Argon2 参数覆盖：精确 provisioning marker；全零、
  marker 变异、截断、legacy 分类；初始化和主信封登录；错误密码；GCM tag 篡改；双信封改封后
  两个密码解出同一 S；记录长度/公开 magic 不随形态变化；相同双密码拒绝；空/非法 UTF-8；
  未知版本、错误长度与 malformed 记录不得进入密码处理。
- Debug 全量构建通过，CTest **29/29**：27 项通过，2 项按受限父令牌约束跳过；新测试约 44.5s，
  因为真实执行多条 512MiB KDF 路径。原 `kdf_v1_tests` 约 3.6s 并保持全部向量通过，证明共享
  运行器重构没有改变卷密码派生。
- MSVC `/analyze` 全量构建无项目诊断；CTest **29/29**（27 通过、2 跳过）。ASan 全量构建及
  CTest **29/29**（27 通过、2 跳过），未报告内存错误。`clang-format` 已应用到新增/重构 C++，
  `git diff --check` 通过。
- 安全复核确认：S、Argon master、HMAC key、AES key object 与解密明文均在 `SecureBuffer` 中；
  CNG/Argon 失败路径由 RAII 清理；头与槽位均认证；主/第二信封等长且均呈随机外观；没有明文
  密码、命中槽位、诱饵启用、档位或模式持久字段。密码长度差异不是秘密；相同密码比较不提前
  接触卡记录。代码与测试均未读取 Windows 凭据、调用 ViKey SDK、写实体卡或修改容器。

#### 剩余边界与提交状态

- codec 只构造完整的新记录，不声称单次 216 字节卡写具备原子性。下一切片须审计 ViKey 用户区
  写入粒度，设计“写暂存区 → 逐字节回读认证 → 提交选择器/恢复”的协议，再把 helper IPC 与
  `ICardStore` 从 32 字节迁移；没有该协议前不得写开发卡。
- provisioning marker 需要客户硬件方或受控配卡工具预写，不能把任意空白用户区自动视为新卡。
  当前开发卡中的 P1 32 字节测试 blob 会明确显示 legacy；迁移还是拒绝并重新配卡尚待下一切片
  决定。
- 用户已于 2026-08-29 决定提交本切片；提交完成后进入 216 字节记录的卡上暂存、回读验证与
  提交协议小切片。该后续切片先使用伪 SDK/helper 测试，不因本次授权扩大到实体开发卡写入。
- 提交：`300e3be [P2.1]add deniable card record codec`。

### 小切片 2：Card Record Journal v1 可恢复提交协议核心

#### 计划、需求与既有实现复核

- 重新核对 N-02/N-06、Card Record v1 固定 216 字节格式、R-03/8.6 与 P2.1 遗留边界。卡记录
  更新既用于首次初始化，也用于以后双信封改封；两者必须走完全相同的物理协议，不能额外写入
  诱饵启用、档位、模式或操作类型。
- 检查 `ICardStore`、`VikeyCardStore`、helper IPC v5、`VikeySession` 和现有开发 blob 往返测试。
  公共接口仍是单一不透明 blob；ViKey SDK 的 `ReadData/WriteData` 接受 32 位地址和长度，但没有
  可依赖的断电原子保证。现有实现固定在地址 0 整体读写 32 字节，写失败后只能声明状态未知，
  不能直接扩为一次 216 字节覆盖。
- 当前 ViKey 用户区实测容量足以容纳协议，但本切片只建立厂商无关的随机访问协议与伪存储故障
  注入测试，不迁移 helper/`ICardStore`，不加载厂商 DLL、不读取开发凭据，也不写实体开发卡。

#### 协议与实现

- 新增私有 `ICardRecordJournalStorage` 随机访问边界和 Card Record Journal v1。物理范围固定 672
  字节：两个 120 字节提交描述符 + 两个 216 字节逻辑记录槽；上层以后仍只取得单个 216 字节记录。
- 描述符含固定 magic/version/length、随机 16 字节 commit id、随机 16 字节 previous id、槽号、
  记录长度、记录 SHA-256 与描述符 SHA-256。读取固定获取两描述符和两槽；稳定态要求两描述符
  相同，中断态只接受新 `previous id` 唯一指向旧 `commit id` 的有向关系。描述符及摘要只承担
  事务恢复，不包含密码语义，也不替代 Card Record v1 的 GCM 认证。
- 提交流程为：验证完整逻辑记录 → 读取当前提交 → 写非活动槽 → 逐字节回读 → 写非活动描述符并
  回读（提交点）→ 用 CNG 随机数覆盖旧记录槽并回读 → 把同一新描述符镜像到旧描述符并回读 →
  再完整恢复验证。写调用即使报错，只有随后回读完全一致才继续。
- 初版审阅先发现“新描述符的槽损坏时回退旧记录”会造成密码/隐藏结构状态静默回滚，已修正为
  先解析提交链再要求所选记录摘要匹配；已发布记录损坏必须失败关闭。双描述符镜像使
  单份元数据后续损坏仍可读取当前代；只有提交过程中新描述符自身撕裂时才允许使用旧代。
- 随后 R-03/8.6 复核又发现公开递增 generation 会泄露记录更新次数；在初始化后的第一次更新通常
  对应创建隐藏盘时，这会成为形态侧信道。协议因此改用 CNG 随机 commit/previous id 链，稳定态
  只留下两份相同随机描述符，不保存计数；该修正使事务顺序可恢复但无法从当前值推断更新次数。
- 同轮复核还发现固定从槽 0 配卡会泄露更新奇偶，且首次初始化后保留的旧槽会直接暴露
  provisioning marker。现改为配卡时随机选择初始槽并随机填充另一槽；每次提交点后、镜像前随机
  覆盖旧槽。初始槽使用独立且不持久化的一字节 CNG 随机位，不能从 commit/previous id 反推；
  成功稳定态不保留旧逻辑记录，活动槽位也被该随机位掩蔽。
- 为闭合发布描述符后、旧槽清理或镜像前掉电的窗口，读路径会把可判定的中断态作为日志恢复处理：
  先随机覆盖所选记录之外的槽，再镜像所选描述符并重新读取四个区域验证；归一化失败时不向上层
  返回记录。完整新描述符若指向损坏槽仍直接失败，不能借恢复回退旧密码状态。
- 受控 provisioning API 只接受精确 `UVBLNK2\0` 逻辑 marker。普通产品初始化不得调用该入口；
  空白用户区、裸 marker、P1 blob 或无有效描述符状态均不能被自动转换成新卡。

#### 测试、安全复核与剩余边界

- 新增伪随机访问存储测试，覆盖空白物理区失败关闭、受控 provisioning、连续两次槽切换、暂存
  部分写后报错、完整写后报错并由回读确认、暂存回读不一致、提交描述符部分写、完整描述符写后
  报错、旧槽清理撕裂、镜像描述符撕裂、随机初始槽、稳定态旧 marker 消除、随机提交 ID 链、
  单描述符损坏、活动槽损坏禁止回滚、双描述符损坏、容量不足和 malformed 输入写前拒绝。新测试
  在 Debug 下独立构建并通过。
- 安全复核确认记录槽始终用 `SecureBuffer` 承载；普通内存只保存公开描述符和 SHA-256；读路径
  固定读取两描述符与两槽，不按卡形态变化；协议没有持久化命中槽、密码类型或隐藏盘状态。SHA-256
  只检测撕裂/损坏，恶意篡改仍由已初始化记录的 GCM 在认证时拒绝。
- 最终随机提交链版本的 Debug 全量构建和 CTest **30/30**：28 项通过，2 项按受限父令牌约束跳过；
  新日志测试约 0.05 秒。MSVC `/analyze` 全量构建无项目诊断，CTest **30/30**（28 通过、2 跳过）。
  ASan 全量构建和 CTest **30/30**（28 通过、2 跳过），未报告内存错误。
- `clang-format` 已应用；`git diff --check` 通过（仅既有 LF→CRLF 提示）。生产日志协议文件扫描未
  出现诱饵/胁迫/档位/模式、命中槽或 generation 语义。物理偏移编译期断言锁定 120+120+216+216
  = 672 字节，无范围重叠。
- SHA-256 元数据不抵抗可任意改卡并重算摘要的攻击者；它只解决撕裂与偶发损坏。初始化记录的恶意
  修改仍由 GCM 在密码认证时统一拒绝；provisioning marker 本来就是非秘密配卡状态，必须依赖受控
  配卡边界。提交点之后若旧槽清理或镜像失败，写操作会报错，但下次读取会选择已发布记录并完成
  归一化；后续初始化/改封状态机必须在报错后重新读取，不能盲目重建 S。
- 下一独立切片才把 ViKey 会话改为 672 字节范围存储、helper IPC 迁移为 216 字节逻辑记录并更新
  `ICardStore`。当前实体开发卡仍是 P1 数据，迁移或重新配卡未决定，禁止写入。
- 用户已于 2026-08-29 决定把本协议核心作为一个完整安全边界提交；随机提交链、旧槽清理、
  读取归一化、测试和技术基线不拆成会产生不安全中间态的独立提交。提交后进入 ViKey 会话、
  helper IPC 与 `ICardStore` 的 216 字节逻辑记录迁移，仍只使用伪 SDK/测试，不写实体卡。
- 提交：`035d8cb [P2.2]add deniable recoverable card record journal`。

### 小切片 3a：闲置槽可验证清理与 ViKey 会话随机访问接入

#### 审计与设计修正

- 进入 ViKey 适配前重新审计 P2.2 的每个断电点，确认“暂存新记录后、发布新描述符前”断电时，
  旧描述符和旧活动记录确实仍然有效，原子性没有问题；但原描述符只摘要活动槽，重新插卡无法知道
  闲置槽已由随机填充变为部分或完整的新记录，因而可能留下可识别的 `UVCRD2` 头和更新历史痕迹。
  这不是数据恢复缺陷，而是 R-03/8.6 下的可否认性与历史清理缺口。
- 采用闲置槽摘要而非每次读取无条件重写：每个描述符新增 32 字节闲置槽 SHA-256，尺寸由 120
  增为 152 字节，物理布局调整为 152+152+216+216=736 字节。正常稳定读取仍为只读；只有检测到
  中断或闲置槽摘要不匹配时才随机覆盖闲置槽、刷新同一提交的摘要并镜像两份描述符。
- 新提交描述符预先绑定即将覆盖旧槽的随机填充摘要。发布前中断由旧描述符发现暂存槽不匹配并
  清理；发布后、旧槽清理前中断由随机提交链选中新记录，再发现其闲置槽不匹配并补完清理。
  恢复过程中若断电造成“同一 commit id、不同闲置摘要”的两份描述符，只接受当前闲置内容唯一
  匹配的那一份；两边同真或同假且元数据不同则按歧义失败关闭。

#### ViKey 会话边界与测试

- 审计确认 `VikeySession::read_user_blob/write_user_blob`、P1 开发往返探针和 helper IPC v5 仍固定
  32 字节。为保持既有实体卡恢复证据，本切片不改这些路径；新增独立的
  `vikey_card_record_io` 适配层，由私有 `VikeyCardRecordStorage` 实现
  `ICardRecordJournalStorage`，通过会话封装的厂商 `GetUserDataSize/ReadData/WriteData` 执行
  736 字节范围内的随机访问。这样 helper 继续独立编译旧会话代码，不被尚未迁移的 IPC 提前绑定
  到 Card Record 核心。
- 每次会话级日志操作先检查厂商用户区容量；偏移与长度同时验证协议边界和厂商 ABI 整数范围。
  写入先复制到 `SecureBuffer` 临时参数，防止厂商 DLL 修改调用方持有的 216 字节记录。
- 新增独立伪 SDK 测试支持任意合法地址/长度，并在真实日志布局上受控 provisioning。针对性测试已通过：
  ViKey 会话读取 marker、提交并读取完整 Card Record v1、厂商修改写参数不影响调用方，以及容量
  只有 735 字节时在任何厂商读取前拒绝。日志故障测试新增断言，证明部分暂存写失败后重新读取
  返回旧记录，同时再次随机覆盖暂存槽并镜像含新闲置摘要的描述符。
- Debug 针对性构建及 `card_record_journal_v1_tests`、`vikey_card_record_io_tests`、原
  `vikey_session_tests` 均通过。第一次全量构建发现 helper 直接编译 `vikey_session.cpp` 而不链接
  `usb_vault_core`；将日志接入拆成独立适配层后，helper 重新链接通过且旧 IPC 行为未变。
- 拆分测试文件之前的 Debug、MSVC `/analyze`、ASan 全量构建和 CTest 均为 **30/30**（28 通过、
  2 跳过），静态分析无项目诊断，ASan 无内存错误；测试拆分后将重新执行最终 31 项矩阵。本切片
  未加载厂商 DLL、未读取开发凭据、未调用 helper、未写实体卡。helper IPC 和 `ICardStore` 的
  216 字节迁移留在下一独立切片。
- 当前提交状态：未提交，等待全量验证和用户审阅。
- 最终拆分后的 Debug、MSVC `/analyze`、ASan 均完成全量构建，CTest **31/31**：29 项通过，
  2 项按受限父令牌约束跳过；静态分析无项目诊断，ASan 无内存错误。额外在三种配置下单独重跑
  日志测试，覆盖恢复过程在只刷新一份同 commit 描述符后再次中断，随后仍能依据唯一匹配的闲置
  摘要保留旧记录、完成随机清理并恢复镜像。`git diff --check` 通过（仅既有 LF→CRLF 提示）。
- 安全复核确认：正常读取不写卡；仅在描述符不一致或闲置摘要不匹配时归一化；活动记录摘要不符
  仍失败关闭而不回滚；日志与 ViKey 适配层不持久化密码种类、命中槽、档位、模式或更新计数。
  SHA-256 仍只用于撕裂/偶发损坏检测，不声称抵抗可任意改写并重算摘要的攻击者。实体卡迁移和
  受控重新配卡仍未授权、未执行。
- 提交：`e89ca26 [P2.2]bind inactive journal slot integrity`、
  `ceff863 [P2.3]add ViKey card record random access`。

### 小切片 4a：helper IPC v6 固定 216 字节卡记录消息

#### 计划与边界审计

- 审计 `ICardStore`、`VikeyCardStore`、helper backend/client/main、IPC v5 和 P1 开发工具后确认，
  现有 `development_blob_round_trip/read/write` 均明确绑定 32 字节，并承担实体开发卡原值恢复证据。
  直接把它们改成 216 字节会破坏 P1 探针；此时直接把 `VikeyCardStore` 切为 216 字节，又会让尚未
  迁移的 P1 `ApplicationSession` 把 Card Record 错当成 32 字节 KDF 输入。
- 因此本切片只建立可独立审核的 IPC 消息边界：协议升为 v6，保留全部旧操作和 32 字节尺寸，新增
  `card_record_read` 与 `card_record_write`。暂不在 helper main 分派，不新增客户端启动 API，也不
  切换 backend/`ICardStore`；未知或尚未执行的新操作仍不会触卡。

#### 实现与当前验证

- `card_record_read` 请求固定为既有开发会话材料，成功响应固定为 216 字节；
  `card_record_write` 请求固定为会话材料加 216 字节记录，成功响应无载荷。请求头仍固定 32 字节，
  响应头仍固定 40 字节，凭据和记录继续由 `SecureBuffer` 承载。
- 新增独立 `CardRecordWriteRequest` 和读写序列化 API；`valid_request/valid_response` 按操作精确
  选择 32 或 216 字节，不能让旧 blob 读响应冒充卡记录响应，也拒绝 215 字节写入。
- Debug 针对性构建、helper 链接和 `vikey_helper_protocol_tests` 已通过。尚待全量 Debug、MSVC
  `/analyze`、ASan、CTest 和最终安全复核；未启动 helper、未加载厂商 DLL、未读取凭据或写卡。
- 当前提交状态：未提交，等待全量验证和用户审阅。
- 最终 Debug、MSVC `/analyze`、ASan 均完成全量构建，CTest **31/31**：29 项通过、2 项按受限
  父令牌约束跳过；静态分析无项目诊断，ASan 无内存错误。`git diff --check` 通过（仅既有
  LF→CRLF 提示）。
- 安全复核确认 helper main 与客户端尚未把新操作列入会话分派：helper 收到合法 v6 新操作时只会
  通过既有默认分支返回 `invalid_operation`，不会读取凭据体、加载厂商 DLL 或触卡；客户端也没有
  对外启动 API。旧开发操作仍严格要求 32 字节，新卡记录操作严格要求 216 字节，成功读响应尺寸
  不能互相冒充，失败响应仍禁止携带载荷或安全属性。
- 提交：`59b8399 [P2.4]add fixed card record IPC messages`。

### 小切片 4b：helper 卡记录执行与联网开发客户端

#### 计划、实现与边界

- 在 P2.4a 提交后继续审计 helper 的独立构建、客户端启动边界、ViKey 日志适配和跨进程伪 SDK。
  helper 使用静态 CRT 并直接编译一组最小源文件，不能把整个 `usb_vault_core` 链入厂商进程；否则
  会同时引入 Argon2、512MiB KDF 工作区和产品密码解封代码，扩大供应商 DLL 所在进程的秘密面。
- 将仅检查公开 magic/version/长度/KDF 参数的 `inspect_card_record` 从加密实现拆到独立
  `card_record_v1_inspection.cpp`。产品核心仍同时链接 codec 与检查器；helper 只链接公开检查器、
  736 字节日志、ViKey 随机访问、会话与既有安全基础，不链接 Argon2 或双信封解密。
- helper main 正式分派 IPC v6 的 `card_record_read/write`：每个请求复验暂存 DLL 摘要、动态加载、
  打开一次认证会话、执行一次日志读或提交、显式关闭并退出。读成功只回传精确 216 字节锁页记录；
  写不重试，任何不能确认的写入或关闭失败向父端报告错误，不能被解释为旧数据仍有效。
- helper 客户端新增明确标注为 development/network-enabled 的一次性 216 字节读写 API，复用既有
  固定摘要、制品句柄锁、权限过滤、单进程 job、缓解策略、净化环境、nonce、超时和无尾随字节
  验证。旧 `development_blob_*` 仍只接受 32 字节，P1 真卡探针和正式旧 blob backend 未迁移。
- 扩展伪 `ViKey.dll`，在不改变旧 32 字节严格状态机的前提下支持有效镜像日志、任意合法日志范围
  读写和 SHA-256 描述符；跨进程测试新增 marker 读取、完整初始化头写入、调用方记录不被厂商写
  参数修改、215 字节输入在暂存前拒绝，以及临时目录清理断言。正式 `ICardStore`、产品装配、UI、
  Windows 开发凭据和实体卡均未切换或访问。

#### 验证与安全复核

- Debug、MSVC `/analyze`、ASan 三套全量构建均通过；三套 CTest 均为 **31/31**：29 项执行通过，
  2 项按既有受限父进程令牌约束跳过。静态分析无项目诊断，ASan 未报告内存错误；
  `vikey_helper_protocol_tests` 与真实 Argon 参数的 `card_record_v1_tests` 针对性重跑通过，
  `git diff --check` 通过（仅既有 LF→CRLF 提示）。
- 跨进程测试可执行文件及扩展伪 SDK 已在三套配置下以 `/W4 /WX` 编译链接。尝试在 Codex 工具的
  沙箱外执行时，先在既有“development vendor runtime staging”用例被路径身份门拒绝：Codex 包
  将 `LocalAppData` 重定向到 `LocalCache`，与此前开发日志记录一致；未放宽最终路径校验。
  用户随后从普通、非管理员 PowerShell 直接运行 Debug `vikey_helper_process_tests`，完整输出
  `vikey_helper_process_tests passed`，证明既有隔离/暂存/清理路径及新增 marker 读取、216 字节日志
  提交和短输入拒绝用例均在正常桌面令牌下通过。该测试只使用伪 `ViKey.dll`，未连接实体卡。
- 安全复核确认客户端在进程创建和 DLL 暂存前验证三项凭据与输入记录固定长度；协议按操作绑定
  32/216 字节响应解析，不能交叉冒充。所有凭据、卡记录、日志槽和 SDK 写参数均由
  `SecureBuffer` 承载并在作用域退出时清零；公开日志描述符和测试中的合成记录不含产品密码。
  helper 不执行慢 KDF、不获知密码命中信封、不持久化档位、模式、诱饵启用、槽语义或更新计数，
  保持 R-03/8.6 的固定形态约束。
- 正常镜像日志读取不写卡；只有既有日志恢复规则检测到中断态时才归一化。写提交继续执行
  “非活动槽写入并回读 → 新描述符发布 → 旧槽随机清理 → 描述符镜像”，且 helper 不自动重试。
  错误后的正式状态机仍必须重新读取再判断，不能假设失败发生在提交点之前。
- 当前提交状态：未提交，等待用户审阅。下一独立切片才设计并切换正式 216 字节 `ICardStore`
  后端；联网开发 helper 仍不得承载生产材料，实体开发卡迁移或重新配卡仍未授权、未执行。
- 提交：`35130b5 [P2.4]execute journaled card record IPC`。

### 小切片 5：正式 216 字节 ICardStore 适配与 helper 后端

#### 需求、架构与实现

- 重新审计 N-02 单一不透明 blob 合约、N-06 的 216 字节逻辑记录、现有 `ICardStore`、P1
  `VikeyCardStore/VikeyHelperBlobBackend` 和 `ApplicationSession`。公共接口无需增加记录类型、状态
  或厂商概念；但旧 ViKey 适配器静态绑定 32 字节并被 P1 开发应用用于裸根秘密，不能原地改成
  216 字节，否则当前应用会把完整 Card Record 当作 KDF 输入并破坏既有真卡恢复证据。
- 新增独立 `IVikeyCardRecordBackend` 与 `VikeyCardRecordStore`。它们继续实现同一个 `ICardStore`
  get/put/capabilities 合约，但读结果和写输入严格要求 **216 字节**；短读/长读映射为
  `device_failure`，短写/长写在触达后端前以 `invalid_configuration` 拒绝，调用方缓冲所有权不变。
  旧 `IVikeyBlobBackend/VikeyCardStore` 保持 32 字节，不改变 P1 探针、旧 helper backend 或开发应用。
- 抽出共享 `vikey_helper_backend.h`，集中一次操作的三项锁页凭据、凭据提供者和非秘密制品配置；
  原 `VikeyHelperBlobBackendConfig` 更名为不绑定载荷形状的 `VikeyHelperBackendConfig`，开发配置解析、
  Windows 凭据提供者和旧 backend 共用该边界，未改变命令行或凭据目标。
- 新增 `VikeyHelperCardRecordBackend`：每次 get/put 都重新获取一组凭据，分别调用 P2.4b 的
  216 字节客户端 API，保留公共卡错误；凭据提供者、制品验证或非卡错误统一收敛为
  `backend_unavailable`。该目标与 helper IPC 单独链接，不进入当前产品可执行文件。
- 跨进程伪 SDK 测试改为走完整
  `ICardStore → VikeyCardRecordStore → VikeyHelperCardRecordBackend → helper → journal` 链，覆盖
  provisioning marker 读取、完整 initialized 记录提交、调用方数据不变、短记录在加载凭据前拒绝、
  每次操作重新取凭据、凭据提供者失败不启动 helper，以及暂存目录清理。旧 32 字节正式链测试
  保留并继续独立执行。

#### 验证、安全复核与剩余边界

- 新增 `vikey_card_record_store_tests`，覆盖 216 字节二进制透传、调用方所有权、215/217 字节写前
  拒绝、215/217 字节后端读失败关闭、公共错误域保持和 capability 不虚报。旧
  `vikey_card_store_tests`、开发配置测试及新适配器测试均针对性构建并通过。
- Debug、MSVC `/analyze`、ASan 三套全量构建均通过；三套 CTest 均为 **32/32**：30 项执行通过、
  2 项按既有受限父进程令牌约束跳过。静态分析无项目诊断，ASan 未报告内存错误；
  `git diff --check` 通过（仅既有 LF→CRLF 提示）。跨进程测试在三套配置中均已重新编译，但因
  Codex 父令牌受限而按设计跳过。用户随后从普通、非管理员 PowerShell 运行最新 Debug
  `vikey_helper_process_tests`，完整输出 `vikey_helper_process_tests passed`，证明正式 216 字节
  `ICardStore` 组合链、旧 32 字节链、凭据生命周期、helper 隔离和暂存清理均在桌面令牌下通过。
- 安全复核确认公共 `ICardStore` 仍只暴露不透明 blob 和 capability，不暴露物理日志槽、提交 ID、
  供应商地址、凭据或错误码。适配器和 helper 后端不检查密码、不保存命中信封、不持久化记录，
  每次操作后凭据和记录由 `SecureBuffer` 清零；没有新增档位、模式、诱饵启用或更新计数状态。
- 当前产品 `usb_vault_app` 仍只链接旧 P1 32 字节桩/开发装配；本切片不改变登录、挂载或 UI，因而
  不会把 216 字节记录误传给 `derive_volume_password_v1`。下一切片应在产品层新增卡记录状态机：
  先分类 provisioning/initialized/legacy/损坏，再实现初始化和登录，不能直接替换现有
  `ApplicationSession` 的 32 字节读取。
- 提交：`8755a4a [P2.5]add helper-backed card record store`。未读取 Windows 开发凭据、未加载
  官方 SDK、未连接或改写实体卡；联网开发 helper 仍不得承载生产材料。

### 小切片 6：产品层卡初始化与统一认证状态机

#### 需求审计、实现与边界

- 重新审计 N-02、N-06、Q-07、R-03 和 8.6，并检查 Card Record v1 codec、正式 216 字节
  `ICardStore` 与仍使用 P1 32 字节开发链的 `ApplicationSession`。本切片只建立可复用的产品层
  生命周期服务，不提前切换 UI、挂载或旧开发装配；口令强度、二次确认与风险确认属于后续 UI
  接入切片，首次建卷向导继续暂缓。
- 新增 `CardRecordService`，先按公开格式分类 provisioning、initialized、legacy、未知版本和损坏
  状态。只有逐字节精确匹配 provisioning marker 才允许初始化；其余状态通过带观测状态的
  `UnexpectedCardRecordState` 失败关闭，且不会进入密码 KDF 或写卡。登录也只有 initialized v1
  才进入双信封解封，未初始化卡不会被误当作密码错误或自动覆盖。
- Card Record 初始化 codec 新增一次性返回“216 字节记录 + 根秘密 S”的锁页结果。产品初始化在
  写入并验证成功后可直接把同一个 S 交给已认证无卷会话，不必再次执行 Argon2id；保留原只返回
  记录的 API，并在该兼容路径立即清除多返回的 S。
- 初始化在慢 KDF 前后各读取并验证一次精确 provisioning 状态，避免用户计算密码期间卡记录已被
  另一流程改变仍遭覆盖；随后只调用一次 `put_blob`，无自动重试。无论写调用报告成功还是失败，
  都重新读取逻辑记录并与预期 216 字节常量时间比较：只有完全一致才返回 S；写报错但复读完全
  一致视为已提交，无法复读或成功写后内容不一致统一按 `device_failure` 失败关闭；若写报错且旧
  记录仍在，则保留原卡错误供上层呈现。

#### 测试、安全复核与剩余风险

- 新增真实 Argon2 参数的 `card_record_service_tests`，覆盖全部公开状态只读分类、初始化后直接得到
  与再次登录相同的 S、非 provisioning 状态零写入、未初始化状态禁止登录、写后报错但精确复读
  成功、KDF 期间状态变化不覆盖、提交前写失败保留旧 marker 与原错误，以及写后损坏/无法复读
  的失败关闭。codec 测试同时验证初始化返回的 S 与日常密码解封结果一致。
- Debug、MSVC `/analyze`、ASan 三套全量构建均通过；三套 CTest 均为 **33/33**：31 项执行通过、
  2 项按既有受限父进程令牌约束跳过。静态分析无项目诊断，ASan 未报告内存错误。用户已在上一
  切片从普通 PowerShell 运行最新 Debug `vikey_helper_process_tests` 并得到 passed；本切片未改
  helper 或跨进程路径，无需重复实体桌面令牌测试。
- 安全复核确认密码与 S 只经 `SecureBuffer` 流转，初始化仅在精确复读后释放 S；未新增本地配置、
  命中信封、档位、模式、诱饵启用或更新计数状态，错误密码与密文损坏仍由 codec 统一拒绝。公共
  `ICardStore` 继续是单 blob get/put，不暴露物理日志或供应商细节。
- 仍有一个由 N-02 接口能力决定的极短竞争窗口：第二次 provisioning 读取与随后 `put_blob` 之间
  若物理卡被替换，单纯 get/put 无法执行原子 compare-and-swap。后续把服务接入插拔控制器时必须
  在一次操作期间绑定并复验同一设备身份；当前双读已经消除耗时最长的 KDF 窗口，但不虚报完全
  原子化。
- 当前提交状态：用户已批准提交；本地提交将在本记录后创建。未读取开发凭据、未加载厂商 DLL、
  未连接或写入实体卡。
- 提交：`95c3b16 [P2.6]add card initialization and authentication service`。

### 小切片 7a：主 App 自动接纳新卡的需求与安全基线

- 用户决定不提供独立开发配卡 EXE。主 App 可在当前硬件适配器确认目标设备类型、厂商会话可用、
  稳定实体身份且完整读取介质后，对没有 USB Vault 产品记录、P1/外来数据或损坏痕迹的卡直接显示
  日常密码设置页，不增加单独接纳确认页。插卡检测严格只读；密码提交前不得写 marker 或日志。
- 该决策修订了 N-06 中“必须由外部受控配卡预写 marker”和“无描述符用户区一律不能接纳”的旧
  表述，但不放松 P2.6 的逻辑记录门：裸介质先由独立 bootstrap 建立精确 provisioning marker，
  随后仍只能调用既有 `CardRecordService` 初始化。未知版本、损坏产品记录、P1 旧 blob、外来非空
  数据、读取失败、短读、未激活/凭据不匹配或实体身份变化均不得转成未初始化。
- 审计现有日志代码发现两个后续实现点：首次日志建立尚未暴露只读介质足迹分类，且现有
  `provision_card_record_journal_v1` 可由调用方直接覆盖任意 736 字节介质；后续须增加“无产品
  足迹/可恢复记录/bootstrap 中断/损坏或外来”边界并限制写入口。初始化页面停留期间 helper 会话
  不保持，因此 ViKey 适配还须把 `VikeyGetHID` 的硬件 ID 纳入操作前后复验，不能只依赖 VID/PID
  或“恰好只有一张卡”。
- 同时发现日志读取目前在返回逻辑记录前只允许 provisioning/initialized v1，导致物理日志完整的
  未来版本记录无法到达 P2.6 的 `unsupported_version` 分类。下一代码切片须把“物理日志完整性”与
  “逻辑记录版本支持”分层：读取可返回固定 216 字节未知版本，写入仍只接受当前完整格式，旧版
  App 因而能明确提示升级而不会把新版本卡当作损坏或待接纳卡。
- 首次 bootstrap 仍有物理事实限制：任意非原子介质的第一笔写都可能在产品意图完整落盘前撕裂。
  协议必须尽早写入可识别意图、对完整 marker/有效描述符中断自动恢复，并对无法证明来源的部分
  痕迹失败关闭；不得用本地进度文件补洞。该边界与日志内已有提交恢复分开测试。
- 当前切片先锁定技术真相与小切片边界，未修改代码、未运行厂商 DLL、未读取凭据或访问实体卡。
  下一实现切片先做厂商无关的介质足迹检查和可恢复 bootstrap，再接 ViKey 硬件 ID/helper IPC，
  最后接主 App 初始化 UI。
- 当前提交状态：用户已批准提交；本地提交将在本记录后创建。
- 提交：`e8186a8 [docs]rebaseline automatic card enrollment`。

### 小切片 7b：厂商无关介质足迹与可恢复 bootstrap

#### 实现与兼容性修正

- 在 736 字节日志私有边界新增只读 `CardRecordJournalMediaState`：`no_product_evidence` 只表示固定
  位置没有已知产品前缀，不自行授权覆盖；`provisioning_interrupted` 只接受槽内完整精确 marker 且
  尚无有效描述符；有效描述符、当前/未来记录前缀、撕裂描述符前缀以及不完整 marker 均归入
  `product_evidence`。检查固定读取两份描述符和两个槽，不执行日志归一化或任何写入。
- 新增幂等 `bootstrap_uninitialized_card_record_journal_v1`：无产品足迹或可证明的完整 marker 中断可
  建立/重建日志；已有可恢复 provisioning 日志直接成功且不重写；initialized、未来版本或损坏产品
  证据拒绝覆盖。底层 provisioning 原语也增加产品证据门，不能再直接覆盖已有有效日志。
- 首次写 marker 若只落下部分前缀，下一次按损坏产品证据失败关闭，不猜测其来源；只有完整 marker
  已回读、后续描述符撕裂时才自动继续。首笔随机闲置槽写在任何产品意图落盘前中断时仍可从调用方
  已授权的无足迹状态重新开始，符合非原子介质无法证明更早意图的物理边界。
- 修正物理日志读取和逻辑版本分类的层次：只要 v1 日志描述符及活动槽摘要完整，读取就返回固定
  216 字节逻辑记录，不再提前限制其必须是当前 provisioning/initialized v1；写入仍只接受当前完整
  格式。由此未来版本记录能到达 P2.6 `unsupported_version`，旧 App 可提示升级而不是误报损坏或
  把卡送入接纳路径。

#### 测试与安全复核

- 扩展日志故障测试，覆盖：空白介质只读无足迹、撕裂描述符/部分 marker 作为产品证据、完整 marker
  中断可恢复、部分 marker 重试零写入失败、描述符中断后 bootstrap 恢复、有效 marker 幂等零写入、
  直接重复 provisioning 拒绝、initialized/损坏 initialized/未来版本零覆盖，以及人工构造摘要完整
  的未来逻辑记录可原样上送 `unsupported_version`。
- Debug、MSVC `/analyze`、ASan 三套全量构建均通过；三套 CTest 均为 **33/33**：31 项执行通过、
  2 项按既有受限父令牌约束跳过。静态分析无项目诊断，ASan 未报告内存错误；`git diff --check`
  通过（仅既有 LF→CRLF 提示）。本切片没有读取开发凭据、加载厂商 DLL、启动 helper 或访问实体卡。
- 安全复核确认介质无产品足迹状态不包含厂商、设备、档位、模式、信封命中或诱饵启用信息，也不
  持久化到本地；它只是后续硬件适配器判定待接纳卡的一个输入。当前仍未解决检查与写入之间的换卡
  窗口，也未识别 ViKey P1/外来非空数据；这两项明确留给下一独立切片的稳定硬件 ID 和 ViKey 私有
  介质策略，主 App 尚不能据此自动接纳实体卡。
- 当前提交状态：用户已批准提交；本地提交将在本记录后创建。
- 提交：`47e5d2e [P2.7]add recoverable card media bootstrap`。

### 小切片 8a：ViKey 会话稳定硬件身份绑定

#### 需求审计与实现

- 重新审计自动接纳基线、ViKey SDK ABI、动态符号加载、会话生命周期及随机访问路径。ABI 与模块层
  已经声明并强制加载 `VikeyGetHID`，但会话此前只保存 SDK 索引 0；拔出原卡后插入另一张卡时，
  同一 helper 进程内的后续用户区操作仍可能落到新卡，不能满足 P2.6 初始化防换卡要求。
- `VikeySession` 现将 `VikeyGetHID` 设为必需函数。确认恰好一张设备后、设置 API Key 前读取一次
  硬件 ID，登录并确认用户权限后再次读取；任一调用失败、返回零或两次值不一致均失败关闭，并按
  是否已经登录执行 `logoff + uninitialization` 或仅 `uninitialization`。
- 成功会话只在进程内保存该非零 ID，并通过只读访问器供后续 helper 协议绑定使用。每次固定 blob、
  日志容量、随机读和随机写在触碰用户区前都重新读取 ID；不一致或无法读取时不执行数据操作。
  参数长度/范围仍先本地拒绝，非法调用不会仅为身份检查而触达厂商 SDK。

#### 测试、安全复核与剩余边界

- 扩展 `vikey_session_tests`，覆盖稳定 ID 成功绑定、缺失 `get_hid`、首次通信失败、零 ID、认证过程
  换卡、活动会话换卡和活动会话身份通信失败；验证所有失败均在用户区容量/读写前停止，并保持精确
  清理顺序。ViKey 日志 IO 伪 SDK 同步提供固定非零 ID。
- Debug、MSVC `/analyze`、ASan 三套全量构建与 CTest 均通过；每套 **33/33**：31 项执行通过、
  2 项按既有受限父令牌约束跳过。静态分析无项目诊断，ASan 未报告内存错误。由于本切片改变了
  helper 内部会话调用序列，Codex 受限父令牌下直接执行按设计返回 skip；用户随后从普通、非管理员
  PowerShell 运行最新 Debug `vikey_helper_process_tests`，完整输出
  `vikey_helper_process_tests passed`，确认新增 `VikeyGetHID` 调用未破坏 helper 隔离、伪 SDK
  会话、216 字节记录链及暂存清理。该测试使用伪 `ViKey.dll`，未访问实体卡。
- 安全复核确认硬件 ID 不是秘密，不写卡、不落盘、不进入日志，也不携带档位、模式、信封命中或
  诱饵启用信息。检查与紧随其后的单次厂商读写之间仍不存在 SDK 原子绑定，属于接口物理极限；
  当前已封闭同一会话内可观察的换卡窗口。
- 仍未封闭“主 App 一次检查后，下一次 helper 进程打开的是另一张卡”的跨进程窗口。下一独立切片
  必须把期望硬件 ID 纳入 helper IPC 请求并由新会话比对，再实现 ViKey 私有的无产品痕迹/P1/外来
  介质分类；在此之前主 App 仍不得自动 bootstrap 实体卡。
- 当前提交状态：用户已批准提交；本地提交将在本记录后创建。
- 提交：`9e50961 [P2.8]bind ViKey sessions to hardware identity`。

### 小切片 8b：helper IPC 跨进程硬件身份绑定

#### 需求审计与实现

- 审计确认 P2.8a 只能封闭单个 `VikeySession` 内的换卡窗口：正式 backend 每次 get/put 都启动新
  helper，新进程此前只知道“当前索引 0”，无法证明它仍是主进程先前检查的实体卡。仅给读写请求
  增加 ID 也不完整，因为主进程还缺少一个不触碰用户区的可信身份建立入口。
- helper IPC 升级到 v7，新增 `card_identity_probe`。它沿用固定摘要、锁页凭据、nonce、净化环境、
  受限进程和超时边界，打开完整认证会话后返回 P2.8a 已经登录前后复验的非零硬件 ID，随后关闭
  会话；不查询用户区容量，不读取或写入任何卡记录。
- `card_record_read/write` 的固定载荷各增加 4 字节非零期望硬件 ID。协议 writer、reader 和客户端
  启动前验证均拒绝零值；helper 登录后先与会话 ID 比对，错配映射为 `device_failure`，并在任何
  日志容量查询或随机访问前关闭。会话本身仍会在每个物理用户区操作前再次调用 `VikeyGetHID`。
- 正式 `VikeyHelperCardRecordBackend` 构造时必须接收一次插卡上下文绑定的期望 ID，并在每次独立
  get/put 中原样传递；它不会在错配后自动探测或静默改绑。P1 的 32 字节
  `development_blob_*` 消息、backend 和探针载荷保持不变。

#### 测试、安全复核与剩余边界

- 协议测试覆盖 v7 身份操作、固定载荷尺寸、身份请求凭据往返、卡记录读写 ID 往返、零期望 ID
  发送前拒绝，以及成功身份响应必须为非零。Debug 定向构建 helper、协议测试和跨进程测试目标
  通过，`vikey_helper_protocol_tests` 已执行通过。
- 跨进程测试扩展为“身份探针得到伪 SDK ID 1 → 绑定 ID 的正式 216 字节读写成功”，并覆盖错配
  ID 返回 `device_failure`、零 ID backend 构造失败、凭据不重试和暂存目录无残留。Codex 父进程
  令牌受限，当前直接执行仍按设计 skip。用户首次从普通桌面令牌补跑时，测试在正式卡记录阶段
  返回 `invalid_operation`；定位确认新增身份探针在认证并读取硬件 ID 后直接登出，而伪 SDK 的
  严格状态机遗漏了这条不访问用户区的合法关闭路径。生产代码调用序列无误，测试替身现允许从
  `level_read` 登出，避免为迎合替身而给生产路径加入无意义的额外 SDK 调用；修复后仍待桌面令牌
  复跑确认。用户随后在普通、非管理员 PowerShell 复跑最新 Debug 产物，完整输出
  `vikey_helper_process_tests passed`，确认身份探针、绑定 ID 的 216 字节读写、错配拒绝及
  暂存清理全部通过。该测试仅使用伪 `ViKey.dll`，未访问实体卡。
- Debug、MSVC `/analyze`、ASan 三套全量构建与 CTest 均通过；每套 **33/33**：31 项执行通过、
  2 项按既有受限父令牌约束跳过。静态分析无项目诊断，ASan 未报告内存错误；协议测试在三套配置
  中均实际执行通过。
- 安全复核确认硬件 ID 是非秘密、易失的供应商私有材料，不进入 Card Record、磁盘、本地配置、
  日志或厂商无关 `ICardStore`，不携带档位、模式、信封命中或诱饵启用信息。身份探针与后续 helper
  操作之间可发生换卡，但后续请求持有旧 ID，因而只会失败而不会改绑；SDK 单次身份检查与紧邻读写
  之间仍无硬件原子事务，由 P2.8a 的逐操作复验缩至厂商调用本身无法消除的窗口。
- 下一切片仍须实现 ViKey 私有介质策略，区分日志、P1 旧 32 字节数据、外来非空数据和真正无产品
  痕迹；在该策略完成并绑定一次插卡上下文前，主 App 不得自动 bootstrap 实体卡。
- 当前提交状态：用户已批准提交；本地提交将在本记录后创建。
- 提交：`56bc3e9 [P2.8]bind helper requests to card identity`。

### 小切片 9a：ViKey 会话内只读介质分类

#### 需求审计与实现

- 审计自动接纳基线、厂商无关日志足迹、ViKey 随机访问和 P1 固定 blob 后确认：P1 只在地址 0
  保存无 magic 的 32 字节随机根秘密，因此“前 32 字节非零、其余空白”无法与同形外来数据可靠
  区分。两者都必须阻止自动覆盖，故适配器公开诚实的合并分类
  `legacy_p1_or_foreign_prefix`，不向 UI 或业务层声称已经证明来源。
- 新增 ViKey 私有 `VikeyCardMediaState` 和严格只读 `inspect_vikey_card_media`。它先复用 736 字节
  日志证据检查：现有/损坏产品痕迹归 `product_evidence`；精确 provisioning 中断仅在日志区外
  仍全零时归 `provisioning_interrupted`，否则归 `conflicting_evidence` 并阻止恢复。
- 没有产品证据时才按 4 KiB 锁页分块扫描完整用户区；完整逐字节全零为 `blank`，仅前 32 字节
  含非零且剩余全部为零为 P1/同形外来前缀，其余为 `foreign_data`。扫描发现足以拒绝接纳的非零
  证据后可以提前停止；只有 `blank` 和 P1 同形判断必须成功读到用户区末尾。
- 每次容量和范围读取仍由 `VikeySession` 在供应商调用前复验同一硬件 ID；容量小于 736 字节在
  首次数据读取前拒绝。分类不写卡、不建立本地状态，也尚未增加 helper IPC 操作或触发 bootstrap。
  当前只认可全零为空白是保守的 ViKey 适配规则，并非已证明所有新卡批次都采用该出厂填充值；
  IPC 接入后仍须用客户新购卡做一次只读实测，未知填充值只会失败关闭而不会被覆盖。

#### 测试、安全复核与剩余边界

- `vikey_card_record_io_tests` 的伪用户区扩为与当前实体卡一致的 8192 字节，覆盖完整全零、P1 同形
  前缀、末字节外来数据、精确 provisioning 中断、日志外冲突数据、有效产品日志以及不足 736
  字节容量；断言检查路径不执行任何 SDK 写调用，容量不足也不进入 SDK 数据读取。
- Debug、MSVC `/analyze`、ASan 三套全量构建和 CTest 均通过；每套 **33/33**：31 项实际通过、
  2 项按既有受限父令牌约束跳过。`/analyze` 无项目诊断，ASan 未报告内存错误。此次没有协议或
  helper 分派变化，上一切片已由用户在普通桌面令牌验证跨进程测试通过；本切片未读取开发凭据、
  未加载厂商 DLL、未访问或写入实体卡。
- 安全复核确认扫描缓冲为固定最大 4 KiB 的 `SecureBuffer`，不会按供应商声明容量一次性分配巨量
  内存；所有读取均保持会话身份复验。产品证据优先于空白启发式，部分产品 magic 不会因其余字节
  为零而降级成新卡。全零外来介质与真正出厂空白在物理上不可区分，但两者均没有可保存的非零
  用户数据，符合用户已经锁定的“可访问 ViKey 且完全无产品痕迹时直接初始化”口径。
- 下一切片应把该枚举作为绑定期望硬件 ID 的只读 helper IPC 结果，并让正式 backend 建立一次插卡
  上下文；仍不得在探测操作里写 marker。之后再单独实现提交密码时的“复验同一 ID + 复验介质状态
  + bootstrap”事务。当前提交状态：用户已批准提交；本地提交将在本记录后创建。
- 提交：`3f5853e [P2.9]classify ViKey user media read-only`。

### 小切片 9b：helper IPC 原子建立介质上下文

#### 需求审计与实现

- 审计 P2.8b 身份探针与 P2.9a 只读分类后确认，若主 App 分别启动两个 helper 获取硬件 ID 和
  介质状态，两次进程之间仍可换卡，无法证明分类属于随后绑定的实体。P2.9b 因此只合并这两个
  只读动作，不提前实现密码提交、marker 写入或 bootstrap。
- helper IPC 升级至 v8，新增固定凭据载荷的 `card_media_probe`。helper 在同一完整认证会话中
  保存登录前后已复验的非零硬件 ID，执行完整用户区分类，显式关闭会话后一次返回 ID 和严格枚举
  状态。成功响应新增独立 `result` 字段，避免截断 ID 或混用 `detail`；协议拒绝 v7 降级、零 ID、
  零值/越界状态、其他操作携带状态，以及失败响应残留状态。
- 联网开发客户端新增 `VikeyCardMediaProbe`，只映射六个受协议验证的厂商私有物理状态，同时保留
  helper 受限边界属性。正式卡记录进程测试改为用该原子探针建立插卡上下文，再把返回 ID 绑定到
  后续 216 字节读写。原有 `card_identity_probe` 保留兼容与独立身份探测用途。
- 介质分类存储适配器在一次检查中缓存已验证容量，避免重复调用供应商容量接口；所有实际范围读取
  仍逐次复验会话硬件 ID。探针不执行任何用户区写入，也不创建本地介质状态。

#### 测试、安全复核与剩余边界

- 协议测试新增 v8 介质请求凭据往返、ID/状态响应往返、v7 降级拒绝，以及零 ID、未知状态、跨
  操作状态字段和失败残留状态的负向覆盖。Debug 聚焦构建 helper、协议测试和进程测试目标通过，
  `vikey_helper_protocol_tests` 实际执行通过。
- Debug、MSVC `/analyze`、ASan 三套全量构建和 CTest 均通过；每套 **33/33**：31 项实际通过、
  2 项按既有受限父令牌约束跳过。`/analyze` 无项目诊断，ASan 未报告内存错误。由于 IPC 版本和
  helper 内部调用序列发生变化，`vikey_helper_process_tests` 仍须由用户在普通、非管理员桌面
  PowerShell 对最新 Debug 产物补跑；用户随后运行最新 Debug 产物并得到完整输出
  `vikey_helper_process_tests passed`，确认原子介质探针、绑定 ID 的正式卡记录链、受限进程边界
  和暂存清理均通过。该测试使用伪 `ViKey.dll`，不访问实体卡。
- 安全复核确认返回的硬件 ID 和介质分类均为非秘密、易失的适配器私有信息，不携带用户区原文、
  凭据、密码命中、信封、模式、档位或诱饵启用信息，不落盘、不写日志、不进入厂商无关 Card
  Record。失败路径清零 `result`，客户端只接受固定枚举；扫描保持产品证据优先和完整读取要求，
  探针本身绝不写卡。探测过程中的每次供应商用户区读取前仍复验同一 ID；无法消除的只剩单次 SDK
  调用内部硬件物理窗口。
- 下一独立切片是用户提交日常密码后的初始化事务：携带本次上下文的期望 ID，在写卡前重新检查
  同一实体及允许的 `blank`/可恢复状态，再执行可恢复 bootstrap 和既有 marker 初始化服务。
  在该事务完成前，主 App 仍不得仅凭探测结果写入实体卡。当前提交状态：全部验证通过，用户已
  批准提交；本地提交将在本记录后创建。
- 提交：`0e99624 [P2.9]probe ViKey card media atomically`。

### 小切片 10a：ViKey 会话内安全 bootstrap 原语

#### 需求审计与实现

- 审计 v8 插卡上下文、厂商无关 bootstrap 和 ViKey 分类后，将写事务继续拆成可独立验证的适配器
  原语与后续 IPC 两层。本切片不改变协议或 helper 分派，只实现同一已认证稳定会话中的写前状态
  复验与可恢复 marker 日志建立。
- 新增 `bootstrap_vikey_card_media`，调用者必须给出插卡上下文原先观察到的精确状态。接口只接受
  `blank` 和 `provisioning_interrupted`；P1/同形前缀、外来数据、冲突证据和产品证据即使由调用者
  主动传入，也会在读取介质前作为配置错误拒绝。
- 原语复用同一个带容量缓存的 ViKey 随机访问存储，完整重做介质分类并要求实际状态与期望状态
  逐值一致；不一致映射为 `device_failure`，且不进入任何供应商写调用。准入后调用既有可恢复
  journal bootstrap，后者写后逐字节复读逻辑 provisioning marker；原语再扫描 736 字节日志区
  之外的完整用户区，发现并发出现的非零尾部时失败关闭。
- 将纯公开 framing 的 `provision_uninitialized_card_record_v1` 实现从密码学记录实现单元移动到
  只读 framing 检查单元，使 SDK 隔离 helper 后续可生成同一 216 字节 marker，而无需链接 Argon2、
  信封加密等主产品密码学实现；公开 API 和 marker 字节格式没有变化。

#### 测试、安全复核与剩余边界

- `vikey_card_record_io_tests` 新增完整全零首次 bootstrap 和精确 provisioning 中断恢复，均复读
  逻辑 marker，并确认日志区外仍全零。负向测试覆盖“原观察为空白但实际变为中断”“空白介质出现
  外来尾字节”，两者均返回 `device_failure` 且写计数不变；四种不允许的期望状态均在任何 SDK
  读写前返回 `invalid_configuration`。
- Debug、MSVC `/analyze`、ASan 三套全量构建和 CTest 均通过；每套 **33/33**：31 项实际通过、
  2 项按既有受限父令牌约束跳过。`/analyze` 无项目诊断，ASan 未报告内存错误。协议、helper 主
  分派和进程调用序列未变化，因此本切片不要求重复普通桌面令牌进程测试；helper 目标已在三套
  配置中重新编译并验证新的 framing 链接边界。本切片未加载厂商 DLL、未访问或写入实体卡。
- 安全复核确认写权限只来自适配器允许状态与精确复验的交集，不能由调用者把受保护状态降级成
  空白。所有范围读写仍在每次供应商调用前复验会话硬件 ID，写入仅限既有 736 字节日志保留区；
  marker 非秘密，不含密码、根秘密、信封、模式、档位或诱饵信息。供应商 SDK 不提供跨调用硬件
  事务，完整分类与首个写调用之间仍存在无法彻底消除的物理窗口；写后全尾区复验可发现但无法回滚
  外部并发写入，因此后续 helper 仍必须先绑定期望硬件 ID，且任何异常都不得自动重试或改绑。
- 下一切片 P2.10b 应把期望硬件 ID 与精确期望介质状态加入固定 IPC 请求，在同一 helper 会话先
  比对 ID，再调用本原语并显式关闭，客户端只暴露一次性开发 bootstrap 操作。当前提交状态：实现、
  测试和安全复核完成，用户已批准提交；本地提交将在本记录后创建。
- 提交：`d504575 [P2.10]guard ViKey media bootstrap`。

### 小切片 10b：IPC v9 同卡/状态绑定 bootstrap

#### 需求审计与实现

- 审计 P2.10a 原语、v8 插卡上下文和正式 216 字节 backend 后确认，bootstrap 请求必须同时携带
  探针得到的硬件 ID 与当时的精确介质状态；只携带 ID 会允许 UI 停留期间介质状态变化后仍写卡，
  只携带状态则无法封闭跨 helper 换卡窗口。
- helper IPC 升级至 v9，新增固定 208 字节 `card_media_bootstrap` 请求：厂商摘要、非零期望 ID、
  4 字节严格状态和三个固定长度厂商凭据。writer、reader 和客户端启动前验证均只接受 `blank` 或
  `provisioning_interrupted`；请求不含日常登录密码、根秘密、marker 副本或最终 Card Record。
- helper 在受限联网开发边界打开完整认证会话，先逐值比对期望硬件 ID，错配时在任何容量查询、
  用户区读写前失败；随后把严格 wire 状态映射到 P2.10a 原语，完整复验并执行可恢复 bootstrap，
  最后显式关闭。成功响应不携带 detail、result 或 payload，失败不重试、不探测新 ID、不改绑。
- 客户端新增一次性开发 bootstrap 入口；四种不允许的内部介质状态在验证 helper 和厂商制品、创建
  暂存目录或启动子进程前返回 `invalid_configuration`。现有探针、P1 blob 和 216 字节读写语义不变。

#### 测试、安全复核与剩余边界

- 协议测试覆盖 v9 操作和固定载荷、摘要/ID/状态/锁页凭据往返、零 ID 与不允许状态发送前拒绝、
  v8 降级拒绝及无数据成功响应。严格伪 SDK 扩为 8192 字节用户区，并以独立测试 PIN 提供每个
  helper 进程全空白介质，不影响既有产品日志路径。
- 进程测试新增“ID 1 + `blank` 成功建立并内部复验 marker”“错 ID 2 返回 `device_failure` 且在
  容量查询前关闭”“期望中断但实际空白返回 `device_failure`”，以及外来状态由客户端在启动前
  返回 `invalid_configuration`；所有路径断言无暂存目录残留。
- Debug、MSVC `/analyze`、ASan 三套全量构建和 CTest 均通过；每套 **33/33**：31 项实际通过、
  2 项按既有受限父令牌约束跳过。`/analyze` 无项目诊断，ASan 未报告内存错误。协议测试在三套
  配置中实际通过；由于 IPC 版本、helper 分派和伪 SDK 调用序列均变化，仍待用户在普通、非管理员
  PowerShell 运行最新 Debug `vikey_helper_process_tests`。用户随后补跑并得到完整输出
  `vikey_helper_process_tests passed`，确认 v9 bootstrap 成功、错 ID/错状态拒绝、既有 helper
  隔离与卡记录链以及暂存清理均通过；该测试只使用伪 `ViKey.dll`，未访问实体卡。
- 安全复核确认写授权是“受限 wire 状态、旧插卡上下文 ID、实际会话 ID、重新分类状态”四者的
  交集；错卡在用户区访问前拒绝，错状态在首个写调用前拒绝。IPC 只增加两个非秘密、易失字段，
  不携带或记录密码、根秘密、信封命中、模式、档位或诱饵信息。marker 写仍限制在 736 字节日志区，
  保持既有写后复读和尾区复验；供应商 SDK 单次调用内部物理窗口仍是不可消除边界。
- 下一切片应在产品层编排“v8 探测上下文 → 用户确认/强度合格的日常密码 → v9 bootstrap → 绑定
  同一 ID 的正式 `ICardStore` → 既有 Card Record 初始化服务”，并定义中途失败后的恢复入口。
  当前提交状态：全部验证通过，用户已批准提交；本地提交将在本记录后创建。
- 提交：`5a959ab [P2.10]bind helper media bootstrap context`。

### 小切片 11a：正式 ViKey 初始化适配边界

#### 需求审计与实现

- 审计 P2.6 厂商无关初始化服务、P2.9b 易失插卡上下文、P2.10b 同卡/状态绑定 bootstrap、正式
  216 字节 helper backend 与开发凭据生命周期后确认：产品层下一步需要的是可替换硬件适配器，
  不应让通用 `CardRecordService`、密码入口或 UI 直接调用 ViKey 私有 helper API。本切片只建立
  该边界，不提前接入日常密码、生成根秘密、写最终 Card Record 或改变 UI。
- 新增 ViKey 私有 `VikeyHelperCardEnrollment`。对象只保存非秘密 helper/厂商制品配置并借用
  凭据提供者；`probe()` 每次取得一套新的锁页开发凭据，调用 v8 原子介质探针并返回仅含非零
  硬件 ID 与严格介质状态的易失上下文；`bootstrap(context)` 再取得另一套新凭据，调用 v9 写事务。
  探测凭据不会跨 UI 等待期保留或复用，适配器也不接收日常登录密码。
- bootstrap 入口在加载凭据、验证制品、建立暂存目录或启动 helper 前拒绝零硬件 ID，以及
  `legacy_p1_or_foreign_prefix`、`foreign_data`、`conflicting_evidence`、`product_evidence`
  四类不可写状态；只把 `blank` 或 `provisioning_interrupted` 连同原始硬件 ID 原样交给 v9。
  helper 仍负责重新认证、比对实际 ID、完整重分类和可恢复 marker 写入，因此适配器不会把旧
  探针结论当作当前写权限。

#### 测试、安全复核与剩余边界

- 进程测试新增正式初始化适配链：独立测试 PIN 的空白伪卡经适配器探测得到 ID 1/`blank`，再
  通过适配器 bootstrap；断言两次操作分别加载一套新凭据。零 ID 与外来状态均在凭据加载前返回
  `invalid_configuration`，凭据提供者失败只调用一次、归一化为 `backend_unavailable`，并检查
  所有路径无 helper 暂存目录残留。
- Debug、MSVC `/analyze`、ASan 三套全量构建和 CTest 均通过；每套 **33/33**：31 项实际通过、
  2 项按既有受限父令牌约束跳过。`/analyze` 无项目诊断，ASan 未报告内存错误。新增正式适配
  成功链位于受限环境会跳过的 `vikey_helper_process_tests`；用户随后在普通、非管理员桌面
  PowerShell 对最新 Debug 产物补跑并得到 `vikey_helper_process_tests passed`。测试仅使用伪
  `ViKey.dll`，不会访问实体卡。
- 安全复核确认适配器没有持有、接收或记录日常密码、根秘密、卡记录、信封命中、模式、档位或
  诱饵启用信息；每套厂商凭据只驻留于一次调用的锁页缓冲，并在一次性 helper 返回或异常展开后
  销毁。插卡上下文只含非秘密且不持久化的厂商私有 ID/介质枚举；无效上下文在秘密取得和任何
  外部副作用前失败。helper 的同卡、精确状态复验、写后复读及不自动重试边界保持不变。
- 下一切片 P2.11b 才在厂商无关产品编排层组合“适配器探测 → 用户提交已确认且强度合格的日常
  密码 → 适配器 bootstrap → 用同一硬件 ID 构造正式 `ICardStore` → 既有初始化服务”，并定义
  中断后重新探测/恢复的状态机。本切片全部验证通过，用户已批准提交；本地提交将在本记录后创建。
- 提交：`7dbab36 [P2.11]add formal ViKey enrollment adapter`。

### 小切片 11b：厂商无关新卡初始化事务

#### 需求审计与实现

- 审计 P2.6 `CardRecordService`、P2.11a ViKey 私有探测/bootstrap 适配器、正式 216 字节 store、
  密码入口和强度需求后，将本切片锁定为可独立审查的产品写事务：它只接收已由后续 UI 提交层完成
  二次确认和强度策略验收的单个日常密码，不在没有明确产品口径时擅自发明强度算法，也不提前改
  登录、插拔路由或首次建卷向导。
- 新增厂商无关 `ICardInitializationTarget` 和 `CardInitializationService`。目标必须把“确保精确
  provisioning marker”和随后返回的正式 `ICardStore` 绑定到同一物理插卡上下文；服务固定先
  调用前者，成功后才把密码移交既有 `CardRecordService::initialize_and_authenticate`，从而复用
  精确 marker 前置检查、KDF 后二次状态检查、可恢复写、逐字节回读和成功直接返回根秘密 S。
- 新增 `VikeyHelperCardInitializationTarget`，只接受非零 ID 且介质状态为 `blank` 或
  `provisioning_interrupted` 的上下文。它用同一 ID 同时构造 P2.11a bootstrap 适配器和正式
  216 字节 backend/store；bootstrap 每次使用新鲜锁页厂商凭据，后续每次 store 操作仍重新认证
  并在 helper 内比对原 ID，不把 ViKey 类型暴露给通用初始化服务。
- 目标仅在 v9 bootstrap 已验证成功后于进程内缓存“marker 已确保”，以便随后 Card Record 写失败
  时同一事务对象不重复写 marker。第一次 bootstrap 尝试前即把上下文置为一次性失败态；任何异常
  后再次调用都会在加载凭据或启动 helper 前返回 `invalid_operation`，必须重新执行插卡探测。该
  易失状态不落盘、不跨进程，也不替代卡上日志恢复证据。

#### 测试、安全复核与剩余边界

- 新增 `card_initialization_service_tests`：覆盖 bootstrap 必须先于任何 store/KDF 工作、成功路径
  固定执行 Card Record 的三次读取/一次写入并得到 initialized v1 与 32 字节 S、bootstrap 失败
  不触达 store，以及伪 bootstrap 发布非精确 marker 时在首次读取后失败且不写卡。
- helper 进程测试将正式初始化适配链改为新 ViKey 目标，覆盖探测和 bootstrap 分别加载新凭据、
  已验证成功的重复 `ensure` 不重启 helper、正式 store 能力边界，以及精确状态错配第一次失败后
  同一目标被永久作废、第二次调用不重新加载凭据。受限 Codex 父令牌下该进程测试仍按设计跳过，
  用户随后在普通、非管理员桌面 PowerShell 补跑最新 Debug 产物并得到
  `vikey_helper_process_tests passed`；测试只使用伪 `ViKey.dll`，未访问实体卡。
- 最终代码在 Debug、MSVC `/analyze`、ASan 三套全量构建和 CTest 中均为 **34/34**：32 项实际
  通过、2 项按受限父令牌约束跳过。`/analyze` 无项目诊断，ASan 未报告内存错误；新增通用事务
  测试在三套配置中均实际执行通过。
- 安全复核确认通用服务不认识厂商 ID/状态、卷形态、档位、模式、诱饵密码或 UI 状态；日常密码
  在进入服务后由锁页 `SecureBuffer` 独占，bootstrap 异常时立即清零，成功时移动交给已审计的
  Card Record 服务消费。marker 未验证前不运行 512 MiB KDF、不生成 S、不触达 store；成功后
  store 仍绑定原 ID。任何不确定 bootstrap 结果都不自动重试或改绑，只能重新探测卡上事实。
- 剩余边界是产品入口路由与密码提交策略：下一切片应定义具体、可测试且不依赖本地历史的日常
  密码确认/强度合约，再把未初始化页接到本事务；产品 marker 已存在的中断恢复路径应直接通过
  正式 store 的 `CardRecordService` 继续，不再次执行裸介质 bootstrap。首次建卷向导继续暂缓。
  本切片全部验证通过，用户已批准提交；本地提交将在本记录后创建。
- 提交：`98eb75b [P2.11]orchestrate vendor-neutral card initialization`。

### 小切片 12a：不可伪造的密码确认与策略边界

#### 需求审计与实现

- 审计 Q-06、P2 入口表、现有 `password_ingress`、WebView 数组传输和 P2.11b 初始化事务后确认：
  主需求只锁定“设置并确认”“最低强度约束”“强度反馈”和遗忘风险确认，没有锁定最小长度、字符
  类别、词典、评分算法或阈值。旧设计稿中的注册页已经被 2026-08-28 决策废弃，当前 P1 页面只有
  登录单密码框，不能拿旧稿隐含规则充当产品口径。
- 本切片因此先建立可独立提交的安全协议边界，不擅自选定最终强度算法。新增可替换
  `IPasswordStrengthPolicy`，只允许返回固定非秘密反馈 `rejected` / `acceptable` / `strong`；策略
  合约禁止保留、持久化、记录密码或派生特征。未知枚举与 `rejected` 一样失败关闭，不能借异常值
  绕过最低门槛。
- 新增 `confirm_password`：接收两份独立锁页 `SecureBuffer`，策略固定只评估主输入一次，再执行
  逐字节完整比较；不一致统一返回 `mismatch`，一致但策略未接受返回 `insufficient_strength`。
  两份输入在所有拒绝路径立即清零，成功时确认副本立即清零，仅把主输入移动进
  `ConfirmedPassword`。
- `ConfirmedPassword` 不可默认构造、不可拷贝，只能由 `confirm_password` 的私有构造路径产生；
  它携带已接受的非秘密强度反馈，原始密码只能通过消费整个证明对象取出。P2.11b
  `CardInitializationService` 已收紧为只接受该证明，不再能用裸 `SecureBuffer` 绕过确认与策略。
  bootstrap 失败时证明内密码在重抛前销毁；成功时原样移动给既有 Card Record 服务消费。

#### 测试、安全复核与剩余边界

- 新增 `confirmed_password_tests`，以编译期断言确认不能默认构造/拷贝且可无异常移动；运行期覆盖
  `strong` 成功、密码原字节和反馈不变、嵌入 NUL/UTF-8 字节、同长度及不同长度错配、错配优先于
  弱口令反馈、策略恰好调用一次、`rejected`/未知枚举都不能创建证明，以及移动后源证明立即作废、
  不能再次释放空密码。
- `card_initialization_service_tests` 改为必须先经测试策略和两份相同输入创建证明，再进入 P2.11b
  事务；既有 bootstrap 顺序、精确 marker、KDF 前失败关闭及最终 initialized v1 覆盖保持通过。
- Debug、MSVC `/analyze`、ASan 三套全量构建与 CTest 均为 **35/35**：33 项实际通过、2 项按受限
  父令牌约束跳过。`confirmed_password_tests` 与初始化事务测试在三套配置中都实际执行通过；
  `/analyze` 无项目诊断，ASan 未报告内存错误。本切片没有修改 helper、IPC、ViKey 调用或进程测试
  语义，P2.11b 已由用户桌面令牌验证，故无需重复 PowerShell 补跑。
- 安全复核确认密码只存在于固定大小锁页缓冲，拒绝、移动覆盖和析构路径均依赖既有
  `SecureBuffer` 的 `SecureZeroMemory`；没有字符串转换、普通容器副本、日志、本地状态或错误文本
  回显密码/长度/评分特征。强度反馈只有三值枚举，不携带字典命中、字符构成、档位、模式或密码
  角色；确认逻辑不触卡、不运行 KDF，也不改变否认性相关卡记录格式。
- 安全复核过程中额外发现 C++ 默认移动会把枚举复制到源对象，可能让已移空的证明仍显示“已接受”。
  现改为显式移动构造/赋值并同步把源强度置为 `rejected`；消费已移动/已消费证明会在取得目标或
  执行 bootstrap 前抛出逻辑错误。初始化测试新增该负向路径，确认卡目标调用计数保持为零。
- 剩余产品决定是具体强度策略及遗忘风险确认文案。下一切片应在实现前锁定可解释、可测试的阈值
  和反馈算法，再接入双输入 WebView 安全传输与初始化页；注册、用户名、忘记密码和首次建卷向导
  继续不进入该页面。本切片全部验证通过，用户已批准提交；本地提交将在本记录后创建。
- 提交：`3abc689 [P2.12]require confirmed password proofs`。

### 小切片 12b：离线卡密码长度基线

#### 需求审计与实现

- 复核 Q-06 后确认 Card Record 可随卡被取得并复制，日常密码是对离线猜测的唯一记忆秘密；卡在场
  和 512 MiB Argon2id 会提高成本，但不能把该密码按普通在线 MFA 的 8 字符下限处理。对照最终版
  NIST SP 800-63B-4 的口令指南，本切片采用长度优先、允许 Unicode/空格且不强制大小写、数字、
  符号组合的基线。客户随后考虑隐藏盘用户需要同时记忆日常／诱饵两套密码，决定把最低长度从
  NIST 单因素建议的 15 字符调整为 12 字符，并暂缓弱密码检测；这是明确的可用性取舍和已知剩余
  风险。18 字符“较强”仅为鼓励更长密码的反馈线，不把长度反馈宣称为熵证明。
- 新增 `OfflineCardPasswordPolicy`：少于 12 个 Unicode 码点或超过 128 个时返回 `rejected`，
  12～17 个返回 `acceptable`，18～128 个返回 `strong`。最大值高于指南要求至少支持 64 字符的
  下限，同时把输入和评估工作限制在可控范围；超过 128 个码点时扫描立即停止。
- 策略直接只读既有锁页 `SecureBuffer`，以无分配 UTF-8 扫描器按 Unicode 标量值而非字节计数；
  拒绝过长编码、截断序列、代理项和超出 U+10FFFF 的编码。策略不复制、不变换、不保留密码，
  也不输出长度、字符构成或字典命中等细粒度特征，只复用 P2.12a 的三值非秘密反馈。
- 本切片刻意不做 Unicode NFC 归一化：若只在设置密码时改变字节，登录会永久无法匹配；归一化
  必须作为初始化和登录共同入口的协议变更独立实现并补兼容测试。也未把该策略接到 UI 或卡事务，
  因而不会在词典策略尚未确定时把“长度合格”误当成完整强度保障。

#### 测试、安全复核与剩余边界

- 新增 `offline_card_password_policy_tests`，覆盖 11/12、17/18、128/129 的全部阈值边界；确认中文
  BMP 字符和四字节 emoji 均按码点而非 UTF-8 字节计数，空格及单一字符类别不触发组合规则；并
  覆盖过长编码、截断、代理项及大于 U+10FFFF 的四类畸形 UTF-8 失败关闭。
- Debug、MSVC `/analyze`、ASan 三套全量构建和 CTest 均为 **36/36**：34 项实际通过、2 项按受限
  父令牌约束跳过；新策略测试在三套配置中均实际通过。`/analyze` 没有项目诊断，ASan 未报告
  内存错误。本切片不修改 helper、IPC 或 ViKey 行为，因此无需用户额外补跑桌面 PowerShell 测试。
- 安全复核确认 UTF-8 扫描在每次索引前验证剩余长度，减法在短路条件保护下不会下溢；畸形输入
  统一拒绝，不调用 KDF、不触卡。最多扫描到第 129 个码点即返回，可避免超大恶意缓冲造成完整
  线性遍历；没有普通字符串副本、堆分配、网络查询、日志、持久化或密码派生特征。
- 常见／已泄露口令及重复模式拦截已由客户决定暂缓。由此保留的风险是 12 字符仅约束长度，不能
  阻止常见短语、连续数字或其他低猜测成本输入；Argon2id 提高每次猜测成本但不会把弱密码变强。
  后续仍需统一 NFC 归一化，再把策略接入初始化与未来诱饵密码设置。遗忘风险确认文案和双输入
  WebView 安全传输仍未实现。本切片已完成实现、测试和安全复核，用户已批准提交；本地提交将在
  本记录后创建。
- 提交：`758668e [P2.12]add offline card password baseline`。

### 小切片 12c：共享 Unicode NFC 密码入口

#### 需求审计与实现

- 审计现有 `password_ingress`、WebView 两类 SAFEARRAY、P2.12a 确认证明和 P2.12b Unicode 码点
  策略后确认：若只在初始化或只在登录一侧规范化，视觉相同的预组字符／组合字符会得到不同 KDF
  输入并永久锁死数据。规范化因此必须位于两条产品路径将来都会复用的 UI 密码入口，并发生在
  双输入比较、强度计数和任何 KDF 之前；具体初始化页面尚未接入，但不得另建第二套入口。
- 对照 Microsoft `NormalizeString` 的显式长度和缓冲区估算语义，新增消费式
  `normalize_password_utf8_nfc`。它拒绝空输入、畸形 UTF-8 和超过 4096 字节的原始桥接值，通过
  `MultiByteToWideChar` → `NormalizeString(NormalizationC)` → `WideCharToMultiByte` 得到唯一 NFC
  UTF-8 字节。所有调用都传显式长度，不依赖 NUL 结尾，因而嵌入 U+0000 不会截断。
- 原始 UTF-8、解码 UTF-16、NFC UTF-16 和输出 UTF-8 均使用 `SecureBuffer` 的锁页分配；每一阶段
  完成后立即清零上一阶段，异常路径由析构继续清零。转换过程不创建 `std::string`、
  `std::wstring`、普通向量或可持久化密码副本，错误只暴露固定类别和系统操作名。
- 既有 `consume_password_utf8` 现固定先检查 4096 字节上限，再复制并擦除可变桥接区，最后消费式
  返回 NFC 密码。现有登录 `UiSessionController` 已自然获得该语义；未来初始化两次输入和诱饵设密
  必须各自通过该入口后再交给 `confirm_password`。WebView 的 `VT_VARIANT` 路径也在分配锁页副本前
  执行同一上限，`VT_UI1` 路径由共享入口执行；两类超限源数组都仍会被原地清零。

#### 测试、安全复核与剩余边界

- 新增 `password_normalization_tests`，覆盖 ASCII 幂等、`e + U+0301` 与预组 `é` 收敛为相同字节、
  韩文 Jamo 合成为 NFC 音节、嵌入 NUL 后的 emoji 保留、过长编码／截断／代理项失败关闭、空输入
  拒绝和 4097 字节前置拒绝。`password_ingress_tests` 另验证桥接源在 NFC 成功、畸形和超限路径
  均清零；WebView 测试验证 4097 元素的 `VT_UI1` 与 `VT_VARIANT` 数组均拒绝、擦除且不触达会话。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **37/37**：35 项实际通过、2 项按受限
  父令牌约束跳过。新增规范化测试、密码入口、UI 控制器和两类 WebView 数组测试在三套配置中均
  实际通过；`/analyze` 无项目诊断，ASan 未报告内存错误。本切片不修改 helper、IPC、ViKey 或
  跨进程调用，故无需用户额外补跑桌面 PowerShell 测试。
- 安全复核确认所有字符数、乘法和 Windows `int` 转换均受 4096 上限约束；`NormalizeString` 的
  估算不足按官方负返回值重试，重试次数和最大容量均受限。原始超限值在任何锁页分配前拒绝，
  UTF-16 中间区使用 `VirtualAlloc` 对齐且全部清零；没有日志、网络、文件、注册表或模式／档位
  状态。规范化不降低 P2.12b 的 12 码点门槛，强度策略将来观察的是 NFC 后的真实码点数。
- 兼容边界是 NFC 现在改变产品 UI 登录的 KDF 输入语义：ASCII 和已经是 NFC 的密码字节不变；
  尚未发布的非 NFC P2 测试记录以及人为用组合形式创建的旧开发记录不承诺可继续登录。正式 P2
  产品记录尚未部署，因此本切片在初始化 UI 接入前锁定规范化协议，不增加迁移或双重尝试旁路。
  仍待实现遗忘风险确认、双输入 WebView 提交和未初始化页面路由；弱密码检测按客户决定暂缓。
  本切片已完成实现、测试和安全复核，用户已批准提交；本地提交将在本记录后创建。

### 小切片 13a：厂商无关的只读卡入口路由

#### 需求审计与实现

- 审计 P2 入口表、P1 `CardPresenceController`、ViKey v8 介质探针、可恢复 Card Record 日志及当前
  WebView 单登录页后确认：P1 控制器只负责物理在场与拔卡卸载，不应混入产品生命周期判断；入口
  路由应作为独立的厂商无关边界。注册、首次建卷向导和旧 P1 卡迁移均不进入本切片，唯一实体
  开发卡未被读取或写入。
- 复核发现 `ICardStore::get_blob()` 不能充当插卡展示阶段的只读检查：可恢复日志读取在中断态会
  随机覆盖闲置槽并镜像描述符。新增 `IReadOnlyCardEntryInspection`，明确要求介质与逻辑记录检查
  均不得修复、归一化或写卡，并禁止把适配器私有硬件身份／介质枚举扩散到通用状态机。
- 新增 `CardEntryRouter` 及固定三态结果。只有适配器已经完整授权的 `initializable_media`，或产品
  日志中的精确 `provisioned_uninitialized`，才进入初始化页；只有 `initialized_v1` 进入登录页。
  blocked、P1、未来版本、malformed、未知枚举和任意检查异常统一进入 `unavailable`，不提供自动
  重试、宽松降级或“看起来空白”旁路。适配器明确报告 blocked 或新介质时不会多做产品记录检查。
- 本切片只锁定可独立验证的核心路由，不修改 P1 HID 到达处理、ViKey helper／IPC、WebView host
  object 或页面。下一切片须提供绑定同一次插卡上下文的 ViKey 严格只读逻辑记录检查，再由应用层
  持有该上下文并把路由结果接到页面；在此之前开发 EXE 的可见行为不变。

#### 测试、安全复核与剩余边界

- 新增 `card_entry_router_tests`，覆盖已授权新介质、blocked 介质、精确 provisioning marker、
  initialized v1、P1、未来版本、malformed、未知介质／记录枚举，以及介质和记录读取异常；同时
  验证各检查最多调用一次、blocked／新介质不会触达记录检查，失败不会在路由器内自动重试。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **38/38**：36 项实际通过、2 项按受限
  父令牌约束跳过；新增测试三套均实际通过，`/analyze` 无项目诊断，ASan 未报告内存错误。改动不
  涉及 helper、IPC、实体卡或跨进程行为，无需用户补跑桌面 PowerShell 测试。
- 安全复核确认路由器没有写接口、密码、卡内原文、硬件 ID、信封命中、档位或模式状态；公开结果
  只有初始化／登录／不可用三态。异常和未知值均收敛到不可用，不能把损坏或未来记录转成可覆盖的
  新卡。剩余风险是严格只读合约尚无 ViKey 实现，且上下文所有权和拔卡清理尚未接入主 App；因此
  当前结果不能宣称 UI 已能识别 P2 卡。本切片已完成实现、测试和安全复核，用户已批准提交；本地
  提交随本记录创建。
- 提交：`bd3a70e [P2.13]route card entry read-only`。

### 小切片 13b：Card Record 日志只读逻辑状态检查

#### 需求审计与实现

- 审计 P2.13a 只读入口合约、`read_card_record_journal_v1` 与 `load_journal` 后确认：正常 store 读取
  会在描述符未镜像、闲置槽摘要不符或提交清理中断时随机覆盖闲置槽并重写描述符，不能用于用户
  尚未提交密码时的页面选择；但内部 `load_journal` 本身只执行容量检查、四次精确读取、SHA-256
  摘要校验和 commit/previous 有向关系选择，没有写调用，可作为只读原语的唯一事实来源。
- 新增 `inspect_card_record_journal_logical_state_v1_read_only`：它复用与正常读取完全相同的最新提交
  选择和活动槽完整性规则，只对选中的锁页 216 字节记录执行公开 framing 分类并返回
  `CardRecordState`。记录原文不离开函数，返回时由 `SecureBuffer` 清零；函数不进入闲置槽随机化、
  描述符刷新或恢复后复读路径。
- 该函数在可证明的提交点前暂存中断中仍选择旧记录，在新描述符已经发布但旧槽清理尚未完成时
  选择新记录；完整未来记录返回 `unsupported_version`，具有有效物理描述符但公开 framing 非法的
  记录返回 `malformed`。空白日志、无可恢复描述符、歧义提交链和活动槽摘要损坏继续抛出底层错误，
  不得被包装成 malformed、未初始化或旧记录回退。
- 本切片不修改 ViKey session、helper IPC、原子介质探针、主 App 或 WebView；唯一实体开发卡没有
  被访问。下一切片应让 ViKey 的一次只读 helper 会话同时返回稳定硬件 ID、保守介质证据和仅在
  `product_evidence` 下有效的该逻辑状态，避免跨会话重新探测或调用可写恢复读取。

#### 测试、安全复核与剩余边界

- 扩展 `card_record_journal_v1_tests`：验证稳定 provisioning、initialized、future 和物理提交有效但
  framing malformed 的分类；验证部分暂存写入后仍选旧 marker、描述符发布后清理撕裂仍选新记录；
  同时覆盖空白日志与活动槽损坏失败。所有成功／失败路径都断言写调用为零，稳定和可恢复场景还
  断言恰好执行四次区域读取。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **38/38**：36 项实际通过、2 项按受限
  父令牌约束跳过；扩展后的日志测试三套均实际通过，`/analyze` 无项目诊断，ASan 未报告内存错误。
  改动不涉及 helper、IPC、跨进程行为或实体卡，无需用户补跑桌面 PowerShell 测试。
- 安全复核确认函数只新增一个只读公开状态出口，不暴露记录字节、摘要、槽位、commit id、是否
  发生中断或是否需要恢复；这些物理细节不会成为 UI 或隐藏盘侧信道。未来版本只被识别为不可登录
  状态，物理损坏不被降级为可覆盖状态。剩余边界是 ViKey 尚未在一次原子探针中携带该状态，主 App
  也尚未持有并清理对应插卡上下文。本切片已完成实现、测试和安全复核，用户已批准提交；本地提交
  随本记录创建。
- 提交：`2563918 [P2.13]inspect journal state without writes`。

### 小切片 13c：ViKey helper v10 原子入口状态探针

#### 需求审计与实现

- 审计 helper v9 `card_media_probe`、客户端响应校验、P2.13b 日志只读原语和正式 enrollment 适配器
  后确认：若主 App 在收到 `product_evidence` 后再通过 `ICardStore` 或第二个独立 helper 调用判断
  记录状态，会分别引入恢复写入或跨会话换卡／状态漂移窗口。逻辑记录状态必须作为既有原子介质
  探针的受限结果，在同一认证 ViKey session 和同一响应中返回。
- helper IPC 升级为 v10，响应头由 44 字节增至 48 字节并新增显式 wire `CardLogicalState`。它不直接
  序列化 C++ 枚举，只允许 provisioning、initialized v1、unsupported 和 malformed 四值。协议
  固定关系为：仅 `product_evidence` 必须携带一个合法逻辑状态；blank、P1 同形前缀、foreign、
  provisioning interrupted、conflicting 均必须为零；所有失败响应和其他操作也必须为零。
- helper 在同一稳定硬件 ID 的已认证会话中先执行完整 ViKey 私有介质分类；仅产品证据继续调用
  P2.13b 只读日志选择，映射公开 framing 后才关闭 session。逻辑检查不调用正常 record read，因而
  不随机覆盖闲置槽或镜像描述符；任一步骤失败先清零介质／逻辑结果，再返回固定错误。
- 客户端把 v10 逻辑 wire 值映射为可选 `CardRecordState`：产品证据必须有值，非产品状态必须无值；
  未知或不一致组合在返回 `VikeyCardMediaProbe` 前按通信失败拒绝。结果仍只保存非秘密安全属性、
  硬件 ID、ViKey 私有介质枚举及可选公开记录状态，不携带记录字节、摘要、槽位或提交链信息。
- 本切片尚未让 ViKey 探针对象实现 P2.13a 的通用路由接口，也未接入主 App/WebView；下一切片应
  建立持有该不可变探针结果的一次插卡上下文，映射成通用三态，并在拔卡时销毁。唯一实体开发卡
  未被访问，本切片进程测试只使用伪 `ViKey.dll`。

#### 测试、安全复核与剩余边界

- 协议测试覆盖 v10 响应往返及全部关系约束：产品证据缺少逻辑状态、非产品介质夹带逻辑状态、
  未知逻辑枚举、失败响应残留逻辑状态均拒绝。ViKey IO 测试验证产品 marker 的只读状态为
  provisioning 且供应商写计数不变。进程测试新增 provisioned、独立 initialized 夹具记录的原子
  返回，以及 blank 探针不携带逻辑状态三条端到端断言。第一次普通桌面补跑暴露伪 DLL 的日志只存
  于单个 helper 进程全局内存，不能在下一 helper 中复读前一进程写入；原断言错误地把夹具当成
  真实持久卡。现用专用 `R` PIN 让新进程生成 initialized 日志来验证 wire 映射，写入持久性仍由
  同进程 ViKey IO／journal 测试覆盖，不虚构夹具不具备的跨进程持久能力。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **38/38**：36 项实际通过、2 项因 Codex
  父令牌已受限而按规则跳过；协议与 ViKey IO 测试三套均实际通过，`/analyze` 无项目诊断，ASan
  未报告内存错误。用户随后在普通非管理员桌面 PowerShell 补跑修正后的最新 Debug
  `vikey_helper_process_tests` 并得到 `passed`；该测试只使用伪 DLL，未访问实体卡。
- 安全复核确认 v10 版本、48 字节头长、nonce、安全属性、payload 长度、非零 ID、介质枚举、逻辑
  枚举及二者关系都在客户端接受响应前校验；降级 helper、尾随 IPC 数据及错误响应残留字段继续
  失败关闭。逻辑状态只描述公开产品生命周期，未增加密码命中、信封槽位、隐藏盘形态、恢复动作或
  写卡入口。验证项已全部闭环，本切片完成实现、测试和安全复核，用户已批准提交；本地提交随本
  记录创建。

### 小切片 13d：ViKey 不可变一次插卡上下文

#### 需求审计与实现

- 审计 P2.13a 通用入口路由、P2.13c v10 原子探针、正式 enrollment 生命周期和初始化目标后确认：
  helper 已能在同一会话返回硬件 ID、介质证据及可选逻辑状态，但原始 `probe()` 仍由 enrollment
  公开，主 App 若分别查询页面路由和初始化目标，可能重复启动 helper 并制造跨会话状态漂移。
  本切片须把一次原子结果封装成明确所有权对象；主 App/WebView 接线及拔卡销毁留给后一切片。
- 新增 ViKey 私有 `VikeyCardInsertionContext`，独占持有不可变 `VikeyCardMediaProbe` 并实现
  `IReadOnlyCardEntryInspection`。blank／provisioning interrupted 映射为可初始化，产品证据映射为
  产品记录，P1 同形前缀、外来数据和冲突证据统一映射为 blocked；产品记录的 provisioning、
  initialized、unsupported、malformed 状态直接读取同一缓存，不调用 helper、store 或日志恢复读。
- 上下文构造时重新验证非零硬件 ID、六态介质枚举及“仅产品证据携带受限逻辑状态”的关系；产品
  缺状态、非产品夹带状态、P1／未知逻辑状态和未知介质枚举均按 `invalid_configuration` 拒绝，不能
  进入路由。非产品上下文若被错误调用逻辑记录接口则按 `invalid_operation` 失败关闭。
- `VikeyHelperCardEnrollment` 现在只公开 `inspect_entry()`，内部恰好执行一次既有原子探针并返回
  `unique_ptr` 独占上下文；原始 `probe()` 已降为私有，减少调用者绕开生命周期对象重复探测的入口。
  上下文仍保留原始探针的 const 引用，供路由选择初始化后把同一硬件 ID 和精确介质状态复制给
  `VikeyHelperCardInitializationTarget`。初始化适配器同时拒绝任何夹带逻辑状态的 blank／中断上下文，
  并保证在取得厂商凭据或启动 helper 前拒绝矛盾输入。
- 本切片不接入 P1 HID 到达处理、主 App、WebView 页面或拔卡事件，也不访问唯一实体开发卡；当前
  开发 EXE 可见行为保持不变。下一切片应由应用层会话独占该上下文，依据通用路由展示初始化／登录／
  不可用页，并在拔卡或新到达事件时先销毁旧上下文。

#### 测试、安全复核与剩余边界

- 新增 `vikey_card_insertion_context_tests`，覆盖六种 ViKey 介质状态、四种允许的产品逻辑状态、
  通用初始化／登录／不可用路由、缓存快照保留，以及零 ID、缺失／夹带／P1／未知状态组合和错误
  非产品逻辑检查。扩展伪 DLL 进程测试，验证正式 enrollment 只加载一套凭据，缓存路由不会启动
  第二个 helper，矛盾初始化上下文也不会加载凭据。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **39/39**：37 项实际通过、2 项因 Codex
  父令牌已受限而按规则跳过；新增上下文测试三套均实际通过，`/analyze` 无项目诊断，ASan 未报告
  内存错误。用户随后在普通非管理员桌面 PowerShell 补跑最新 Debug `vikey_helper_process_tests`
  并得到 `passed`；该测试只使用伪 `ViKey.dll`，未访问实体卡。
- 安全复核确认路由对象只持有非秘密硬件 ID、安全属性、厂商介质枚举及公开生命周期状态，不含卡内
  原文、密码、摘要、槽位、commit id、信封命中或模式信息；路由只读 const 快照，不能写卡、恢复
  日志或静默改绑。`unique_ptr` 与不可拷贝基类避免上下文被无意复制，删除对象即可清除一次插入绑定。
  剩余风险是主 App 尚未实际持有并在拔卡时销毁对象；当前结果不能宣称页面已经识别 P2 卡。
  本切片已完成实现、测试和安全复核，用户已批准提交；本地提交随本记录创建。
- 提交：`6846fc8 [P2.13]bind immutable card insertion context`。

### 小切片 13e：厂商无关插卡上下文所有者

#### 需求审计与实现

- 审计 P2.13a 路由、P2.13d ViKey 不可变上下文、P1.7 `CardPresenceController`、HID 通知和当前
  WebView 主程序后确认：现有开发 App 仍在启动时固定构造 P1 32 字节 blob backend，卡到达只更新
  presence，拔卡只负责卸载；若此时直接替换成 P2 路由，唯一旧 P1 开发卡会被正确阻断，但初始化
  页面和正式 216 字节登录尚未接好，反而会破坏仍需保留的 P1 真卡回归入口。因此先独立完成通用
  易失所有权边界，不修改现有 P1 EXE 的可见行为，实际主 App/WebView 接线继续作为下一切片。
- 新增 `ICardEntryInspectionSource`，厂商适配器每次只返回一个独占、只读的插卡检查对象。该接口不
  暴露厂商身份、卡存储、密码或写操作，并通过不可拷贝基类约束来源生命周期。
- 新增 `CardEntryContextController`：初始状态没有 route 或上下文；每次接受新的物理到达事件时，
  固定先销毁旧上下文，再向来源请求一次新上下文并通过 `CardEntryRouter` 固化初始化／登录／不可用
  路由。来源抛出异常或返回空指针统一发布 unavailable 且不保留上下文，不自动重试，也绝不回退
  旧卡 route。拔卡先销毁上下文再清除 route，重复拔卡幂等；对象析构同样释放最后的插卡绑定。
- 本切片没有把 vendor-specific 上下文、硬件 ID、介质细分或逻辑记录状态上送到公开控制器；公开
  状态仍只有 no-card（`optional` 无值）及 P2.13a 三种固定 route。控制器不调用 helper、store、KDF、
  挂载或任何写卡接口，唯一外部操作由只读来源在一次同步调用中完成。
- 计划文档审计同时发现 P2.12c 和 P2.13a～P2.13d 的状态摘要仍停留在“待提交／未接入上下文”旧
  文案，已按实际提交 `f56614e`、`bd3a70e`、`2563918`、`fed99c8`、`6846fc8` 纠正；技术需求与
  当前实现没有冲突。

#### 测试、安全复核与剩余边界

- 新增 `card_entry_context_controller_tests`，覆盖无卡初态、单次来源调用和 route 固化、有效上下文
  保留、新到达在来源调用前销毁旧绑定、blocked 上下文保留至拔卡、拔卡幂等清理、控制器析构清理，
  以及换卡来源异常／空结果统一失败关闭。测试明确验证换卡探针失败后旧上下文已销毁，不能继续
  使用旧卡的初始化或登录 route。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **40/40**：38 项实际通过、2 项因 Codex
  父令牌已受限而按规则跳过；新增生命周期测试三套均实际通过，`/analyze` 无项目诊断，ASan 未
  报告内存错误。本切片未修改 helper、IPC、伪 DLL 进程测试或实体卡路径，无需用户补跑普通桌面
  PowerShell 测试。
- 安全复核确认新控制器不接触密码、卡记录原文、摘要、硬件 ID、模式、档位或信封状态；异常路径
  只保留公开 unavailable，不泄漏失败细节。旧上下文在新来源调用前释放，避免换卡期间同时存在两份
  实体绑定；来源失败没有旧状态回退，拔卡/退出也不会留下可继续使用的 route。剩余边界是
  `ICardEntryInspectionSource` 的 ViKey 组合、HID 到达/拔卡调用顺序以及 WebView route 查询尚未接入；
  当前开发 EXE 仍是 P1 兼容路径，不能宣称页面已经识别或初始化 P2 卡。本切片已完成实现、测试和
  安全复核，用户已批准提交；本地提交随本记录创建。
- 提交：`180a446 [P2.13]own volatile card entry context`。

### 小切片 13f：ViKey 来源与 HID 生命周期组合

#### 需求审计与实现

- 审计 P2.13e 上下文所有者、正式 ViKey enrollment、P1.7 presence／强制卸载和 Windows HID 广播
  过滤后确认：P2 不能另写一套设备路径解析，也不能在拔卡卸载失败时恢复已经失效的卡上下文；同时
  重复 HID arrival 不应被当作失败后的自动重试。现有 P1 重载仍须保留，以免在 P2 页面尚未接入时
  破坏唯一旧开发卡的回归程序。
- `VikeyHelperCardEnrollment` 现在实现厂商无关 `ICardEntryInspectionSource`。通用入口调用仍复用
  既有 `inspect_entry()`，只加载一套新鲜锁页开发凭据、启动一个 v10 helper，并把具体 ViKey 上下文
  以只读基类独占指针交给应用层；不增加第二次介质检查或写卡入口。
- 新增 `CardDeviceLifecycleController`，组合 `CardPresenceController` 与
  `CardEntryContextController`。首次到达先标记物理 presence，再执行一次只读来源并发布固定 route；
  重复到达返回原 route 且不再次调用来源。拔卡固定先销毁入口上下文和 route，再调用原 P1.7
  `force_unmount_active()`；即使强制卸载抛异常且会话仍残留，也绝不恢复已移除卡的上下文。
- 新增不含厂商细节的 `CardDeviceLifecycleTransition`，只公开原 presence 转换、可选三态入口 route 和
  `entry_changed` 刷新位。该结构不携带 VID/PID、硬件 ID、介质细分、错误码、密码、信封或模式。
- Windows HID 解析提取为单一内部只读过滤器，P1 旧重载和 P2 新重载共享完全相同的事件类型、
  payload 长度、HID class、UTF-16 终止和精确 VID/PID token 检查。无关或畸形广播不会触发来源、
  清理上下文或卸载；P1 公开行为和函数签名保持不变。
- 本切片仍不修改 `tools/app/main.cpp`、WebView host object 或页面；P2 组合已具备接线条件，但当前
  开发 EXE 继续走 P1 路径。下一切片应决定独立 P2 开发组合的对象构造／销毁顺序和 route 查询边界，
  不应在初始化双输入页面完成前直接覆盖现有 P1 真卡回归入口。

#### 测试、安全复核与剩余边界

- 新增 `card_device_lifecycle_controller_tests`，覆盖首次到达顺序、重复到达不重试、来源失败仍保留
  物理 presence 但 route 为 unavailable、拔卡先销毁上下文再普通／强制卸载、重复拔卡幂等，以及
  强制卸载抛异常后会话虽残留但上下文不恢复。扩展 HID 测试验证 P2 重载仅接受同一严格目标，外来
  HID 不改变既有 route；扩展伪 DLL 进程测试，使正式 enrollment 经通用来源接口返回同一个具体
  ViKey 上下文并继续绑定初始化目标。
- 第一次 `/analyze` 构建只报告进程测试的 `dynamic_cast` 结果由自定义断言检查、分析器无法证明
  非空；已改成显式 `if (nullptr) [[noreturn]] fail()`。修正后三套完整构建无项目诊断。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **41/41**：39 项实际通过、2 项因 Codex
  父令牌已受限而按规则跳过；新增生命周期和扩展 HID 测试三套均实际通过，ASan 未报告内存错误。
  用户随后在普通非管理员桌面 PowerShell 补跑最新 Debug `vikey_helper_process_tests` 并得到
  `passed`；该测试只使用伪 `ViKey.dll`，未访问实体卡。
- 安全复核确认拔卡时入口上下文的销毁先于任何可能失败的卸载调用，防止物理卡已离开后 UI 仍持有
  可登录／初始化 route；重复 arrival 不会把 unavailable 状态静默重试成可用。共享 HID 过滤没有
  放宽边界，来源异常继续由 P2.13e 收敛为 unavailable。剩余风险是同步 helper 调用未来接入窗口
  线程时最多会阻塞到既有超时，且主 App/WebView 尚未实际使用该组合。本切片已完成实现、测试和
  安全复核，用户已批准提交；本地提交随本记录创建。
- 提交：`64325da [P2.13]coordinate card device lifecycle`。

### 小切片 13g：精确 provisioning marker 恢复目标

#### 需求审计与实现

- 审计 WebView host、P2.13e 上下文所有权、P2.11 初始化目标和需求中“重新探测发现精确
  provisioning 日志后直接继续、不得再次 bootstrap”后发现：现有
  `VikeyHelperCardInitializationTarget` 只接受 blank／provisioning interrupted。若初始化在 marker
  已提交后中断，下一次严格只读探针会正确进入 initialization route，但动作层无法构造目标，页面
  接线会形成死路。因此先补该恢复边界，再做通用所有者到初始化动作的交接。
- 初始化目标上下文验证现接受且仅接受两类状态：非零硬件 ID 的 blank／provisioning interrupted
  且不携带逻辑状态；或 `product_evidence + provisioned_uninitialized` 的精确组合。后者来自同一次
  v10 原子只读探针，目标构造后直接把易失 `ProvisioningState` 设为 verified。
- 对 verified marker 调用 `ensure_provisioning_record()` 立即返回，不取得厂商凭据、不启动 helper、
  不重写 marker；随后暴露的正式 216 字节 store 仍绑定原探针硬件 ID。产品初始化服务后续会通过
  该 store 重新读取并确认 marker，再执行最终双信封记录事务，因此没有把只读 route 当作写授权。
- blank／provisioning interrupted 保持既有 pending → v9 bootstrap → verified 语义；零 ID、bare
  介质夹带逻辑状态、产品缺逻辑状态、initialized／unsupported／malformed、P1 同形前缀、外来／
  冲突及未知枚举继续在凭据加载和任何外部副作用前按 invalid configuration 拒绝。
- 本切片不修改 helper、IPC、HID、主 App、WebView 或实体卡内容。下一切片可安全地设计通用入口
  所有者到初始化目标的短生命周期交接，而无需对 marker 恢复另设本地进度标志或第二次 bootstrap。

#### 测试、安全复核与剩余边界

- 新增 `vikey_card_initialization_target_tests`：使用会在任何凭据请求时抛错的 provider，验证精确
  marker 连续两次 ensure 均零凭据加载、正式 store 仍可取得；验证两种 bare 上下文仍可构造；并
  覆盖零 ID、矛盾组合、其余产品状态、P1／外来／冲突和未知枚举全部拒绝且零凭据加载。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **42/42**：40 项实际通过、2 项因 Codex
  父令牌已受限而按规则跳过；新增目标测试三套均实际通过，`/analyze` 无项目诊断，ASan 未报告
  内存错误。本切片没有修改 helper／IPC／进程测试或实体卡路径，无需用户补跑普通 PowerShell。
- 安全复核确认 marker 组合必须精确匹配，不会把 initialized、未来、损坏或非产品介质降级为可
  覆盖状态；skip-bootstrap 只避免重复 marker 写，最终初始化仍通过硬件 ID 绑定 store 和产品服务的
  marker 复读，探针与提交之间换卡会失败。目标不缓存凭据或卡记录，新增状态仅在进程内易失。
  剩余边界是通用上下文所有者尚不能请求该目标，主 App/WebView 也尚未接线。本切片已完成实现、
  测试和安全复核，用户已批准提交；本地提交随本记录创建。

### 小切片 13h：一次性同步初始化动作

#### 需求审计与实现

- 审计 P2.13e～P2.13g、`ConfirmedPassword`、厂商无关初始化服务与需求中“不确定失败必须作废
  上下文”后确认：不能把可写初始化目标返回给 UI 长期保存，也不能在目标构造或写卡失败后继续
  使用原 route。否则拔卡事件与 UI 持有对象之间会形成悬空写授权，并可能绕过新一次只读探测。
- `ICardEntryInspectionSource` 新增可替换的目标工厂边界。默认实现返回空目标并失败关闭；工厂只可
  根据来源此前返回的精确只读上下文构造短生命周期目标，不得在构造阶段加载凭据或执行卡 I/O。
- `CardEntryContextController::initialize_and_authenticate()` 只接受当前精确
  `initialization_required` route 和非空上下文。它在调用来源工厂前先把 route 降为 unavailable，
  再把独占上下文移入局部变量；随后目标与上下文都只存在于同步调用栈，目标先销毁、上下文后销毁。
- 动作把不可复制的 `ConfirmedPassword` 直接移动给既有 `CardInitializationService`，固定沿用
  bootstrap → marker 复读 → KDF／根秘密生成 → 最终记录写入与复读验证。成功返回根秘密给未来
  已认证状态；工厂异常、空目标、bootstrap／记录失败和成功都不会恢复 route 或上下文，重复调用
  只能得到 invalid operation，必须物理拔卡后重新插入并完成新探测。
- `VikeyHelperCardEnrollment` 的目标工厂只接受由自身适配类型表达的
  `VikeyCardInsertionContext`，把同一个原子探针快照复制给正式初始化目标。外来通用上下文在凭据
  加载和任何外部副作用前拒绝；目标构造本身不启动 helper，实际操作仍按原 ID 和精确状态复验。
- 本切片不修改 WebView、页面、HID、helper IPC、实体卡数据或现有 P1 开发入口。下一切片可接入
  初始化双输入页面提交与已认证无卷状态，但首次建卷向导继续暂缓。

#### 测试、安全复核与剩余边界

- 新增 `card_entry_initialization_controller_tests`，通过正式 Card Record 初始化事务覆盖成功路径，
  并验证控制器在目标工厂运行前已经失效、上下文持续存活到事务结束、目标先于上下文销毁、最终
  记录为 initialized v1；另覆盖无卡、登录／不可用 route、工厂异常、空目标、bootstrap 失败和
  重复调用，确认不合格 route 不消耗只读上下文，而已开始的初始化在任何结果下都不可重试。
- 扩展 `vikey_card_initialization_target_tests`，验证 enrollment 工厂接受原始 ViKey 插入上下文且
  零凭据加载，并在外来上下文上返回 invalid configuration、仍不加载凭据。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **43/43**：41 项实际通过、2 项因 Codex
  父令牌已受限而按规则跳过；新增测试三套均实际通过，`/analyze` 无项目诊断，ASan 未报告内存
  错误。本切片没有修改 helper／IPC／伪 DLL 进程测试或实体卡路径，无需用户补跑普通 PowerShell。
- 安全复核确认写目标不会逃逸到 UI 或事件循环，控制器在任何外部初始化动作前不可逆撤销授权；
  密码证明只向正式服务移动一次，目标构造不接触凭据，正式服务继续在 bootstrap 失败时清除密码。
  未发现本地状态、硬件 ID、密码、信封、模式或档位信息新增落盘。剩余边界是主 App/WebView 尚未
  提供初始化双输入页面，也尚未消费成功返回的根秘密进入已认证无卷状态。本切片已完成实现、测试
  和安全复核，用户已批准提交；本地提交随本记录创建。

### 小切片 14a：易失认证会话与拔卡清理

#### 需求审计与实现

- 审计既有 `ApplicationSession`、P2.13h 初始化动作、Card Record 服务和“已认证但可无卷”需求后
  确认：P1 会话固定读取 32 字节裸 blob 并立即挂载，且明确不保留卡材料，不能承接 P2 解封后的
  根秘密，也无法表达合法无卷状态。为保持 P1 真卡回归路径，未修改或替换该兼容会话。
- 新增厂商无关 `CardAuthenticationSession`，仅在锁页 `SecureBuffer` 中拥有一份精确 32 字节
  Card Record v1 根秘密。建立操作按移动语义消费输入，拒绝空、短、长根秘密，也拒绝覆盖已激活
  会话；公开边界只提供 authenticated 布尔状态与清理操作，不暴露根秘密字节、密码角色或卷形态。
- P2.13h 的入口初始化动作现不再向调用方返回根秘密，而是在最终记录写入和复读成功后直接移动到
  空的 `CardAuthenticationSession`。已有认证态会在 route/context 消费、目标构造、凭据加载和写卡
  前返回 invalid operation，保留原只读上下文与既有认证，避免初始化成功后根秘密经过 UI 所有权。
- `CardDeviceLifecycleController` 现编译期强制接收 `IVolatileCardAuthenticationSession`，不保留可
  漏接清理的二参数构造器；P1 继续直接使用原 `CardPresenceController`。首次有效到达按 presence →
  清陈旧认证 → 只读探测执行，重复到达不清当前会话、不重探测。
- 拔卡顺序固定为：销毁插卡上下文／route → 清除并释放根秘密 → 调用既有强制卸载。即使卸载失败
  且卷仍残留，也不恢复已离开卡的上下文或根秘密；重复拔卡重复执行幂等清理，不产生新状态。
- 本切片不新增根秘密消费者、登录动作、卷派生、WebView 页面或建卷向导。后续挂载边界须以受审计
  的作用域方式借用根秘密，不能为方便而重新增加 UI 可见 getter。

#### 测试、安全复核与剩余边界

- 新增 `card_authentication_session_tests`，覆盖无认证初态、移动建立、三种错误长度、活跃会话拒绝
  替换、清理幂等及清理后可建立新会话。扩展入口初始化测试，验证成功直接建立认证态，活跃认证在
  工厂前拒绝且不消耗 route/context；既有工厂／bootstrap 失败仍不会建立认证。
- 扩展设备生命周期与 HID 测试，验证首次到达在探测前清陈旧认证、重复到达不清理、拔卡严格按
  context → root → unmount 顺序、卸载失败不恢复认证，以及 P2 HID 组合必须显式传入认证会话。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **44/44**：42 项实际通过、2 项因 Codex
  父令牌已受限而按规则跳过；新增和扩展测试三套均实际通过，`/analyze` 无项目诊断，ASan 未报告
  内存错误。本切片没有修改 helper／IPC／ViKey 进程协议或实体卡路径，无需用户补跑 PowerShell。
- 安全复核确认根秘密只有一个锁页所有者，错误输入、拒绝替换、显式清理和析构均复用
  `SecureBuffer` 的清零释放；UI 没有字节访问器，生命周期对象无法省略根秘密清理依赖。拔卡在任何
  可能失败的卸载前清秘密，避免残留卷导致旧认证材料继续驻留。剩余边界是登录 route 尚未有同类
  一次性认证动作，认证会话也尚未以受限作用域向 P2 挂载／建卷派生提供根秘密。本切片已完成实现、
  测试和安全复核，用户已批准提交；本地提交随本记录创建。

### 小切片 14b：同卡只读登录目标

#### 需求审计与实现

- 审计 P2.13 登录 route、P2.14a 易失认证会话、Card Record 认证结果和后续卷派生后确认：登录失败
  不能一律消费上下文。精确的错误密码结果 `CardCredentialRejected` 应允许同一次插卡重试；设备
  读取失败、硬件 ID 不匹配、记录状态变化或日志歧义等非凭据错误必须作废上下文。同时，成功登录
  输入仍要用于 `KDF(S, user input)` 派生卷密码，不能先只建立根秘密会话再无条件销毁输入。因此本
  切片只建立同卡只读目标，不提前实现会丢失登录输入或错误消费上下文的半成品动作。
- 新增厂商无关 `ICardLoginTarget`，只暴露返回锁页缓冲的 `read_record()`，刻意不继承或暴露
  `ICardStore`，从类型边界上排除登录流程的卡写能力。对象不可复制、不可移动，只用于一个短期
  登录尝试；适配器工厂默认返回空并失败关闭。
- `VikeyHelperCardEnrollment` 现可根据自身产生的原始 `VikeyCardInsertionContext` 构造登录目标。
  工厂拒绝任何外来 inspection 类型；目标只接受非零硬件 ID 且精确为 `product_evidence` +
  `initialized_v1` 的原子探针快照，缺失／矛盾状态、其他介质或逻辑状态及未知枚举都在凭据加载和
  I/O 前拒绝。
- 目标构造仅保存非秘密探针快照并组装后端，不加载凭据、不启动 helper。实际 `read_record()` 才
  通过正式 `VikeyCardRecordStore` 读取；后端每次取得新鲜短期厂商凭据，并由 helper 重新比对捕获
  的硬件 ID。上层只得到 Card Record 锁页缓冲，不能借该目标执行 `put_blob()`。
- 本切片不修改 helper／IPC、HID、实体卡数据、主 App 或 WebView，也不实现密码解封、认证会话
  建立或卷挂载。下一切片须设计受限的登录成功交接，同时落实“仅错误密码保留上下文，其他失败
  作废”的重试边界。

#### 测试、安全复核与剩余边界

- 新增 `vikey_card_login_target_tests`：验证精确 initialized v1 上下文及 enrollment 工厂构造过程
  均零凭据加载；外来上下文、零硬件 ID、缺失逻辑状态、provisioning／未来／损坏记录、裸卡、P1
  同形前缀、外来／冲突介质和未知枚举全部返回 invalid configuration，且拒绝路径不加载凭据。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **45/45**：43 项实际通过、2 项因 Codex
  父令牌已受限而按规则跳过；新增测试三套均实际通过，`/analyze` 无项目诊断，ASan 未报告内存
  错误。本切片未修改 helper／IPC／伪 DLL 进程测试或实体卡路径，无需用户补跑 PowerShell。
- 安全复核确认上层接口没有写能力，目标只接受同源精确上下文，所有状态校验都先于凭据和 I/O；
  后续读取仍由正式后端绑定原硬件 ID，探针与读取之间换卡会失败。目标不持有用户密码、根秘密或
  卡记录原文，读取结果由调用方以锁页缓冲接收。剩余风险集中在尚未实现的登录动作：必须区分可
  重试的凭据拒绝与应撤销上下文的其他失败，并把登录输入和根秘密以受限作用域交给挂载派生，禁止
  增加 UI 可读秘密访问器。本切片已完成实现、测试和安全复核，用户已批准提交；本地提交随本记录
  创建。
- 提交：`e2a3a51 [P2.14]bind read-only card login target`。

### 小切片 14c：可重试登录动作与一次性成功交接

#### 需求审计与实现

- 审计 P2.14b 只读登录目标、Card Record 解封、P2.14a 易失认证会话、P1 `ApplicationSession` 与
  现行派生规则后确认：P1 会再次读取 32 字节裸卡 blob，且仅有 A 容器时仍用用户输入派生，不能
  复用于 216 字节 P2 记录和固定 `KDF(S,"diskA")`。本切片因此只完成厂商无关登录动作与同步成功
  交接，具体 P2 挂载处理器留作下一独立边界。
- 新增 `ICardLoginSuccessConsumer`：它是同步、非 UI、不可复制的所有权交接接口，一次接收锁页根
  秘密 S 与原始 NFC 登录输入。未来实现可把 S 移入易失认证会话并立即完成卷密码派生；登录输入
  不得在调用返回或抛出后保留。
- `CardEntryContextController::authenticate_login()` 只接受精确 login route 和现存上下文。目标工厂
  和 read 均在原上下文仍有效时执行；目标离开作用域后再处理结果。空目标、factory／读卡异常、
  公开记录不再是 initialized v1，以及密码学系统异常都会把 route 降为 unavailable 并销毁上下文。
- 只有 `unlock_card_record_v1` 的固定 `CardCredentialRejected` 保留 route/context；该类型按既有
  协议同时覆盖错误密码与不可区分的信封认证损坏，避免通过上下文失效差异新增损坏探针。下一次
  用户尝试重新构造目标、读取卡记录、取得新鲜厂商凭据并复验捕获的硬件 ID，不缓存上次记录。
- Card Record 解封 API 会消费密码，而后续 B 派生仍需要同一输入。动作因此只在锁页 `SecureBuffer`
  中短暂复制一份供认证 KDF 消费；原输入不产生普通堆副本。成功后先清除 216 字节记录缓冲、撤销
  route/context，再把 S 和原输入按移动语义交给处理器；处理器抛错也不会恢复旧授权。
- 安全复核进一步发现 KDF 数秒内可能发生拔卡、换卡或记录变化，不能只等待可能被同步 UI 调用延迟
  的 HID 移除消息。动作现让不持有凭据的同一只读目标跨 KDF 存活；密码解封成功后再次读取，强制
  取得新鲜厂商凭据、复验原硬件 ID，并常量时间比对完整 216 字节记录。复读失败或任一字节变化均
  清除 S／输入、销毁目标和上下文，不调用成功处理器；复读一致后先销毁目标并清除两份记录再交接。
- 本切片不修改 helper／IPC、ViKey 适配器、HID、实体卡、现有 P1 挂载或 WebView。下一切片须实现
  具体 P2 成功处理器，明确无卷、仅 A、仅 B、A+B 和挂载失败时的认证会话语义。

#### 测试、安全复核与剩余边界

- 新增 `card_entry_login_controller_tests`，使用正式 Card Record KDF 验证同一次插卡先输错、上下文
  保留、再输入正确密码成功；成功处理器收到与初始化相同的 S 和原登录输入，且调用前 route/context
  已撤销；成功尝试还必须执行第二次读取。另覆盖 KDF 期间记录变化、factory 异常、空目标、首次
  读卡失败和公开记录状态变化全部失败关闭，以及初始化／不可用 route 在目标构造前拒绝且不消费
  上下文。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **46/46**：44 项实际通过、2 项因 Codex
  父令牌已受限而按规则跳过；新增登录动作测试三套均实际通过，`/analyze` 无项目诊断，ASan 未报告
  内存错误。本切片没有修改 helper／IPC／伪 DLL 进程测试或实体卡路径，无需用户补跑 PowerShell。
- 安全复核确认错误凭据路径销毁两份密码和本次记录／目标但保留非秘密上下文；其他错误清秘密后
  撤销上下文；成功必须通过同卡同记录复读，并在秘密离开动作前销毁目标、清除两份记录及撤销授权。
  成功处理器不向 UI 暴露 getter，且输入和 S
  只通过锁页移动所有权传递。剩余风险是具体处理器尚未实现，当前主 App 仍不能执行 P2 登录或挂载；
  后续必须确保处理器把 S 交给 `CardAuthenticationSession`、只在同步派生期间借用秘密，并在失败
  路径清除输入且不意外回退 P1 的裸 blob 读取。本切片已完成实现、测试和安全复核，用户已批准
  提交；本地提交随本记录创建。

### 小切片 14d：独立 P2 登录挂载会话

#### 需求审计与实现

- 审计 8.6、P2.14a～P2.14c、既有 `mount_session()`、P1 `ApplicationSession` 和拔卡生命周期后
  确认：底层 B→A、失败回滚及 Shell 通知事务可以共享，但 P1 会话会再次读取 32 字节裸卡材料，
  且旧 A-only 兼容分支使用登录输入，不能成为 P2 成功处理器。P2 还必须把“卡认证成功”与“卷操作
  成功”分离，合法表达已认证无卷和认证后挂载失败。
- 新增 `CardLoginMountSession`，同时实现一次性 `ICardLoginSuccessConsumer` 与最小活动卷生命周期，
  但编译期明确不实现 P1 `IApplicationSessionActions`。它不接收 `ICardStore`、不重新读卡，也没有
  `authenticate_and_mount()` 入口；成功交接先把精确 32 字节 S 移入 `CardAuthenticationSession`，
  再在同一同步调用栈内借用根秘密与原 NFC 登录输入执行卷操作。
- `CardAuthenticationSession` 仍无公开字节 getter；只有应用内部、受测试的挂载核心获得 friend
  访问。原登录输入不写普通堆副本，成功和所有异常路径均显式清零，调用返回或抛出后不保留输入。
  已有认证或活动残留卷会在任何新挂载前拒绝重复交接并清除新输入，禁止覆盖根秘密或叠加会话。
- 把 `CardPresenceController` 的依赖从带 P1 登录入口的 `IApplicationSessionActions` 收窄为新的
  `IActiveVolumeSession`。P1 接口继续继承该最小生命周期，旧 App/UI 行为不变；P2 会话可直接参与
  既有拔卡强制卸载顺序，而无需获得裸卡登录能力。
- 在既有挂载事务内新增独立 P2 策略：B 存在时仍先以 `KDF(S,user input)` 挂 B，再以固定
  `KDF(S,"diskA")` 挂存在的 A；仅 A 时也固定使用 `diskA`；A/B 都不存在时返回合法空挂载结果。
  原 P1 策略保持不变，继续保留其 A-only 兼容语义和无容器错误。
- 卡认证在进入处理器前已完成，因此具体卷探测、派生或挂载普通失败会原样上送但保留易失认证，
  且不虚构活动盘符；B 已挂而后续 A 失败时继续使用既有回滚，只有普通和强制回滚均失败才记录精确
  残留盘符供后续普通／拔卡强制卸载。认证状态仍由既有生命周期在拔卡时先于卸载清除。
- 本切片未修改 helper／IPC、ViKey 适配器、实体卡、Card Record 密码学、主 App 或 WebView；主
  App 仍使用 P1 兼容会话，P2 入口和恒定单密码框的 UI 组合留待下一独立切片。

#### 测试、安全复核与提交状态

- 新增 `card_login_mount_session_tests`，覆盖无卷、仅 A、仅 B、A+B 均先建立认证，普通挂载失败
  保留认证且不产生挂载，B 回滚失败保留精确残留并可重试卸载，错误根长度、重复交接、B→A 普通
  卸载及强制卸载状态。编译期断言 P2 会话是成功处理器和活动卷会话，但不是 P1 登录接口。
- 扩展 `mount_orchestrator_tests`，锁定 P2 A-only 使用固定 `diskA`、无卷成功且不执行派生／挂载；
  既有 P1 单容器、B gate、回滚和布局校验测试继续通过。生命周期／HID 测试改为直接伪造最小活动
  卷接口，证明插拔路径不再依赖 P1 登录能力。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **47/47**：45 项实际通过、2 项因 Codex
  父令牌已受限而按既有规则跳过；`/analyze` 无项目诊断，ASan 未报告内存错误。`git diff --check`
  通过。本切片不改 helper／IPC／伪 DLL 进程路径，无需用户手动补跑 PowerShell 测试。
- 安全复核确认 P2 类型不持有卡 store 或厂商后端，没有 P1 裸材料回退；根秘密只有既有锁页会话
  长期持有且仅受限核心借用，登录输入只存在于同步栈上的锁页所有者；无卷及卷失败不会泄露密码角色、
  模式、档位或容器形态到持久状态。剩余风险是主 App/WebView 尚未组合 P2 入口、处理器和恒定单密码
  框，实体驱动挂载仍沿用 P1 已验证的底层事务证据。本切片已完成实现、测试和安全复核，用户已批准
  提交；本地提交随本记录创建。

### 小切片 15a：厂商无关的产品 UI 原生会话边界

#### 需求审计与实现

- 审计 Q-07、8.6、P2.12c 共享 NFC 入口、P2.13 插卡上下文、P2.14 登录／认证／挂载链，以及现有
  P1 `UiSessionController`、COM host、WebView 页面和主 App 后确认：现有 UI 只有 P1 单次提交，
  不能直接复用；本切片先建立不含 ViKey、COM 或页面实现的产品原生控制器，后续接线不得越过它
  直接触达卡记录、硬件身份或 P1 32 字节裸卡入口。
- 新增 `ICardEntryActions`，只向 UI 控制器暴露初始化、登录与当前通用 route；插卡到达／移除、
  原始上下文、厂商身份和记录字节仍留在生命周期所有者内部。新增组合接口
  `IAuthenticatedCardVolumeSession`，约束登录成功交接和活动卷生命周期必须来自同一个 P2 会话，
  `CardLoginMountSession` 继续不实现 P1 登录接口。
- 新增 `CardProductUiSessionController`，只投影 no-card、initialization-required、login-required、
  authenticated、unavailable 五态及统一 `mounted` 布尔值。认证优先于已经消费的 route；若认证已
  清除但仍有残留挂载，则固定显示 unavailable 并禁止新卡动作，清理成功后才重新显示保留 route。
- 登录固定消费一个可变 UTF-8 区，复用 NFC 密码入口并把重试／上下文失效语义交给 P2.14c；初始化
  要求密码与确认来自互不重叠的内存区，分别执行 NFC 后再生成 P2.12 不可伪造确认并应用当前强度
  策略。成功、凭据拒绝、路由拒绝、畸形 UTF-8、强度／确认失败和下游异常都会擦除全部桥接输入。
- 安全复核时曾按旧需求文本尝试加入“已主动确认口令遗忘后果”的布尔门槛；用户于 2026-08-31
  明确该决策随忘记密码整体协议暂缓，本切片已撤回该门槛，并同步修订需求与里程碑。控制器不保存
  该确认位，也不允许未来页面把它误作本地持久状态。
- 本切片不修改 helper／IPC、ViKey 适配器、HID 通知、实体卡、COM/WebView 或主 App 组合。下一
  切片应扩展安全数组桥接与页面状态机，再将正式 ViKey 来源、生命周期控制器、P2 会话和驱动容器
  配置在 composition root 中组合；在此之前开发 EXE 可见行为仍是 P1。

#### 测试、安全复核与提交状态

- 新增 `card_product_ui_session_controller_tests`，覆盖五态投影、认证优先级、遗留挂载阻断与清理、
  登录 NFC／来源擦除／可重试凭据拒绝、认证后挂载失败、初始化双输入独立 NFC、匹配和强度拒绝、
  首输入 UTF-8 失败时第二输入仍擦除、错误 route、重叠输入拒绝，以及统一卸载转发。
- 首次完整 Debug 增量测试中，`card_device_lifecycle_controller_tests.exe` 因测试对象仍按新增虚接口前
  的旧 `CardEntryContextController` 布局编译，而核心库已使用新布局，触发无效内存读取并留下系统
  错误弹窗。执行全量 clean rebuild 后同一测试立即通过；这是增量构建旧对象失配，不是产品卡流程
  缺陷。后续涉及公开多态布局的切片必须至少执行一次干净重建。受限测试进程的遗留弹窗需由用户
  点击“确定”关闭，当前会话无权强制结束该进程。
- Debug、MSVC `/analyze`、ASan 三套干净构建与完整 CTest 均为 **48/48**：46 项实际通过、2 项因
  Codex 父令牌受限按既有规则跳过；`/analyze` 无项目诊断，ASan 未报告内存错误。新增测试三套均
  实际通过，`git diff --check` 通过；不涉及 helper／伪 DLL 或实体卡，无需用户
  手动补跑 PowerShell。
- 安全复核确认 UI 边界不暴露硬件 ID、密码命中的信封、模式、档位、容器形态或卷类型，不记录或
  持久化输入及状态；两个初始化输入不能别名，所有拒绝和异常路径统一清零，残留挂载也不能与新
  插卡授权叠加。剩余边界是 WebView/COM 复制面、页面强度反馈和正式 App 组合尚未实现；本切片已
  完成实现、测试和复核，用户已批准提交；本地提交随本记录创建。

### 小切片 15b：P2 WebView 安全自动化桥接

#### 需求审计与实现

- P2.15a 提交为 `cb8f95d [P2.15]add product UI session boundary`，未推送。继续审计 Q-07、8.6、
  R-03、P1 WebView 数组适配器／host object、P2.15a 控制器、产品页和 `tools/app/main.cpp` 后确认：
  现有页面及主程序仍只组合 P1 32 字节裸卡链，不能直接改名复用；本切片只建立独立 P2 COM 边界，
  不改变开发 EXE 的可见行为。
- 新增 `CardProductWebViewHostObject`，固定只暴露 `submitLoginPasswordBytes`、
  `submitInitializationPasswordBytes`、`unmount`、`hasActiveSession` 和 `getSessionState` 五个方法。
  五态只编码为 0～4 的中性整数；旧 P1 `submitPasswordBytes` 及硬件 ID、密码角色、模式、档位、
  容器形态、卷类型和根秘密均不存在于可查询成员表。
- 把 P1 SAFEARRAY 入口拆为可复用的 `consume_webview_password_array`：仅接受一维 `VT_UI1` 或
  WebView Runtime 的 `VT_VARIANT`／字节范围 `VT_I4` 数组，在锁页所有者中复制输入后交给产品
  控制器统一执行 NFC；P1 入口继续调用原控制器，不增加第二套规范化。
- 初始化方法按 COM 逆序参数读取两个数组，并在检查任一输入之前同时建立擦除守卫。支持的来源
  数组在成功、长度／类型错误、确认或强度拒绝和下游异常路径均作 best-effort 清零；类型错误的
  `argument_error` 精确指向 COM 参数索引。错误参数数量也会清理已提供的最多两个支持数组。
- 依据用户 2026-08-31 决策再次清理需求中的旧表述：口令遗忘确认继续暂缓，不是卡初始化或明文
  导出确认屏的不可跳过提交条件；本桥不新增该布尔位或任何持久确认状态。
- 本切片不修改 WebView 页面、HID、ViKey helper／IPC、实体卡或主 App composition root。下一
  小切片应实现五态初始化／登录页面并用受控假后端完成 UI 全流程；再下一切片才组合正式 ViKey、
  HID 生命周期、P2 会话和真实容器挂载。

#### 测试、安全复核与提交状态

- 新增 `card_product_webview_host_object_tests`，覆盖五态整数投影、禁止成员表、单输入 NFC／擦除／
  重试与成功、双输入 COM 顺序／独立数组／NFC 相等、确认失败、两个参数各自的类型错误及索引、
  错误参数数量清理、统一卸载、活动会话查询和 property 调用拒绝；P1 数组与 host object 回归测试
  继续通过。
- 定向构建首次发现 `SafeArrayGetDim` 的 Windows 头文件签名不接受 `const SAFEARRAY*`，将内部
  计数辅助函数改为非 const 引用后通过。这是编译期限定符问题，没有生成或运行失配二进制，也未
  复现 P2.15a 陈旧对象布局导致的系统错误弹窗。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **49/49**：47 项实际通过、2 项因 Codex
  父令牌受限按既有规则跳过；`/analyze` 无项目诊断，ASan 未报告内存错误。新增桥接测试三套均
  实际通过，P1 回归通过；不涉及 helper／伪 DLL 或实体卡，无需用户手动补跑 PowerShell。
- 安全复核确认桥接不会向页面返回秘密或可区分形态；双输入在首个错误发生前已同时受擦除守卫，
  锁页副本由 RAII 清除，产品控制器仍是唯一 NFC 与策略入口。无法主动擦除的剩余面仍是 WebView2、
  JavaScript 引擎和 COM 内部可能产生的短期副本；不支持的字符串等非数组参数只拒绝而不解释或
  修改。页面尚未调用本桥，正式 App 也仍是 P1，因此当前还不能进行真实 P2 UI 全流程验收。本切片
  已完成实现、测试和安全复核，用户已批准提交；本地提交随本记录创建。

### 小切片 15c：五态产品页面与假后端 WebView2 闭环

#### 需求审计与实现

- P2.15b 已提交为 `fab54a4 [P2.15]add product WebView bridge`，未推送。审计 Q-07、8.6、R-03、
  P1 产品页／页面契约／真实 Runtime 测试、P2.15a 控制器、P2.15b 自动化桥、当前 CMake 打包和
  `tools/app/main.cpp` 后确认：现有 `index.html` 与 `session.js` 必须继续服务尚未迁移的 P1 开发
  EXE；本切片新增独立 P2 页面资产，避免页面先切换而主程序仍注册旧方法的不可运行半接线状态。
- 新增 `card_session.html`／`card_session.js`，完整渲染无卡、需初始化、需登录、已认证和不可用五态。
  初始化态只显示日常密码与确认两个输入；登录态恒定只显示一个密码框；已认证无卷时不虚构挂载，
  有活动卷或遗留卷时只提供统一安全卸载。页面不出现用户名、模式选择、档位、密码角色、容器形态、
  卷类型或口令遗忘确认。
- 初始化页面按 JavaScript NFC 后 Unicode 码点数展示当前 12～17“可接受”、18～128“较强”反馈，
  低于 12 或高于 128 显示长度问题；该反馈只帮助用户，P2.15a 原生策略仍执行最终裁决。规范等价的
  两个输入不会被页面错误拒绝。
- 登录与初始化均在调用桥前清空 DOM 字段；`TextEncoder` typed array 和传给 COM 的普通数值数组
  在成功、拒绝、编码异常和桥异常路径重复兜底清零。卡状态变化事件会立即清空尚未提交的两个 DOM
  输入并合并异步刷新；原生五态与活动卷两次查询若发生竞争，任何“仍有卷但得到动作 route”的组合
  在页面侧也固定降为不可用，最终原生控制器仍会再次校验 route。
- 新增真实 WebView2 假后端驱动，使用正式 P2 控制器与 P2 `IDispatch`，不绕过数组封送。流程覆盖
  无卡 → 初始化 → 已认证无卷 → 登录、错误密码可重试 → 正确登录挂载 → 清认证但保留遗留卷的
  不可用状态 → 统一卸载 → 返回无卡，并验证规范等价初始化、嵌入 NUL／非 ASCII 登录字节及插拔
  清空未提交输入。
- 本切片不修改主 App、HID、ViKey helper／IPC、实体卡或容器。下一切片应在 composition root 中
  组合正式 ViKey 插卡来源、P2 生命周期、认证／挂载会话、P2 host object 和本页面；届时即可开始
  真卡带 UI 流程测试，但“新卡初始化”仍依赖经只读验证的空白测试卡。

#### 测试、安全复核与提交状态

- 新增 `card_product_ui_page_contract_tests`，锁定两个 route-specific 密码控件、五态完整性、禁止
  字段、严格 CSP、P2 方法名、DOM／typed array／普通数组擦除、NFC 长度反馈阈值、异步串行、插拔
  清空与刷新合并，以及活动卷竞争时 fail-closed。既有 P1 页面及 Runtime 资产保持不变。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **50/50**：48 项实际通过、2 项因 Codex
  父令牌受限按既有规则跳过；`/analyze` 无项目诊断，ASan 未报告内存错误。P2 真实 WebView2
  假后端闭环在三套构建的普通本机权限下均退出 0；受限沙箱内新旧两套 Runtime 测试都退出 1，
  对照确认是 WebView2 启动环境限制而非 P2 页面断言失败。不涉及 helper／伪 DLL 或实体卡，无需
  用户手动补跑 PowerShell。
- 安全复核确认页面不持久化或记录输入、状态和强度，不展示可区分密码命中结果；错误密码与初始化
  拒绝均使用通用反馈，残留挂载优先提供清理而不允许新授权。页面无法主动擦除 JavaScript 字符串、
  DOM/WebView2/COM 内部副本的既有 R-03 剩余风险仍在；可控字节数组均已清零，插拔也会立即删除
  可见 DOM 值。正式 App 尚未组合本页面，因此当前“带 UI 全流程”只完成假后端验收，不代表实体卡
  或真实容器已通过。本切片已完成实现、测试和安全复核，用户已批准提交；本地提交随本记录创建。

### 小切片 15d：P2 ViKey 开发主程序组合

#### 需求审计与实现

- P2.15c 已提交为 `c281211 [P2.15]add five-state product UI`，未推送。审计 Q-07、8.6、R-03、
  P2.11～P2.15 全链、现有 P1／ViKey 开发应用 composition root、HID 回调、窗口关闭守卫和 CMake
  依赖后确认：正式组合必须另建默认排除的 P2 开发目标，不能把尚需保留的 P1 开发入口原地替换，
  也不能从旧 `ApplicationSession` 或 32 字节 blob store 借用登录能力。
- 新增 `usb_vault_vikey_p2_development_app`，标题明确标记“P2 ViKey 联网开发模式”，继续要求既有
  网络开发确认开关、helper／供应商目录绝对路径及两个 SHA-256。它只组合
  `VikeyHelperCardEnrollment -> CardEntryContextController -> CardAuthenticationSession ->
  CardLoginMountSession`，再接入 `CardProductUiSessionController`、P2 host object、五态页面、
  `CardDeviceLifecycleController` 和严格 VID/PID HID 通知。
- 启动枚举发现卡时只执行一次原子只读探测；页面提交初始化后才可能进入 marker／Card Record 写
  事务。HID 到达、移除、认证撤销和活动卷变化均刷新页面；拔卡沿既有控制器固定先销毁入口上下文、
  清根秘密，再普通／强制卸载。窗口关闭守卫新增 P2 重载：无卷认证可直接退出，活动或遗留卷只有
  成功清理后才允许关闭，失败保留状态供重试。
- 开发凭据和配置头文件从 P1 `vikey_helper_blob_backend` 改为只依赖通用 `vikey_helper_backend`；
  CMake PUBLIC 依赖相应降为 `usb_vault_core`。P1 开发应用仍显式链接自己的 blob backend，P2 目标
  的 Ninja 链接查询已不含它。默认应用、P1 ViKey 开发应用和 P2 ViKey 开发应用三者保持独立。
- P2 应用打包 `card_session.html`／`card_session.js`，注册 P2 `IDispatch`；默认和 P1 开发应用仍
  打包原 `index.html`／`session.js` 并注册 P1 host object。本切片没有启动任何开发 GUI，没有读取
  Windows 凭据、启动 helper、访问实体卡或容器，也没有写卡授权扩张。

#### 测试、安全复核与提交状态

- 扩展 `ui_close_guard_tests` 覆盖 P2 无卷直接关闭、卸载失败阻止关闭并保留状态、重试成功后允许
  关闭。开发配置／凭据聚焦测试、默认应用、P1 开发应用和 P2 开发应用的定向构建均通过。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **50/50**：48 项实际通过、2 项因 Codex
  父令牌受限按既有规则跳过；三个应用目标在三套配置均构建成功，`/analyze` 无项目诊断，ASan 未
  报告内存错误。P2 链接图只含 Card Record backend、helper client／protocol、正式 card store、
  HID 和 WebView2 等预期依赖；导入表含开发凭据所需 `CredReadW` 及 HID／SetupAPI，不含
  `ViKey.dll`。不涉及伪 DLL 进程变更，无需用户手动补跑 PowerShell。
- 安全复核确认 P2 composition root 不构造 P1 裸卡读取链；凭据仍由每次 helper 操作新鲜加载到锁页
  所有者，命令行只承载非秘密路径、摘要和显式开发确认。联网 helper、Windows Credential Manager
  开发部署和仓库外未签名厂商 DLL 仍不满足生产隔离要求。实体卡 UI 全流程、当前容器状态及新卡
  出厂全零口径尚未验证；下一切片应先用现有非空白开发卡做只读入口／不可用页冒烟，再由用户选择
  是否准备专用空白卡进行有写入的初始化验收。本切片已完成实现、测试和安全复核，并已按用户批准
  提交为 `ce7390b`。

### 小切片 15e：一次性 P1 开发卡重置工具

#### 需求审计与实现

- P2.15d 已提交为 `ce7390b [P2.15]add development product application`，未推送。用户确认当前唯一
  P1 卡没有用户数据或正式容器，客户卡也将直接从当前 P2 版本开始，因此不实现根秘密迁移，只需
  一个能把该开发卡恢复为 P2 可接纳空白状态的最简工具。
- 审计 N-06、完整用户区分类、ViKey 会话随机访问、helper IPC／客户端和既有 P1 写入探针后确认：
  普通 P2 bootstrap 必须继续拒绝 `legacy_p1_or_foreign_prefix`；重置也不能只把地址 0 的 32 字节
  写零而跳过硬件身份与完整区域复验。该模糊状态无法证明一定来自 P1，因此仅由开发工具的显式
  永久擦除确认承担人工授权，不扩散到产品 UI 或自动接纳路径。
- 新增默认构建排除的 `usb_vault_vikey_p1_card_reset`。命令行只接受既有联网开发确认、helper／供应商
  绝对路径与摘要，以及精确的永久擦除确认参数。工具先用新鲜凭据原子探测唯一插卡；已是 blank
  时零写入成功退出，精确 P1 同形前缀才进入重置，其他状态全部拒绝。重置后再次加载新鲜凭据，
  要求同一硬件 ID 的新探测结果为 blank 才报告成功。
- helper IPC 升至 v11，新增固定尺寸 `development_media_reset`：请求只携带开发部署材料、非零期望
  硬件 ID、固定 `legacy_p1_or_foreign_prefix` 状态和锁页厂商凭据。helper 打开会话后先比对硬件 ID，
  再在同一会话复验完整介质分类；外来尾部、产品证据、冲突、空白和状态变化都不会触发写入。
- ViKey 私有重置原语按 4 KiB 分块对完整用户区写零并逐段回读，地址 32 之后的区域先处理，最后才
  清除原 32 字节前缀。供应商报告写失败时只在回读证明目标段已全零后接受；前缀部分落盘则报告
  `device_failure`，卡仍保持 P1 同形状态，可由重新探测后再次显式运行；前缀完整落盘则为 blank。
- 本工具不保留或导出 P1 根材料、不创建 Card Record、不设置登录密码、不处理容器、不接 UI，也
  不是忘记密码重置入口。本切片只构建和运行假卡测试及无参数拒绝路径，没有加载 Windows 开发凭据、
  启动真实供应商 DLL、访问或写入实体卡。

#### 测试、安全复核与提交状态

- `vikey_card_record_io_tests` 覆盖完整区域清零、前缀最后写、所有非目标期望状态零读写拒绝、实际
  状态变化零写入拒绝、前缀部分写失败保持可重试、重试完成，以及供应商误报失败但完整写入经回读
  接受。协议测试覆盖 v11 操作、固定载荷、硬件 ID／状态往返和零 ID／外来状态拒绝；helper 进程
  测试及伪 DLL 已接入 P1 同形介质、成功重置、错 ID 和错误状态路径。
- Debug、MSVC `/analyze`、ASan 三套完整构建与 CTest 均为 **50/50**：48 项实际通过、2 项因 Codex
  父令牌受限按既有规则跳过；重置 CLI 在三套配置均单独构建成功，`/analyze` 无项目诊断，ASan 未
  报告内存错误。Debug 无参数启动在任何凭据或卡访问前输出用法并退出 1；`git diff --check` 通过。
- 安全复核确认破坏能力留在默认排除的开发 EXE 和联网开发 helper 操作中，需两个精确确认参数，
  并以探测 ID、helper 内同卡复验和最终同 ID blank 复验约束目标。剩余风险是该 32 字节形状本质上
  仍可能是同形外来数据；用户已明确授权当前无数据开发卡永久擦除，正式产品仍拒绝自动覆盖。
  本切片实现和三套验证已完成，用户已批准提交，并另行明确授权在提交成功后运行工具重置当前
  实体开发卡；本地提交已创建为 `27faf01`。

#### 实体执行结果

- 提交后使用该切片构建的重置 CLI、helper 和白名单匹配的官方 x64 `ViKey.dll` 执行实体检查。
  从 Codex 包身份直接启动的两次尝试都在卡访问和写入前被本地制品最终路径门拒绝：Windows 将
  联网 helper 的 `LocalAppData` 临时目录虚拟化到了 `Packages` 路径，安全门按设计识别为重定向并
  fail closed。诊断改动已撤回，源码恢复为提交内容。
- 随后经 Windows Explorer 的普通桌面令牌运行同一工具；原子介质探测报告当前卡已是 `blank`，
  因而工具按设计执行 **零次写入** 并以退出码 0 成功结束。未创建 P2 Card Record，下一步可直接
  进入 P2 初始化；开发记录不保存本次探测到的硬件 ID。
- 运行器、结果文件及两组临时 helper／供应商 DLL 副本已删除。仓库只留下本条实体结果记录，
  `27faf01` 之后没有新的实现改动；本记录尚未提交，等待用户决定。

### 小切片 15f：初始化密码强度与常驻显隐控件

#### 需求审计与实现

- 用户在实体 P2 初始化页验看时要求把纯文字长度反馈改成“密码强度＋三个格子”，空密码不显示，
  不再显示还差几个字符；少于 12 字符时初始化按钮必须灰色且不可点击，常态提示改为“请输入并
  确认日常密码，至少12字符”。审计 P2.12c 原生 NFC／12～128 码点策略和 P2.15c 页面后决定只
  细化前端投影，不增加组合字符规则、在线泄露检查或新的强度裁决。
- 强度格只读取第一个“设置日常密码”输入：NFC 后 1～11 显示一格红色，12～17 显示两格黄色，
  18～128 显示三格绿色，超过 128 显示三格红色并保持不可提交；空输入完全隐藏。初始化按钮仅在
  第一个输入满足 12～128 且确认输入非空时解除灰色，双输入一致性及最终强度仍由原生边界裁决。
- 用页面自有按钮替换 WebView2 只在首次输入／聚焦时出现的瞬态密码揭示控件，并屏蔽原生
  `::-ms-reveal`。日常密码和确认密码各有独立、可访问的 `aria-controls`／`aria-pressed` 状态；
  点击外部不会移除图标或复位显隐。提交、插拔、路由变化和统一 DOM 清理都会把两个输入恢复为
  `type=password`，避免把明文显示状态带入下一次输入。
- 页面和真实 Runtime 夹具的缓存版本升为 `p2-15f-password-controls`。WebView2 夹具新增空输入隐藏、
  11 字符禁用、一／二格状态、两个显隐按钮失焦保持及提交后复位断言；测试 harness 为每个失败标签
  分配独立退出码。首次运行据此发现原夹具在外部刷新刚清空输入、页面仍 busy 时提前点击的竞态，
  已改为同时等待稳定登录态，不改变产品逻辑。

#### 测试、安全复核与提交状态

- Debug、MSVC `/analyze`、ASan 三套默认构建和完整 CTest 均为 **50/50**：48 项实际通过、2 项因
  Codex 父令牌受限按既有规则跳过；`/analyze` 无项目诊断，ASan 未报告内存错误。三套默认排除的
  真实 WebView2 假后端目标均单独构建并退出 0，Debug 页面契约定向测试也退出 0。Debug P2 ViKey
  开发应用已在旧窗口关闭后重新链接，并复制新版 HTML／JavaScript；未访问实体卡或真实凭据。
- 安全复核确认灰色按钮只是提前拒绝，不能替代原生 NFC、长度和双输入确认；脚本仍不记录或持久化
  密码。用户主动显形后，密码会按预期以文本形式存在于 DOM／WebView2 内部，延续 R-03 已知不可
  擦除副本风险；本切片只让该状态跨失焦保持，不跨提交、插拔或路由清理。可控 typed array、普通
  数组和 DOM 值的既有提交前清除与异常兜底未削弱。实现、三套验证和安全复核已完成，等待用户在
  实体初始化页目测后决定是否提交；`27faf01` 后的实体执行记录也将随本切片一并纳入候选提交。

#### 目测反馈修订（初始化页反馈稳定性）

- 用户在初始化页目测后提出四点：强度框必须一开始就占位、失败提示不得顶掉底部常态指引、
  “无法初始化安全卡，请检查两个密码输入后重试。”措辞不准、失败后不该清空两个输入框。
- 强度输出移入 `.strength-slot` 占位容器：该容器只在 `initialization-required` 状态存在，并以
  `min-height` 预留强度条高度，因此强度框从空输入到出现不再改变卡片高度；登录路由仍不预留。
- 新增独立的红色 `#card-session-error` 输出，排在 `#card-session-status` 之下。`render` 增加第二个
  参数，底部常态指引始终保持为 `messageForCurrentState()`，失败只追加下一行；任何一次输入都会
  清除该行，避免留下过期报错。
- 失败措辞按原生裁决口径拆分：`initializationInputsMatch` 用与 `password_ingress` 相同的 NFC 归一化
  比较两个输入，一致性失败显示“初始化失败，两次密码不一致。”，其余仍可重试的失败显示
  “初始化失败，请重试。”，不把硬件或策略失败误报成密码不一致。
- 失败保留输入不削弱既有清除顺序：两个 DOM 值仍在原生调用之前清空，明文只保存在两个局部
  字符串里，成功路径、失败分支和 `finally` 三处各自释放；仅当路由仍是 `initialization-required`
  且没有待处理的外部刷新时才回填输入框，插拔导致的清空仍然生效。该保留使明文字符串的存活
  时间跨越一次原生调用，是 R-03 已知不可擦除副本风险在本切片的一处扩大，按用户要求接受。
- 页面与夹具缓存版本升为 `p2-15f-init-feedback`；契约测试新增占位、红色失败行位置、措辞与
  输入保留断言，WebView2 夹具新增一次真实的不一致提交，断言底部指引不变、红行文案、两个
  输入原样保留，以及再次输入后红行消失。Debug 完整 CTest 为 49/50：新增断言全部通过，唯一失败
  的 `vikey_helper_process_tests` 是既有网络隔离限制（`network isolation: ViKey helper exited without a
  complete response`），与本次改动无关。P2 开发应用窗口仍打开导致 EXE 无法重链接，但本次只改
  HTML／JavaScript，两个资源已直接复制到 `out/build/dev`。
- 提交：`896e3f0 [P2.15]refine initialization password feedback`。

### 小切片 16a：认证后仪表盘外壳

#### 需求、计划与既有实现审计

- P2.15f 已按用户批准提交为 `896e3f0 [P2.15]refine initialization password feedback`，当前切片
  从干净工作区开始。用户指定以仓库根目录 `usb-vault_ui-disign.html` 为视觉来源：登录成功直接
  进入现有风格的仪表盘，删除全部 Emoji 和账号管理，在加密盘／隐藏盘概览右侧增加新建入口；
  文件管理、备份、历史和设置不隐藏，但必须不可点击，鼠标悬停显示“正在开发中”。
- 复核 Q-07、8.4、8.6、P2.15 的五态页面／窄化桥、现有 WebView2 页面契约及静态原型后确认：原型
  包含注册、忘记密码、`admin`、内联事件、模拟卷数组、文件内容、备份任务和历史记录，不能直接
  打包。正式页面只能复用颜色、顶栏、侧栏、卡片和弹层骨架，继续使用 `script-src 'self'` 的 CSP
  与外部脚本，并且不得为仪表盘新增账号、档位、密码角色、容器形态或本地持久状态。
- 当前原生桥只投影五态和统一活动卷布尔值，尚不能准确区分 A/B 的单独存在、容量或挂载情况。
  因此本切片不虚构两个卷的卡片数据，只显示“安全卡已认证”和统一“活动卷已挂载／未挂载”；
  A/B 概览保留中性占位，真实数据投影随建卷链一并设计。

#### 实现

- `ui/card_session.html` 保留原插卡、初始化、登录和不可用页，新加认证后应用外壳。只有原生状态为
  `authenticated` 才隐藏安全入口并显示仪表盘；`unavailable` 即使有遗留挂载也返回原安全清理页。
  顶栏只显示产品名、已认证卡状态和存在活动卷时的安全卸载，不包含用户名、头像、下拉菜单、注册
  或忘记密码。
- 仪表盘、加密盘管理和隐藏盘管理为可点击导航；文件管理、备份中心、操作历史和系统设置使用
  无事件处理器的 `aria-disabled=true` 元素，保留可见的 SVG 图标，并同时用 `title` 与自绘 tooltip
  在鼠标悬停／键盘聚焦时提示“正在开发中”。所有图标均为内联几何 SVG，产品 HTML／脚本不含
  Emoji。
- 仪表盘的加密盘概览与隐藏盘概览、两个管理页均提供“新建”按钮。按钮只在已认证且非 busy 时
  可用，打开对应的建卷流程预览弹层；由于真实建卷后端属于后续切片，预览仅列出参数、UAC、创建
  挂载三步，继续按钮明确禁用，不调用 host object，也不伪造成功结果。
- 页面状态变化统一关闭建卷弹层并回到仪表盘；拔卡、路由失败或遗留挂载均不会把仪表盘留在屏幕。
  活动卷的安全卸载复用既有 `bridge.unmount()`，完成后留在已认证仪表盘并更新统一活动卷状态。
  页面缓存版本升为 `p2-16a-dashboard-shell`。

#### 测试、安全复核与提交状态

- 页面契约测试新增认证态独占切换、三个正式导航页、A/B 各两处新建入口、四个不可用导航项及
  tooltip、无内联事件／账号／模拟数组／Emoji 的断言。WebView2 驱动新增认证后仪表盘、导航、建卷
  弹层、活动卷安全卸载及外部状态变化退出仪表盘的断言，并为新增失败点和驱动异常配置退出标签。
- Debug、MSVC `/analyze`、ASan 三套默认构建和完整 CTest 均为 **50/50**：48 项实际通过、2 项因
  Codex 父令牌受限按既有规则跳过；`/analyze` 无项目诊断，ASan 未报告内存错误。三套默认排除的
  WebView2 集成目标均构建成功，Debug P2 ViKey 开发应用已重新打包新版 HTML／JavaScript；页面
  契约定向测试退出 0，`git diff --check` 通过。
- 本轮用显式进程等待复核真实 WebView2 退出码时，产品集成测试和**未修改的** typed-array WebView2
  基线均在当前宿主下于页面驱动消息前退出 1；这属于当前 Codex／WebView2 Runtime 环境限制，不能
  继续把 PowerShell 启动 GUI 后遗留的旧 `LASTEXITCODE=0` 视为成功。浏览器安全策略同时拒绝直接
  打开本地 `file://` 页面，未采用 localhost 或其他方式绕过。因此本切片仍需用户在 P2 开发应用中
  目测认证后页面；本记录不宣称真实 Runtime 自动化通过。
- 安全复核确认仪表盘完全由原生 `authenticated` 状态驱动，未扩大 COM 方法面，未读取或持久化
  A/B 形态、密码、根秘密、硬件身份或账号数据；原型的模拟内容没有进入产品资源。开发中导航在
  所有认证情形下保持一致，不形成档二／档三差异。剩余风险是弹层尚未实现焦点圈闭，且真实 A/B
  建卷、容量状态和实体卡视觉验收均未进入本切片。
- 提交：`dc0fb3a [P2.16]add authenticated dashboard shell`。

### 小切片 16b：认证会话内的加密盘 A 建卷事务

#### 需求、计划与既有实现审计

- 审计 N-05、N-08、8.6、现有 `CardLoginMountSession`、挂载编排、P1.4 建卷 helper 和 P1.7
  一次性测试夹具后确认：A 与 B 必须独立创建；A 的卷密码固定为 `KDF(S,"diskA")`；普通 A 建卷
  也必须经过带 `requireAdministrator` manifest 的同一个独立 helper，不能因形态不同省略 UAC；
  代码和本地产物不得新增档位、模式或容器组合状态。P1.7 夹具属于 disposable evidence，本切片只
  重新实现已验证的暂存、发布和清理语义，没有把夹具链接进产品。
- 本切片边界限定为真实 A 建卷后端，不修改页面或 COM 方法：认证会话以受控作用域使用已持有的
  根秘密 S，创建标准容器、发布并立即挂载。容量输入的 UI、进度呈现和错误文案留给下一切片，
  B 普通／隐藏卷及其卡记录事务继续保持独立。

#### 实现

- `IAuthenticatedCardVolumeSession` 新增仅接收字节容量的 `create_container_a()`；UI 不提供根秘密、
  登录密码或派生结果。`CardLoginMountSessionCore` 是 `CardAuthenticationSession` 唯一既有友元，
  只有已认证且 A 尚未挂载时才可借用锁页根秘密，以固定五字节 `diskA` 和 A 的既有固定盐执行
  KDF。固定输入与 64 字节卷密码均保留在 `SecureBuffer`，在派生、创建、挂载的成功和异常路径
  显式清除。
- 正式会话从主程序接收同目录的 `usb_vault_volume_creator.exe`，构造时复用既有 helper 文件校验；
  `create_standard()` 因 helper manifest 无条件触发 UAC。最终路径 `container_a.hc` 和固定暂存路径
  `container_a.hc.preparing` 都先执行绝对路径、非目录、非 reparse 检查，存在任一目标即拒绝，
  helper 继续通过 `CREATE_NEW` 防止竞态覆盖。
- helper 成功返回后才把暂存文件视为本事务所有；随后用不带 replace 标志且要求
  `MOVEFILE_WRITE_THROUGH` 的重命名发布。创建调用自身失败时沿用 helper 内部的调用者身份清理，
  不删除可能由检查竞态产生的外来文件；发布失败时删除已确认归本事务所有的暂存文件，清理失败
  以 `ContainerCreationCleanupError` 同时保留原始操作与清理异常，且诊断文本不含路径或秘密。
- 发布后用 SHA-512、PIM 0、可写、延迟 Shell 通知的固定选项立即挂载 A。挂载失败保留已经完成的
  最终容器，下一次登录可按既有 A-only 路径挂载，不因演示失败删除有效数据；Shell 通知失败发生
  在精确挂载状态记账之后，后续普通／拔卡强制卸载仍能清理真实盘符。认证状态在这些普通建卷或
  挂载失败后均保留，继续由物理拔卡生命周期统一撤销。

#### 测试、安全复核与提交状态

- 扩展 `card_login_mount_session_tests`，覆盖未认证、非法容量、最终／暂存目标已存在、固定
  `diskA` 与盐、创建失败、发布失败及清理、清理二次失败、发布后挂载失败保留最终容器、通知失败
  保留真实挂载状态，以及成功后的重复创建拒绝。其他四个会话假实现只补齐尚未由 UI 调用的新接口，
  既有 UI／WebView／关闭守卫语义不变。
- Debug、MSVC `/analyze`、ASan 三套默认构建和完整 CTest 均为 **50/50**：48 项实际通过、2 项因
  Codex 父令牌受限按既有规则跳过；`/analyze` 无项目诊断，ASan 未报告内存错误。Debug P2 ViKey
  开发应用和 UAC 建卷 helper 已成功链接，`git diff --check` 通过。本切片未从页面发起真实 UAC，
  也未在实体卡上执行建卷或挂载，因此不宣称真机建卷闭环已验收。
- 安全复核确认没有新增根秘密 getter、用户口令输入、档位字段、日志、注册表或配置写入；所有路径
  采用固定 A 名称，派生与挂载显式锁定既有算法参数。剩余风险是进程在 helper 成功与发布之间被
  强制终止时可能留下固定 `.preparing` 文件，后续尝试会失败关闭而不会自动删除；发布后挂载失败会
  留下有效 `container_a.hc`，下一次登录负责恢复挂载。页面仍只有禁用的预览按钮，下一切片才接入
  容量输入、原生调用和忙碌／成功／失败反馈。
- 提交：`5f35775 [P2.16]add authenticated container A creation transaction`。

### 小切片 16c：加密盘 A 容量与页面建卷闭环

#### 需求、计划与既有实现审计

- P2.16b 已按用户批准提交为 `5f35775`，本切片从干净工作区开始。审计现有五态 UI 控制器、受限
  `IDispatch` host object、认证后仪表盘弹层、WebView2 真实 Runtime 夹具及 P1.4 已验证容量后确认：
  页面只需提供 A 容量，不应接触 S、卷密码、路径、盐或挂载参数；隐藏盘 B 仍须保持独立预览，
  不能因共用弹层误调用 A 建卷。
- 为最快完成可演示链路，当前 UI 使用 20～1048576 MiB 的整数范围，默认 64 MiB；20 MiB 是既有
  P1.4 实际标准卷测试已覆盖的下界，1 TiB 上界用于阻止 JavaScript 数值和乘法失控，并不代表磁盘
  空间预检。实际空间不足继续由建卷 helper 明确失败。

#### 实现

- `CardProductUiSessionController::create_container_a()` 只在原生状态精确为 `authenticated` 时接受
  32 位 MiB 数值，在 C++ 中复验上下界并以 64 位无溢出换算为字节，再调用 P2.16b 会话事务。
  普通失败不会撤销认证或伪造活动卷；会话后端继续负责已有目标、派生、UAC、发布与挂载裁决。
- P2 WebView host object 新增单一方法 `createContainerAMib`。无类型信息的 COM 边界只接受非负
  `VT_I4`、`VT_UI4` 或精确整数 `VT_R8`，拒绝字符串强制转换、分数、非有限值、越界和错误参数数；
  页面仍无法读取硬件 ID、密码角色、容器组合、S 或卷密码。
- 加密盘弹层新增容量输入、范围说明和 `aria-live` 状态行。20 MiB 以下、超过 1 TiB、非十进制整数
  均保持“创建加密盘”禁用；有效提交先进入全局 busy，禁用导航、弹层关闭和重复提交，显示正在
  请求管理员权限。成功刷新原生活动卷状态并在弹层显示“创建成功”，返回按钮改为“完成”；失败
  重新读取原生状态，认证仍有效时保留容量供重试，并提示若已发布但未挂载可重新插卡恢复。
- 页面状态离开 `authenticated` 会强制关闭并清空弹层临时状态；原有外部状态变化合并机制仍在操作
  结束后执行。隐藏盘入口继续显示后续切片说明，隐藏 A 容量表单，继续按钮保持禁用且提示正在
  开发中。资源缓存版本升为 `p2-16c-container-a-ui`。

#### 测试、安全复核与提交状态

- 控制器测试新增错误 route、上下界、精确 MiB→字节换算、成功挂载投影和失败保留认证；host
  object 测试新增 `VT_R8`／`VT_UI4` 成功、分数／字符串／越界／缺参拒绝、失败状态不变。页面契约
  锁定容量控件、范围、仅 A 路由、容量专用桥以及 busy／成功／恢复提示；Node `--check` 退出 0。
- WebView2 真实 Runtime 夹具现实际走完：认证后进入 A 弹层、19 MiB 禁用、64 MiB 第一次原生失败
  后显示可重试错误、第二次成功、活动卷与完成态刷新、关闭弹层，再执行原登录／残留卷／卸载回归。
  Debug、`/analyze`、ASan 三套该 GUI 集成 EXE 均隐藏运行并退出 0。
- Debug、MSVC `/analyze`、ASan 三套默认构建和完整 CTest 均为 **50/50**：48 项实际通过、2 项因
  Codex 父令牌受限按既有规则跳过；`/analyze` 无项目诊断，ASan 未报告内存错误。三套 P2 开发
  应用和默认排除的 WebView2 集成目标均成功链接，`git diff --check` 通过。本切片没有发起真实 UAC、
  创建实体容器或访问实体卡，真机建卷仍需用户目测验收。
- 安全复核确认新桥只增加公开容量，不增加秘密输入、根秘密 getter、档位／模式／容器组合字段或
  本地持久化；页面和原生两层独立验证容量，后端仍使用不覆盖发布与固定派生。当前最小实现的已知
  风险是 COM 建卷调用同步等待 KDF、UAC helper 和格式化，操作期间主窗口可能暂时不响应，HID 拔卡
  事件在该真实耗时窗口内的及时处理尚未专项验收；用于当前演示时必须保持安全卡插入，正式交付前
  应把长任务迁出 UI 线程并补拔卡竞态验收。统一活动卷布尔值也无法提前判断 A 是否已存在，重复
  创建由后端失败关闭并显示通用错误。
- 首次实体卡联调暴露出部署缺口：`dev` 缓存未设置 `USB_VAULT_FORMAT_DLL`，P2 应用和提权 helper
  虽可成功链接，但 helper 同目录没有 `VeraCryptFormat.dll`，因此创建在格式化开始前以
  `format_backend_unavailable` 失败；没有留下最终容器或 `.preparing` 文件，重新插卡不能恢复。
  已核对并部署此前真实集成验收所用的补丁版 DLL（SHA-256
  `E411BB9552ACA94B1285570E45C3D77D44652519309B7D33EC0A4EF99AF28AB3`），确认含
  `VeraCryptFormatBinary` 导出；旧 `out/vendor` 文件缺少该导出，明确未复用。
- CMake 新增只挂在默认排除 P2 开发应用上的后端部署目标：配置了补丁 DLL时，每次构建 P2 应用都
  重新执行 `copy_if_different`，即使 helper 本身未重链也会补回被删除的运行时 DLL；未配置时则以
  明确命令行提示使该 P2 目标构建失败。普通默认构建、单元测试和不需要实体建卷的目标继续允许在
  没有第三方 DLL 时配置和运行。
- 修复后重新生成 Debug dev 目录并成功链接 P2 应用；输出目录 DLL 摘要与验收产物完全一致，导出
  表含 `VeraCryptFormatBinary`。P2 目标的增量构建实际执行了部署命令；独立无 DLL 配置目录中只构建
  部署目标，按预期以退出码 1 和明确配置提示失败。Debug 完整 CTest 为 **50/50**：48 项实际通过、
  2 项按既有环境限制跳过。本次部署修正没有运行提权 helper、创建容器或访问实体卡，下一步仍需
  用户重新启动已重建的 P2 应用并完成一次真实 UAC／格式化／挂载联调。
- 实体 ViKey 联调随后确认修复后的 P2 应用已成功创建并挂载 A；用户由成功界面继续提出容量与盘符
  展示需求。提交：`792bc06 [P2.16]connect container A creation UI`。

- 提交：`792bc06 [P2.16]connect container A creation UI`。

### 小切片 16d：加密盘 A 实时信息投影

#### 需求、计划与既有实现审计

- P2.16c 已按用户批准提交为 `792bc06`。审计 N-05、8.4、8.6、现有
  `CardLoginMountSessionState`、固定 `ContainerLayout` 和 `VolumeProvider::mounted_volumes()` 后确认：
  A 是明确的独立普通卷，可在认证后显示其存在、容量和盘符；但不得为展示新增档位、B 的卷类型、
  最近盘符或历史记录。会话已经可信记账 A/B 各自挂载标志和固定盘符，容量则须从固定 A 文件的
  当前属性读取，不能只缓存本次建卷输入，否则应用重启后信息会丢失。
- 本切片只补 A 的只读状态投影和现有 UI 占位替换，不增加重新挂载方法或 B 建卷。安全卸载后的
  已认证会话显示“已创建、未挂载”；恢复挂载仍沿用拔插后登录链，避免在展示切片扩张卷生命周期。

#### 实现

- `IAuthenticatedCardVolumeSession` 新增仅含 `exists/mounted/size_bytes/drive_letter` 的 A 状态快照。
  生产运行时用 `GetFileAttributesExW` 实时读取固定 `container_a.hc`，继续拒绝目录和 reparse point；
  盘符只在会话精确记账为 A 已挂载时返回。若会话声称已挂载但背文件已消失，状态查询失败关闭，
  不向页面伪造一致结果。
- UI 控制器仅在 `authenticated` 路由查询 A；容量只在文件大小能精确换算为 32 位 MiB 时投影，
  否则以 0 表示不可展示。COM 增加三个无参数只读方法，分别返回 A 状态、MiB 容量和当前盘符，
  不返回容器路径、S、卷密码、硬件 ID、B 状态或 VeraCrypt 类型。
- 仪表盘和“加密盘管理”页共用实时字段：未创建、已创建未挂载和已挂载三态；存在时显示容量，
  仅挂载时显示盘符。A 已存在后隐藏两个“新建加密盘”入口；安全卸载后保留容量并把盘符显示为
  “未挂载”，重新插卡登录后恢复固定盘符。隐藏盘页面和入口保持原样。缓存版本升为
  `p2-16d-container-a-status`。

#### 测试、安全复核与提交状态

- 会话测试覆盖 A 不存在、存在未挂载、挂载后容量／盘符以及背文件矛盾失败；控制器和 COM 测试
  覆盖未认证门控、MiB 精确换算、三态及 `VT_I4/VT_UI4/VT_BSTR` 类型。页面契约锁定两个 A 信息区、
  三个只读桥调用、移除旧 A 占位和不查询 B 类型；契约测试主函数同时补异常捕获，使断言失败正常
  输出并退出，不再形成 Windows 未处理异常弹窗。
- Debug 定向 5/5、两个 JavaScript `node --check` 和隐藏运行的真实 WebView2 集成均已通过；集成
  流程验证初始化后 A 未创建、首次创建失败不变、第二次成功后显示 64 MiB／`X:` 并隐藏新建入口，
  以及重登后恢复同一信息。Debug、MSVC `/analyze`、ASan 三套默认构建和完整 CTest 均为
  **50/50**：48 项实际通过、2 项按既有环境限制跳过；三套 P2 开发应用和真实 WebView2 集成目标均
  成功链接，三套 GUI 夹具隐藏运行退出 0，`/analyze` 无新增项目诊断，ASan 无内存错误，
  `git diff --check` 通过。当前实体 `container_a.hc` 文件属性确认为 67108864 字节（64 MiB），检查
  仅读取路径属性，没有打开或读取容器内容。
- 安全复核确认状态只存在于进程内并仅在认证路由返回；没有新增配置、注册表、日志、路径、盘符
  历史、档位、B 形态或卷类型输出。三次 COM 只读查询可能被异步插拔事件穿插，页面会严格校验
  状态／容量／盘符组合并转入既有失败关闭路径，外部变化仍由合并刷新恢复，不会显示拼接出的错误
  盘符。剩余功能限制是安全卸载后的当前会话没有“重新挂载”动作，界面会明确要求重新插卡登录；
  本切片也尚未在实体应用中目测新版信息区，自动 WebView2 只使用假后端的固定 `X:`。
- 提交：`12c6b82 [P2.16]show live container A status`。

### 小切片 16e：登录挂载策略化与手动挂载默认

#### 需求、计划与既有实现审计

- 本切片起于 2026-09-01 关于多容器形态的讨论，结论已写入《需求梳理清单 v2》**8.7**。
  8.7.5 定：**默认不自动挂载**，"登录后自动挂载"是用户可开的单一布尔设置；该布尔对模式与档位
  完全不敏感（同 N-07 挂载计数器的论证），不构成 8.4 意义上的档位痕迹。
- 审计 `CardLoginMountSessionCore::consume_authenticated_login` 后确认：挂载此前写死在登录成功
  路径里，`mount_authenticated` 与 `MountRollbackError` 残留记账都只服务于该路径。多容器下
  "登录挂载全部容器"本就不成立，且 Spike 6 的五秒预算在三个以上卷时必然超线，而并行挂载已于
  2026-08-17 四增补实测作废。故先把登录与挂载解耦，不在本切片引入容器集合。
- 本切片只改策略，不删除既有 B 挂载、回滚与残留处理，也不新增按容器挂载动作。

#### 实现

- 新增 `LoginMountPolicy { manual, mount_on_login }`，由 `CardLoginMountSession` 构造参数显式传入，
  无默认值，调用点必须表态。`manual` 为产品默认，P2 ViKey 开发应用已改为传 `manual`。
- `consume_authenticated_login` 在 `manual` 下只建立易失认证即返回，完全不进入挂载运行时。
  登录输入本就只为那一次同步挂载而存在，因此该路径在认证建立后立即 `clear()`，明文存活时间
  较此前缩短，R-03 的可擦除副本面收窄。
- `mount_on_login` 完整保留 P2.14 行为：挂载结果记账、`MountRollbackError` 残留盘符记录与
  失败关闭路径均未改动，B 容器链路原样保留待后续切片使用。

#### 测试、安全复核与提交状态

- 会话测试原有 28 处构造全部改为显式 `mount_on_login`，既有断言无一放宽；新增两个 `manual`
  用例：一个断言登录后运行时事件为空、`mounted()` 为假且已存在的容器投影为"存在未挂载"，
  一个断言 `manual` 登录后仍拒绝二次交接、且建卷路径照常发布并挂载。
- Debug **49/50**、MSVC `/analyze` **50/50**（无项目诊断）、ASan **48/50**。三处失败均为
  `vikey_helper_process_tests` 与 `vikey_helper_hid_compatibility_tests` 的既有环境限制
  （网络隔离；ASan 下为 `0xC0000135` DLL not found），位于 ViKey helper 进程链，与本切片
  改动的 `card_login_mount_session` 与 `tools/app/main.cpp` 无关。`git diff --check` 通过。
  未访问实体卡或真实凭据。
- 安全复核确认策略是纯进程内构造参数，不写配置、注册表或日志，不新增任何可推出档位的产物；
  默认值方向是收紧（少挂载、密码更早清除），不放宽任何既有校验。
- 🔴 **已知功能缺口（本切片刻意留下）**：登录不再挂载，而"按容器挂载"动作尚不存在，故安全卸载
  后的容器在本切片状态下无法重新挂载。该缺口由下一切片（容器集合与分别挂载）关闭；本切片
  单独不构成可用形态。
- 提交：`4547fd9 [P2.16]make login mounting opt-in`。

### 小切片 16f：会话内容器集合与按容器挂载

#### 需求、计划与既有实现审计

- 承接 8.7.1。用户已在 2026-09-01 拍板：**多个 A 类容器全部共用固定 salt**，即继续使用
  `kContainerASalt` 与固定 `"diskA"` 输入。该决定使卷密码派生不再需要任何 per-container 材料，
  因此**容器清单不必持久化**，8.7.4 的分层列表在本切片可以完全绕开：会话内记住本次接管的容器，
  重启后由用户重新选择文件。这同时让 8.4 本地状态层不出现任何容器清单。
- 审计现有 `CardLoginMountSessionState` 后确认：`container_a_mounted` / `container_b_mounted`
  两个布尔无法表达多容器，但 N-06 第 5 条「先 B 后 A」的卸载顺序必须保住。故 B 继续单独记账，
  A 类改为集合。
- 本切片只做 native 边界，不动 UI；`create_container_a` 与 `container_a_status` 保留为适配器，
  16a～16d 的页面与桥接在本切片内保持编译与行为不变。

#### 实现

- `CardLoginMountSessionState` 由两个布尔改为 `std::vector<SessionContainer>` 加 B 的单独标志。
  `SessionContainer` 只存在于进程内，含会话内自增 id、路径、文件名、盘符与挂载标志；接管是
  幂等的，登录挂载、残留记录与显式挂载都收敛到同一条目。
- `IAuthenticatedCardVolumeSession` 新增 `create_container(path, size)`、`mount_container(path)`、
  `unmount_container(id)` 与 `container_statuses()`。新增 `ContainerStatus` 只投影
  id、**文件名**、挂载标志、容量与盘符 —— 完整路径、salt、卷密码、S、档位与卷类型仍不跨边界；
  入站路径由用户在文件对话框中选定，出站只给 id 与文件名。
- 盘符改为挂载时分配：运行时新增 `allocate_drive_letter()`，生产实现用 `GetLogicalDrives()`
  自 Z 向 D 取第一个空闲盘符；核心再排除本会话已记账的盘符，双重校验后才下发挂载。
- `create_container` 复用原 A 事务：staging → publish → 分配盘符 → 挂载 → 接管，失败清理与
  `ContainerCreationCleanupError` 原样保留。`unmount_container` 卸载后**保留条目为"已创建、
  未挂载"**，可直接重新挂载而无需再次选择文件 —— 这关闭了 16e 刻意留下的功能缺口。
- `unmount_active` / `force_unmount_active` 仍先释放容器 B，再遍历全部已接管容器，
  N-06 第 5 条顺序未变。

#### 测试、安全复核与提交状态

- 会话测试的假运行时改为路径无关：存在性、容量、建卷、发布、清理、挂载与卸载都能应答任意路径，
  同时保留全部既有 A 断言（事件序列仅按实现顺序插入 `allocate`）。新增五个用例覆盖：两个容器
  取得不同盘符且按接管顺序投影文件名与容量、同一路径只接管一次且已挂载不可重复挂载、卸载后
  条目保留并可重新挂载且分配新盘符、未认证与文件缺失时拒绝挂载且不接管、`unmount_active`
  释放全部已接管容器但不丢弃条目。
- 另有三个只用到 A 表面的假实现（关闭守卫、UI 控制器、COM 宿主）补齐四个新纯虚方法，
  均以 `logic_error` 明示本切片不使用，避免沉默的默认实现。
- Debug **49/50**、MSVC `/analyze` **50/50**（无项目诊断）、ASan **48/50**。三处失败仍是
  `vikey_helper_process_tests` 与 `vikey_helper_hid_compatibility_tests` 的既有环境限制，
  与本切片无关。`git diff --check` 通过。未访问实体卡或真实凭据。
- 安全复核确认容器集合只存在于进程内，不写配置、注册表、日志或任何清单；跨边界字段不含完整
  路径与任何可推出档位的信息。所有 A 类容器共用固定 salt 是用户明示决定，代价是同一密码下各
  容器卷密码相同（8.7.1 已记录），本切片按该决定实现。
- 🟡 遗留：UI 仍只显示固定容器 A（走适配器），多容器列表、按容器挂载入口与"登录后自动挂载"
  设置开关由下一切片接线。
- 提交：`527492b [P2.16]adopt containers per session`。

### 小切片 16g：UI 设计基线与产品页换肤

#### 需求审计与实现

- 用户指出当前界面观感不足，要求先出一份"高端、简洁"的全流程设计预览，再把已实现的内容套过去。
  审计旧稿 `usb-vault_ui-disign.html` 后确认它只能作为视觉参考而不能作为改造起点：其用户名／注册／
  忘记密码三页签与 N-06（认证锚点是卡上双信封、取消注册）和 Q-02（忘记密码不实现）冲突；
  "加密盘／隐藏盘"分栏导航与 8.7.5「仪表盘不按卷类型分类展示」冲突；应用内文件管理器已按 8.7.5
  降级为 v1.1。故重画而非改色。
- 新增 `usb-vault_ui-design.html`：26 屏全流程预览，分设计基线、会话入口、主界面、创建容器、
  维护与数据四段，含设计语言页与流程地图。每屏标注实现状态（已实现／部分实现／未实现）、
  设计说明与需求依据编号，屏内窗口按宿主实际尺寸等比缩放。
- 设计语言定为石墨底（#08090b）+ 香槟金点缀 + 发丝线分隔：金色只出现在品牌标记、当前焦点与
  主操作三处；图标全部内联线性 SVG，取代旧稿 emoji；字体走系统栈，不引外链资源（WebView2 离线）。
- 设计稿中三处按现有结论明确落定：入口恒显且创建流程逐字对称（8.7.2）、第二个密码不作正确性
  校验只要求输入两遍（8.7.3）、导出路径与历史一律不落盘（Q-04 可验收项 3、8.4）。屏 21 标注了
  8.5「更新诱饵仅档三渲染」与 8.7.2「UI 差异本身就是档位」的张力，待复核。
- `MILESTONES.md` 两处设计稿引用改指新文件（P1 最小 UI 行、P2 UI 全量行），并注明旧稿仅存档。
- `ui/card_session.html` 只换视觉层：会话入口改为整窗居中构图（盾形标记 → 字距字标 → 金色发丝线
  → 状态标题 → 表单），仪表盘统计卡改为图标 + 小标签 + 数值，侧栏当前项用左侧金条，建卷对话框
  重排步骤预览与容量输入。DOM 结构、全部 id 与 data 属性、五态、CSP、单一脚本引用、四个
  "正在开发中"灰项与既有 IA 均原样保留；`card_session.js` 未修改，缓存版本号不变。
- 设计稿中的统一容器视图（8.7.5）与线上仍在的分栏 IA 暂不一致，属已知待办，不在本切片范围。

#### 测试、安全复核与提交状态

- 以脚本逐条镜像 `card_product_ui_page_contract_tests` 中全部针对页面的断言并全部通过：两个
  route-specific 密码控件与 confirmation 字面量、严格 CSP、单一 `<script>`、三段强度条、
  2×`data-create-kind`、4×`aria-disabled` + `data-tooltip`、两处 A 信息字段、容量控件字面量、
  `.strength-slot` 与 `#card-session-error` 样式字面量、状态行先于错误行、禁用词与 emoji 清单。
  另核对 `card_session.js` 的全部 `getElementById` 目标、两个 `querySelectorAll` 选择器与六个
  dataset 写入值（`busy`／`level`／`score`／`sessionState`／`tone`／`view`）在新样式中均有对应规则。
- 🟡 **本次未运行 CTest**：本机 shell 无 cmake／MSVC 环境变量，正式
  `ctest -R card_product_ui_page_contract_tests` 待下一次带环境的构建补跑。已在 VS 开发者环境下
  成功重建 `usb_vault_vikey_p2_development_app`，POST_BUILD 拷贝生效，部署页面与源文件逐字节一致。
- 视觉核对用 headless Edge 按宿主窗口 884×601（对应 `tools/app/main.cpp` 的 900×640）渲染登录态、
  初始化态、仪表盘与建卷对话框四张静态图逐一目视确认，布局与居中正常。
- 安全复核确认本切片只改样式与呈现标记：未新增脚本、外链资源、存储、状态字段或桥方法，CSP 未变，
  页面仍不持久化或记录任何输入与状态；新增的设计稿是仓库根目录的独立预览文件，不参与构建与打包。
- 另在 `out/` 下新增 `p2_run_dev_app.ps1`（按 .gitignore 不入库），照 `p1_7_step2_dev_app.ps1` 的
  形式固化 P2 开发应用启动前置：六个产物存在性、helper／厂商 DLL／补丁 `VeraCryptFormat.dll`
  三个 SHA-256 校验，以及部署页面是否比 `ui/` 源文件旧。
- 提交：`cbf3f01 [ui]adopt graphite design baseline`。

### 小切片 16h：多个加密盘容器接入 UI

#### 需求审计与实现

- 16f 已提交为 `527492b`，会话侧的 `create_container` / `mount_container` /
  `unmount_container` / `container_statuses` 均已可用，但 UI 仍只显示固定容器 A（走适配器）。
  审计 8.7.1（容器数量不设上限、A 类卷密码固定从卡派生、多容器无密码隔离必须明示）、
  8.7.2（创建入口恒显）、8.7.4（容器路径不得明文落盘）、8.7.5（只读状态展示、不按卷类型分类）
  与 8.4（本地状态层不得留下容器清单）后确定本切片边界：只把已实现的会话能力接到界面上，
  不新增核心方法、不动 B、不引入任何持久化。
- 新增 `ui::IContainerFileChooser`：页面永远不提供也拿不到路径，选择完全由原生侧完成。
  这既避免页面控制的字符串到达文件系统，也让路径不跨自动化边界。
- 新增 Windows 适配器 `platform::windows::ShellContainerFileChooser`（独立静态库，
  与 HID 适配器同样留在核心之外）。两个对话框都带 `FOS_DONTADDTORECENT` 且**不调用**
  `SetClientGuid`，因此不写最近使用记录、不记住上次目录 —— 记住位置就等于把容器清单写进
  本地痕迹，属 8.4 明文禁止项。
- `CardProductUiSessionController` 增加容器集合投影与三个动作：`create_container(size)`、
  `mount_container()`（选文件）、`mount_container(id)`（已接管的重新挂载）、
  `unmount_container(id)`。取消对话框是正常结果，返回 `false` 而不是异常；越界容量与错误
  路由在打开对话框**之前**就被拒绝。投影只含 id、文件名、挂载标志、容量与盘符，
  容量不是精确 MiB 倍数时报 0（不可用）而不是四舍五入，未挂载却带盘符一律失败关闭。
- 控制器保留一份 id→路径的进程内映射，只为让卸载后的容器无需重新选文件即可挂回；它镜像
  会话已持有的数据，不写任何位置。登录期接管、映射里没有的 id 会退回选择文件。
- P2 COM 面新增十个方法：`createContainerMib`、`mountContainer`、`mountAdoptedContainer`、
  `unmountContainer`、`getContainerCount` 与五个按位置读取的只读事实。仍无路径入参、
  无秘密出参、无 B 形态或卷类型。
- 页面：仪表盘第三块统计改为「加密盘数量」，加密盘概览与加密盘管理共用同一个容器列表，
  每行给出文件名、挂载状态、容量、盘符和一个挂载/卸载按钮，另加「打开已有」入口与
  顶栏状态行（容器动作的失败与取消不能显示在已隐藏的会话面上）。列表用 DOM 构建，
  文件名只经 `textContent` 落地，页面控制器全文不出现 `innerHTML`。
- 创建入口不再因「已存在容器」而隐藏或禁用 —— 按 8.7.2，入口消失本身就是档位。
  加密盘管理页明示「所有加密盘共用同一个密码，分开存放不等于分开保护」与「软件不会记录
  加密盘的位置」。缓存版本升为 `p2-16h-container-collection`。

#### 测试、安全复核与提交状态

- 控制器测试新增四例：未认证不查询也不暴露集合、选中路径与容量原样到达会话、越界容量在
  打开对话框前即被拒、取消不产生容器、卸载后条目保留且不再带盘符、按 id 重新挂载不再询问
  文件、未接管 id 拒绝、第二个容器经对话框接管、未挂载却带盘符时失败关闭。
- COM 宿主测试新增四例：按位置读取全部事实且越界位置返回 `E_INVALIDARG`、字符串参数与
  缺参被拒且不触碰对话框与会话、取消如实回报为 `VARIANT_FALSE`、按 id 挂载/卸载走通且
  未知 id 返回 `DISP_E_TYPEMISMATCH`、状态查询失败时不编造计数或名称。
- 页面契约测试改写：锁定新的桥方法名、两处容器列表与两个「打开已有」入口、容器状态的
  逐字段校验、文件名以 `textContent` 渲染且全文无 `innerHTML`、创建入口不再被已有容器隐藏、
  以及页面**不再**引用四个已退休的 A 专用桥方法。
- 真实 WebView2 假后端夹具已按新界面改写并**实跑通过（退出 0）**：夹具页镜像新的容器列表与
  「打开已有」入口，仍加载真实 `ui/card_session.js`，走完无卡 → 初始化 → 已认证无容器 →
  切到加密盘管理 → 建卷（注入失败 → 用户取消 → 成功）→ 校验行内文件名／容量／盘符且创建
  入口仍可见 → 重新登录后信息恢复 → 残留卷 → 卸载 → 回到无卡。假选择器固定在第二次调用返回
  空串，用来覆盖"取消不是失败"这条新路径。
- ⚠️ **顺带修复了一处既有断裂**：`card_product_webview_integration_tests` 自 16f 起就没有实现
  会话新增的四个纯虚方法，因该目标默认排除而未被发现，本切片补齐后才能编译。
- Debug 全量构建通过，完整 CTest **49/50**：唯一失败是 `vikey_helper_process_tests`
  （`network isolation: card communication failed`），属实体卡不在位的既有环境限制，与本切片
  无关。两个 `node --check` 通过。用 headless Edge 按 884×601 渲染仪表盘与加密盘管理两页目视确认。
- 安全复核：路径两个方向都不跨 COM 边界；出站只有会话内 id、文件名、容量、盘符；
  没有新增持久化、日志或配置写入；没有引入档位、卷类型或 B 形态字段；文件名以纯文本渲染，
  构造过的文件名无法变成标记。
- 🟡 遗留风险与待办：
  1. **通用文件对话框的本地痕迹仍需 P3 逐字节验收**。已用 `FOS_DONTADDTORECENT` 且不设
     client GUID，但 ComDlg32 自身的 MRU 行为必须实测确认为零写入，这正是 8.4 最容易漏掉的一类。
  2. 容器清单不持久化，重启后需重新选择文件；8.7.4 要求的「加密分层清单存于容器 A 内」尚未实现。
  3. 全部 A 类容器共用固定 salt（8.7.1 待定项），同一密码下各容器卷密码相同 —— 16f 已记录，本切片未改。
  4. 四个 A 专用 COM 方法与控制器的 `create_container_a` / `container_a_status` 现已无页面调用，
     属可清理的死面，留待单独一刀切除。
  5. 建卷与挂载仍同步占用 UI 线程（16c 已记录）；同名不同目录的容器在列表中显示相同名称。
- 提交：`a8aa281 [ui]add the multi-container page and bridge`。

### 小切片 16i：切除单容器 A 专用界面面

#### 需求审计与实现

- 16h 之后，页面已全部改走容器集合，四个 A 专用 COM 方法与控制器的
  `create_container_a` / `container_a_status` 再无调用方，属自动化边界上的死面。用户明确要求
  一刀切除，本切片只做删除与更名，不改任何行为。
- COM 面删除 `createContainerAMib`、`getContainerAState`、`getContainerASizeMib`、
  `getContainerADriveLetter` 四个方法及其 DISPID、名称与四个 `invoke_*` 实现。
- 控制器删除 `create_container_a`、`container_a_status` 与 `ContainerAUiState` /
  `ContainerAUiStatus`；容量常量更名为 `kContainerCreationMinimumMib` /
  `kContainerCreationMaximumMib`（不再有"A"这一层含义）。
- 页面与夹具的建卷控件同步更名：`create-container-a-form` → `create-container-form`、
  `create-container-a-capacity` → `create-container-capacity`，脚本内的 `containerACapacity*`
  一并改名。
- **未动会话层**：`IAuthenticatedCardVolumeSession::create_container_a` 与 `container_a_status`
  仍在（`create_container_a` 只是固定路径版的 `create_container`），目前只剩测试调用。
  它们与登录挂载策略共用固定 `container_a` 布局，是否一并折叠留待单独评审。

#### 测试、安全复核与提交状态

- 删除三个控制器测试与三个 COM 宿主测试（连同四个 A 专用 invoke 辅助），但把其中仍然成立的
  两处覆盖迁到新入口上：创建失败后仍保持已认证路由、以及非整数容量（`VT_R8` 64.5）必须被
  `exact_uint32` 拒绝。
- 完整 CTest **49/50**（唯一失败仍是实体卡不在位的 `vikey_helper_process_tests`）；
  真实 WebView2 假后端夹具重跑退出 0；两个 `node --check` 通过。
- 安全复核：本切片只减不增，自动化边界的方法数从 19 降到 15，没有新增入参、出参或状态。
- ⚠️ P2 开发应用 exe 本次**未能重新链接**（`LNK1168`，用户正在运行该程序）。源码与页面已是最新，
  下次构建即可覆盖。
- 提交：`7495836 [ui]drop the single-container bridge`。

### 小切片 16j：移除 P1 应用链

#### 需求审计与实现

- 用户决定后续只在 P2 现有基础上改动，P1 阶段的内容可以清理。审计依赖关系后确认 P1 应用链的
  唯一生产消费方是 `tools/app/main.cpp` 的非 P2 分支，边界干净；ViKey 的 P1 blob／helper 链
  （32 字节卡存储、blob backend、helper blob 操作、p1_7 夹具与探针）耦合到 helper IPC，
  单独作为下一个边界，本切片不动。
- 删除 19 个文件：P1 页面资产（`ui/index.html`、`ui/session.js`）、`ApplicationSession`
  及其内部头、`UiSessionController`、P1 `webview_host_object`、`FixedBlobCardStore`，
  以及只为它们存在的 7 个测试文件与两个 WebView2 夹具资产。
- `tools/app/main.cpp` 按"P2 恒定义、P1 开发应用恒未定义"求值后折叠：668 → 517 行，
  预处理条件从 55 条降到 **0 条**，组合根只剩 P2 一条链路。
- CMake 移除 `usb_vault_app` 与 `usb_vault_vikey_development_app` 两个可执行目标，
  从 `usb_vault_core` 移除四个 P1 源文件；测试工程移除六个 P1 目标。
- `webview_password_array` 删除只为 P1 控制器存在的 `submit_webview_password_array`；
  `ui_close_guard` 删除 P1 重载，只保留产品重载。

#### 测试、安全复核与提交状态

- `webview_password_array_tests` **改打到共享入口上**而不是删掉：七个用例改为直接调用
  `consume_webview_password_array` 并校验消费结果与源数组擦除，只删掉两个纯粹验证"转发给 P1
  会话"的用例（等价的失败擦除覆盖已在 P2 宿主测试里）。`ui_close_guard_tests` 删除三个 P1 用例，
  保留产品用例；`card_login_mount_session_tests` 删除"未派生自 `IApplicationSessionActions`"的
  静态断言（该接口已不存在）。
- 完整 CTest 从 50 项降到 45 项，**44/45 通过**：唯一失败仍是实体卡不在位的
  `vikey_helper_process_tests`。真实 WebView2 假后端夹具重跑退出 0。
- 安全复核：本切片只删除，不新增任何接口、状态或落盘；P2 组合根不再有任何条件编译分支，
  阅读与审计面积明显收窄。
- ⚠️ P2 开发应用 exe 仍未能重新链接（`LNK1168`，用户正在运行该程序）；`main.cpp` 目标文件已
  编译通过，关闭程序后重新构建即可。
- 🟡 下一个边界（未做）：ViKey P1 blob 链 —— `vikey_card_store.cpp`（32 字节）、
  `vikey_helper_blob_backend`、helper 的 blob IPC 操作、`p1_7` 夹具与诊断、P1 卡重置工具、
  blob 往返探针，以及依附其上的 `vikey_card_store_tests` 与 `vikey_helper_process_tests`。
  这条链会动到 helper 协议，须单独评审后再切。
- 提交：`880960c [cleanup]remove the P1 application chain`。

### 小切片 16k：隐藏盘容器 B 的会话层建卷

#### 需求审计与最少切片规划

- 用户要求先做 B 类容器的最少建卷流程，当前不追求安全性细节，测试始终在稳定插卡状态下进行。
- 审计既有实现后确认底座已经就绪，缺口只在中间层：
  - ✅ `VolumeCreator::create_with_hidden`（P1.4）已能建外层+隐藏卷，含 UAC、位图扫描与进度；
  - ✅ `rewrap_card_record_v1`（P2.1）已能把根重新封进两个等长信封；
  - ✅ `MountOrchestrator` 登录时已按 `KDF(S, 用户输入)` 挂 B —— 输入什么就挂出外层还是隐藏卷；
  - ❌ 会话层没有建 B 的入口，运行时接口只有 `create_standard`；
  - ❌ 卡记录服务没有"设置诱饵口令"的操作；
  - ❌ 控制器／COM／页面没有 B 的建卷路径。
- **完整 B 流程的最少切片数：4。** 16k 会话层建卷（本切片）→ 16l 卡记录设诱饵口令 →
  16m UI 全链（控制器＋COM＋页面＋契约＋WebView2 夹具）→ 16n 登录后把 B 纳入容器列表。
  16k 与 16l 是两个互不相关的子系统，合并可以压到 3 刀，但会牺牲单独回退能力。
- 三条为"最少"服务的默认取舍，均已记录：
  1. **B 固定使用 `ContainerLayout::container_b` 的路径，全机只允许一个**。这样 8.7.6
     「首个 B 的卡记录改写时机」这个待定项根本不会出现，也不需要在 UI 里选路径。
  2. **建 B 必须让用户再输一次当前登录口令**：manual 策略在登录成功后立刻清掉了口令明文，
     而隐藏卷密码 `KDF(S, P_real)` 需要明文，会话不可能凭 S 反推。
  3. **顺序为先建容器、再改写卡记录**（16l 接线）；中途失败时容器在、诱饵口令未生效，可重试。

#### 实现

- 运行时接口新增 `create_with_hidden`，生产实现直接转发到 `VolumeCreator`。
- `IAuthenticatedCardVolumeSession::create_container_b(outer_bytes, hidden_bytes,
  login_password, decoy_password)`：校验认证与尺寸（两者 512 对齐、隐藏卷必须小于外层）、
  拒绝已存在或已接管的目标，然后**用登录口令派生隐藏卷密码、用新设口令派生外层密码**，
  复用 A 已有的 staging → publish → 分配盘符 → 挂载 → 接管事务，失败时清理未发布的暂存文件。
  挂载的是隐藏卷（当前登录者那一侧），接管后与其它容器同列。
- 产品恒把"当前登录口令"当真实口令、把"新设口令"当诱饵口令，不判断自己处在哪一侧（8.7.2）。

#### 测试、安全复核与提交状态

- 会话测试新增三例：派生出的两个卷密码必须不同且分别落到外层/隐藏、事件序列为
  `exists → derive×2 → create:hidden → publish → allocate → mount → notify`、接管后可在
  `container_statuses()` 中看到；未认证、尺寸不可用（0／非 512 对齐／隐藏卷 ≥ 外层）、空口令
  一律在触碰运行时之前拒绝；格式化失败后不留会话状态。
- 夹具补强两处：测试布局给容器 B 一个与 A 不同的 salt（原先两者都是全零，派生分支无法区分），
  `require_events` 失败时改为打印实际事件序列，便于定位。
- 完整 CTest **44/45**（唯一失败仍是实体卡不在位的 `vikey_helper_process_tests`）。
- 安全复核：口令明文只在本次调用内存在，派生完即清零；跨边界仍只有 id／文件名／容量／盘符，
  没有引入档位、卷类型或"这是 B"的持久化标记。本切片没有触碰卡记录，也没有真实建卷。
- 提交：`187ac1c [P2.16]create the hidden volume container`。

### 小切片 16l：卡记录的第二口令

#### 需求审计与实现

- 16k 已提交为 `187ac1c`。本切片补齐 B 流程的第二块：让卡记录能同时被"当前登录口令"与
  "新设的第二口令"打开，否则用第二口令根本登录不进来，B 的外层卷也就永远挂不出来。
- 底座 `rewrap_card_record_v1`（P2.1）已存在，但**上层没有任何通路**：`CardRecordService`
  只有初始化与认证，`ICardLoginTarget` 明确注释「intentionally exposes no ICardStore write
  capability」，而 `ICardInitializationTarget` 绑定的是 provisioning 上下文。所以本切片比
  原计划厚一层，补了从已认证状态写卡的完整通路。
- `CardRecordService::set_second_password(existing, second)`：只接受 initialized v1 记录，
  空口令直接拒绝，随后 `rewrap_card_record_v1` 把根重新封进两个信封，写入后**读回比对**，
  写报错但记录确实落盘的情况按成功处理（沿用初始化那套可恢复提交语义）。
  记录原语需要调用方口令两次（一次开旧记录、一次当作替换信封），`SecureBuffer` 故意不可拷贝，
  因此复制是显式的、锁页的，并与原件一起被消费。
- 新增 `card::ICardUpdateTarget`（只有 `card_store()`），**与只读的登录目标分开**：登录永远
  不能写卡，改写信封必须能写。`ICardEntryInspectionSource::make_update_target` 默认返回空，
  不支持的适配器 fail-closed。
- `ICardEntryActions::set_second_card_password(existing, second)` 由 `CardEntryContextController`
  实现：空口令在触碰适配器之前拒绝且**不作废插入**（输入手误不该逼用户重新插卡）；路由不是
  login_required 时拒绝；适配器创建或写卡失败时把插入退役（卡此刻可能是新旧任一记录，
  状态不再可知），由用户重新插卡。
- ViKey 侧新增 `VikeyHelperCardUpdateTarget` 与 `make_update_target`，复用既有
  `VikeyHelperCardRecordBackend::write_record`（只校验硬件 ID 与凭据，不带介质状态门），
  因此不需要改 helper 协议。
- 命名刻意保持中性：代码里叫 `second_password`，不叫 decoy／诱饵 —— 卡层不记录哪个口令是哪个，
  与 8.6「档位不是运行时实体」和记录原语自身的注释一致。

#### 测试、安全复核与提交状态

- `card_record_service_tests` 新增三例：改写后两个口令解出同一个根且记录不与改写前逐字节相同、
  非 initialized 状态与空口令一律拒绝且不写卡、`write_then_throw` 按读回结果接受而
  `throw_without_write` 如实报错且记录不变。
- `card_entry_context_controller_tests` 新增四例并补了一个假更新目标：路由不对时不碰适配器、
  空输入不退役插入、适配器不支持时 fail-closed 并退役插入、成功路径只写一次卡且改写后两个
  口令都能解开同一个根。
- 完整 CTest **44/45**（唯一失败仍是实体卡不在位的 `vikey_helper_process_tests`）；
  真实 WebView2 假后端夹具重跑退出 0。
- 安全复核：口令只在调用内存在，显式副本与原件同时被消费；卡层不落任何"哪个是诱饵"的标记；
  写卡仍绑定原始硬件 ID，且每次操作重新加载凭据。⚠️ 未做实体卡验证 —— 本切片全部为假存储测试。
- 🟡 已知代价：改写失败时插入被退役，用户需重新插卡重试；这是"卡状态不可知就不继续"的取舍。
- 提交：`00b6746 [P2.16]commit a second card record password`。

### 小切片 16m：隐藏盘建卷的 UI 全链

#### 需求审计与实现

- 16k／16l 已分别提交为 `187ac1c`、`00b6746`。本切片把两半接到页面上，构成 B 的完整建卷路径。
- 🔴 **提交顺序与 16k 记录的取舍相反，此处更正**：16k 写的是"先建容器、再改写卡记录"，
  实现时重新推演了失败面后改为**先改写卡记录、再建容器**。理由是可重试性：
  - 容器先建而卡改写失败 → B 已存在但外层口令不在卡上，重试会因目标已存在而**直接死锁**；
  - 卡先改而建卷失败（包括用户取消 UAC）→ 卡上多一个暂时开不了任何东西的口令，无害，重试即可。
- `CardProductUiSessionController::create_container_b(outer_mib, hidden_mib, 当前口令,
  诱饵, 诱饵确认)`：三个输入缓冲在所有路径上擦除、互相重叠即拒；路由与尺寸（外层在
  20–1048576 MiB、内层非零且小于外层）先校验；诱饵经 `confirm_password` 走**与日常口令同一套
  强度策略**（12–128 码点），随后先调 `set_second_card_password`、再调 `create_container_b`。
  两个秘密各显式复制一份给消费型的卡接口，原件留给只读的建卷接口，用完即清。
- COM 新增 `createContainerBMib`（5 参：外层容量、内层容量、当前口令、诱饵、诱饵确认）。
  COM 逆序投递，页面数组在每条路径上都被擦除；参数数量、类型或范围错一律 fail-closed。
- 页面：隐藏盘对话框不再是"正在开发中"占位，改为两个容量输入 + 三个口令输入；文案固定为
  「为该容器设置诱饵密码」「再次输入诱饵密码」「请务必输入正确 —— 软件不会、也无法替你校验它」
  （8.7.2 逐字对称、8.7.3 不作正确性判定）。按钮在两次诱饵一致且长度合规前保持禁用，
  避免把强度拒绝变成一次失败的往返。建卷步骤预览只描述标准盘流程（要选保存位置），
  因此隐藏盘路径下隐藏该预览；对话框加入内部滚动，避免在 601px 客户区高度下压掉页脚。

#### 测试、安全复核与提交状态

- 控制器测试新增两例：卡记录先于卷、且**卡失败时卷绝不创建**；未认证／尺寸越界／内层不小于
  外层／两次诱饵不一致／三个缓冲互相重叠一律在触碰卡与卷之前拒绝。
- COM 宿主测试新增一例：五个参数按 COM 逆序落位、三个口令数组调用后全部清零、参数数量错误
  时不触达产品。
- 契约测试改写：五个 `type="password"` 与五处 `autocomplete/spellcheck`、B 表单的六个 id、
  三句固定文案、`createContainerBMib` 桥调用、页面**只做两次一致性比较而不判断诱饵对错**、
  以及"正在开发中"占位已彻底移除。
- 🔴 **真实 WebView2 假后端夹具已跑通完整 B 流程（退出 0）**：真实页面填两容量 + 三口令 →
  一次五参 COM 调用 → 假后端逐一比对四份口令载荷（卡记录的既有/新设、卷的内层/外层）与两个
  容量 → 容器列表变为两项 → 重新登录后信息恢复。这条覆盖正是五参逆序投递唯一能被证伪的地方。
- 完整 CTest **44/45**（唯一失败仍是实体卡不在位的 `vikey_helper_process_tests`）。
- 安全复核：三个口令只在调用内存在，DOM 值在编码后立即清空，数组在 finally 中清零；
  显式复制与原件同时清除；跨边界仍无路径、无卷类型、无"哪个是诱饵"的标记。
- ⚠️ 已知取舍与遗留：诱饵输入框**没有显隐切换**（最少流程未做），12 位以上口令要盲打两遍；
  当前口令输错会导致卡记录改写失败并退役插入，需要重新插卡；实体卡与真实建卷仍未验证。
- 提交：`f87a12a [P2.16]create the hidden container from the page`。

### 小切片 16n：隐藏盘并入统一容器集合

#### 需求审计与实现

- 16m 已提交为 `f87a12a`。B 的最后一块：此前 B 的挂载状态由 `CardLoginMountSessionState`
  里一个**独立布尔量** `container_b_mounted` 记账，根本不进 `containers`，因此登录挂出的 B
  在容器列表里**完全看不见**，只有 16k 建卷当场那一次会被接管。
- 改为一套集合：`SessionContainer` 增加内部标志 `release_first`（仅决定卸载顺序，不跨应用边界），
  登录挂出 B、残留卷记账、以及 16k 的建卷全部走同一个 `adopt()`。删除 `container_b_mounted`，
  `mounted()`、盘符占用校验、`unmount_active`、`force_unmount_active` 全部只读这一个集合。
- 卸载顺序仍是**先带隐藏卷的容器、再其余容器**（N-06 第 5 条），实现方式从"布尔量先于循环"
  改为"按 `release_first` 分两轮遍历"，事件序列不变。
- `mount_container(path)` 现在**明确拒绝固定的 B 路径**：它的两个卷都由用户口令派生，而
  manual 策略登录后会话已不持有该口令，用固定的标准派生去试只会得到一次莫名其妙的失败。

#### 测试、安全复核与提交状态

- 会话测试新增两例：登录同时挂出两个容器时，集合里按接管顺序出现 `a.hc`（X:）与 `b.hc`（Y:），
  `unmount_active` 仍先放 B 再放 A、且卸载后两个条目都保留为"未挂载"且不带盘符；
  以固定 B 路径调用标准挂载被拒且不触碰运行时。
- 既有的 B-before-A 顺序、残留卷、强制卸载与 `mounted()` 用例全部原样通过，说明这次是纯重构。
- 完整 CTest **44/45**（唯一失败仍是实体卡不在位的 `vikey_helper_process_tests`）；
  真实 WebView2 假后端夹具重跑退出 0。
- 安全复核：`release_first` 只存在于会话内部结构，`ContainerStatus` 与 COM 投影一字未变，
  页面依旧拿不到路径、卷类型或"哪个是隐藏盘"的标记。
- 🟡 **B 流程到此闭合，但有一处产品级缺口须由用户定夺**：当前 `LoginMountPolicy` 默认 `manual`，
  登录不挂任何卷，而 B 的两个卷都必须用**用户口令**派生 —— 会话在登录后已清掉该口令。
  因此**重新登录后 B 无法被挂载**，除非二选一：
  1. 打开"登录后自动挂载"（把 P2 应用切到 `mount_on_login`，代价见 8.7.5 的五秒预算）；
  2. 新增一个"挂载该容器"时再输一次口令的动作（又一刀 UI）。
  16k 建卷当场挂出的 B 在本次会话内可正常使用与卸载，不受此限制。
- 提交：`2c1c925 [P2.16]adopt the hidden container into the collection`。

### 小切片 16o：登录后免输入挂载任意已有容器

#### 需求审计与决策

- 16n 已提交为 `2c1c925`。用户提出的目标状态：**登录后不自动挂载，但用户可以自由地免输入挂载
  任何已有的 B 类容器，挂出外层还是隐藏卷由登录时用的口令决定。**
- 审计派生链后确认这可以做到，且不必保留用户口令：卷密码是
  `Argon2id(password=P, salt=容器salt, secret=S)`（`kdf_v1.h`、`argon2id_runtime.cpp`），
  salt 直接进 Argon2id。于是：
  - **全部 B 共用一个 salt** → 登录时跑一次 Argon2id，保留 64 字节结果，之后任意 B 都能免输入挂载；
  - **每容器一个 salt**（8.7.1 第 3 案）→ 无法预派生，每次挂载都要 P 在手。
  🔴 **因此该目标状态等价于选定 8.7.1 的第 1 案（全部 B 共用一个 salt）**，代价是同口令下各 B
  的卷密码相同 —— 该性质 A 类容器现已存在（共用固定 `kContainerASalt`）。
  ✅ 用户已于 2026-09-01 定案选第 1 案，8.7.1 的待定项已改写为定案并记录理由与代价；
  当前代码形态（每类一个编译期常量 salt）与该决定一致，无需改动。
- 🔴 **保留派生结果而不是保留口令**：那 64 字节只能开这一类容器 —— 开不了卡记录、开不了 A
  （输入与 salt 都不同）、也不是用户会再次敲的东西，更不能拿去登录。相较保留 `P_real`，
  被 dump 抓走的损失面小一个量级，同时仍满足 R-03「用后清零、锁页、禁止可重分配容器」。
  唯一未满足的是"驻留时间最小化"从毫秒放宽到一次会话，这是为达成目标状态明确付出的代价。

#### 实现

- 登录时按固定路径探测两个容器：存在即**接管为"未挂载"条目**（不挂载，保持默认不自动挂载）；
  B 存在时顺手用登录输入派生其卷密码并留在会话内。派生只发生在这一刻 —— 这是登录输入唯一
  还存在的时刻。代价：B 存在时登录多一次 Argon2id（Spike 6 口径约 1.3–1.7s，登录总耗时
  约 2.5s，仍在 P-002 的五秒预算内）；B 不存在则零开销。
- 会话新增 `mount_adopted_container(id)`：条目是 B 就用保留的派生密码，否则用固定的卡派生；
  已挂载则空操作，未接管的 id、文件消失、以及"保留密码已被丢弃"都各自失败关闭。
- **强制卸载（拔卡路径）连同保留的派生密码一起丢弃**；主动"安全卸载"保留它，这样同一会话内
  可以卸载后再挂回来。`SecureBuffer::clear()` 只清零不释放，因此丢弃用的是整体替换而非 `clear()`。
- 控制器按 id 的挂载改为直接转发到会话，**删掉 16h 留下的 id→路径映射**（会话本来就知道路径，
  那份是当时没有 id 挂载接口的权宜之计）；COM 的 `mountAdoptedContainer` 不再表示"已取消"。
- **页面一行未改**：容器行上那个「挂载」按钮本来就走 `mountAdoptedContainer(id)`。

#### 测试、安全复核与提交状态

- 会话测试新增三例：手动登录只为 B 派生一次、两个容器都以未挂载状态接管；
  按 id 挂载时 B 用保留的派生密码、A 用卡派生（逐字节比对实际传给挂载的密码），
  两者落在不同盘符；重复挂载空操作、未知 id 拒绝；强制卸载后 B 无法再挂（密码已丢弃），
  而主动卸载后仍可挂回。
- 既有登录用例的事件期望按新的探测顺序更新（先 B 后 A）；手动策略用例改为断言
  "只探测、不挂载、接管为未挂载"。
- 完整 CTest **44/45**（唯一失败仍是实体卡不在位的 `vikey_helper_process_tests`）；
  真实 WebView2 假后端夹具重跑退出 0。
- 安全复核：保留的是派生结果而非人类口令，锁页、析构清零、拔卡即丢；跨边界投影未变，
  页面仍拿不到路径、卷类型或"哪个是隐藏盘"。⚠️ 明文驻留时间从毫秒延长到一次会话，
  已在上文明确记为达成目标状态的代价。
- 🟡 遗留：只有布局里固定的两个容器能在登录后被列出，用户自选路径的容器仍不持久化
  （8.7.4 的加密分层清单未做）；实体卡与真实建卷仍未验证。
- 提交：`a23ef1d [P2.16]mount adopted containers without retyping`。

### 小切片 17a：开放系统设置页

#### 需求审计与决策

- 16o 已提交为 `a23ef1d`。侧栏「系统设置」此前是禁用占位项。设计稿
  （`usb-vault_ui-design.html:1155`）给这一页定了 3 个开关 + 关于块 + 2 条说明，
  且自带一条原则：「设置项刻意保持极少。每多一个开关，就多一处可能泄露使用方式的本地记录」。
  因此本切片的做法是**从设计稿的既定条目里落地**，而不是新增设置项。
- 三个开关的现状裁定：
  - **拔卡即卸载** —— 已实现（P1.7 强制卸载 + P2.13f HID 生命周期），基本需求-3，
    8.4 视其为否认性的组成部分。做成**锁定常开、不可操作**的开关。
  - **登录后自动挂载全部容器** —— 8.7.5 明确授权落盘，`LoginMountPolicy` 也已就位
    （`card_login_mount_session.h:24`），但它必须在**登录之前**被读到，因此不能存进容器 A，
    会成为产品的**第一个本地持久化产物**并直接进入 P3 的逐字节比对验收。
    🔴 另有 8.7.5 的红字约束：开启后必须对自动挂载的卷数设上限或重谈 P-002 五秒预算。
  - **记住容器位置** —— 依赖 8.7.4 的加密分层容器清单（存于容器 A 内），该清单未实现
    （见 16o 遗留）。
- **用户决定（2026-09-02）**：先按设计稿把两个开关的 UI 正常做出来，不做未实现的后端，
  也不做禁用或「正在开发中」标注。已向用户说明代价：**两个开关目前只活在当前页面，
  刷新即回默认值**，「记住容器位置」显示为开启并不代表路径真的被保存。
  后端分别由 17b（8.7.5 落盘产物 + 卷数上限）与 8.7.4 的容器清单切片补齐。
- 关于块的两条设计稿条目被否决：
  1. 「安全卡 · VK·7C2A」—— `card_product_ui_session_controller.h:21-24` 的不变量明确要求
     投影不暴露硬件标识，卡序列号跨 UI 边界会直接破掉它，且是天然的本地状态泄露点；
  2. 「代码签名 · 已签名 · EV」—— EV 签名是 P3 交付物，现在写上去即为不实陈述。
     两行均删除，并由契约测试断言它们不会回来。
- 设计稿右侧的两条说明文案与每行副标题按用户要求一并去掉，行内只留标题与开关。
- **不能加的一类开关**已一并记录：例如「挂载外层时保护隐藏卷」。按 R-01，保护与否是模式的
  自动结果（模式 B 绝不开、模式 C 自动开），一旦做成开关，它在档二下就是一个处于关闭态的
  相关开关项，正撞 `需求梳理清单_v2.md:1907`。同理不加「默认建卷目录／上次导出路径」
  （Q-04 可验收项 3 点名的最易漏项）与语言／主题／开机自启这类零产品价值的本地偏好。
- 三个开关本身**不构成 8.4 痕迹**：它们在档二与档三下的存在与默认值完全相同，
  8.4 禁止的是"处于关闭态的**档位相关**开关项"，与这三项无关。

#### 实现

- 侧栏禁用占位 `<div>` 改为 `<button class="nav-item dashboard-nav" data-section="settings">`，
  新增 `#dashboard-section-settings`。分区切换沿用既有的
  `.dashboard-nav` / `.dashboard-section` 泛化实现，未改动路由代码。
- 会话组三行开关，顺序同设计稿：登录后自动挂载全部容器（默认关，依 8.7.5）、
  记住容器位置（默认开，依设计稿）、拔出安全卡时立即卸载全部卷（开且锁定）。
  前两者是 `<button role="switch">`，锁定项是 `<span role="switch" aria-disabled="true">`
  并配一枚锁形图标 —— **锁定项不是按钮，点击它不进入任何处理器**。
- 关于组五行：卷加密 `AES-256-XTS`、卷头 PRF `HMAC-SHA-512`、卷密码派生
  `Argon2id · m=512 MiB · t=2 · p=1`、安全卡记录 `216 字节 · AES-256-GCM + HMAC-SHA-256`、
  产品版本。
- `card_session.js` 新增 `[data-setting]` 的点击处理器，**只翻转自身 `aria-checked`**：
  不触碰原生桥、不写任何浏览器存储、不参与 `busy` 串行化（它不产生任何副作用）。
  脚本缓存串同步升为 `p2-17a-settings`。
- 版本号来源定为 CMake：`project(usb_vault VERSION 0.2.0)`，经
  `USB_VAULT_PRODUCT_VERSION` 编译定义传给契约测试与页面做绑定校验。
  ⚠️ `0.2.0` 是本切片给出的占位版本号，正式版本方案待定。
- 未新增任何 COM 接口、原生投影字段或本地文件写入。

#### 测试、安全复核与提交状态

- 页面契约测试新增 `test_settings_page_is_static_and_matches_the_native_facts`，
  并为该测试目标加上 `include/` 与产品版本定义，使其可直接读取原生常量：
  - 断言设置页可达且 `data-section` 数与 `dashboard-section-` 数相等；
  - 断言段内恰有 3 个 `role="switch"`、2 个 `data-setting`，锁定项是带
    `aria-disabled="true"` 的非按钮且全段仅此一个 `aria-disabled`，段内无 `<input>`；
  - 🔴 **两条"尚未落盘"的守卫**：脚本中 `data-setting` 只出现一次（仅用于选取），
    且处理器体恰为翻转 `aria-checked` 一行；页面与脚本**均不得出现**
    `localStorage`／`sessionStorage`／`indexedDB`／`document.cookie`。
    17b 接入落盘时必须显式改这两条，改不动就说明动到了不该动的地方；
  - 关于块的 Argon2id 参数由 `kKdfV1MemoryKib/1024`、`kKdfV1Iterations`、
    `kKdfV1Parallelism` 拼出后比对，记录长度由 `kCardRecordV1Size` 比对，
    版本由 `USB_VAULT_PRODUCT_VERSION` 比对 —— 任一漂移即失败；
  - 断言设置页不含「安全卡状态」「代码签名」「EV」。
- `test_authenticated_dashboard_is_real_state_only` 的禁用导航断言改为按
  `class="nav-item nav-item-disabled"` 计数（3 个），不再按全页 `aria-disabled` 计数 ——
  后者已被设置页的锁定开关污染，是这次唯一一处因新增元素而必须收紧作用域的断言。
- **三条负向验证**（临时改文件后重跑，确认守卫真的会咬）：版本改成 `9.9.9` →
  `FAIL: about version has drifted from the CMake product version`；插入
  `<input type="checkbox">` → `FAIL: settings page offers a control this slice cannot persist`
  （该守卫在本次改为开关后已按上述新形态重写）；在处理器里加一行 `localStorage.setItem` →
  `FAIL: the product page persists state in browser storage`。三次均已还原。
- 真实 WebView2 假后端夹具新增设置页一段：点击导航后设置页 `active`、加密盘页让出；
  两个开关的默认值为关／开；点击自动挂载翻转为开、再点回关；**点击锁定项后其状态不变**；
  最后切回加密盘页继续原流程。夹具的 `card_product_webview_integration.html` 是独立维护的
  结构副本，已同步补上导航、分区与三个开关，失败令牌表新增 `fail-settings-navigation`、
  `fail-settings-defaults`、`fail-settings-switch`。夹具退出 0。
- 三套构建：Debug CTest **44/45**（唯一失败为实体卡不在位时的 `vikey_helper_process_tests`，
  `0x93 card communication failed`；插卡后的 `/analyze` 运行该例即通过）；
  MSVC `/analyze` 构建退出 0、无任何项目诊断，CTest **45/45**；
  AddressSanitizer CTest **43/45**，失败的 `vikey_helper_process_tests` 与
  `vikey_helper_hid_compatibility_tests` 均因 ASan 下 helper 子进程以 `0xC0000135`
  （DLL not found）启动失败 —— 既有环境限制，与本切片改动的文件无链接关系。
- 安全复核：本切片不写本机磁盘、不新增跨边界字段、不展示硬件标识或任何档位／模式／
  容器形态信息；三个开关的存在与默认值在档二与档三下完全相同，不构成 8.4 意义上的痕迹。
- 🟡 遗留：登录后自动挂载与记住容器位置**只有 UI，没有后端**，状态不持久；
  `0.2.0` 为占位版本号。
- 提交：`5cdec95 [P2.17]open the settings page`。

### 小切片 17b：容器视图统一，不再按卷类型分栏

#### 需求审计与决策

- 17a 已提交为 `5cdec95`。用户提出：隐藏盘既然能建了，「隐藏盘概览」不该还挂着
  「建卷能力接入后……」的过时文案；并要求把「打开已有」改称「导入」。
- 🔴 **审计后发现真正过时的不是文案，而是分栏本身。** 16n 已把 B 并入统一的
  `containers` 集合，而页面上每个 `[data-container-list]` 渲染的都是同一份
  `container_statuses()` —— **建出来的隐藏盘此刻就显示在「加密盘」列表里**，
  仪表盘的「加密盘数量」统计的也是全部容器。分栏标题名不副实已有一段时间。
- **隐藏盘不可能拥有自己的列表**，且不应该有：
  - 8.7.5 明写「仪表盘不按卷类型分类展示」，与 8.6「档位不是运行时实体」一致；
  - 跨边界的 `ContainerUiStatus` 只有 `id / name / mounted / size_mib / drive_letter`，
    **故意不带卷类型**。要给隐藏盘单独建列表就得新增卷类型字段跨边界，那是直接的 8.4 风险。
  - 设计稿的仪表盘注记也是同一口径：「侧栏不再按'加密盘／隐藏盘'分类。旧原型与当前实现
    都分了两栏，而分类展示本身就是档位痕迹 —— 统一为'容器'一个视图。」
- 用户于 2026-09-02 定案：按设计稿改成单一容器列表，**先不做隐藏卷标注**。
  （设计稿里那个「隐藏卷」标签依 8.7.5 是可以显示且无需落盘的 —— 驱动挂载后经
  `MountedVolumes.volume_type` 返回，诱饵密码挂出的是外层自然没有该标签。
  本切片按用户要求不做，留作后续可选项。）

#### 实现

- 侧栏「加密盘管理」＋「隐藏盘管理」两项合并为单一「容器」入口，带容器计数徽标；
  「磁盘管理」分组标签随之取消。
- `#dashboard-section-encrypted` 与 `#dashboard-section-hidden` 合并为
  `#dashboard-section-containers`；仪表盘删掉「隐藏盘概览」占位卡，只保留一个
  「容器 · 按当前密码可打开的容器」分组。
- **两个建卷入口都保留在两个视图里**（各 2 处，共 4 个按钮）：8.7 要求建卷入口常驻可见，
  藏掉任何一个都等于告诉旁观者这台机器上有什么。
- 统计卡「加密盘数量」→「容器数量」；空状态文案改为「还没有容器。容器是一个加密文件，
  挂载后在系统里显示为一个普通磁盘。……」；分组说明改为「所有容器共用同一个密码，
  分开存放不等于分开保护」。
- 「打开已有」／「打开已有加密盘」改称「导入」／「导入容器」。
  ⚠️ **仅改称谓，行为未变**：`mount_container(path)` 仍只用固定的标准派生
  `KDF(S, kContainerASalt, "diskA")`，且仍显式拒绝固定的 B 路径
  （`card_login_mount_session.cpp:641`）。**目前导入不了隐藏盘。**

#### 隐藏盘导入的可行性结论（本切片不实现，仅记录）

- 因 8.7.1 定案选案 1（**全部 B 共用一个 salt**，`container_layout_v1.h:19` 的
  `kContainerBSalt` 是编译期常量），B 的卷密码为 `Argon2id(登录口令, kContainerBSalt, S)`，
  **同卡同登录口令下该密码可开任意 B 容器**；会话已在登录时保留它
  （`card_login_mount_session.cpp:393`）。因此导入 B 技术上可行。
- 🔴 **但有硬约束**：该派生只在登录当刻、且固定路径上存在 B 时发生
  （`:391` 的 `container_exists` 判断），而登录口令此后即被清除。
  **固定路径上没有 B 时，整个会话都导入不了任何 B。** 这恰是用户最想导入的场景。
- 🔴 **不应做成独立的「导入隐藏盘」按钮**：产品无法从文件本身判断 A 还是 B（这正是 8.4
  要的效果），独立按钮等于要用户自行声明档位，而 8.6 只允许产品在建卷选择
  「是否带隐藏卷」那一次接触档位信息，用完即焚。
- **建议方案**：单一「导入」动作，先试标准派生、失败再试会话保留的 B 密码，用户不声明任何东西；
  挂出外层还是内层由登录口令决定，8.7.5 已论证该逻辑自洽。代价是失败路径多一次挂载尝试。

#### 测试与提交状态

- 页面契约测试：`test_authenticated_dashboard_is_real_state_only` 改为断言
  `dashboard-section-containers` 存在、`dashboard-section-encrypted` 与
  `dashboard-section-hidden` **均已不存在**、`data-container-list` 恰为 2 处，
  两个建卷入口仍各 2 处。
- WebView2 假后端夹具的结构副本与驱动同步改名，导航、建卷、挂载、卸载全流程原样通过，退出 0。
- 完整 CTest 与三套构建结果见下条提交记录。
- 提交：`ef02638 [ui]unify the container view`。

### 小切片 17c：新建容器改为单一入口＋容器类型选择

#### 需求审计与决策

- 17b 已提交为 `ef02638`。用户要求把新建容器流程也做成设计稿的样子：
  **一个「新建容器」按钮，点击后先弹出容器类型选择**，并给出该屏的四处文案改写。
- 审计 8.7.2「入口恒显」后确认：**入口由两个按钮收敛为一个按钮＋一屏两张卡片，
  并不削弱恒显要求**，反而更贴合设计稿注记「两个入口恒显、视觉权重完全对等」——
  两张卡片始终同时出现、顺序固定、默认恒选「标准容器」，与当前登录的是哪一个密码无关。
  相比之下，旧版并排两个按钮里「新建隐藏盘」这个称谓本身就把档位摆在了主界面上。
- 用户指定的四处文案（设计稿 `create · choose type` 一屏）：
  1. 引导语改为「容器是一个加密文件，挂载后显示为一个普通磁盘。请选择要创建的容器类型：」；
  2. 标准容器改为「一个标准结构的容器，含一个加密卷，使用日常密码进行解密与挂载。」；
  3. 隐藏卷容器改为「一个双层结构的容器，含一个外层卷与一个隐藏卷，使用诱饵密码或日常密码进行解密与挂载。」；
  4. 「两种结构在磁盘上字节不可区分。选择哪一种，只有你自己知道。」整块删除。
- 🔴 第 4 条同时让设计稿注记「'字节不可区分'是本产品的卖点，也是唯一允许出现在这一屏的
  安全性说明」失去所指，一并从 `usb-vault_ui-design.html` 的 notes 里删掉，
  避免设计稿与实现再次不同步。新文案未引入「档位／模式／胁迫」等 8.6 禁用词，仍只描述结构本身。

#### 实现

- 仪表盘与容器视图里的两个建卷按钮（共 4 个）各合并为一个 `data-create-container`
  「新建容器」按钮（共 2 个），带设计稿的 `+` 图标。
- 建卷对话框拆成两步，由新的 `creationStep`（`"type"` / `"params"`）驱动：
  - 第一步 `#create-type-step`：`role="radiogroup"` ＋ 两张 `role="radio"` 卡片
    （`data-create-kind="encrypted"` / `"hidden"`），样式照设计稿 `.pick` 移植为
    `.type-pick`（选中态金色描边＋金色图标底）；标题「新建容器」，页脚「取消／下一步」。
  - 第二步：沿用既有的容量表单与隐藏盘表单，标题与提交按钮文案不变；
    页脚「返回」改为「上一步」，可回到类型选择。
- `openCreationFlow()` 不再接收 kind：**每次打开都固定停在第一步、固定预选「标准容器」**，
  「打开建卷对话框」这一动作本身不携带任何档位信息。
- 新增 `resetCreationInputs()`，在打开、返回上一步、关闭三处统一清空容量与三个密码框。
  🔴 这里比 17b 更严：**旧版关闭对话框时诱饵密码仍留在 DOM 里**，要等下次打开才被覆盖；
  现在离开参数步的任一路径都立即清空，符合 8.6「用完即焚」。
- 类型选择步弹窗宽度扩到 `40rem`（`.modal-overlay[data-step="type"]`），窄屏两张卡片转单列。
- 资源版本号 `?v=p2-17a-settings` → `?v=p2-17c-create-flow`。

#### 测试、安全复核与提交状态

- 页面契约测试：`test_authenticated_dashboard_is_real_state_only` 的断言由
  「两种 `data-create-kind` 各 2 处」改为「`data-create-container` 恰 2 处、
  两种 `data-create-kind` 各恰 1 处」；恒显要求的表述随之从「两个按钮各出现在两个视图里」
  改写为「两张卡片始终同时存在于唯一的建卷入口内」。脚本版本断言同步更新。
- WebView2 集成驱动新增两段：打开后必须停在类型步（标题「新建容器」、按钮「下一步／取消」、
  标准容器预选、隐藏卷未选）；在参数步改动容量后按「上一步」，必须回到类型步且容量已复位为 64。
  夹具新增 `fail-creation-type-step` / `fail-creation-back-step` 两个失败码。
  隐藏盘分支改为「新建容器 → 点隐藏卷卡片 → 下一步」，并校验两张卡片的 `aria-checked` 互斥翻转。
- 安全复核：
  - 8.7.2 逐字对称 —— 两张卡片的存在、顺序与默认选中项均与当前密码无关，未新增任何按档位分支；
  - 8.7.3 不得验证 —— 本切片未触碰第二密码的校验路径，仍只做两次输入比对；
  - 8.6 用完即焚 —— 离开参数步即清空密码框，较旧版收紧；
  - 未新增 `innerHTML` 或任何拼接进 DOM 的字符串，卡片文案是静态标记。
- 全量 CTest 45 项通过 44 项；唯一失败项 `vikey_helper_process_tests` 需插入实体 ViKey 卡
  （报 `card communication failed`），与本切片无关。WebView2 集成测试退出 0。
- ⚠️ 遗留不一致：第二步仍沿用「新建加密盘／新建隐藏盘」旧称谓，与第一步卡片的
  「标准容器／含隐藏卷的容器」不齐。设计稿对应屏为「新建标准容器／新建含隐藏卷的容器」，
  用户未点名要改，本切片未动，留作后续文案统一。
- 提交：`26d305a [ui]create containers from one entry`。

### 小切片 17d：参数步迁到「容器」口径，日常密码不再重输

#### 需求审计与决策

- 17c 已提交为 `26d305a`。用户要求把参数步也做成设计稿的样子，并给出七条具体改动。
- 🔴 **第 5 条（去掉「当前密码」输入框）不是 UI 改动。** `create_container_b` 有两处要日常密码明文：
  一是派生隐藏卷密码，二是 `set_second_card_password → rewrap_card_record_v1` 重封卡记录。
  后者把密码当作**封装材料**而非解锁材料，且卡记录按设计不带槽位标签，无法只替换其中一个信封。
- 用户追问：会话保留日常密码后，诱饵密码登录的人能不能拿到它？**不能。**
  会话保留的是「本次登录用的那一个」——`consume_authenticated_login` 收到什么就存什么，
  诱饵登录留下的就是诱饵密码。日常密码在卡上只以 `Argon2id(日常密码)` 为密钥的
  AES-256-GCM 信封形式存在，解开它需要日常密码本身；**持有 S 也没用，S 是信封里的载荷不是钥匙**。
  同一段代码、同一个缓冲、同一个生命周期，因此保留在两种登录下逐字对称，8.7.2 不受损。
- 用户于 2026-09-02 定案：按「会话保留登录口令」实现，并额外加一道重写卡记录的确认。

#### 审计中查出的两个既有缺陷（非本切片引入）

1. 🔴 **卡记录只有两个信封。** `card_record_v1.cpp` 的 `build_record` 固定写 primary 与 secondary
   两个信封（未用时填随机字节）。`set_second_password(existing, second)` 把记录重建为
   `{second, existing}`，**第三个密码必然被挤掉**：
   - 日常登录 + 每次都设同一个诱饵密码 → `{诱饵, 日常}`，稳定（第 6 条文案要守的正是这个不变量）；
   - 日常登录 + 换一个新密码 Y → `{Y, 日常}`，旧诱饵作废，旧 B 的外层卷再也打不开；
   - **诱饵登录 + 建含隐藏卷的容器 → `{新设密码, 诱饵}`，日常密码的信封被抹掉，真实用户永久锁在卡外。**

   🔴 **根因不是槽位不够，是产品认不出哪个信封是哪个。** `rewrap_card_record_v1` 不替换某个槽，
   而是拿 S 从零重建整条记录，只写进传入的那两个密码。即便改成「保留未知信封、只替换一个」，
   产品也只能识别**一个**信封——当前登录口令能打开的那一个，其余全是不透明字节。
   而这个「认不出」是刻意的：`card_record_service.h` 明写记录
   "stores no slot label, no activation bit, and nothing recording which password is which"，
   加标签本身就是档位痕迹。
   因此**加第三个信封解决不了问题**：对手在诱饵登录下反复建容器，第一次填掉空槽，
   之后每次都得盲选淘汰，N 只把代价从 1 次操作抬到 N−1 次操作。
   另注：`rewrap_card_record_v1` 有 `constant_time_equal(primary, secondary)` 的拒绝分支，
   诱饵登录下对手**不能**把新密码设成诱饵密码本身，必须设一个新的，而那个新的必然挤掉日常
   —— 这条路径上没有无害操作。
   候选缓解（均未实现，留作 8.7 层面的设计决策）：
   1. **盲淘汰改成对称拒绝**：≥3 槽 ＋「绝不淘汰本会话认不出的信封」，记录满即失败，
      两种登录下逐字一致地失败。损害有界且不泄露，但需要区分「真信封」与「未用时填的随机字节」，
      而那个区分本身又是一个不能存在的标志位——这是这条路要先解决的问题。
   2. 把诱饵密码从卡记录里拿掉，只当卷密码不当登录凭据。整类问题消失，但胁迫时用户在登录框前
      无东西可交，**砸掉产品前提**，排除。
   3. 卡记录备份／根密钥托管，让锁死可恢复。属于另一个功能，且备份文件本身是痕迹。
   本切片只做了明示勾选让用户知情。⚠️ 并且要记住：**该勾选文案在诱饵登录下同样显示**，
   等于把「这么做会锁死真实用户」讲给对手听；8.7.2 要求两侧逐字一致，无法只讲给真实用户听。
   它没有暴露「存在第三个密码」，只讲后果，故判定可接受。
2. 🔴 `create_container_b` 创建成功后**没有把派生出的隐藏卷密码写回 `state_.container_b_password`**，
   而 `mount_adopted_container` 对隐藏卷容器要求该值非空。于是「本会话建出来的隐藏容器，
   卸载之后无法再挂载」。本切片顺手修复，并补了往返挂载的测试。

#### 未采纳的一项

- 提问时曾把「登录时无条件派生隐藏卷密码，顺带消除登录耗时侧信道」并入选项一，实现时未做。
  那次派生在两种密码下是**同一个调用、同一个 salt**，只是算出的 key 不同且从不离开内存，
  所以外部唯一的可观测量是「跑了几次 Argon2id」，它只回答一个问题：
  `container_b.hc` 存不存在。诱饵登录对此没有任何专属优势。
- 🔴 而这个问题已经被更响地泄露了：`container_layout_v1.h:11` 把文件名硬编码为
  `container_b.hc`，`tools/app/main.cpp:180` 把它放在 exe 同目录下；且该文件在当前实现里
  **只可能由 `create_container_b` 产生**，那条路径必定 `create_with_hidden`。
  于是 `container_b.hc` 存在 ≡ 这台机器有一个含隐藏卷的容器 —— 对手 `dir` 一下即可，
  不需要登录也不需要秒表。**固定文件名才是比登录时序响得多的档位泄露源。**
  在文件名还硬编码着的时候去堵时序差，是在没有墙的房子上封窗户，代价是每次登录多约 1 秒。
- 因此登录侧的无条件派生**与 8.7.4 绑定**：等容器路径清单加密存放、文件名与位置任意之后，
  「某处存在一个隐藏容器」才第一次成为秘密，那时才必须抹平登录时序。
  而且到那一步，正确做法多半是把派生整个挪出登录路径（首次挂载时按需派生），连 1 秒都不用付
  —— 本切片已经把 `create_container_b` 挪成按需派生，剩下的只有登录那一处。
- 🟢 **本切片顺带解锁了统一导入。** 17b 记录的阻塞是「B 卷密码只在登录当刻、且固定路径上存在 B
  时派生，登录口令随后即清除」。会话现在保留了登录口令，任意时刻都能派生，
  「先试标准派生、失败再试 B 派生，用户不声明档位」的统一导入方案已可实现，
  它同时是 B 改用任意路径（进而消掉 `container_b.hc` 硬编码）的前置条件。

#### 实现（原生）

- `CardLoginMountSessionState` 新增 `login_password`：本次登录用的那个口令本身。
  与既有的 `container_b_password`（派生值）并列，生命周期一致，
  在 `force_unmount_active()`（拔卡路径）里一并清空。
- `create_container_b` 去掉 `login_password_utf8` 形参，改用 `state_.login_password` 派生隐藏卷密码；
  会话未捕获登录输入时直接 `logic_error`，不臆造替代值。
- `IAuthenticatedCardVolumeSession` 新增 `copy_session_password()`：只为「需要密码本身而非派生值」
  的那一个操作存在。没有让会话层反向依赖卡入口层去驱动 `ICardEntryActions`，保持既有分层方向。
- `CardProductUiSessionController::create_container_b` 少两个形参，登录口令改从会话取；
  新增 `hidden_fits_outer()`，在边界上挡住「内层 > 总容量 3/4」，与页面同一条规则。
- `createContainerBMib` 由五参降为四参（总容量、内层容量、诱饵密码 ×2）。

#### 实现（页面）

- 参数步全部改为容器口径：标题「新建标准容器／新建含隐藏卷的容器」，
  标签「容量／总容量／内层容量」，提交按钮统一为「创建容器」并去掉图标（第 7 条），
  「上一步」左侧加左箭头。按钮内文案改由 `#create-volume-cancel-label` 承载，
  图标用 `hidden` 开关，**不引入 `innerHTML`**（页面契约测试仍钉死这一条）。
- 容量单位可切换 MiB / GiB / TiB（第 1 条）：单位是一个带上下箭头的按钮，点开是单位列表；
  切换时同步重写输入框的 `min`/`max`，让 spinner 与校验对同一组边界。
  **隐藏容器的两个容量共用一个单位**，避免跨单位比较。内部一律按 MiB 过桥。
- 越界即禁用创建，并在容量框下方红字说明（第 2 条）：「容量超出最大值。」「容量低于最小值。」
  「内层容量过载。」「请输入整数容量。」
- 「内层卷至多占总容量的 3/4」（第 4 条）：判据 `hidden * 4 > outer * 3`，页面与原生边界同规则。
  现有默认 128 / 96 恰好等于 3/4，仍然合法。
- 提权说明改为「创建过程需要管理员权限，系统稍后会弹出确认窗口。」，图标换成感叹号（三角警示），
  并**移到容器类型选择屏**（第 3 条）。放在选择之前，就无需再靠「所有类型都会请求」一句话来保证对称。
- 去掉「当前密码」输入框（第 5 条），诱饵密码提示改为
  「注意：若您曾设置过诱饵密码，此处需与曾经的诱饵密码保持一致。」，
  样式与提权提示同为 `.creation-note`（第 6 条）。
- 新增卡记录重写的勾选确认，未勾选则「创建容器」保持禁用；文案在两种登录下完全相同。
- 资源版本号 `?v=p2-17c-create-flow` → `?v=p2-17d-container-parameters`。
- 设计稿 `usb-vault_ui-design.html` 同步：提权提示移屏并换感叹号图标，容量加单位切换控件，
  内层提示改 3/4，诱饵提示改为 note，勾选确认补入，两个页脚按钮按第 7 条调整；
  失效的注记（「提权说明必须写成'所有类型都会请求'」）改写为新的理由。

#### 测试、安全复核与提交状态

- 会话核心测试：建卷用例改走 `consume_authenticated_login` 而非直接 `establish`，
  新增「已认证但未捕获登录输入 → 拒绝且不触达 runtime」，
  新增「本会话建出的隐藏容器卸载后可再挂载」。
- 控制器测试：断言卡记录用的口令来自会话（`session_password_calls == 1`）而非页面；
  新增「会话已释放登录输入 → 卡与卷都不触达」；容量用例补 `{64, 49}` 越 3/4 与 `{64, 48}` 恰好合规两条。
- 宿主对象测试：五参降四参，剩余两个密码数组仍逐个校验已清零。
- 页面契约测试：断言页面**不再含** `create-container-b-password`、脚本不再含 `containerBPassword`；
  密码输入框数由 5 降为 4；新增单位换算后的边界断言、3/4 规则断言、勾选确认断言；
  诱饵提示的固定文案改为新句。
- WebView2 集成驱动：新增单位切换往返（2048 MiB 合法 → 切 GiB 后 2048 GiB 越界并红字提示 →
  1 GiB 合法）、内层 97/128 触发「内层容量过载。」、取消勾选后创建按钮禁用三段；
  夹具新增 `fail-creation-unit` / `fail-hidden-capacity-rule` / `fail-hidden-acknowledgement`。
- 安全复核：
  - 8.7.2 逐字对称 —— 保留的是「登录用的那一个」而非「日常密码」，两种登录下同一路径同一生命周期；
  - 8.7.3 不得验证 —— 未触碰第二密码校验路径，仍只做两次输入比对；
  - R-03 / 8.6 —— 新增的明文只是登录口令本身，而 S 已在会话中常驻且严格更强；
    拔卡路径与登录重入均清空；页面侧密码在编码后立即清空，两个字节数组在 finally 里各清一次；
  - 未新增 `innerHTML`，新控件文案全为静态标记。
- ⚠️ 未做（不在七条之内，需原生支持）：设计稿参数屏还有「名称」与「保存位置」两个字段，
  当前标准容器仍由原生选择器选路径、隐藏容器仍用固定路径。
- 提交：`bb625b5` 与 `5006513` 两笔，见下节补记。

#### 17d 提交状态补记

- 原生会话保留登录输入、四参数隐藏容器桥及本会话隐藏容器重挂载修复已提交为
  `bb625b5 [P2.17]create the hidden container without retyping the login`。
- 参数步容器口径、容量单位与校验、提权提示和卡记录重写确认 UI 已提交为
  `5006513 [ui]migrate the parameter step to container wording`。两笔均未推送。

### 小切片 17e：当前认证会话操作日志

#### 需求审计与决策

- 复核 `需求梳理清单_v2.md` 8.4「本地状态层不可区分」与 8.6「档位不是运行时实体」，并检查
  当前会话的创建、挂载、卸载真实提交点、UI 控制器、COM host object、产品页和 WebView2 集成夹具。
- 原设计稿的操作日志屏承诺「最近 30 天、存放在标准容器内」，并示例展示卡标识、容器容量、
  拔卡卷数、强制卸载占用详情等；当前既无安全的持久化载体，这些字段也会扩大 8.4 痕迹面。
  本切片收敛为**仅当前进程内的当前认证会话**，最近 30 天持久日志暂缓。
- 记录时机以已经完成的原生事实为准，而不是以前端动作或一个笼统总结果为准：
  - 成功认证交接完成后记录「打开会话成功」；登录失败、密码尝试和失败次数完全不记录；
  - 容器发布成功即记录「创建成功」；随后挂载另记一条。因此发布完成但挂载失败时精确形成
    「创建成功 + 挂载失败」，不会误导用户容器不存在；
  - 已接受的创建、挂载、卸载操作失败只记通用失败，不记错误文本、耗时或内部阶段；
  - 取消文件选择、页面校验失败和错误路由不属于已接受操作，不生成日志。
- 日志 schema 固定为事件枚举、结果枚举、完成时间、**文件名**和已确认盘符；禁止自由文本、完整路径、
  卡／硬件标识、密码角色、信封命中、容器类型／结构、隐藏尺寸、保护状态、导出迁移和文件活动。
- 采用固定 128 槽环形缓冲；记录是 best-effort 且 `noexcept`，分配或时钟异常不得改变安全／卷操作结果。
  清空本身不生成事件；新认证和拔卡／强制会话释放都会清除旧日志。

#### 实现

- `IAuthenticatedCardVolumeSession` 新增封闭的 `OperationEvent` 快照和清空边界；正式
  `CardLoginMountSessionCore` 在会话状态中持有 128 槽环形缓冲，并在认证、发布、驱动挂载和卸载的
  权威完成点写入。强制释放只清空日志，不记录一个用户已无法在该会话页面读取的拔卡事件。
- `CardProductUiSessionController` 仅在认证路由投影日志，限制最多 128 条并逐项验证时间、枚举、文件名、
  盘符及事件字段组合；任何路径、未知枚举或不可能形状均失败关闭。
- COM 自动化面新增 7 个窄成员：数量、时间、事件、结果、文件名、盘符和清空。时间以可被 JS 精确表示的
  Unix 毫秒数传递；查询、越界和控制器异常统一返回失败，不伪造替代事实。
- 产品侧栏的「操作日志」现为可进入页面，最新事件在前，以纯 `textContent` 构造四列表格；每次 native
  refresh 都重建内存投影，不使用 `innerHTML`、`localStorage`、`sessionStorage`、IndexedDB 或 cookie。
  页面再次验证 128 上限、整数时间、固定枚举、文件名无路径和事件字段组合，支持清空当前会话日志。
- `usb-vault_ui-design.html` 的对应屏同步为当前会话语义，删除 30 天、标准容器持久化、卡标识、容量、
  强制卸载详情等尚未实现或不应记录的内容。

#### 测试与安全复核

- 会话核心覆盖：新会话替换旧日志、清空不自增、成功创建拆成创建／挂载两条、发布后挂载失败、发布前
  创建失败、卸载失败后重试、拔卡清除，以及连续 130 条事件只保留最新 128 条且顺序不乱。
- 控制器／COM 测试覆盖认证路由门、合法投影、清空、完整路径、不可能事件形状、超 128 条、查询失败、
  COM 类型与越界。页面契约钉死当前会话文案、全部 bridge 调用、二次字段校验、纯文本渲染和零浏览器持久化。
- WebView2 集成夹具覆盖创建失败、创建与挂载分离、最新优先展示、清空不产生新事件，以及重新认证后只剩
  新的「打开会话」；Debug、`/analyze`、ASan 三套 GUI 均退出 0。
- Debug、MSVC `/analyze`、ASan 三套完整 CTest 均为 **45/45**：43 项实际通过、2 项按受限父令牌规则
  跳过；`/analyze` 无项目诊断，ASan 未报告内存错误。最后加入的拔卡清除与共享 128 上限后，三套均重新
  构建成功，相关 5 项测试各 **5/5** 通过。`node --check` 两个脚本通过。
- 安全复核确认：日志仅进程内、仅认证态可读、没有秘密或身份材料、没有持久化写入；创建结构在两类密码
  下使用同一事件名和字段，B 的形态不会跨边界。失败详情被压缩成单一结果，记录失败不会反向改变卷操作。
- 🟡 遗留：当前没有跨进程／最近 30 天历史，应用退出后日志消失；如果以后持久化，必须先设计与 8.7.4
  加密分层清单兼容的载体，并重新做档二／档三本地产物逐字节不可区分验收。
- 提交：`6988595 [P2.17]add current-session operation history`，未推送。

### 小切片 17f：去掉重复的卡状态指示，登录给出进行中反馈

#### 需求审计与决策

- 17e 已提交为 `6988595`。用户提出三条界面改动：去掉主页绿色的「安全卡已认证」、
  开发窗口标题改名、登录输密码后要有加载动画（原话「不然用户会以为卡死」）。
- 审计发现「安全卡已认证」在页面上出现**两次**：顶栏的绿色胶囊
  （`card_session.html` 的 `.card-badge` ＋ `.status-dot`）与仪表盘第一张统计卡
  「安全卡状态 / 已认证」（绿色 `stat-icon success`）。两者语义完全重复，
  而整个已认证外壳本来就只在认证态渲染，这句话是同义反复，故一并去掉。
- 顺带与设计稿注记对齐：「『卡已认证』用中性金而不是绿色徽章 —— 绿色留给『已挂载』
  这一真正需要一眼分辨的状态」。改后绿色在该界面上只剩「已挂载」一处。

#### 实现

- 删除顶栏 `.card-badge` 与仪表盘首张统计卡；统计栏由三列改两列
  （活动卷状态、容器数量）。随之删除已无引用的 `.card-badge` / `.status-dot` /
  `.stat-icon.success` 三条样式，并把 `.card-badge` 从顶栏 flex 选择器组里摘掉。
- 开发窗口标题 `tools/app/main.cpp:39`
  「USB Vault - P2 ViKey 联网开发模式」→「USB Vault - ViKey 开发模式」。
  ⚠️ 「联网」在 `需求梳理清单_v2.md:956` 与 `MILESTONES.md:342` 里特指
  「只允许开发凭据、开发卡与非生产 blob」这一档，用于区分将来的离线／生产模式。
  标题里现在只保留「开发模式」，安全上最要紧的「这不是生产版」仍在，细分口径不再出现在标题。
  无测试断言该标题。
- 会话动作按钮内嵌不确定型转圈，并在忙时把文案换成进行时：
  登录「正在验证安全卡」、初始化「正在初始化安全卡」、卸载「正在安全卸载」。
  解卡要跑一次 Argon2id 加卡 I/O，是数秒静默，缺少动效会被读成卡死。
  🔴 **不做进度条**：设计稿明确「进度按真实阶段拆分，不用假进度条」，
  而登录链路拿不到真实阶段，能给的诚实指示只有不确定型转圈。
  转圈沿用设计稿 `.spin` 参数（`.8s linear infinite`），禁用态自动切金色描边，
  并加 `prefers-reduced-motion` 降速。
- 按钮结构改为 `<span class="spinner">` ＋ `<span id="card-session-action-label">`，
  脚本只写 label 的 `textContent`——契约测试钉死页面脚本不得出现 `innerHTML`。
- 资源版本号 `?v=p2-17e-operation-history` → `?v=p2-17f-login-progress`。

#### 测试与提交状态

- 集成驱动里 `action.textContent !== "尝试安全卸载"` 会被按钮新增子元素带来的空白打翻，
  改为读 `card-session-action-label`。这与 17c「上一步」按钮是同一个坑，
  凡是给按钮加图标就会再遇到一次。
- WebView2 假后端页面同步补 spinner 与 label 两个节点。
- 页面契约测试 PASS；WebView2 集成测试退出 0（已核对部署的 `card_session.js` 时间戳新于源文件）。
- 全量 CTest 见提交记录。
- ⚠️ 构建期一次 `LNK1168`：开发 EXE 正在运行导致无法写入，关闭后重建通过，与改动无关。
- 提交：`a2008f2 [ui]drop the duplicate card badge and show login progress`。

### 小切片 17g：设置页重新初始化安全卡

#### 需求审计与决策

- 17f 已提交为 `a2008f2`。用户要求在设置页增加把一张 `initialized_v1` 卡破坏性退回
  provisioning 状态的正式入口，并给出了另一位 agent 的流程方案（设置页执行、成功后自动关闭
  应用、下次启动由现有入口路由进初始化页）。
- **实现前逐条核实该方案的三个前提，均成立**：
  1. `provision_uninitialized_card_record_v1()`（`card_record_v1.h`）已存在，产出的正是受控
     出厂流程写的那个精确 marker；
  2. `commit_card_record_journal_v1` 调用的 `validate_logical_record`
     （`card_record_journal_v1.cpp:366`）**同时接受** `provisioned_uninitialized` 与
     `initialized_v1`，不存在阻挡 initialized → provisioning 的状态机守卫，因此不需要新增
     helper IPC；
  3. `card_entry_router.cpp:20` 已把 provisioning marker 路由到初始化页。
- ⚠️ 一处不可用：`provision_card_record_journal_v1` 在检测到 product evidence 时会显式拒绝，
  那是出厂路径。重置必须走普通 `commit` 路径。
- 关闭应用是必需的而非偷懒：`CardEntryContextController` 的注释写明它为**至多一次物理插入**
  持有固定路由，重置后这张卡不再符合被路由时的形态，进程内无法重新分类。

#### 实现

- `CardRecordService::reset_to_provisioning()`：只接受 `initialized_v1`；写入 marker 后
  **复读比对**，不一致即 `fail_unverified_write()`。marker 与出厂产物逐字节相同，卡上不留任何
  被替换身份的痕迹。
- `ICardEntryActions::reset_card_record()` / `CardEntryContextController`：复用既有
  `make_update_target`；**成功与失败都退休该插入**（route → `unavailable`、context 释放）。
- `CardProductUiSessionController::reset_card_record()`：先 `unmount_active()`，
  🔴 **仍有挂载则抛错且完全不写卡**——挂载会比打开它的密钥活得久，那个盘符将永远没人回收
  （`unmount_active` / `force_unmount_active` 都靠 `state_.containers` 遍历）。
  写卡成功后释放会话密钥、清操作日志、清 S。
- 新增 `IAuthenticatedCardVolumeSession::release_session_secrets()`：清空
  `login_password` 与 `container_b_password`。
  🟡 原打算借 `force_unmount_active()` 顺手清，但控制器测试的 fake 里写着
  `throw std::logic_error("forced unmount is outside the UI contract")`——那是在说借错了门，
  遂改为显式接口。
- 新增 `IUiCloseRequestSink`（`card_product_webview_host_object.h`）与 `resetCardRecord`
  桥方法。提交成功后 `PostMessageW(WM_CLOSE)`——用 Post 不用 Send，让 WebView2 的这次调用
  先完成栈展开。开发应用实现该 sink，测试夹具传 `nullptr` 或记录用的假实现。
- 页面：设置页新增「安全卡」危险区与确认弹窗，勾选前「重新初始化」保持禁用，
  支持 Esc 与点击遮罩关闭。成功后页面停在「已重新初始化，正在关闭应用。」而不刷新状态——
  刷新会短暂路由到一个本会话已无法操作的卡。
- 资源版本号 `?v=p2-17f-login-progress` → `?v=p2-17g-card-reset`。

#### 测试、安全复核与提交状态

- `card_record_service_tests` 新增三项：重置后卡上是逐字节精确的 marker、原两个密码都只得到
  状态拒绝（不是凭据答案）、卡可再次初始化；非 initialized 状态拒绝且 `put_calls == 0`；
  🔴 写入未落卡时必须报错且记录不变——否则用户会以为密钥已销毁而其实还在。
- `card_product_ui_session_controller_tests` 新增：已挂载时拒绝且 `entry.reset_calls == 0`；
  成功后密钥释放、日志清空、S 清除、路由不再是 authenticated、再次调用被拒。
- 页面契约新增：危险区元素齐全、三句代价文案在位、`resetAcknowledge.checked` 与
  `bridge.resetCardRecord()` 都在。
- 全量 CTest 44/45（唯一失败为需实体卡的 `vikey_helper_process_tests`）；
  WebView2 集成测试退出 0。
- 安全复核：重置只在已认证态可达；写卡前强制无挂载；写后复读校验；marker 与出厂产物不可区分；
  会话密钥、操作日志与 S 全部释放；插入被退休，不会继续在错误路由上操作。
  桥方法不接受任何参数，页面无法影响写入内容。
- 🟡 备选方案（未采用，成本更低）：重置后不关应用，改为提示拔出并重新插卡，
  由现有 `CardDeviceLifecycleController` 到达路径重新定路由，可省掉 `IUiCloseRequestSink`
  这一整层。已向用户说明，用户维持关闭应用的方案。
- ⚠️ 构建期两次 `LNK1168`：开发 EXE 正在运行导致无法写入，与改动无关。
- 提交：`ca59fdc [P2.17]reset the security card from settings`。

### 小切片 17h：保持一次性插卡上下文并恢复重置可达性

#### 提交后审计与修正决策

- 17g 已提交为 `ca59fdc`。提交后的真实组合审计发现：P2.14c 在初始化或登录成功前会按设计撤销
  一次性插卡上下文，因此已认证页面虽然显示重置入口，原实现仍要求 `login_required` route 与旧
  context，生产组合中会得到 `invalid_operation`。同一问题也使初始化后立即设置第二密码不可达。
- 一份未提交的候选改动尝试在初始化和登录成功后重新绑定 context。该方案未采用：它会破坏
  P2.14c 已验证的边界——成功处理器收到 S 与登录输入前必须先撤销插卡上下文，不能为了后续写卡
  复活或长期保留旧硬件／介质观察。
- 最终方案是写卡动作自己执行一次新的只读 `inspect_inserted_card()`，只接受当下仍路由为
  `initialized_v1` 的实体卡，并从该新上下文构造与同一硬件 ID 绑定的短生命周期 update target。
  成功和任何失败都会销毁该新上下文；登录／初始化的一次性语义保持不变。
- 破坏性重置额外接收 `IVolatileCardAuthenticationSession` 作为显式授权门。未认证调用在加载
  凭据或访问适配器之前拒绝；产品 UI 只有认证态返回可用，且仍先卸载全部卷，未卸载干净则不写卡。

#### 实现与验证

- `CardEntryContextController::set_second_card_password()` 与 `reset_card_record()` 改为新鲜探测、
  固定路由检查、创建 update target、执行记录事务后立即退休；控制器不再依赖已经消费的 route/context。
- UI controller 新增窄能力查询 `can_reset_card_record()`，COM bridge 只返回布尔值；设置页据此禁用
  不可达按钮并显示明确原因。重置失败文案不再声称“安全卡未被改动”，因为写入阶段异常无法作此保证。
- 单元测试覆盖：初始化后旧上下文已销毁但第二密码写入可通过第二次探测完成；登录成功同样不保留
  context；未认证重置零适配器访问；认证重置只探测／写入一次并留下精确 provisioning marker；
  UI、COM 和 WebView fake 均验证认证授权参数。
- Debug 全量 CTest **45/45**：43 项实际通过，2 项按环境限制跳过；受影响 6 项在 Debug、
  MSVC `/analyze`、ASan 三套均通过。三套 WebView2 集成程序均退出 0；`/analyze` 无项目诊断，
  ASan 未报告内存错误；两个 JS 文件 `node --check` 通过；`git diff --check` 通过。
- 安全复核：未保存或复活旧插卡上下文；新探测只在用户确认写动作时发生并在动作末尾销毁；
  update target 仍绑定探测所得的非零硬件 ID；未新增密码、S、硬件标识或记录原文的 UI 暴露／日志；
  P2.14c 的“成功交接前撤销上下文”及 R-03 / 8.6 均保持不变。
- 提交：用户已批准；本节随 `5aed730 [P2.17]reinspect the card before record writes` 提交，未推送。

### 小切片 18a：ViKey 开发凭据配置程序

#### 需求与既有实现审计

- 用户要求把手工打开 Windows 凭据管理器的流程收敛成一个最小配置程序，供开发测试包录入
  UserSN、API Key 与用户 PIN。本切片只配置**当前 Windows 用户、当前机器**上的开发凭据，
  不访问安全卡、不启动 helper、不加载 `ViKey.dll`，也不把它扩展为生产凭据部署方案。
- 复核既有 `WindowsCredentialProvider`、`VikeySession` 和小切片 11 的实体卡结论后，沿用三个固定
  Generic Credential 目标：`usb-vault-vikey-dev-user-sn`、`usb-vault-vikey-dev-api-key`、
  `usb-vault-vikey-dev-user-pin`。输入规则与真实会话保持一致：UserSN 为 32 个十六进制 ASCII，
  API Key 为 128 个十六进制 ASCII，用户 PIN 为 8 个字母或数字。
- 工具仍为 `EXCLUDE_FROM_ALL` 的开发目标；普通产品构建图与正式 helper 不因此获得写凭据能力。

#### 实现

- 新增 Win32 GUI `usb_vault_vikey_development_credentials_config`：三个 `ES_PASSWORD` 遮蔽输入框，
  输入长度由控件限制，全部长度与字符集合法前保存按钮保持禁用；窗口不提供命令行秘密参数、显示
  开关、剪贴板日志或自动读取／回显现有值。
- 页面文本明确“仅限开发卡和非生产材料”。保存时把三个 UTF-16 控件值逐项转换进锁页
  `SecureBuffer`，拒绝非 ASCII，然后调用共享验证器；中间宽字符缓冲与 ASCII 缓冲均由析构清零。
- `store_credentials()` 在第一次 `CredWriteW` 之前验证完整三项，以 `CRED_TYPE_GENERIC`、
  `CRED_PERSIST_LOCAL_MACHINE` 写入固定目标；随后复用正式读取器复读三项并逐字节比对。系统返回的
  Credential Blob 仍在 `CredFree` 前由既有 RAII 清零。程序以 `asInvoker` 运行，不要求管理员权限。
- 写入 API 没有多凭据事务：若 Windows 在第二或第三项失败，可能留下已更新的前缀。失败文案不会
  宣称未修改，而是要求修正后重新保存**全部三项**；成功才清空三个控件并报告已复读验证。

#### 验证、安全复核与遗留

- 扩展 `vikey_development_credentials_tests`，覆盖 UserSN/API Key/PIN 的合法字符集、错误长度、
  非法字符与未知字段。Debug、MSVC `/analyze`、ASan 三套目标均构建成功且该测试均通过；
  `/analyze` 无项目诊断，ASan 未报告内存错误。
- Debug GUI 做了两次“空输入、隐藏启动、确认消息循环存活、关闭指定新进程”的烟测，均通过且没有
  调用 `CredWriteW`。依赖表只有 `ADVAPI32`、`KERNEL32`、`USER32`、`GDI32` 与 MSVC Debug 运行库，
  没有 ViKey、网络、Shell 或提权依赖；`git diff --check` 通过。
- 安全复核：凭据值不会进入仓库、命令行、日志或错误文案；C++ 侧临时副本使用锁页内存并清零。
  但 Win32 Edit 控件自身仍会在进程普通内存持有输入，且当前用户上下文中的其他程序可读取 Generic
  Credential，因此此工具只能承载当前开发卡的非生产材料，不能作为量产秘密管理结论。
- ⚠️ 未执行真实三项写入验收，以免覆盖开发机现有可用凭据；用户可在审阅后用真实值执行一次，
  再启动既有 P2 开发程序完成闭环。当前生成的是 Debug EXE，仍依赖 Debug VC 运行库；给其他机器
  下载的 Release／自包含打包属于下一独立切片。
- 提交：`10ce729 [P2.18]add the ViKey credential configurator`。

### 小切片 18b：P2 内部开发测试包

#### 范围与交付边界审计

- 18a 已按用户批准提交为 `10ce729`。用户随后要求制作可供其他人下载测试的包。本切片先交付
  **Windows 11 x64 内部开发测试包**，不是公开发布或生产安装程序：主程序仍是联网开发模式，
  没有代码签名，VeraCrypt 驱动与 WebView2 Runtime 仍是机器前置条件。
- ViKey SDK 根目录及 DLL 示例目录没有发现明确的再分发许可证；patched VeraCrypt FormatDLL 的
  工程记录也一直要求最终法律复核与 source-offer 处理。因此预览包可以用于受控内部评估，但在
  供应商书面授权和 VeraCrypt 分发复核完成前，不能称为允许公开发布的客户下载包。
- 挂载实现直接访问已加载的 VeraCrypt `0x126` 驱动，不自行注册或启动内核驱动。应用户要求，
  本包改为附带上游原版 VeraCrypt 1.26.29 安装程序供测试者显式安装；WebView2 使用静态 Loader，
  但实际 Runtime 仍不随包复制，Windows 11 通常已有。

#### 构建与打包实现

- `package-release` CMake preset 固定 Release、`BUILD_TESTING=OFF` 与静态 MSVC Runtime
  (`MultiThreaded`)；新增显式聚合目标，只构建 P2 主程序、凭据配置器、ViKey helper、建卷 helper
  及它们的正式依赖，不把测试、PDB、LIB、ILK 或任意扫描到的 DLL 混入包。
- 新增 `package-p2-development.ps1`：只接受普通非 reparse 文件／目录；强制 package-release
  cache 形态；固定拒绝非白名单 ViKey x64 DLL 与非审计 FormatDLL；要求部署 HTML/JS 与源码摘要
  一致，并验证 VeraCrypt 源码基线为 `b48e31f5...` 后复制上游 License/NOTICE、项目补丁和说明。
  后续纳入的 VeraCrypt 安装程序同时固定 SHA-256、产品版本／原文件名及 IDRIX Authenticode 签名
  证书指纹，暂存时统一命名为 `prerequisites\VeraCrypt Setup 1.26.29.exe`，不会自动执行。
- 输出目录名只接受窄 ASCII 集并显式拒绝 `.`／`..`，规范化后再次验证父目录；既有输出一律拒绝，
  不删除或覆盖。暂存只复制固定七个构建产物及一个供应商 DLL，生成源码／第三方／helper 身份记录、
  全文件 `SHA256SUMS.txt`，再压缩为 ZIP。
- 双击入口 `Start USB Vault.cmd` 以 `%~dp0` 生成可移动的绝对路径；启动前用 Windows `certutil`
  复核 helper、VeraCryptFormat.dll 与 ViKey.dll 的固定 SHA-256，再把 helper／vendor 摘要作为既有
  P2 开发身份参数传给主程序。命令行仍不包含 UserSN、API Key、PIN、S 或卷密码。
- 包含简体中文使用说明、根目录短版 `测试须知.txt` 与第三方 notice。短版便条列出卡／
  开发凭据等条件、安装顺序、VeraCrypt 摘要与签名验证、`sc.exe query veracrypt` 驱动状态验证，
  以及 WebView2 注册表版本检查和微软官方下载入口。流程是同一 Windows 用户先安装并核验驱动，
  再运行凭据配置器，插卡后双击启动脚本；建卷时只有 `usb_vault_volume_creator.exe` 触发 UAC。

#### 验证、安全复核与预览产物

- 全新 `package-release` 配置与 80 个编译／链接步骤通过。四个 EXE 的导入表都没有
  `MSVCP140`／`VCRUNTIME140`／UCRT 调试 DLL，因而测试机无需另装 VC++ Redistributable；主程序
  仍只依赖 Windows 系统库，未静态导入 ViKey.dll。
- 嵌入 manifest 复核：主程序、凭据配置器、ViKey helper 均为 `asInvoker`；仅建卷 helper 为
  `requireAdministrator`。Debug build、错误的 x86 ViKey.dll、输出碰撞和 `..` 包名均按预期在
  创建包前／覆盖前失败。
- 首次含中文便条的 `preview3` 在解压校验时暴露了清单编码缺陷：文件本身存在，但 ASCII
  `SHA256SUMS.txt` 把文件名写成 `?????.txt`，逐项验证按预期失败。构建身份与全文件清单均改用
  UTF-8 后生成 `preview4`；新目录解压后的 19 个清单项全部复核通过，中文便条路径保持完整，且未
  发现 PDB/ILK/LIB/EXP/OBJ。部署 HTML/JS 与 `ui/` 源文件逐字一致；失败的 `preview3` 目录、ZIP
  和验证展开目录随后均已删除，避免误交付。
- 用户随后把根目录短版便条定名为 `测试须知.txt` 并给出完整正文；消息显示所需的 Markdown 转义
  在纯文本文件中还原为真实文件名／命令字符，同时修正注册表路径中 PowerShell 变量前不可缺少的
  `\` 分隔符。`preview5` 解压后确认新文件存在、旧名不存在、正文与源文件逐字一致，19 项摘要再次
  全部通过；已移除被替代的 `preview4` 目录、ZIP 和验证展开目录。
- 从全新解压目录分别隐藏启动 Release 凭据配置器与真实 P2 主程序：两者均稳定进入消息循环；测试
  随后只关闭其新建进程，未录入凭据、登录、写卡或建卷。启动脚本另在临时副本中用无写入配置器
  代替主程序，实际通过三项 `certutil` 校验和相对路径启动；产生的唯一进程已按核对后的路径关闭。
- 本机 `spike/_setup/VeraCrypt_Setup_1.26.29.exe` 与安装目录留存副本均为 40,832,408 字节，产品版本
  1.26.29，SHA-256 `DCA9DDEC...ECD3964B` 与上游发布清单吻合；签名状态 `Valid`，签名者
  IDRIX SARL，证书指纹 `86E1D426...B29B5F`。打包使用前者作为只读输入，包内复核结果相同。
- 当前预览产物：`out/packages/usb-vault-p2-development-test-0.2.0-preview5.zip`，大小
  30,600,268 字节，ZIP SHA-256 为
  `712F666D04B67343FE21DDE8CE9D89C76AEF15707E5B58979BA23F44F629908B`。
- 安全复核：包内没有凭据或卡／容器材料，启动参数只有路径与摘要；供应商 DLL 固定为已验证 x64
  摘要，FormatDLL 固定为实体建卷验收过的补丁摘要；上游安装程序须同时通过固定摘要、版本元数据
  与签名者指纹才会暂存，且只由测试者明确选择后通过 UAC 运行。打包脚本不递归收集产物且不覆盖
  既有目录。错误安装程序在创建输出前被拒绝且未遗留目录。全量 Debug CTest 45/45，其中 43 项
  实际通过、2 项实体卡环境限制跳过；package-release 聚合目标复建通过；PowerShell 语法解析及
  `git diff --check` 通过。
- 风险仍为：USB Vault 自身的 CMD/HTML/EXE 均尚未签名，摘要不能建立整包发布者身份；附带上游
  安装程序扩大了下载体积与版本过期面，且 VeraCrypt 聚合分发／source-offer 和 ViKey DLL 再分发
  授权仍待正式法律复核。因此只允许受控内部测试，不能发布为客户正式安装包。
- ⚠️ 当前 preview5 的 `BUILD-IDENTITY.txt` 诚实记录为 `fd6078d + dirty working tree`（该哈希已随提交信息重写失效，现对应 `10ce729`），因为打包链
  本身尚未提交。若用户批准该切片，应在提交后从 clean tree 重新生成最终预览名与摘要，再交付下载；
  现阶段不把 preview5 描述为最终客户包。
- 提交：`4c49834 [P2.18]add the internal development test package`，未推送。

### 小切片 18c：提交信息格式统一

#### 需求与范围审计

- 用户发现 `ce7390ba` 正文里的 `\n\n` 是字面量而非换行（PowerShell 下 `git commit -m` 不转义），
  并以 `fab54a4` 为范本定下提交信息规范：正文每段一个物理长行、段落之间真空行、
  50–80 个英文单词、固定两段（一段说改了什么，一段说验证结果）。
- 审计 `022fcc4..HEAD` 共 20 笔：2 笔已完全合规、1 笔仅字面量 `\n` 需修、
  10 笔正文不足 50 词需扩写（`022fcc4` 完全没有正文）、6 笔超 80 词需压缩，
  另 1 笔为本轮新提交的 18b。

#### 实现与核对

- 先提交 18b 打包切片（`4c49834`，80 词），再用 `git filter-branch --msg-filter` 一次改写 20 笔。
- 扩写素材全部取自本文件各切片记录，不从 diff 反推；压缩时保留有安全含义的事实
  （不泄露登录身份、写后回读、挂载时拒绝重置、先写卡再建容器的顺序理由），
  砍掉的是设计论证与逐条测试清单。
- 改写后逐笔核对：20 笔的树哈希与作者／提交时间戳与改写前完全一致，
  正文词数落在 62–80，零字面量 `\n`，无手动折行段落。
- 本文件 24 行共 25 处失效短哈希按 old→new 映射重写，9 处 `待定`／`待用户审阅决定`
  占位按规则 6 补齐为 `- 提交：`<hash> <subject>``。
- ⚠️ preview5 的 `BUILD-IDENTITY.txt` 仍写着改写前的 `fd6078d`，该对象已不可达；
  已在 18b 记录中注明现对应 `10ce729`，从 clean tree 重新出包时自然消除。
- 改写前已打备份 ref `backup-before-msg-rewrite`（指向改写前的 `fd6078d`），确认无误后可删除。
- 改写范围全部未推送（`origin/main` 停在 `7956356`，本地领先 131 笔），无需 force-push。
- 提交：`895bc27 [docs]unify the commit message format`。

### 小切片 19a：B 类容器改为用户自选路径且不限个数

#### 需求审计

- 用户要求把 B 类容器从「固定 `container_b.hc`、全机只能建一个」改成与 A 类一致：
  用户自选路径、创建个数不设上限。
- 需求源头是 `需求梳理清单_v2.md` 8.7.1：「容器数量不设上限，用户自选路径……A 类容器为单个
  普通卷，B 类容器为外层卷 + 隐藏卷，两类均可建多个」，以及同节已定案的
  「所有 B 类容器共用同一对密码」「N 个容器的 KDF salt 共用一个」。
- 小切片 16k 当初把 B 钉死在固定路径，是明确记录在案的最小切片取舍，本切片正是来还这笔债的。
- 8.7.2「入口恒显、流程逐字对称」与 8.7.3「第二个密码不得验证」是本切片的硬约束：
  建 B 的界面与提示不得因登录身份或已建过几个而改变，第二个密码仍只做两遍确认、不判对错。

#### 实现

- `create_container_b` 在会话层、控制器层与 COM 层统一加上路径参数，路径由原生
  `IContainerFileChooser::choose_new_container()` 提供，页面既不给路径也不收路径，
  与 A 类的建卷完全同构；用户取消选择时返回 `false`，页面按「已取消创建」处理。
- 控制器把顺序定为：校验形态 → 取会话口令 → 两遍确认第二个密码 → 弹选择框 →
  改写卡记录 → 建卷。取消发生在改写卡记录之前，因此取消不会动卡。
- 会话状态里的 `container_b_password` 改名为 `hidden_container_password`，
  语义从「那一个 B 的密码」变成「所有隐藏卷容器共用的卷密码」，并改为**首次使用时才派生**：
  容器可以在任意路径，登录时无从判断是否存在，不应为可能用不到的派生付出一次 Argon2id。
  会话本来就为改写卡记录保留着登录口令明文，惰性派生不新增任何驻留秘密。
- `mount_container`（页面上的「导入」）去掉了「拒绝固定 B 路径」的分支，改为
  **先试标准派生、驱动回 `authentication_failed` 时再试登录派生**；成功时按实际打开的
  那一侧记录 `release_first`，之后在同一会话里重新挂载不再重复探测。
  没有这一步，改为自选路径后跨会话建出的 B 容器将永远无法再挂载。
- `ContainerLayout::container_b` 保留未动：其 salt 仍是 8.7.1 定案的编译期共用常量，
  其固定路径退化为「文件恰好在那里时登录顺带接管」的遗留分支，与 A 的固定路径对称。

#### 测试

- 会话层新增：多容器（两个不同路径、各自盘符、第二次只派生诱饵那一侧、同路径仍拒绝重复接管）、
  导入回退到登录派生并记住卷类别、两种派生都打不开时如实报错；
  原「导入拒绝固定 B 路径」一例被上述两例取代。登录不再预派生，相关事件序列同步更新。
- 控制器层新增：取消选择返回 `false` 且不碰卡与卷、创建落在选中路径、第二个容器照样通过。
- COM 层新增：`createContainerBMib` 现在回传布尔值，取消时为 `VARIANT_FALSE`，两条密码数组
  在取消路径上同样被清零。
- 页面契约测试改为断言四参数桥调用只出现一次并且结果被使用，另加取消分支的文案与判空。
- 真实 WebView2 端到端夹具：诱饵容器现在走第 4 次选择框并落到与标准容器不同的路径，
  操作日志断言随之从 `hidden.hc` 改为 `handover.hc`。
- 全量 Debug CTest **45/45 全通过**；`card_product_webview_integration_tests` 单独构建后
  实跑退出码 0。

#### 安全复核

- 新的回退路径只用会话已持有的两种派生结果依次尝试，不会向用户追问、不会提示"这是哪一类容器"，
  失败信息不区分「密码不对」与「不是我们的容器」，符合 8.4 的 UI 不可区分要求。
- 跨边界的数据没有变化：仍只有 id／文件名／容量／盘符，路径只在原生侧存在，页面拿不到。
- 惰性派生使「登录后立刻被拔卡」的窗口内少一份派生密钥；`release_session_secrets` 与
  `force_unmount_active` 仍同时清掉登录口令与派生结果。
- 🔴 **遗留风险（8.7.6，本切片未解决且因多容器而放大）**：卡记录恒定两个信封，每建一个 B
  都会用当次输入的诱饵密码改写主信封。放开个数后，若用户第二次输入了**不同的**诱饵密码，
  先前所有 B 的外层卷将不可再打开，且按 8.7.3 产品不得校验、检测不出来。
  现阶段只由页面上那句固定文案「若您曾设置过诱饵密码，此处需与曾经的诱饵密码保持一致」承担，
  8.7.6 的两个候选方案仍待客户拍板。
- 提交：`5f7f7bf`（`[P2.19]create hidden containers at user-chosen paths`）。

### 小切片 19b：容量列改为报告已挂载的卷容量

#### 需求审计

- 用户在实机验证 19a 时发现：总容量 128 MiB、隐藏卷 96 MiB 的 B 类容器，无论挂的是隐藏卷
  还是外层卷，app 的「容量」都显示 128 MiB，而资源管理器对隐藏卷显示 96 MiB。
- 排查结论：那一列读的是**容器文件大小**（`container_size_if_present`），不是卷容量。
  文件恒为 128 MiB，所以它永远显示 128，与当前挂的是哪一侧无关——不是外层卷算错了。
- 外层卷显示 128 本身是对的：按 8.4／8.6，外层卷本来就横跨整个容器；若外层改显示
  「总容量 − 隐藏卷」，这个差值本身就证明了隐藏卷存在。
- 用户据此定案：把「容量」换成挂载后的卷容量，未挂载时与「盘符」一样显示 `—`。

#### 实现

- 会话运行时接口新增 `mounted_volume_size(wchar_t)`：按盘符在驱动的 `MountedVolumes`
  里取 `disk_length`，驱动没有该盘符时返回空。文件回答不了这个问题——隐藏卷小于宿主容器，
  标准卷也比文件小一个头部组。
- `container_statuses()` 改写：未挂载一律 `size_bytes = 0`（容器持有两个卷时，究竟报哪一个
  要到挂载才定），已挂载一律问驱动；驱动说盘符上没有卷则抛错，不退回文件大小。
- `displayable_size_mib` 改为**四舍五入到整 MiB**：卷容量从不是 MiB 的整数倍，
  逐字节精确校验会把这一列清空；超出建卷区间仍按不可用（0）处理。
- 控制器校验收紧：容量与盘符现在共用一条规则——都描述已挂载的卷，因此二者恰好在挂载时存在。
- 页面 `容量` 事实项与 `盘符` 写成同一形状：挂载才渲染数字，否则 `—`。

#### 测试

- 会话层新增：128/96 的隐藏卷容器与 64 的标准容器同时在挂，断言分别报 96 与 64
  （卷容量而非文件大小）；卸载后报 0；驱动丢掉盘符时 `container_statuses()` 抛
  `std::runtime_error`。测试替身据此改为按盘符维护一张卷容量表，挂载写入、卸载抹除。
- 控制器层新增：卷容量比整 MiB 少 128 KiB 时仍显示 64 MiB（四舍五入），
  超出建卷上限时显示 0（不可用）。
- 原先两例断言「未挂载容器仍报文件大小」，按本切片改为断言 0。
- 全量 Debug CTest **45/45 全通过**；`card_product_webview_integration_tests` 实跑退出码 0。

#### 安全复核

- 跨边界字段没有增加：仍只有 id／文件名／容量／盘符，容量的**来源**从文件换成驱动，
  页面拿到的形状不变，路径依旧只在原生侧。
- 未挂载时不再公布容器文件大小。对 B 类容器而言文件大小就是「总容量」，
  先前那一列等于在任何时刻都无条件公布外层卷的真实上限，现在只有挂上才有数。
- 已挂载时真实密码下显示 96、诱饵密码下显示 128，这一差异只对已经通过认证的那一方可见，
  且与资源管理器早已显示的口径一致，未引入新的可观测差异；两类容器的显示规则完全相同，
  观察者无法从这一列区分 A 与 B。
- 驱动答不上来时抛错而不是回落到文件大小，避免出现「看起来像卷容量、其实是文件大小」的静默错值。
- 提交：`11b0780 [P2.19]report the capacity of the attached volume`。
### 小切片 20：建卷位置改由页面填写名称与路径

#### 需求审计

- 用户要求把建卷时的原生保存对话框换成 `usb-vault_ui-design.html` 里的表单形态：
  在应用内填「名称」与「路径」，右侧「浏览」按钮打开原生选择框只负责填充字段。
- 设计稿该屏（`create · standard` / `create · hidden`）本身**保留**了「浏览」按钮，
  所以本切片不是「去掉原生选择框」，而是**把路径字符串的归属权从原生侧移到页面侧**。
- 🔴 硬约束冲突：`include/usb_vault/ui/container_file_chooser.h` 自 P2.15b 起明确写着
  「页面既不提供也不接收路径」。本切片要推翻它，因此代价必须显式补偿，不能默默改掉。
- 设计稿「名称=工作资料 / 路径=work.vc」那种**独立于文件名的显示标签未采纳**：
  它需要 8.7.4 的加密路径列表（存于容器 A 内）才有地方存放，而该列表尚未实现，
  且依赖仍未拍板的 8.7.6。经与用户确认，本切片取「名称即文件名」。

#### 实现

- 新增 `include/usb_vault/ui/container_target_path.h` 与 `src/ui/container_target_path.cpp`：
  纯字符串的 `resolve_container_target_path(directory, name)`，把保存对话框原先在结构上
  给出的保证逐条写成显式检查——只收盘符绝对路径（拒 UNC、`\\?\`、`\\.\` 与一切相对形式），
  拒 `.`／`..` 层级、空层级、以空格或点号收尾的层级、保留设备名（含带扩展名形式）、
  控制字符、名称中的分隔符与通配符，整条路径限 259 个 UTF-16 码元（下游走无 `\\?\` 前缀的
  Win32 调用）。扩展名 `.hc` 由本单元追加，页面无权决定文件如何被识别。
- `IContainerFileChooser::choose_new_container()` 改为 `choose_container_directory()`，
  只服务「浏览」按钮；`choose_existing_container()`（导入）语义不变，路径仍不跨边界。
- 控制器：`create_container(name, directory, size_mib)` 与
  `create_container_b(name, directory, ...)`，返回值由 `bool` 改为 `void`——
  字段在点「创建容器」前已确定，**「取消建卷」这一结果不复存在**。
  目录必须已存在，只探测不代建：代建目录本身就是 8.4 禁止的本地产物。
- COM 层：`createContainerMib(名称, 路径, 容量)`、
  `createContainerBMib(名称, 路径, 总容量, 内层容量, 诱饵密码×2)`，新增
  `chooseContainerDirectory()`。两个字符串在调用返回前就地清零（`LocationArgumentsWiper`）。
- 页面：两个表单各加「名称」「路径」「浏览」，并有一份与原生同规则的提前拒绝用于逐字段提示；
  名称与路径在交给桥的同刻从 DOM 清空。原生侧仍是唯一裁决。
- 按 8.7.2 顺带修正一处既有不对称：建卷流程预览原先只对标准容器显示，
  理由注释写的是「隐藏容器用固定路径」——该前提自 P2.19a 起已不成立。
  后经用户要求，该预览与参数屏描述文案一并删除。

#### 测试

- 新增 `container_target_path_tests`：接受形态（含中文名、已带 `.hc`、其它扩展名、
  `console`／`COM0` 这类仅形似设备名者）与拒绝形态（UNC、`\\?\`、`\\.\`、相对路径、
  `..` 穿越、空层级、空格点号收尾、CON/NUL/COM1/LPT9、控制字符、超长路径）逐条覆盖。
- 控制器层：非法名称与非法路径在**触达会话之前**即被拒；不存在的目录同样按字段拒绝；
  B 类容器的路径校验发生在改写卡记录与消费诱饵密码**之前**，且拒绝时两个密码缓冲仍被擦除。
- COM 层：六参调用逐位断言，两个密码数组与两个字符串均在调用返回后为零。
- 另用 Node 把页面侧的两个校验函数抽出来，与原生单元**同一批用例**对跑，逐条一致——
  该步骤查出页面正则里的反斜杠转义写错（`/[\/]/` 只匹配正斜杠、`D:\Vault` 提示渲染成 `D:Vault`）。
- 全量 Debug CTest **45/46**；未通过的 `vikey_helper_process_tests` 需插实体卡
  （报 `card communication failed`），与本切片无代码重叠。
- `card_product_webview_integration_tests` 实跑退出码 0：驱动脚本覆盖表单开局为空且不可提交、
  「上一步」连路径一起清空、非法名称与非法路径各自在自己字段下报错、「浏览」只填字段、
  失败后两个字段被清空且需重填。

#### 安全复核

- 🔴 **本切片确实扩大了泄露面**：页面提供的字符串现在会到达文件系统。补偿是
  `resolve_container_target_path`——保存对话框原先靠结构保证的每一条，现在都是显式检查，
  且没有比它更弱的路径可以绕过去。契约头文件已改写，不再声称页面不接触路径。
- **零痕迹义务未随路径转移**：浏览框仍不带 client GUID、仍带 `FOS_DONTADDTORECENT`；
  四个新输入框全部 `autocomplete="off"`；名称与路径不写本地／会话存储；
  跨边界 BSTR 就地清零；页面契约测试把「关闭浏览器辅助」的计数从 4 提到 8 并断言。
- **8.7.2 逐字对称**：两类容器拿到完全相同的两个字段、相同文案、相同校验规则，
  桥参数表保持平行，建卷流程预览不再按类型区分。
- 跨边界投影未增字段：`ContainerUiStatus` 仍只有 id／文件名／容量／盘符，
  「名称」是文件名而非新的可持久化标签，会话跨会话仍不持久化任何容器记录。
- 提交：`cdeb4b3 [P2.20]name and place new containers on the form`。

### 小切片 21：建卷内层容量下限在会话层收口

#### 需求审计

- 本切片来自 2026-09-06 关于容器路径记忆的讨论。讨论中曾把这条判成「唯一的下限检查只在
  页面侧、一个 0 会一路走到 FormatDLL」，**核对后不成立**：控制器层
  `card_product_ui_session_controller.cpp:227` 已经拒绝 `hidden_size_mib == 0`，
  页面那条路到不了 FormatDLL，P2.20 立的「原生侧是唯一裁决」没有被违反。
- 真正缺的是更深一层：`CardLoginMountSessionCore::create_container_b` 是应用层契约入口，
  测试与非 UI 调用方直接走它。它自己那段把该对尺寸的其余不变量都写全了——非零、512 对齐、
  隐藏卷小于外层——唯独漏了隐藏卷为 0。0 满足对齐、也不大于等于外层，于是穿过整段校验。
- 定性因此是**内层 API 契约补齐**，不是修一个可被利用的漏洞。

#### 实现

- `create_container_b` 的尺寸校验补 `hidden_size_bytes == 0`，异常文案相应改为
  「must be non-zero, aligned, and leave room for the outer volume」。
- 就近加注：这一条在 UI 层已经拒绝的前提下仍要写在这里，因为这里是应用层契约，
  且该对尺寸的其余不变量都陈述在同一个条件里。

#### 测试

- `card_login_mount_session_tests` 的无效尺寸对表补 `{1024, 0}`。原有四对覆盖不到它：
  `{0, 0}` 与 `{513, 0}` 都先被 outer 的非零／对齐检查拦下，`{1024, 513}` 与 `{1024, 1024}`
  的 hidden 非零。改动前该用例会失败（不抛异常），是真回归用例。
- 按本轮定的测试纪律**只构建并运行这一个目标**，实跑通过、退出码 0。
  全套留到 P2.27，本切片**未跑**其余目标、未构建 dev app、未实机验证。

#### 安全复核

- 无可观察行为变化：被新拒绝的输入在控制器层早已被拒，产品经由页面到达不了这个分支。
- 未触碰任何密钥派生、跨边界字段、落盘行为与卡记录路径；页面与控制器的既有下限一并未动。
- 不引入按容器类型区分的可观察差异：本改动只收紧 B 类会话入口对非法参数的拒绝，
  A 类与 B 类的合法路径完全不变，8.7.2 的逐字对称不受影响。
- 提交：`0b16dd8 [P2.21]refuse a zero-length hidden volume in the session layer`。

### 小切片 27：路径记忆收尾 —— 记住容器位置开关、全套测试、主文档回写

#### 需求审计

- P2.27 在 `MILESTONES.md` 里的定义是收尾三件事：全套测试、实机、主文档回写四项
  （8.7.4 改为逐条密封／8.7.6 选案／Q-04 澄清／新增「A 类容器之间无隔离」告知项）。
- 审计发现回写之前还缺一件东西：**8.7.4 自己要求的「记住容器位置」开关没有后端**。
  该开关是 P2.17a 留下的两个空开关之一（只有 UI、状态不持久，重开即回默认），
  而 8.7.4 的原文写的是「用户可关闭并改为每次手动选择」。不补上就会把一段描述产品里
  并不存在的行为写进主文档，故本片先补齐它再回写。
- 另查出一处文档欠账：`DEVELOPMENT_LOG.md` **缺 P2.22～P2.26 五片的条目**
  （AGENTS.md「开发记录」要求每个阶段都更新）。本片没有补写这五条 —— 补写要逐个复核
  已提交的差异，属独立工作量，作为遗留项报出，由用户决定是否补。
- 顺带修复 8.7.4 原文里被前一轮编辑吃掉的一行（「（部分带隐藏卷、部分是以真实密码建立的
  普通卷），此时诱饵密码无法挂出后者；」整行丢失，且多出两个空行），已按原文恢复。
- 🔴 **收尾跑到 `card_product_webview_integration_tests` 时查出一处 P2.26 的回归**，详见下方
  「测试」。该目标 `EXCLUDE_FROM_ALL` 且没有 `add_test`，必须点名构建、点名运行，
  于是 P2.22～P2.26 一路没跑到它。

#### 实现

**`include/usb_vault/application/product_settings_v1.h` / `src/application/product_settings_v1.cpp`（新增）**

- 位置 `%LOCALAPPDATA%\usb-vault\settings.txt`，内容是单一布尔量
  `remember-container-paths=0/1`，与容器索引分开存放 —— 索引必须与随机数不可区分，
  设置则是一个对档位不敏感的普通配置，两者放一起会给索引文件加上明文头。
- **缺文件答「开」，文件在却读不出答「关」**：前者是产品默认，第一次运行没有任何选择需要尊重；
  后者是曾经做过选择而现在不可读，唯一安全的读法是不写任何东西。两者刻意不同，用
  `GetFileAttributesW` 区分。超过 4 KiB 的文件按不可读处理 —— 不管它是什么，反正不是这个。
- 写入是临时文件 + `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)` 一步换上，
  读者不会读到写了一半的选择（半个文件会被判成损坏，而损坏在这里意味着停止记录路径）。

**会话层（`card_login_mount_session*`）**

- `CardLoginMountSessionRuntime` 加 `remember_container_paths()` / `set_remember_container_paths()`，
  生产实现接到上面那两个函数；`IAuthenticatedCardVolumeSession` 同步加公开的读写。
- `remember_container_path` 与 `adopt_remembered_containers` 在函数开头即按开关返回：
  关掉后**不写新路径、登录也不取回列表**。
- `set_remembering_container_paths` **不动索引**。关掉是「停止记录」不是「遗忘」，
  而且开关必须可逆 —— 一个悄悄销毁记录的开关，再打开也回不来。
- `forget_container`（从列表移除）**不受开关约束**：那是针对某一个容器的明确指令，不是策略。
  否则关掉开关的用户就再也清不掉开着时写下的记录。

**UI 与桥**

- `CardProductUiSessionController`：**读不挑路由**（设置页在登录前就要把开关显示在真实位置），
  **写要求已认证**（改设置和其他会话操作同级）。
- 桥新增 `getRememberContainerPaths`（DISPID 32，零参，返 `VT_BOOL`）与
  `setRememberContainerPaths`（DISPID 33，单参，**必须是 `VT_BOOL`**），与既有成员同形。
- 页面 `refreshRememberSwitch` 从原生读回位置并写 `aria-checked`；点击只发命令，随后**重读**，
  失败时保持原位并给出错误提示 —— 开关显示的永远是原生侧真实持有的值，不是刚才点了什么。
  另一个空开关「登录后自动挂载」保持页面本地，它要等 8.7.5 的卷数上限或重谈五秒预算。

**集成夹具页与它的护栏**

- `tests/card_product_webview_integration.html` 是集成测试自己的极简夹具页（176 行，只放
  产品脚本要查的那些 id），**不是产品页的拷贝**。P2.26 往产品页加了「从列表移除」弹窗，
  夹具页没跟着加，于是 `card_session.js` 启动时在 `forgetCloseButton.addEventListener` 上
  对 null 取属性抛异常 —— 页面再也没报出过状态，驱动第一步 `waitForState("no-card")` 超时。
  已按夹具页自己的极简风格补上那六个 id。
- **新增 `card_product_ui_page_contract_tests` 的一条护栏**：把脚本里每一处
  `document.getElementById("…")` 的 id 抽出来，逐个要求夹具页里有。它把一个「必须有人记得
  手动跑」的 GUI 目标的一半价值搬进了 CTest —— 那种「加了元素忘了同步夹具」的漏，从此在
  CTest 里就红。已反向验证：把夹具页里任一个 id 改掉，该用例立刻失败（退出码 1）。

**「从列表移除」确认弹窗文案**（用户定稿）

- 「文件仍在原处，随时可以用「导入」重新加回来」→「文件仍在原路径，可通过「导入」重新添加至
  列表」。页面契约钉的是那句**承诺**而不是那句话，断言随之从「重新加回来」改钉
  「重新添加至列表」；「不会删除容器文件」原样保留。

#### 测试

- **新建 `product_settings_v1_tests`**：默认为开、往返、关掉后重读仍是关、文件在却读不出答「关」、
  超大文件按不可读处理。
- **`card_login_mount_session_tests`**：关掉后新路径不写入、登录不取回列表、**已有记录不被擦除**、
  再打开原样回来；「从列表移除」在开关关闭时照样清得掉记录。
- **`card_product_ui_session_controller_tests`**：读在未认证下也答得出，写在未认证下抛
  `wrong_route` 且**一次都没触达会话**。
- **`card_product_webview_host_object_tests`（本片新增用例）**：布尔跨界往返、未认证下 `E_FAIL`
  且不触达会话、传数字被 `DISP_E_TYPEMISMATCH` 拒、参数个数不对被 `DISP_E_BADPARAMCOUNT` 拒。
  这一层此前只有假会话的桩，桥成员本身没有用例。
- **`card_product_ui_page_contract_tests`**：原断言「所有开关都不得触达原生」已随本片失效，
  改为逐个说清楚 —— 有 store 的那个必须过桥且从原生重读，没 store 的那个仍不得出现
  `autoMount` 字样；**浏览器存储的禁令对每个开关都不变**。
- **全套（本计划中唯一一次）**：Debug **50/50**、MSVC `/analyze` **50/50**（无项目诊断）、
  ASan **48/50**。ASan 的两处失败是 `vikey_helper_process_tests` 与
  `vikey_helper_hid_compatibility_tests` 下 helper 子进程 `0xC0000135`（DLL not found）的
  既有环境问题，与本片无关，Debug 与 `/analyze` 下这两项都过。
- 🔴 **`/analyze` 首次覆盖 P2.22～P2.26 的新测试文件，查出两处并已修** —— 这正是「每片只跑
  最小目标、全套留到收尾」这条纪律要在收尾这一次付的账：
  1. `container_index_v1_tests.cpp` 的格式常量对比触发 C6326（两个常量相比）。改为
     `static_assert`：它们本来就是编译期常量，钉在编译期比钉在用例里更强，漂了就不该链出
     一个能跑的用例。
  2. `card_login_mount_session_tests.cpp` 的多登录用例触发 C6262（单帧 19452 字节）。
     根因是一个会话核心把整份操作历史按值带在身上，两个并排就超了 `/analyze` 的 16 KiB。
     引入 `in_one_login(runtime, work)` 把每次登录关进自己的栈帧，四个多登录用例全部改走它，
     断言一条未改。
- **`card_product_webview_integration_tests`（点名构建、点名运行）**：修好夹具页后实跑
  **退出码 0**。驱动脚本新增覆盖「记住容器位置」：默认为开、点一下后**等**它变成关
  （这个开关的位置来自原生侧，比另外两个晚一拍才落定）、再点回开；原生侧断言这一程恰好
  两次写入且最终为开。
  🔴 **修复前它在第一步就失败（退出码 10 `fail-no-card`）**，且经 `git stash` 后用 HEAD
  重新构建复现，确认与本片改动无关，是 P2.26 留下的。定位过程：先把断言拆开确认不是
  `action.hidden`，再探到 `surface.dataset.busy` 卡在 `true` —— 页面初始化就抛了。
- `node --check ui/card_session.js`、`node --check tests/card_product_webview_integration.js`
  通过；`git diff --check` 通过。
- **未做：实机**。`usb_vault_vikey_p2_development_app` 已点名重建（`out\build\dev`，
  与 UI 资产同批刷新），真卡走查待用户执行。

#### 安全复核

- **settings.txt 对档位不敏感**：内容只有一个布尔量，档二档三逐字节相同，不记录任何路径、
  不记录容器是否存在、不记录哪个密码登录的。8.4 的逐字节比对不受影响。
- **关掉开关后索引文件不再被读写**，因此一台从未打开过记忆的机器上该文件根本不存在。
  这一可观察差异说的是「用户关了记忆」，而 settings.txt 本来就明说了这件事，**不携带档位**。
- **关闭不删除记录**是安全决策而非偷懒：删除不可逆，一次误点即永久丢失，而这一层没有钥匙，
  无从掂量正在丢的是什么（与 8.7.4「损坏即拒」同一条理由）。
- 新桥成员不接受也不返回路径、口令或档位，只有一个布尔量；写路径要求已认证，
  未认证调用在触达会话之前就被拒。
- 页面未新增任何浏览器存储，也不缓存开关状态：每次都从原生重读，失败就保持原位，
  不会出现「页面显示已保存、实际没保存」。
- 夹具页只是测试夹具，不参与产品打包：补进去的六个 id 不影响产品页，也没有放宽页面契约
  测试的任何一条既有断言（浏览器存储禁令、跨边界字段、行内按钮必须过确认流程均原样保留）。
- 主文档回写未放宽任何约束：8.7.6 仍标注「待客户选择」，本片只补上论据材料；
  Q-04 的边界写的是**导出路径与导出历史仍然一个字都不许写**，只澄清容器自身路径不在其列。
- 提交：`0427d77 [tests]pin the container index format at compile time`、
  `4c1c15c [P2.26]restore the elements the integration fixture page lost`、
  `145b63b [P2.27]remember whether to remember where containers are`、
  `2a9881c [ui]say where a dropped container file stays`；
  `4833456 [docs]write the container path memory decisions back`。

### 小切片 28：导入不再自动挂载，改由设置页的开关决定

#### 需求审计

- 需求来自用户：「导入容器默认不自动挂载，做成一个按钮开关在设置页『自动尝试挂载导入的容器』」。
- 与主文档核对，方向一致而非冲突：8.7.5 的默认就是"不自动挂载"，此前只有登录那条路遵守它，
  导入这条路仍然选中即挂。P2.25 之后更明显 —— 从索引读回的容器以未挂载身份进列表，
  而同一个文件走"导入"却会被打开，同一件事两种结果。
- 🔴 **审计出一处必须记明的代价**：不挂载就没有任何东西验证过这个文件确实是容器。
  导入因此不再当场报"打不开"，错误推迟到首次挂载。这与索引读回的条目本就一致，
  且好过"为了验证而擅自挂载"，已写进 8.7.5。

#### 实现

**设置层（`product_settings_v1`）**

- 同一个 `settings.txt` 里加第二个布尔量 `mount-imported-containers`。
- **两个键共用一个文件，所以写入必须是读—改—写**：`set_*` 各自把邻居的当前值原样写回，
  否则改一个就会把另一个悄悄打回默认值。
- 读法与邻居**故意不同**：「记住容器位置」区分"没有文件"（产品默认开）与"文件在但读不出"
  （答关）；这一个不需要区分 —— 产品默认已经是"什么都不打开"那一侧，所以没有文件、读不出、
  以及**旧版本写下的、根本没有这个键的文件**，三者答案相同，升级路径天然安全。

**会话层**

- 新增 `adopt_container(path)`：查存在 → 以未挂载身份进列表（`derivation_known=false`）→
  记路径。**不派生、不占盘符、不挂载**，也**不写会话日志** —— 没有卷被碰过，列表里多出的那一行
  就是事件本身，与登录时从索引读回的条目完全同路。
- 已在列表里的路径直接返回原 id，重复导入不会多出第二行。

**控制器与桥**

- 分流放在控制器：`mount_container()` 拿到路径后按开关走 `mount_container(path)` 或
  `adopt_container(path)`。会话层保持两个各自诚实的原语，不在里面藏策略。
- 桥新增 `getMountImportedContainers`（DISPID 34）与 `setMountImportedContainers`（35），
  与「记住容器位置」那对同形：零参返 `VT_BOOL`，单参且必须 `VT_BOOL`，写要求已认证。

**页面**

- 设置页第三个开关「自动尝试挂载导入的容器」，`aria-checked="false"` 出厂。
- 页面里"有 store 的开关"从写死一个改成**表驱动**（`storedSettings`），
  `refreshRememberSwitch` 相应变成 `refreshStoredSwitches`；点击仍然只发命令、随后重读，
  失败保持原位。没有 store 的开关照旧自己翻。
- 导入失败文案改为「无法导入该容器，请确认文件仍在原位置且与当前安全卡匹配。」——
  关闭开关时失败只可能是文件不在了，旧文案只提"与安全卡匹配"已不准确。

#### 测试

- `product_settings_v1_tests`：新设置默认关、旧文件（无该键）答关、往返；
  **另一条专钉读—改—写**：先关掉记住路径、再打开导入挂载，两个值互不冲掉。
- `card_login_mount_session_tests`：`adopt_container` 只产生 `exists:` 一个事件、
  条目未挂载且无盘符、路径进索引、**操作日志一条不增**，随后首次挂载走完整探测；
  另一条钉文件不存在时抛出且不留行、不写路径。
- `card_product_ui_session_controller_tests`：默认导入只走 `adopt`、开关打开后才走 `mount`、
  取消选择两条都不触达；开关读不挑路由、写只在已认证。
- `card_product_webview_host_object_tests`：布尔跨界、未认证 `E_FAIL` 且不触达会话、
  传数字与参数个数不对各自被拒。
- `card_product_ui_page_contract_tests`：开关计数从 3 改 4、`data-setting` 从 2 改 3，
  新开关必须 `aria-checked="false"` 出厂且两个方法都到桥；原「记住容器位置」那条契约
  随页面改为表驱动而改钉 `refreshStoredSwitches` 与 `stored.write`。
- `card_product_webview_integration_tests`：夹具页补第三个开关，驱动脚本覆盖它的默认与点击，
  原生侧断言恰好一次写入且最终为开。实跑退出码 0。
- **两处既有断言被本片改动打破，已按新事实更新**（不是放宽）：控制器里"已挂载的不许移除"
  那条用例依赖"导入即挂载"，现显式先打开开关；页面契约的开关计数同理。
- 按用户要求**只跑最小目标**：上述七个目标全部通过。**未跑**其余测试、未跑 `/analyze` 与 ASan、
  未实机。`node --check` 两个 JS 文件通过。

#### 安全复核

- `settings.txt` 仍只有布尔量，档二档三逐字节相同；新键不记录任何路径、容器或档位信息，
  8.4 不受影响。
- **默认值方向是收紧而非放宽**：改动后产品在用户没有明确要求时**更少**打开卷，
  与 8.7.5 的默认、以及 8.7.4 "读回的条目不自动挂"一致。
- `adopt_container` 不派生任何密钥、不占盘符、不接触卡记录；它写进索引的仍然只有路径，
  仍不记录"哪个密码开得了它"。
- 导入不再验证容器有效性这一点已在 8.7.5 写明，不是被遗漏的行为差异。
- 新桥成员只收发一个布尔量，写路径要求已认证；页面未新增任何浏览器存储。
- 提交：`9a5d197 [P2.28]let an import stop at the list`。

### 决策记录：8.4 本地状态层的边界（2026-09-07）

本条不是代码切片，是一次范围决定的留档。起因是讨论"下一步做导出还是做应用内文件管理器"时，
我引用 8.7.5 的原话说文件管理器"会新增『最近打开』一类本地状态泄露面"，被用户追问机制。

#### 结论

- **原理由站不住**：那个列表是我们自己写的，不写即可。松散地把它当作反对文件管理器的论据，
  实际上掩盖了真正的三处泄露。
- **真实机制只有三处**：① 向 shell 索取图标与缩略图会进 `thumbcache_*.db`，且**泄露的是文件
  内容派生物而非路径**，卷卸载后仍在；② "打开"必然让第三方程序把 `X:\...` 记进自己的 MRU；
  ③ 复用 shell 组件时其默认 MRU 行为须逐个显式关闭（P2.20 已有先例）。
- **用户决定：①②明示接受**。据此在 8.4 增设「本层的边界」：本层只约束**本产品自己写下的**
  产物；系统与第三方程序因用户使用已挂载卷而写下的痕迹不计入，**包括由我方界面触发、
  但由 Windows 写下的情形**（这正是文件管理器能用 shell 组件的前提）。
- 🔴 **豁免不向我方产物延伸**：我们自己的最近打开列表、上次目录、排序方式仍一律禁止。

#### 为什么必须写进文档

`MILESTONES.md` 的 P3 验收行原文是"逐字节比对……**全部本地产物**，差异必须为零或与档位无关"，
**不分作者**。缩略图缓存与档位有关（档二不会有），照原文这条验收必然失败。不写清楚的后果很具体：
P3 那轮比对会跑出一堆已被接受的差异，做的人分不出哪些是真问题。已同步收窄该行的比对范围。

#### 改动

- `需求梳理清单_v2.md` 8.4：新增「本层的边界」小节（三类豁免、触发方与写入方的区分、
  对我方产物不放宽的红线、以及与 Q-04 同型的书面确认留档提醒）。
- `需求梳理清单_v2.md` 8.7.5：修正文件管理器降级理由，明记原第二条理由作废，
  降级理由只剩工作量与"资源管理器已够用"；并记下该功能因此比原估计便宜。
- `MILESTONES.md`：P3 验收行同步豁免；第六节范围演进史新增 2026-09-07 一行。

#### 尚未做

该决定目前只是项目内决定，**未取得客户书面确认**。8.4 新小节里已按 Q-04 的先例写下留档提醒。

- 提交：`b061989 [docs]bound the local state layer to what this product writes`。

---

### 决策记录：新设 P3「数据管理」，原 P3 顺推为 P4（2026-09-08）

用户要求开工侧栏那个「文件管理」入口。它在页面上一直是 `nav-item-disabled` 占位
（`ui/card_session.html:604`），而 8.7.5 把应用内文件增删改移降级为 v1.1，v1 只留只读状态展示。
把它做出来是范围变更，已明示并由用户拍板提前。

#### 为什么连「备份导出」一起挪

排切片时发现两件事扣在一起：

1. **P2 的出口条件里写着「备份导出全部可用」，而它一行代码都没有** —— `src/` 下搜不到任何导出
   或迁移实现，页面侧栏的「备份中心」和「文件管理」一样是禁用占位。P2 欠着这笔账。
2. **把文件拖出加密卷就是明文导出**，Q-04 的三条可验收项当场生效（不可跳过的风险确认屏、
   隐藏盘导出单独二次确认、导出动作零本地痕迹）。而 Q-04 明写这套确认屏"可与 Q-02 使用同一
   确认组件"。文件管理器与备份中心共用同一个受 Q-04 约束的边界。

分在两个阶段做，这套确认屏必然要做两遍、再抽一次。故合为一个阶段。

#### 编号怎么定的

新设 **P3「数据管理」**，原 P3「加固与交付」整体推为 **P4**。改号影响 29 处引用，按性质分三类
处理：

- `MILESTONES.md` 12 处、`需求梳理清单_v2.md` 9 处 —— 都是活的验收要求与排期，**全部改写**。
- `需求梳理清单_v2.md:529` 一处 —— 当年一段假设性推演里的"等硬件到 P2/P3 才发现"，属存档分析，
  **不动**。
- 🔴 **本文件 7 处 —— 一个字都不改。** 它们写在带日期的历史条目里，是当时的记录，改了即失真。
  **对照规则：本日志中 2026-09-08 之前出现的 `P3`，一律指现在的 P4（加固与交付）。**
  该规则已同步写进 `MILESTONES.md` 第一节。

#### 本轮已定的四项

- **删除是永久删除，且不设二次确认**，按用户指定的"和系统文件管理器同标准"。
  ⚠️ 留档：Explorer 的 Shift+Delete 实际是**有**确认弹窗的，无确认只在删到回收站时成立，
  故本产品比 Explorer 更利落。这一点已向用户说明，用户维持原决定。补确认屏随时可加。
- **不进卷内回收站**：`$Recycle.Bin` 会在容器里默默占空间且用户看不见，又要走 shell 的
  undo 栈，多一块痕迹面。
- **图标自绘**，按扩展名分几类画内置 SVG，完全不碰 shell，thumbcache 注入面为零。8.7.5 那句
  "可以正常使用 shell 组件"是允许而非要求，本轮不用。
- **出卷即拒**：文件管理器只管容器内部（含跨容器），拖向普通磁盘直接拒绝并指向备份中心。
  Q-04 的整套确认屏留给 P3.11 起的导出与迁移一次做成共用件，不在文件管理器里先做一遍。

另定一条实现口径：**目录列表一次调用回一个打包串**，不沿用 `getContainerName(index)` 那套逐项
accessor —— 500 个文件按老模式是 2000 次串行异步 IPC，页面会肉眼可见地卡。

#### 顺带查出的一处过期文案

`ui/card_session.html:665` 那句"软件不会记录容器的位置，退出后需要重新导入对应文件"**自 P2.25
起即已作废** —— 现在既有 `container_index.bin` 也有「记住容器位置」开关。这是 P2.25／P2.27
漏掉的回写，已记进 P3.10 的回写清单。

#### 改动

- `MILESTONES.md`：新增第五节 P3「数据管理」（出口条件、合并理由、四项已定事实、P3.1～P3.10
  切片表、P3.10 四项回写清单、P3.11 起的导出与迁移）；原第五节 P4 及其后各节顺推编号；总览表
  新增 P3 行并收窄 P2 行；P2 出口条件删去"备份导出"，两行导出任务移入 P3；第七节范围演进史新增
  2026-09-08 一行；第八节工期风险条注明本次提前直接加在其上。
- `需求梳理清单_v2.md`：9 处活引用改 P4；工期表新增 P3 行；两行导出任务的阶段列改标 P3；
  8.7.5 与第五节条目表加 2026-09-08 的前向指针（原文暂留，完整回写在 P3.10）。

#### 尚未做

- **范围变更未取得客户书面确认。** 与 Q-04 同型，建议补书面件留档。
- P3.11 起的导出与迁移尚未拆片。
- 8.7.5 的完整回写留在 P3.10，目前只有前向指针。

- 提交：`7d89a50 [docs]open P3 for the data-management work`。
---

### 小切片 P3.1：卷内路径解析

P3 的第一片，纯算法层，不碰文件系统。新增 `usb_vault::volume::resolve_volume_entry_path`
与 `resolve_volume_child_path`，把「会话认定的盘符 + 页面递来的相对路径」变成一条绝对路径，
并拒绝页面能说出口的一切越界写法。它是 `resolve_container_target_path` 的对应物：那一个管
建卷表单，这一个管卷内容。

#### 为什么是两个函数而不是一个

浏览、删除、进目录递的是**路径**；重命名和新建递的是**名字**。把名字丢进路径解析器会让名字里
的一个分隔符悄悄把条目挪到别处 —— `rename("a", "b\c")` 就成了移动。所以名字单独走
`resolve_volume_child_path`，被按「它自称的那一个组件」检查，多一段就拒。

#### 冒号是这一片最要紧的一条

`a:b` 在 Windows 上不是一个叫 `a:b` 的文件，是 `a` 上的一条备用数据流（ADS）。流不是页面
看得见、删得掉、盘得清的条目，所以相对路径的任何组件里都不许出现冒号。这与
`container_target_path` 不同 —— 那边盘符需要恰好一个冒号，要单独放行；这边一个都不需要，
直接进禁用字符表。

#### ⚠️ 这一片保证不了什么，写在头文件里

`MILESTONES.md` 的 P3.1 行写了「拒……重解析点越界」，**做不到，也不该在这里做**。判断一个
目录是不是指向卷外的 junction 或符号链接，要开句柄、要比对最终路径，那是 I/O。本片是纯函数，
它能保证的只有**字符串层面越不出去**。卷内的重解析点仍可指向卷外，任何字符串检查都看不见。
拒绝跟随它是存储层开句柄时的义务（P3.2／P3.3），已写进头文件注释，不含糊过去。

#### 变异测试查出的一处冗余

第一次变异把 `.` / `..` 的显式拒绝改坏，**测试照样通过** —— 因为「组件不得以点结尾」那条规则
先把 `..` 和 `.` 都兜住了。换成拿掉禁用字符表里的冒号后测试立刻报 4 条失败，并确认失败时
进程退出码为 1（不是 0，否则 CTest 会把它当通过、整套测试形同虚设）。

显式的越界检查**保留**：它给出更准的错误消息，且「以点结尾」那条规则将来若改动，越界不能跟着
一起失守。但由此记下一条本套测试的已知边界：`require_rejects` 只断言抛了
`std::invalid_argument`，**不区分「因正确的理由被拒」和「被另一条规则顺手拒掉」**。这是沿用
`container_target_path_tests` 的既有写法，不是本片新引入的。

#### 落在 `volume` 而不是 `ui`

`container_target_path` 放在 `ui/` 是因为它只服务建卷表单。本片将来要被存储层（P3.2~P3.4）
和会话层（P3.5）调用，放 `ui/` 会让 `application` 反向依赖 `ui`。故落在
`include/usb_vault/volume/volume_entry_path.h`。

#### 改动

- 新增 `include/usb_vault/volume/volume_entry_path.h`、`src/volume/volume_entry_path.cpp`。
- 新增 `tests/volume_entry_path_tests.cpp`（6 组：join 与规范化、越界与 rooted、组件不是名字、
  子名必须恰好一个组件、盘符校验、Win32 长度上限）。
- `CMakeLists.txt` 与 `tests/CMakeLists.txt` 接线。

#### 验证

按 P3 测试纪律只构建并运行本片的最小目标：`review-clean`（Ninja + Debug + `/W4 /WX`）下
`volume_entry_path_tests` 编译无警告、CTest 1/1 通过；另完成上述变异测试确认断言与退出码可靠。
全套留到 P3.10。

- 提交：`2855467 [P3.1]refuse every page path that could leave the volume`。
---

### 小切片 P3.2：目录枚举，并在这里真正堵住卷外逃逸

新增 `usb_vault::volume::list_volume_directory`：给一条驱动器绝对路径，回一份
`VolumeDirectoryListing`（名称／是否目录／是否重解析点／大小／修改时间，外加一个 `truncated`）。
`.` 与 `..` 丢掉，顺序是「目录在前，然后按序数不分大小写」，条目上限 20000。

#### 🔴 P3.1 欠的账在这里还上

P3.1 只能保证**字符串**越不出卷；卷内的 junction 指向哪儿是运行时的事，路径写着 `X:\...` 也
照样能落到 `C:\Windows`。所以这里开目录句柄后用 `GetFinalPathNameByHandleW` 问文件系统
「你实际打开的是哪个盘」，盘符对不上就拒。**句柄故意不带 `FILE_FLAG_OPEN_REPARSE_POINT`** ——
目的不是拒绝打开链接，而是查明跟过去会到哪儿，而这个答案只有跟过去才存在。

重解析点本身**照常列出并打标**，不隐藏。一个「资源管理器里看得见、我们这儿看不见」的文件管理器
比诚实显示再拒绝进入更糟。拒绝进入由上层按这个标做。

顺带定死一条：最终路径若不是 `\\?\X:\...` 而是 `\\?\Volume{GUID}\...`（只能经挂载点抵达的卷），
**拒而不解析** —— 本产品的容器一律挂在盘符上，一份说不出自己盘符的列表也就无法自证没越界。

#### 排序为什么是序数而不是区域感知

页面要按下标回指条目。区域感知排序会让同一个目录在不同机器上排出不同顺序，下标就指向别的条目了。
故用 `CompareStringOrdinal(..., TRUE)`。

#### ⚠️ 两次变异测试，第一次的结论是错的

第一次把越界比较改坏，测试**照样通过**，一度以为 subst 用例是假的。查下来是**变异方法坏了**：
构建命令写了 `>nul 2>&1`，而那个变异触发了 `/WX` 下的告警导致构建失败，跑的还是旧 exe。

写了个独立探针确认 subst 盘完全可用（`CreateFile(Z:\)` 成功，最终路径 `\\?\C:\...`），再换一个
不触发告警的变异、且不吞构建输出重跑 —— 这次精确报出 `a path resolving onto another volume was
listed`、退出码 1。**越界检查确实是开火的那一个，测试是实的。**

📝 **记一条纪律：变异测试绝不能吞构建输出。** 吞了就分不清「测试没咬」和「根本没编过」，
而这两者的结论正好相反。

#### 测试怎么造出「路径说一个卷、文件系统说另一个卷」

用 `DefineDosDeviceW` 造一个指向临时目录的盘符（subst 的机制），它与卷内 junction 触发的是
**同一个判据** —— 最终路径落在别的盘 —— 但不需要第二块物理卷、不需要任何权限，在任何机器上都
确定可跑。另外用 `FSCTL_SET_REPARSE_POINT` 手搓了一个卷内 junction（结构体 `ntifs.h` 里才有，
用户态 SDK 没有，故自行声明），用来验证 `is_reparse_point` 如实上报。两个用例本次实机都真的
执行了，没有走 skip 分支。

#### 已知边界

- **截断发生在枚举期、排序之前**，所以 `truncated` 为真时留下的是「其中 20000 条」而不是
  「字典序前 20000 条」。上限是给病态目录的保险丝，不是分页，头文件里写明了。
- 目录的 `size_bytes` 恒为 0。目录自身记录的大小不是文件管理器里「大小」这个词的意思。

#### 改动

- 新增 `include/usb_vault/volume/volume_directory_listing.h`、
  `src/volume/volume_directory_listing.cpp`。
- 新增 `tests/volume_directory_listing_tests.cpp`（10 组，含临时目录夹具、junction 构造、
  subst 盘构造）。
- `CMakeLists.txt` 与 `tests/CMakeLists.txt` 接线。

#### 验证

按 P3 测试纪律只跑本片最小目标：`review-clean`（Ninja + Debug + `/W4 /WX`）下编译无警告，
`volume_directory_listing_tests` 通过；连带 `volume_entry_path_tests`、`volume_provider_tests`
共 3/3。另完成上述变异测试。全套留到 P3.10。

- 提交：`ded4a7b [P3.2]ask the file system which volume it actually opened`。
---

### 小切片 P3.3：删除、移动、新建目录，且递归删绝不钻进链接

新增 `usb_vault::volume` 三个入口：`delete_volume_entry`、`move_volume_entry`、
`create_volume_directory`。同时把 P3.2 里的越界检查抽成内部共用件
`src/volume/volume_containment.{h,cpp}`，因为列表和这三个操作问的是同一个问题。

#### 🔴 递归删除遇到 junction 是这一片唯一真正危险的地方

`X:\work\link` 若是指向 `C:\Users\<name>` 的 junction，天真的递归删会**清空用户主目录** ——
容器里删个文件夹，损失落在容器外，且不可逆。所以目录链接一律用 `RemoveDirectoryW` 只摘链接、
绝不下钻；`delete_directory_tree` 每一层都靠 `FindFirstFile` 拿到的属性判定，进入前就分流。

判属性用 `FindFirstFileW` 而不是 `GetFileAttributesW`：前者回答的是「这个名字」，后者可能回答
「链接指向的那个东西」，而后一个问题在这里处处是错的。

**已实测**：造一个卷内 junction 指向另一个目录，递归删父树后目标目录及其文件完好。再把守卫改坏
做变异，测试精确报出 `a recursive delete walked through a junction and emptied it`、退出码 1。
守卫是实的。

#### 几条已定语义

- **永久删除，不进回收站**（P3 已定）。卷内 `$Recycle.Bin` 会在容器里默默占空间且用户看不见，
  又要走 shell 的 undo 栈，多一块痕迹面。
- **只读文件照删**，先清 `FILE_ATTRIBUTE_READONLY`。Windows 上只读是属性不是权限，且调用方
  已经决定了。属性只清被删的那一个条目，不波及别处。
- **重命名与移动是同一个函数**。对文件系统它们就是一个操作，在这一层分成两个只会多出两种写法
  表达同一件事；「重命名」不过是两个父目录相等的那种移动。
- **绝不覆盖已存在的目标**。`MoveFileExW` 不带 `MOVEFILE_REPLACE_EXISTING` —— 覆盖会毁掉一个
  用户没点名的文件，而这一层没有确认屏可问，回 `ERROR_ALREADY_EXISTS` 让上层去说。
- **目录不能移进自己或自己的子树**，在问文件系统之前就拒。文本前缀比较是可靠的，因为两条路径都
  出自 `resolve_volume_entry_path`：不含 `.`、`..`、重复分隔符或正斜杠，故「是前缀」等价于
  「是祖先」。已测 `X:\workshop` 不会被误判为在 `X:\work` 里面。
- **跨卷移动在这一层直接拒**，它是「复制 + 删除」，失败模式不同，不该藏在一个承诺重命名的名字
  后面。留给 P3.4。
- **`create_volume_directory` 只建一级**，父目录必须已存在，免得打错路径时悄悄建出一串。

#### 顺带

测试夹具抽成 `tests/volume_test_support.h`（临时目录、junction 构造、subst 盘构造），
P3.2 的测试改用它，P3.4 会继续复用。`path_exists` 这个名字是刻意避开
`std::filesystem::exists` 的 —— 同名会因 ADL 在每个调用点都产生二义。

#### 改动

- 新增 `include/usb_vault/volume/volume_file_operations.h`、
  `src/volume/volume_file_operations.cpp`。
- 新增内部共用件 `src/volume/volume_containment.{h,cpp}`，`volume_directory_listing.cpp`
  改为使用它。
- 新增 `tests/volume_test_support.h`、`tests/volume_file_operations_tests.cpp`（13 组）。
- `CMakeLists.txt` 与 `tests/CMakeLists.txt` 接线。

#### 验证

按 P3 测试纪律只跑本片最小目标：`review-clean`（Ninja + Debug + `/W4 /WX`）编译无警告，
`volume_file_operations_tests` 通过；连带三个 volume 目标共 4/4。另完成上述 junction 变异测试。
全套留到 P3.10。

- 提交：`6d2105b [P3.3]unlink a junction instead of walking into it`。

---

### 小切片 P3.4：跨容器复制与移动，且不留半个文件

新增 `usb_vault::volume::copy_volume_entry` 与 `move_volume_entry_by_copying`。前者把一个条目
（文件或整棵目录）复制到另一处，同卷跨卷都行；后者复制完再删源，就是跨容器的「移动」。两者都收
一个 `TransferCancellation`（`std::function<bool()>`），页面将来要用它做取消。

#### 为什么不是把 `move_volume_entry` 扩成能跨卷

P3.3 的头文件里写过一句「跨卷是复制加删除，失败模式不同，不该藏在一个承诺重命名的名字后面」，
这一片没有推翻它。跨卷的那个要读写每一个字节、耗时与数据量成正比、可以被取消、可以没空间 ——
重命名一样都不会。调用方必须知道自己叫的是哪一个，所以是两个名字。

但 `move_volume_entry_by_copying` **不要求两条路径在不同卷上**。要求了名字就得叫
`..._across_volumes`，而那样它在单卷机器上根本无法被测到 —— 见下面「已知边界」。同卷调用它只是
慢，不会错，判断该走哪条由 P3.5 的会话层比一次盘符即可。

#### 一次预走查，把「没空间」和「有链接」都挡在第一个字节之前

`survey_source` 先把源整个走一遍，同时做两件事：加总字节数、拒绝任何重解析点。然后按目标盘符
问 `GetDiskFreeSpaceExW`，不够就报 `ERROR_DISK_FULL`。

两件事合在一次走查里是有意的：**任何一条不满足，都在还没写任何东西的时候就退出**，于是没有需要
回滚的残留。空间检查问的是驱动器根而不是目标目录 —— 空闲空间是卷的属性不是目录的，且根目录在
目标还不存在时就已经在了；顺带这个 API 回的是「本账户可用」，配额会削减的正是这个数，而那才是
复制真正拥有的空间。

⚠️ 空间检查是**礼貌而非保证**：检查与写入之间空间会变，文件系统仍可能中途写满，那条路径按普通
失败处理（回滚 + 上报）。头文件里写明了。

#### 🔴 回滚绝不能吃掉用户已有的东西

这一片最危险的地方不是复制，是**回滚**。目标已存在时复制必须失败，而失败后的清理如果不加区分，
清掉的就是用户自己的目录 —— 一次「复制到已有文件夹」的误操作会变成删库。

所以目标目录的 `CreateDirectoryW` **写在 try 之外**：它若因 `ERROR_ALREADY_EXISTS` 失败，
回滚作用域根本没打开过，那个目录不在任何清理路径上。单文件同理，`copy_file_entry` 里
`ERROR_FILE_EXISTS` / `ERROR_ALREADY_EXISTS` 是唯一不做清理就直接抛的分支。

**已变异实测**：把 `CreateDirectoryW` 挪进 try 里，测试精确报出「the refused copy took away what
the destination already held」，退出码 1。

#### 取消用 `PROGRESS_CANCEL` 而不是 `PROGRESS_STOP`

两者的差别正好是这一片的出口条件：`CANCEL` 会让 `CopyFileEx` **自己把写了一半的目标文件删掉**，
`STOP` 把碎片留在原地。目录层面的残留则由外层 `catch` 调 `delete_volume_entry` 收走 —— 复用
P3.3 那个不跟随链接的递归删，而我们自己造的目标树里本来就不可能有链接。

回调里包了 `try/catch`：异常不能穿回文件系统自己的栈帧，而一个会抛的取消回调本身就够格停下来。
头文件里把「不得抛」写成了契约。

#### 链接：既不跟随也不重建

跟随它读的是卷外的东西（可能极大，且不是用户指的那个）；在目标里重建它，等于往容器里写了一个
出口，而这一层剩下的代码整天都在拒绝这种出口。两条都不是「复制」，所以直接拒。

**已变异实测**：把 `require_not_a_link` 的判据换成一个永不置位的属性，测试报出「a tree holding
a link was copied」等 4 条，退出码 1 —— 说明不拒的话它真的会跟着 junction 把卷外文件复制进来。

#### 几条已定语义

- **目标绝不覆盖、绝不合并**。已存在就失败，报 `ERROR_FILE_EXISTS`／`ERROR_ALREADY_EXISTS`，
  让上层去说；这一层没有确认屏可问。
- **不能复制进自己**（同卷时按 P3.3 的 `is_same_or_inside` 判），已测 `X:\treehouse` 不会被
  误判为在 `X:\tree` 里。
- **源只在复制完整成功后才删**。取消或失败都不动源；反过来若复制成功而删源失败，条目会同时存在
  于两处，报删除的错 —— **宁可留重复也不留窟窿**。
- 文件的时间戳与属性随 `CopyFileEx` 带过去；**目录自己的时间戳不带**。

#### 已知边界

- 🔴 **自动化测试没有真正跑过「两个物理卷之间」的复制。** 造不出来：本套测试造第二个盘符的手段是
  `DefineDosDeviceW`（subst），而 subst 盘的最终路径落在 `C:`，正好会被 P3.2 起的越界检查拒掉 ——
  那个夹具存在的目的就是触发拒绝。复制逻辑本身与盘符无关，跨卷特有的只有「盘符比较」与「复制 +
  删源」两处，后者已在同卷上测到。**真正的双容器复制留给 P3.10 实机验证**，已记入待办。
- 目标路径比源路径长，深树复制可能撞上 `MAX_PATH`。撞上就是一次普通失败（回滚 + 上报），没有
  静默截断。
- 递归深度不设上限，与 P3.3 的递归删一致；实际由路径长度上限兜住。

#### 测试怎么造出「复制到一半失败」和「空间不够」

- **中途失败**：`ExclusiveFile` 以不共享（share mode 0）占住树里的一个文件，`CopyFileEx` 打开它时
  必然 `ERROR_SHARING_VIOLATION`。不需要满盘、不需要拔盘、不需要权限。注意预走查用的是
  `FindFirstFile`（不开文件），所以失败精确地发生在复制阶段 —— 正是回滚要管的那一段。
- **空间不足**：`try_make_sparse_file` 造一个**声称 8 TiB、实占约 0** 的稀疏文件。先试了 1 PB，
  `SetEndOfFile` 失败（超出 NTFS 单文件上限），测试走了 skip 分支；改成 8 TiB 后本次真的执行了。
  📝 再记一条：**skip 分支必须打印**，否则一条从没跑过的用例看起来和通过一模一样。
- **取消**：单文件那条用「第 1 次问答否、之后答是」，第 1 次正好是复制前的那一次检查，于是取消
  必然发生在 `CopyFileEx` 的进度回调里。目录那条用「目标里一旦有文件就取消」，它在预走查期间
  恒假（那时目标还不存在），进复制阶段后必真，与目录枚举顺序无关。

#### 四次变异测试

守卫是不是真的在开火，逐条验过（每次都不吞构建输出，且确认退出码为 1）：

| 变异 | 测试报出 |
|---|---|
| 目标目录的创建挪进回滚作用域 | the refused copy took away what the destination already held（2 条） |
| 去掉树复制的回滚 | a failed copy left part of the tree behind（3 条） |
| 链接判据换成永不置位的属性 | a tree holding a link was copied（4 条） |
| 无空间分支改报 `ERROR_ACCESS_DENIED` | a copy with nowhere to put the bytes went ahead（`(got 5: ...)`） |

最后一条顺带确认了新加的 `require_win32_error` 不是空断言 —— 它真的在比对错误码，而不是见到
任何异常就放行。这补上了 P3.1 记下的那条已知边界（`require_rejects` 不区分「因正确的理由被拒」）
在本片新增用例上的缺口。

#### 改动

- `include/usb_vault/volume/volume_file_operations.h`：新增 `TransferCancellation`、
  `copy_volume_entry`、`move_volume_entry_by_copying`。
- `src/volume/volume_file_operations.cpp`：预走查、空间检查、进度回调、回滚、递归复制。
- `tests/volume_test_support.h`：新增 `write_text_file`／`read_text_file`／`count_files_below`、
  `ExclusiveFile`、`try_make_sparse_file`。
- `tests/volume_file_operations_tests.cpp`：新增 12 组用例与 `require_win32_error` 断言辅助，
  并给既有的非驱动器绝对路径那组补上复制的两条。
- 无 CMake 改动：两个新入口落在既有的 `volume_file_operations.cpp` 里。

#### 验证

按 P3 测试纪律只跑本片最小目标：`review-clean`（Ninja + Debug + `/W4 /WX`）编译无警告，
`volume_file_operations_tests` 通过；连带四个 volume 目标 4/4。**本次运行无任何 skip 分支**
（junction、subst 盘、稀疏文件三个夹具都真的执行了）。另完成上表四次变异测试。全套留到 P3.10。

- 提交：`54691b1 [P3.4]copy across containers without leaving half a file`。

---

### 小切片 P3.5：接到会话，按容器 id 而非盘符寻址

把 P3.1～P3.4 的存储层接到 `IAuthenticatedCardVolumeSession`，新增六个入口：
`list_container_directory`、`delete_container_entry`、`rename_container_entry`、
`create_container_directory`、`move_container_entry`、`copy_container_entry`。存储层本身仍走
`CardLoginMountSessionRuntime`（新增六个虚函数），所以会话这一层能在没有真实卷的情况下被测。

#### 🔴 页面只能说容器 id，绝不能说盘符

能说出盘符的页面就能说出机器上任何一个盘符 —— 包括系统盘。id 只能指到本会话打开过的容器，而
「那个容器现在在哪个字母上」只有会话知道。**六个入口没有一个接受盘符，也没有一个会从路径里读
盘符出来。**

#### 「出卷即拒」是两半，都不是这一层自己做的

- 字符串那一半在 `resolve_volume_entry_path`／`resolve_volume_child_path`（P3.1）：本层是页面
  字符串变成路径的唯一入口，所以也是它必须止步的地方。
- 运行时那一半在存储层（P3.2／P3.3）：卷内 junction 指向哪儿要开句柄才知道。

这一层的职责因此很窄：**把 id 换成一个盘符，把页面的字符串交给正确的那个解析器**。测试断言的
也正是这两件事 —— 存储层假件只记录「每次调用收到的绝对路径」。

#### 名字与路径走两个解析器，所以重命名不可能变成移动

页面递「路径」的地方（浏览、进目录）用 `resolve_volume_entry_path`；递「名字」的地方（删除、
重命名、新建、移动的那一项）用 `resolve_volume_child_path`，名字被按「它自称的那一个组件」检查。
用错解析器，`sub{B}new.txt` 这样的新名字就会把条目挪到别处 —— 这条已做变异实测（见下表）。

#### id 相等即同卷，不必比盘符

本会话从不把两个容器挂在同一个字母上（`reserve_drive_letter` 会跳过已记录的字母），所以
**两个 id 相等就是同一个卷，不等就是两个卷**。`move_container_entry` 据此分流：同 id 走
`move_entry`（一次重命名，瞬时），不同 id 走 `move_entry_by_copying`（读写每一个字节）。
调用方不必比较盘符，也不必声明自己想要哪一种。

#### 认证要查，且与卸载不同

`mounted_drive_letter` 拒三种情况：未认证、id 无人应答、容器已收录但未挂载。**未认证要拒**，与
`mount_adopted_container` 一致，而与 `unmount_container` 不同 —— 卸载是把卷交回去的动作，必须
一直可用；读写卷里的内容不是，一个不知怎么活过了拔卡的挂载不该继续供内容。

#### 🔴 本片最值得记的一条：`invalid_argument` 本身就是 `logic_error`

第一版测试用 `require_throws<std::logic_error>` 断言「未挂载即拒」。**变异测试直接拆穿了它**：
把未挂载那条守卫整个删掉，测试照样通过。原因是未挂载的容器 `drive_letter` 是 `'{B}0'`，
`volume_root` 会因「不是驱动器字母」抛 `std::invalid_argument` —— 而 `invalid_argument`
派生自 `logic_error`，断言收下了它。

这不只是测试的问题，也是**页面的问题**：「没有这个容器」和「这个容器没挂载」是要分别说给用户听
的两句话，而基类断言把它们糊成了一句。故新增 `require_throws_plain_logic_error`（是
`logic_error` 但**不是** `invalid_argument`），改用它之后同一个变异立刻被咬住。

📝 纪律再加一条：**断言异常时要问「这个类型能不能被它的派生类顺手满足」**。这与 P3.1 记下的
「`require_rejects` 不区分因何被拒」是同一类账，只是这一次是继承而不是规则重叠。

#### ⚠️ 取消还没接到这一层

存储层的 `TransferCancellation` 在这里传的是空。跨容器移动要读写每一个字节，而会话入口是同步的，
页面没有第二个线程可以放这件事，也就产生不出一个有意义的取消回调。**头文件里写明了这个缺口**，
留给 P3.6／P3.9 连同异步一起解决，已记入 `MILESTONES.md`。

#### 顺带：四个 UI 假会话补了桩

`IAuthenticatedCardVolumeSession` 加了六个纯虚，四个测试里的假会话
（controller／host object／integration／close guard）随之补上抛 `std::logic_error` 的桩，
并注明「文件管理器不属于本夹具，P3.6 才开始用」。这四个目标本轮都构建并运行过，没有留下坏掉的
构建 —— 最小切片纪律管的是跑哪些测试，不包括容忍别的目标编不过。

#### 改动

- `include/usb_vault/application/card_login_mount_session.h`：产品边界新增六个入口。
- `src/application/card_login_mount_session_internal.h`：运行时新增六个虚函数，核心新增六个方法
  与私有的 `mounted_drive_letter`。
- `src/application/card_login_mount_session.cpp`：生产运行时转发到存储层；核心实现。
- `tests/card_login_mount_session_tests.cpp`：假件记录调用，新增 6 组用例与
  `require_throws_plain_logic_error`。
- 四个 UI 测试的假会话补桩。
- 无 CMake 改动。

#### 验证

按 P3 测试纪律只跑本片最小目标：`review-clean`（Ninja + Debug + `/W4 /WX`）编译无警告，
`card_login_mount_session_tests` 通过；受接口变更波及的四个 UI 目标一并构建，注册为测试的三个
连同本片共 4/4 通过（`card_product_webview_integration_tests` 是 `EXCLUDE_FROM_ALL` 的手动
夹具，只构建）。另完成下表四次变异测试，每次都不吞构建输出且确认退出码为 1。

| 变异 | 测试报出 |
|---|---|
| 删掉「未挂载即拒」 | an unmounted container was read from anyway（换用新断言后才咬住） |
| 移动一律走重命名 | a move between containers did not become a copy and a delete |
| 重命名的新名字改用路径解析器 | a rename did not become a move between two names in one directory |
| 删掉认证检查 | an unauthenticated session served a directory listing |

全套留到 P3.10。

- 提交：`bb7cc9d [P3.5]let a page reach a container by its identifier`。

---

### 小切片 P3.6：控制器投影 + WebView 桥

把 P3.5 的六个会话入口镜像到 `CardProductUiSessionController`，再镜像到 WebView 宿主对象
（`listContainerDirectory`／`deleteContainerEntry`／`renameContainerEntry`／
`createContainerDirectory`／`moveContainerEntry`／`copyContainerEntry`，DISPID 36～41）。
两者是机械镜像，所以合成一片。

#### 🔴 目录列表一次调用回一个打包串

P3 开篇已定：不沿用 `getContainerName(index)` 那套逐项 accessor。500 个文件按老模式是 2000 次
串行异步 IPC，页面会肉眼可见地卡；这里是 1 次。

格式：**记录之间用 `{B}x1e`，字段之间用 `{B}x1f`**。选控制字符不是图省事 —— 挂载卷上的文件名
**不可能**含控制字符（Windows 本身不允许，控制器又在这之前拒掉了任何含控制字符的列表），所以
**这个格式不需要转义，也就没有转义可以写错**。JSON 手工拼接的引号与反斜杠转义 bug 在这里根本
不存在。页面侧一行 `split` 即可。

**首记录是头**，只放截断标志。于是空目录回的是 `"0"` 而不是空串 —— 空串与「调用失败什么也没回」
分不开。

字段顺序：名称、是否目录、是否链接、字节数、修改时间（Unix 毫秒）。链接**照常列出并打标**，
沿用 P3.2 定的调子：一个「资源管理器里看得见、我们这儿看不见」的文件管理器比诚实显示更糟。

#### 控制器对「不可能的名字」失败关闭

`is_displayable_name` 拒绝空名、控制字符、以及 `{B}/:*?"<>|` 这九个字符。Windows 的文件系统
本身不允许它们出现在名字里，所以**一份含它们的列表不是一份文件列表，是卷被动过的证据**。

这里是**拒整份列表**而不是跳过那一条。跳过会让页面画出一个与卷里实际内容不同的目录，而这正是
这类攻击想要的效果；拒绝则会说出来。代价是一个坏名字挡住整个目录 —— 在一个安全产品里这个取舍
是对的，且已写进注释。

#### 浏览在错误路由上是拒绝，不是回空列表

`container_statuses()` 在未认证时回空表，因为页面在每个状态下都会轮询它。目录列表不一样：
**一份空列表会被读成一个空目录**。所以六个入口在 `authenticated` 之外一律 `throw_wrong_route`。

#### 参数擦除

新增 `DispatchTextWiper`：把一次调用里所有 `VT_BSTR` 参数的字符清零（长度与 BSTR 本身不动，
非字符串参数根本没被当字符串读过，原样留下）。理由与容器保存位置那条相同 —— **页面正在看的目录
名和它挑中的条目名，是加密卷里的内容**。回给页面的打包串也同样在返回后清掉。

#### HRESULT 映射

`std::invalid_argument`（未知 id、越界路径、不是一个组件的名字）→ `DISP_E_TYPEMISMATCH` 且
`argument_error = 0`；其余（错误路由、未挂载、文件系统拒绝）→ `E_FAIL`。页面因此能分开「我传错了」
与「现在不能做」。

#### ⚠️ 仍然是同步的

跨容器移动要读写每一个字节，而桥是同步的 —— 调用它就会卡住 UI 线程。**本片没有任何页面代码调它**
（页面是 P3.7～P3.9），所以这个缺口现在不可达，异步与取消仍按 P3.5 的记录留给 P3.9。头文件里写明了。

#### 四次变异测试

| 变异 | 测试报出 |
|---|---|
| `is_displayable_name` 不再拒绝 | a listing entry the page cannot be shown was projected anyway |
| 打包串丢掉头记录 | the packed listing is not what the page parses |
| 浏览去掉路由门 | an unauthenticated route browsed a container |
| 桥把两个容器 id 读反 | the move arrived with its two ends confused |

#### 改动

- `include/usb_vault/ui/card_product_ui_session_controller.h`：新增
  `DirectoryEntryUiStatus`、`DirectoryUiListing` 与六个入口。
- `src/ui/card_product_ui_session_controller.cpp`：投影、`is_displayable_name`、五个直通。
- `src/ui/card_product_webview_host_object.cpp`：六个 DISPID 与名字、`pack_directory_listing`、
  `DispatchTextWiper`、四个 invoke 处理（删除与新建目录共用一个，移动与复制共用一个）。
- `tests/card_product_ui_session_controller_tests.cpp`、
  `tests/card_product_webview_host_object_tests.cpp`：假会话由抛桩改为记录，各新增 4／3 组用例。
- 无 CMake 改动。

#### 验证

按 P3 测试纪律只跑本片最小目标：`review-clean`（Ninja + Debug + `/W4 /WX`）编译无警告，
`card_product_ui_session_controller_tests` 与 `card_product_webview_host_object_tests` 均通过；
受核心改动波及的 `ui_close_guard_tests` 一并构建并通过，`card_product_webview_integration_tests`
（`EXCLUDE_FROM_ALL` 的手动夹具）只构建。另完成上表四次变异测试，每次都不吞构建输出且确认退出码
为 1。全套留到 P3.10。

- 提交：`9a109c1 [P3.6]bring a whole directory across the bridge in one call`。

---

### 小切片 P3.7：启用侧栏入口，做出浏览主视图

侧栏「文件管理」从 `nav-item-disabled` 占位改成真入口（`data-section="files"`），新增
`dashboard-section-files`：容器选择器、面包屑、列表、以及三个态 —— 没有容器／容器未挂载／
文件夹为空。「备份中心」仍是占位，那是 P3.11 起的事。

#### 🔴 页面自始至终不知道盘符

页面说的是「哪个容器（id）＋容器内哪个目录（相对路径）」，绝对路径由原生侧拼。面包屑、进目录、
容器切换，没有一处产生或读取盘符。相对路径用 `/` 连接 —— P3.1 的解析器会把正斜杠规范化，这样
JS 里就不必到处写转义反斜杠，少一类写错的机会。

#### 一次调用取回整个目录，页面这边一行 `split`

`listingRecordSeparator = "{B}u001e"`、`listingFieldSeparator = "{B}u001f"`，与 P3.6 的桥一一对应。
`parseDirectoryListing` 校验：头记录只能是 `"0"`／`"1"`，每条记录恰好 5 个字段，名字非空，
两个布尔位只能是 `0`／`1`，大小与时间必须是安全整数。**不合规就抛**，不猜、不容错 ——
一份读错的目录列表会让页面显示出与卷里实际内容不同的东西。

#### 三个态必须分得开

「还没有容器」「容器未挂载」「文件夹是空的」在屏幕上都是一行灰字，但意思完全不同，尤其后两者：
**一个空列表若被当成空目录，用户会以为容器里的东西没了。** 三句话分别写死在三个分支里，契约测试
逐条钉住。截断也单独说（「条目太多，只显示了其中一部分」），不假装是全部。

#### 出错时退回根目录

读不到某个目录（被移走、已不在这个容器里）时，页面把路径清空退回根 —— 根是唯一一个永远有地方
可落的位置，否则页面会卡在一个进不去也退不出的路径上。

#### 链接照常显示并打标

沿用 P3.2／P3.6 的调子：列出来、标上「链接」。点进去若真的越界，原生侧会拒，页面把错误显示出来。
藏起来比显示更糟。

#### 边界文案写在页面上

「这里只管容器内部。把文件移出加密卷就是明文导出，要走『备份中心』，不在这里做。」—— 这是 P3
开篇定的边界，写在页面上，也被契约测试钉住。P3.9 的拖放会真正执行这条拒绝。

#### 📝 集成夹具页那一课，这次没有再踩

契约测试里那条「脚本 `getElementById` 的每一个 id，集成夹具页都必须有」（P2.26 的教训）本轮直接
生效：新增四个 id 的同时补进 `tests/card_product_webview_integration.html`。做了变异实测 ——
夹具页去掉 `file-list-body`，测试立刻报出。

#### 🔵 契约测试测不到什么，以及这次怎么补的

页面契约测试是**文本断言**：它能钉住「这段代码在」，钉不住「这段代码跑起来是对的」。所以本片额外
做了一件事：用 node 把 `parseDirectoryListing` 从已发布的 `card_session.js` 里原样切出来求值，
喂给它**P3.6 宿主对象测试里钉死的那个打包串**，验证解析结果逐字段正确，并确认它拒绝空串、非法
头、字段数不对、布尔位非法、空名字五种畸形输入。

**这是一次真正的跨层验证**：C++ 侧写出的格式与 JS 侧读入的格式由同一个字符串串起来，两边各自
的测试都钉住了它。（这只是本轮的一次性验证脚本，不是提交进仓库的测试 —— 项目里还没有 JS 测试
运行器，建立它不在本片范围内。真正的页面行为验证仍在 P3.10 实机。）

另外用 `node --check` 确认了 72KB 的 `card_session.js` 改动后语法无误。

#### 三次变异测试

| 变异 | 测试报出 |
|---|---|
| 侧栏入口改回 `nav-item-disabled` | unfinished navigation is not visibly present and unavailable |
| 集成夹具页丢掉 `file-list-body` | the integration fixture page is missing an element the product script looks up |
| 页面读一个桥不写的字段分隔符 | the packed listing is not read the way the bridge writes it |

#### 改动

- `ui/card_session.html`：侧栏入口、`dashboard-section-files`、约 25 行 CSS。
- `ui/card_session.js`：四个元素查找、`parseDirectoryListing`、容器选择／面包屑／列表三个渲染、
  `refreshFileManager`、三个监听器；`showDashboardSection` 记录当前分区并在进入 files 时取一次
  列表；`renderContainers` 顺带同步选择器。
- `tests/card_product_webview_integration.html`：真入口 + 新分区 + 四个 id。
- `tests/card_product_ui_page_contract_tests.cpp`：侧栏占位数 2 改 1，新增一组文件管理契约。
- 无 CMake 改动。

#### ⚠️ 已知边界

- **列表不轮询**。用户进入该分区、切换容器、进出目录时各取一次；容器在别处被卸载后，列表会停留在
  旧内容直到下一次触发。页面没有定时器，本片也不打算引入一个。
- 目录只能进不能「上一级」按钮 —— 面包屑本身就是上一级，点祖先即可回退。

#### 验证

按 P3 测试纪律只跑本片最小目标：`card_product_ui_page_contract_tests` 通过（`review-clean`，
`/W4 /WX` 编译无警告）。另完成上表三次变异测试、node 语法检查与上述跨层解析验证。全套留到 P3.10。

- 提交：`785e25d [P3.7]open the file manager on a container the page cannot name a drive for`。

---

### 小切片 P3.8：删除与重命名

列表每行加「重命名」「删除」两个按钮，工具栏加「新建文件夹」，重命名与新建共用一个取名对话框
（`file-name-dialog`）。

#### 🔴 删除是永久删除，且只问一次 —— 按已定事实实现

2026-09-08 已定：**和系统文件管理器同标准，不设二次确认，不进回收站。** 本片照此实现：点「删除」
直接调 `deleteContainerEntry`，没有对话框。

但**页面必须把这件事说出来**，所以分区里加了一条常驻提示：「删除是永久删除，不进回收站，点下去
就没有第二次询问 —— 与系统文件管理器同标准。」常驻提示不是二次确认，它不拦任何东西；它只是让
按钮旁边站着一句实话。契约测试同时钉住两头：**这句话必须在**，以及**页面上不许出现
`file-delete-dialog`** —— 后者是为了让「将来有人悄悄加回确认屏」这件事必须先改测试、也就必须
先回头看那条决策。

⚠️ MILESTONES 里那条留档仍然成立：Explorer 的 Shift+Delete 其实**有**确认弹窗，无确认只在删到
回收站时成立，所以本产品比 Explorer 更利落。要补确认屏是一处局部改动，随时可加。

#### 🔵 顺带把「新建文件夹」也做了，这是一处计划外的小扩张

P3.8 那一行只写了「删除与重命名」。但 `create_volume_directory` 从 P3.3 一路建到 P3.6 的桥，
而 P3.9 是移动与拖放 —— **不在这一片放入口，它到 P3 结束都不会有任何页面能调到它**，即一条建好
却死掉的通路。它与重命名共用同一个取名对话框，代价是一个按钮加一个分支。

**记在这里，是因为它确实超出了那一行的字面范围。** 同类的还有一个：`copy_container_entry` 至今
没有页面入口（拖放复制不在 P3.9 的字面范围内），这一条**没有**顺手做，留给 P3.9 决定。

#### 一个动作只有一个刷新点

P3.7 把「列表刷新」挂在了 `renderContainers()` 上，本片撤掉了：动作完成后要显示错误消息，而
`render()` 顺手 `void` 出去的那次刷新会把消息覆盖掉 —— 一个真实的竞态。现在刷新只有四个来源：
进入分区、切换容器、进出目录、以及一次动作结束（`runFileAction` 与 `runContainerAction` 各自
`await` 完再写消息）。容器在「容器」页被卸载时，文件视图也因此跟着更新了 —— 这比 P3.7 记的
「停留在旧内容」要好一些。

#### 取名对话框的几条

- 重命名预填原名并全选，直接回车即确认；名字没变或为空就当取消，不发请求。
- `maxlength="255"`，与 `kVolumeEntryNameMaximumLength` 对齐；其余规则不在页面重做一遍，
  由原生侧拒绝并回消息 —— **校验只写一处**，页面重写一份迟早会和原生侧对不上。
- 输入框自成 `.file-name-text` 一类而不复用 `.creation-text`：8.7.2 那条契约按数量钉死了两个建卷
  表单的控件，蹭它的类会把这个对话框算进那两个表单里去。CSS 用选择器并列复用，样式一致，契约不动。

#### 顺带修的一处既有契约

「每个文本控件都要关掉浏览器辅助」那条按数量断言（8 个 `autocomplete="off"` / `spellcheck="false"`）
改为 9 个 —— 容器内的文件名是加密卷的内容，比容器路径更该关掉输入法记忆与拼写检查。

#### 三次变异测试

| 变异 | 测试报出 |
|---|---|
| 常驻提示改成一句软话 | the page does not say that deleting is permanent and unasked |
| 重命名忘掉原名（两个参数都传新名） | the rename does not carry both the old and the new name |
| 页面上加一个 `file-delete-dialog` | a delete confirmation appeared without the decision being revisited |

#### 改动

- `ui/card_session.html`：工具栏按钮、`操作` 列、常驻删除提示、取名对话框、四条 CSS。
- `ui/card_session.js`：八个元素查找、`openFileNameFlow`／`closeFileNameFlow`／`submitFileName`／
  `deleteFileEntry`／`runFileAction`，行内两个按钮，列表点击处理改为同时管进目录与两个动作；
  刷新点收敛。
- `tests/card_product_webview_integration.html`：新增七个 id。
- `tests/card_product_ui_page_contract_tests.cpp`：新增一组契约，另改两处既有计数。
- 无 CMake 改动。

#### 验证

按 P3 测试纪律只跑本片最小目标：`card_product_ui_page_contract_tests` 通过。另完成上表三次变异
测试、`node --check` 语法检查，并重跑了 P3.7 那个跨层解析验证（格式未变，仍然对得上）。
全套留到 P3.10。

- 提交：`8116f0d [P3.8]delete once and rename in place`。

---

### 小切片 P3.9：移动与拖放，含跨容器，且拖出去时指路

行可拖，落点有三种：**本容器的子目录**（行本身）、**面包屑上的任一祖先**（往上移）、
**另一个已挂载容器**（列表上方那条虚线拖放条）。按住 Ctrl 拖为复制。

#### 🔴 拖动只携带一个标记，绝不递给 shell 任何文件

`setData("text/plain", "")` —— 就这一个空标记。**不设 `DownloadURL`，不碰
`dataTransfer.files`，不用 `items.add`。** 一旦页面把文件放上拖动数据，「拖到桌面」就成了一次
明文导出，而 Q-04 要求的那两道确认屏一道都没经过。契约测试三条负断言把这三个 API 名字钉死在
「不许出现」上，并做了变异实测：加一行 `setData("DownloadURL", ...)`，测试立刻报出。

#### 🔴 落在本页管不到的地方，就说清楚出口在哪

`dragend` 时若本页没有接住这次拖放（`fileDropHandled` 为假），页面报：
「没有移动。文件只能放进本容器的文件夹，或另一个已挂载的容器。要把文件拿出加密卷，那是明文导出，
请到『备份中心』做。」

这正是 P3.9 那一行要求的「拖向普通磁盘时拒绝并指向备份中心」。实现上它比字面要求覆盖得更宽：
拖到窗口外、拖到页面空白处、拖回原地，都会得到同一句话 —— 因为这三种情况用户想知道的是同一件事。

#### 🔵 复制在这一片补上了入口（第二处计划外扩张，同样记档）

P3.9 那一行只写「移动与拖放」。但 `copy_container_entry` 从 P3.4 建到 P3.6 的桥，若这一片不给它
入口，它到 P3 结束都是一条死通路（P3.8 的日志里已经把这个问题挂起，说「留给 P3.9 决定」）。
Ctrl+拖=复制 是系统文件管理器的通行手势，代价是一个 `event.ctrlKey` 三元和一句提示文案
（「按住 Ctrl 拖为复制」，写在拖放条上，不是隐藏手势）。**至此 P3.1～P3.6 建的每一条通路都有页面
入口了。**

#### ⚠️ 跨容器移动仍然会冻住界面 —— 这一片只让它「看起来在忙」而不是「看起来死了」

桥是同步的：跨容器移动在画这个页面的那条线程上逐字节读写。P3.5 就记下了这个缺口，本片**没有**
解决它 —— 引入线程模型是 P3 计划里没有排的一大块，硬塞进最后一个切片是错的做法。

本片只做了一件便宜而真实的改善：`runFileAction` 在发起调用前 `await` 一个
`setTimeout(resolve, 0)`，让「禁用 + 忙」的那次绘制真正落到屏幕上再开始阻塞。一个看起来在忙的
界面和一个看起来死了的界面，对用户是两件事。契约测试把这一行钉住了。

🔴 **异步与取消仍是未解项**，已在 `MILESTONES.md` 记为 P3.6／P3.9 的遗留，现随本片一并顺延给
P3.10 复核后决定放 P4 还是补做。

#### 几条实现上的取舍

- 落点高亮用一个 `fileDropHighlight` 变量托管，`dragleave`／`drop`／`dragend` 都会清掉，
  避免拖过多个目标后留下一串高亮。
- 拖到自己身上（目录行等于被拖的那一项）在 `dragover` 就不放行，不必等到调用才被原生侧拒。
- 拖到另一个容器落在**它的根**：拖放条上放的是容器名，它说的是「哪个容器」，没说「容器里哪儿」。
- 面包屑最后一节（当前目录）没有 `data-file-depth`，所以拖到它上面等于没落点，会走上面那句提示。

#### 三次变异测试

| 变异 | 测试报出 |
|---|---|
| 拖动数据里加 `DownloadURL` | the drag hands something other than a marker to the shell |
| `dragend` 不再报「没有移动」 | a drag that left the page is not answered with the backup centre |
| 去掉忙态绘制前的那次让出 | a blocking transfer starts before the page looks busy |

#### 改动

- `ui/card_session.html`：拖放条容器、六条 CSS（含拖中与落点高亮）。
- `ui/card_session.js`：`transferFileEntry`／`renderFileDropTargets`／`reportFileDropRefused`
  与高亮托管，行加 `draggable` 与两个 dataset，三组 `dragover`／`drop` 监听
  （列表／面包屑／拖放条）、`dragstart`／`dragend`，`runFileAction` 加一次让出。
- `tests/card_product_webview_integration.html`：新增 `file-drop-targets`。
- `tests/card_product_ui_page_contract_tests.cpp`：新增一组契约（含三条负断言）。
- 无 CMake 改动。

#### 验证

按 P3 测试纪律只跑本片最小目标：`card_product_ui_page_contract_tests` 通过。另完成上表三次变异
测试与 `node --check`。**拖放本身是纯交互，契约测试只能钉住代码在不在、数据给不给，给不出「拖起来
是对的」—— 这一条必须留到 P3.10 实机。** 全套留到 P3.10。

- 提交：`922c284 [P3.9]let a drag cross containers and never leave one`。

---

### 小切片 P3.10：全套测试与主文档回写（实机未做）

P3 文件管理器的收尾。**三件事里做完两件**，第三件我做不了，如实记在这里。

#### ✅ 全套测试：53/53 通过

`review-clean`（Ninja + Debug + `/W4 /WX`）全量构建无警告，`ctest` 53 个目标全过，耗时 216.9 秒。
这是本计划中唯一一次全套（P3.1～P3.9 每片只跑自己的最小目标，沿用 P2.21～P2.27 的做法）。

#### ✅ 主文档回写：四项全部完成

1. **8.7.5 的降级条撤销** —— 原文那段"建议降级为 v1.1"的论证不再留在正文，改写为已实现的定稿：
   实现范围、只管容器内部的边界、四项已定事实的**逐条落地情况**，以及未解项。原论证连同它是
   怎么被推翻的（③ 泄露面那条 2026-09-07 自行推翻）整段移到条目末尾留档，工期风险那句照旧保留。
2. **第五节条目表**那行（`需求梳理清单_v2.md:1625`）同步为「P3.1～P3.9 已实现并提交，
   8.7.5 正文已回写」。
3. **设计稿 `usb-vault_ui-design.html` 两处** —— 第 498 行「三处结构性差异」的第③条改为
   「已提前回 v1，侧栏『文件管理』是真入口，只管容器内部」；第 732 行「应用不内置文件管理器」
   整段改写为「内置，只管容器内部；出卷是明文导出，走备份中心」，并注明原文担心的"最近打开"
   痕迹面已于 2026-09-07 自行推翻。
4. 🔴 **`ui/card_session.html` 那句自 P2.25 起即已作废的文案** —— 「软件不会记录容器的位置，
   退出后需要重新导入对应文件」改为「是否记下容器位置由『系统设置』里的开关决定：开着则下次登录
   自动列出，关掉则退出即忘」。这是 P2.25／P2.27 漏掉的回写，本轮补上。

#### ⬜ 实机：没做，也做不了

P3.10 那一行的第三件事是实机验证。它要真实安全卡、要真实建卷与挂载、要人在屏幕前拖一个文件。
**本轮没有执行，不能算完成。** MILESTONES 的 P3.10 状态因此记为 🟨 而不是 ✅。

实机时至少要走一遍：进入「文件管理」→ 选容器 → 面包屑上下 → 新建文件夹 → 重命名 → 删除 →
同容器内拖动 → **两个真实容器之间拖动**（这一条自动化测试根本造不出来，见 P3.4 的已知边界）→
拖到窗口外确认得到「请到备份中心」那句话 → Ctrl 拖复制。

#### 🔴 P3.10 查出、未关闭的一项：图标自绘没有做

8.7.5 的四项已定事实里，「图标自绘，按扩展名分几类画内置 SVG」**至今没有实现** —— P3.7 的列表
只有名称／大小／修改时间三列。这不是欠账（P3.7 那一行本就没排图标），但**已定事实未兑现就是
未兑现**，不能靠"没排进切片"糊过去，所以同时记进了 `MILESTONES.md` 和 8.7.5 正文。

要加图标时必须自绘 SVG，不得取用 shell 图标 —— 否则 thumbcache 注入面回来，P4 的逐字节比对
多一个变量。

#### 其余仍然挂着的两项（此前已记，这里汇总）

- ⚠️ **异步与取消**：会话层与桥都是同步的，跨容器移动会冻住界面且无法取消。存储层的取消回调已经
  备好，缺的是把调用挪出 UI 线程。P3.5 记入，P3.9 只做到"先把忙态画上屏再阻塞"。
- ⚠️ **跨卷复制的自动化测试造不出来**：subst 盘会被越界检查拒掉（那个夹具本就是用来触发拒绝的），
  所以两个真实容器之间的复制只能实机验证。

#### 改动

- `需求梳理清单_v2.md`：8.7.5 整条回写，第五节条目表一行同步。
- `usb-vault_ui-design.html`：两处文案。
- `ui/card_session.html`：一句作废文案。
- `MILESTONES.md`：P3 阶段状态、P3.10 行、四项回写清单标完成，新增「图标自绘未做」一条。
- 无代码改动，无 CMake 改动。

#### 验证

全套 `ctest` 53/53 通过（本计划唯一一次）。文案改写后重跑
`card_product_ui_page_contract_tests` 仍通过，`node --check` 通过。**实机未执行。**

- 提交：`0be84e1 [P3.10]run the whole suite and write the file manager back into the docs`。

---

### 计划：P3.11 起「导出与迁移」拆片（2026-09-09）

文件管理器落地后，按 `MILESTONES.md` 原本写的「待文件管理器落地后再排」把这一段拆成 P3.11～P3.17。
**本条只是计划，没有任何代码改动。**

#### 存储层不需要新代码 —— 这一点值得先说清楚

`copy_volume_entry`（P3.4）本来就允许目标落在另一个卷上：它检查的是**每一端的父目录都停在它
自己那个卷上**，不是「两端必须同卷」。所以明文导出与文件管理器的分野**不在存储层**。

分野在**会话层的入口形状**：文件管理器的入口两端都按容器 id 寻址，**结构上就出不了卷**；
导出另有一个接受绝对目标路径的入口，名字直说它会把明文写到卷外。这比"同一个函数加一个
allow_outside 参数"安全得多 —— 那种参数迟早会有人传错。

#### 🟡 拆片时撞上一个真问题：二次确认按字面实现会泄露档位

Q-04 可验收项第 2 条要求「**导出隐藏盘内容时**单独二次确认」。按字面做，矛盾无法回避：

- 会话知道容器是不是隐藏卷（`release_first`），但**这个事实不能穿过应用边界** ——
  `ContainerStatus` 刻意不含卷类型，理由是 8.7.2 那句「**UI 差异即档位**」。
- 若由会话判断"这个要二次确认"，那么**页面弹了几道屏，就说出了这个容器是不是隐藏卷**。
  对手看一眼就够，不必破解、不必逼问。
- 若页面先试一次、被拒后再弹第二道，"被拒"同样是档位信号。

**提议：每一次明文导出都无条件走两道确认。** 与 8.7.3「创建 B 时新设的第二个密码不得验证」
是同一种思路 —— **宁可无条件，也不要有条件，因为条件本身即档位**。它比 Q-04 的字面要求更严格
（加密盘导出也要两道），代价是标准容器导出多点一次。

**这一条改的是产品交互，已标 🟡 待用户拍板，我不替他定。** 若用户坚持按字面实现，那就必须接受
「弹屏次数即档位」这个泄露面，并写进 8.4 的已接受清单 —— 但那与 8.7.2 的既有口径直接冲突，
所以我认为不该那样做。

#### 切片表要点

七片：P3.11 明文导出（会话）、P3.12 密文迁移导出（会话）、P3.13 零痕迹核查、
P3.14 控制器+桥、P3.15 页面与两道确认屏、P3.16 页面密文迁移、P3.17 收尾。

其中 **P3.13 单独成片**是有意的：Q-04 第 3 条明说「导出动作不得留任何本地痕迹」**不属于
"用户自担风险"的范畴，是我方实现义务**。把它混进别的片里，很容易变成"顺手看一眼"。
单独一片，且要求逐条确认：不进 `OperationEvent`（封闭枚举里本就没有 export —— 这一片要确认
它没被偷偷加上）、不写索引、不写设置、不留目标路径。

密文迁移导出的五条边界（Q-04 2026-08-24 补充）逐条落进 P3.12 那一行，包括那条容易漏的
**"可做当次复制校验，但校验值不得持久化"**。

#### 顺带记下两条仍然挂着的事

- ⚠️ **Q-04 至今没有书面确认**。它属"用户自担风险"类型，出事时需要举证依据；Q-04 正文与第八节
  都建议补书面件归档，至今是口头/即时确认。
- 🔴 **"导出不留痕迹"必须并入 P4 的逐字节比对验收范围**。用户承担的是"备份被人找到"的风险，
  不是"软件替他记录了他备份过隐藏盘"的风险。

- 提交：`bd7540b [docs]slice the export work and name the leak in asking twice`。

---

### 🔴 事故：`review-clean` 构建树的头文件依赖一直是失效的（2026-09-09 查出并修复）

做 P3.11 时两个 UI 测试目标突然失败，症状离奇：控制器调 `container_statuses()` 却执行到了我新加的
`export_container_entry` 桩上。**这是 vtable 错位** —— 两个翻译单元看到的接口不是同一个。

#### 根因

`out\build\review-clean/CMakeFiles/rules.ninja` 里记的是

```
msvc_deps_prefix = 注意: 包含文件:
```

而这台机器上 `cl.exe /showIncludes` 实际吐出的前缀与它不一致。前缀对不上，ninja 就**一条头文件
依赖都记不下来**。后果：

- 改头文件不触发任何重编译；
- 往接口里加一个纯虚函数会挪动 vtable 槽位，而没重编的旧目标文件照旧链接得上 ——
  **不报错，直接跑错**；
- 整场会话里刷屏的 `注: 包含文件:` 噪声，就是 ninja 没吃掉这些行的直接证据。我一直把它当噪声。

39 个核心目标文件里，头文件改动后只有 2 个被重编 —— 正是我直接改过 `.cpp` 的那两个。

#### 项目其实早就防着这一条

`CMakeLists.txt` 第 11～38 行有一道闸：MSVC + Ninja 时要求控制台代码页为 65001，否则**配置阶段
直接失败**，并指向 `tools\configure-preset.cmd`。注释写得很清楚：「header-only edits appear to
need no work, and stale objects can survive ABI changes」。

**`review-clean` 是绕过这道闸配出来的**（它不在 `CMakePresets.json` 里，只能手工 `cmake -B`）。
闸拦得住新建的构建树，拦不住一个已经配坏的。

#### 处置

1. **删掉重配**：`chcp 65001` + vcvars 后重新配置 `review-clean`，前缀重新探测。
2. **新增 `review-clean` 预设**（configure／build／test 三处），于是它能且只能走
   `tools\configure-preset.cmd review-clean` 这条带闸的路配出来。原先它无法用预设配置，
   这正是当初被手工配坏的原因。
3. **`MILESTONES.md` 写明**该目录必须用那个工具配。
4. **写了一个依赖探针**：只改头文件的注释，看包含它的目标文件会不会被重编。
   修复前会答 `DEPS BROKEN`，修复后答 `DEPS OK`。

#### 这件事对已有结论的影响

**全部重跑了一遍**：从零配置、从零构建、全套 `ctest` —— **53/53 通过，零警告，耗时 213 秒**，
其中包含刚才那两个失败的 UI 目标。所以：

- P3.10 记的「全套 53/53 通过」**成立**，本次干净重跑复现了它；
- P3.1～P3.9 每片的最小目标结论也成立 —— 那些片我都直接改过对应的 `.cpp`，
  重编是被文件时间戳触发的，没走依赖那条路。

📝 **纪律加一条：`注: 包含文件:` 那类刷屏不是噪声，是 ninja 没在吃依赖的证据。** 我看了一整场
才反应过来。下次在陌生构建树上跑第一次测试前，先跑一次依赖探针。

- 提交：`0a1b0c3 [build]give the clean test tree a preset so its deps cannot rot`。

---

### 小切片 P3.11：会话层的明文导出入口

新增 `export_container_entry(id, 相对目录, 名字, 绝对目标目录)`：把已挂载容器里的一个条目复制到
本机上的普通位置。**这就是 Q-04 说的明文导出**，写出去的东西不再受本产品任何机制保护。

#### 🔴 为什么是一个新入口，而不是给复制加个开关

文件管理器的 `copy_container_entry` 两端都按容器 id 寻址，**结构上就出不了卷**。导出这一个接受
绝对目标路径。两者不是一个函数加一个 `allow_outside` 参数的关系 ——

**开关迟早会有人传错，名字不会。** 一个叫 `export_` 的函数在调用点就写着"这行代码会把明文写到卷
外"，而 `copy(..., true)` 不写着任何东西。

#### 存储层一行没改

`copy_volume_entry`（P3.4）本来就允许目标落在另一个卷上 —— 它检查的是**每一端的父目录都停在
它自己那个卷上**，不是"两端必须同卷"。所以导出与文件管理器的分野**不在存储层，在会话层的入口
形状**。这一点在拆片时就看出来了，本片证实了。

#### `resolve_exported_entry_path`：这个单元里唯一允许落在卷外的解析器

放在 `volume_entry_path.h`，与其余几个并排，但注释第一句就写明它是那个例外。

实现上有个便宜的观察：**一条绝对目录就是"盘符根 + 一条相对路径"**。于是三个字符的根单独判，
其余部分交给已有的 `append_relative` 逐段走同一套规则。因此它免费继承了 `..`、空段、通配符、
控制字符、结尾空格或点、保留设备名、超长这一整套拒绝，而我没有重写一份校验器 ——
**校验规则重写一份，迟早和原版对不上。**

顺带一条语义：**导出的文件保留自己的扩展名**。这与 `resolve_container_target_path` 相反，那边会
追加 `.hc`（建卷时文件类型由产品决定），这边是把一个已有文件搬出去，产品无权改它是什么。

#### 目标落在已挂载容器上 = 拒

那不是导出，是复制，而文件管理器已经在做复制，且不带导出的任何后果。按盘符判：目标盘符若等于
本会话任一已挂载容器的盘符就拒（大小写不敏感）。容器一旦卸载，那个盘符立刻又是普通位置 ——
这一条也测了。

#### 🔴 什么都不记

`record_operation` 一次都没调，索引没写，设置没写，目标路径没留。Q-04 第 3 条明说这**不属于
"用户自担风险"的范畴，是我方实现义务** —— 用户接受的是"备份可能被人找到"，**不是**"软件替他
记下了他备份过隐藏盘"。测试直接断言导出前后 `operation_history()` 的长度和索引写次数都不变。

#### 确认屏不在这一层

Q-04 要两道确认，那是 P3.15 的事。这个入口执行的是一次**已经被同意过**的导出，它不问。
把"问"和"做"分在两层，是因为问什么、问几次还没定（见拆片时记的那个 🟡 无条件两道确认的提议）。

#### 四次变异测试

| 变异 | 测试报出 |
|---|---|
| 导出后补一条 `record_operation` | the export was written into the operation history |
| 去掉"目标在已挂载容器上就拒" | an export into a mounted container was accepted |
| 解析器改成"非空即通过" | a relative location was accepted（另加 drive-relative 一条） |
| —— | 三条均确认退出码为 1 |

#### ⚠️ 本片的测试结论一度是假的

第三次变异复验时，`volume_entry_path_tests` 显示 exit=0 却打印了失败 —— 那是我把
`./exe | tail` 的退出码当成了 exe 的退出码。改成分开跑后确认是 1。**管道会吃掉退出码**，
与"变异测试不能吞构建输出"是同一类错误的两个面。

另外本片撞上的构建树事故（头文件依赖失效）单独记在上一条，两个 UI 目标那次失败与 P3.11 的代码
无关，干净重建后 53/53 全过。

#### 改动

- `include/usb_vault/volume/volume_entry_path.h`、`src/volume/volume_entry_path.cpp`：
  新增 `resolve_exported_entry_path`。
- `include/usb_vault/application/card_login_mount_session.h`、
  `src/application/card_login_mount_session_internal.h`、`.cpp`：新增导出入口与
  `upper_drive_letter`。
- `tests/volume_entry_path_tests.cpp`：新增一组解析器用例（4 条通过形态 + 13 条拒绝形态）。
- `tests/card_login_mount_session_tests.cpp`：新增 3 组用例。
- 四个 UI 假会话补桩（P3.14 接线）。
- 无 CMake 改动，无运行时接口新增（导出复用既有的 `copy_entry`）。

#### 验证

按 P3 测试纪律只跑本片最小目标：`card_login_mount_session_tests` 与 `volume_entry_path_tests`
均通过（`review-clean` 重配后，`/W4 /WX` 编译无警告）。另完成上表变异测试。
全套留到 P3.17。

- 提交：`9a092b1 [P3.11]give the plaintext export its own name`。

---

### 小切片 P3.12：密文迁移导出

新增 `export_container_file(id, 绝对目标目录)`：把一个已收录容器**自己的文件**逐字节复制到用户
选定的位置。这是 Q-04 2026-08-24 补充的那条路 —— 用户带去另一台机器，用**原卡 + 原口令**继续
挂载。

#### 🔴 已挂载的容器拒绝迁移

驱动正在往已挂载的卷里写，此时复制容器文件，拿到的是一份**飞行中的快照** —— 最好也只是
crash-consistent。Q-04 边界第 4 条明写「成功前不得把不完整副本当成可用迁移结果」，而在这一层
唯一能保证这件事的办法就是**拒绝**，而不是复制完祈祷它是好的。

所以入口要求先卸载。卸载后那个文件就是一个普通文件，迁移随即可用 —— 两种情形都测了。

⚠️ 顺带一提：即便不拒，VeraCrypt 多半也持有该文件句柄，`CopyFileEx` 会撞共享冲突。**但靠对方
碰巧失败不是保证**，把它写成一条明说的规则才是。

#### 五条边界逐条对照

1. **逐字节复制既有容器文件，不建新格式、不解密、不重加密、不改源与副本** —— 复用
   `copy_volume_entry`，源只读、目标新建。出来的就是进去的那个文件。
2. **不含根密钥、口令、派生结果、PIN、恢复码、档位、模式或"是否存在隐藏卷"的标记** ——
   **由构造保证**：我们一个字节都没加。隐藏卷的不可区分性本来就在容器文件里，原样复制既不引入
   也不破坏它。测试断言这次调用只产生**一次** `copy_entry`，没有别的写。
3. **换机后仍须原卡与原口令，卡丢仍不可恢复** —— 无需实现，Q-02 不变。
4. **失败／空间不足／取消／目标已存在四种情况** —— 前三种由 P3.4 的存储层给：预走查、空间检查、
   回滚；目标已存在由 `copy_volume_entry` 不覆盖的既定语义给（回 `ERROR_FILE_EXISTS`）。
   ⚠️ **取消仍未接到这一层**，与 P3.5 记的同一个缺口。
5. **不记录导出历史、目标路径、最近使用记录** —— 没有 `record_operation`，没写索引，没写设置。
   测试断言导出前后 `operation_history()` 长度与索引写次数都不变。

#### 一处与明文导出**相反**的规定：目标可以在另一个已挂载容器里

`export_container_entry`（P3.11）拒绝目标落在已挂载容器上 —— 那是复制不是导出。这一个**允许**：
**放进容器里的容器仍然是加密的**，让明文导出危险的那些理由一条都不适用。两个入口的规则不同，
是因为它们搬的东西不同，不是疏漏。

#### 🔵 「可做当次校验」没有做

Q-04 边界第 4 条写的是「**可做**当次复制校验，但校验值不得持久化」—— 是允许，不是要求。
对一个几 GB 的容器再读一遍会让迁移耗时翻倍，而 `CopyFileEx` 的可靠性不需要我们复核。
**故不做，并记在这里**，免得将来有人以为是漏了。

#### 三次变异测试

| 变异 | 测试报出 |
|---|---|
| 迁移后补一条 `record_operation` | the migration was written into the operation history |
| 副本改名（追加 `.bak`） | the migration did not copy the container file under its own name |
| 去掉"已挂载就拒" | a mounted container was copied out from under the driver |

#### 改动

- `include/usb_vault/application/card_login_mount_session.h`、内部头、`.cpp`：新增迁移入口。
- `tests/card_login_mount_session_tests.cpp`：新增 3 组用例。
- 四个 UI 假会话补桩（P3.14 接线）。
- 无 CMake 改动，无运行时接口新增（复用 `copy_entry`）。

#### 验证

按 P3 测试纪律只跑本片最小目标：`card_login_mount_session_tests` 通过。受接口变更波及的四个 UI
目标一并构建并运行，全过 —— 这也顺带确认了上一条修好的头文件依赖真的在工作：加一个纯虚函数，
ninja 这次自己把控制器重编了。另完成上表三次变异测试。全套留到 P3.17。

- 提交：`cf2e1aa [P3.12]copy a container out without opening it`。

---

### 小切片 P3.13：零痕迹核查

Q-04 可验收项第 3 条：**导出动作不得留任何本地痕迹**。它单独成片，是因为 Q-04 正文明写这条
**不属于"用户自担风险"的范畴，是我方实现义务** —— 用户接受的是"备份可能被人找到"，
**不是**"软件替他记下了他备份过隐藏盘"。混进别的片里，它会退化成"顺手看一眼"。

#### 一次问遍整个导出面

新增一组用例，把会话能写的**每一处**在导出前拍下快照，然后逐个入口、逐种结局地重问：

- 会写的地方：`operation_history()`、运行时事件流、索引写次数、索引内容本身、
  「记住容器位置」写次数、「自动挂载导入容器」写次数。
- 问的结局：**成功**（明文导出、迁移导出）、**失败**（存储层抛异常）、**被拒**（路径不合规）。

📝 **新增的地方要加进那个快照列表** —— 注释里写明了这一点，否则将来加一处能写的地方，
这组用例会静静地漏过它。

#### 🔴 失败那一档才是关键

成功路径不记录，多数实现都做得到。**真正容易错的是失败**：一次"导出失败"是最诱人被记下来的
事件，而记下它就等于说出"这台机器上有人试过导出"。所以夹具加了一个 `fail_copy`，让存储层在
被调用后抛，用来问这个问题。

已变异实测：给明文导出加一个 `catch` 里的 `record_operation(..., failed, ...)` ——
测试精确报出「a failed plaintext export left a local trace」。

#### 反向断言：证明这组检查不是空的

最后一句故意做了一次**普通卸载**，并断言历史**增加了一条**。否则「导出什么都没记」这个结论，
和「这个会话根本什么都不记」是分不开的 —— 前者是设计，后者是坏了。

#### 🔴 把 `OperationKind` 钉成封闭的，附一个 MSVC 的坑

`OperationKind` 是封闭枚举，加一个 `container_exported` 就违反 Q-04。原本想用「switch 不写
`default`，漏一个枚举值就报 C4062」这个经典手法钉住它 —— **第一版没用**：加了第五个枚举值，
构建照样通过。

查下来：**C4062 在 MSVC 是默认关闭的，`/W4` 不打开它**（要 `/Wall` 或按号启用）。所以那个
switch 对它本该抓的事情是静音的。改成在该函数前后 `#pragma warning(error : 4062)` 局部提为错误
之后，同样的变异立刻报：

```
error C4062: 未处理枚举 "OperationKind" 的开关中的枚举器 "OperationKind::container_exported"
```

另外保留了四个枚举值的 `static_assert`：**那组钉的是每个值在线上的编号**（桥要用），
switch 钉的是**只有这四个**。两件事，两个手段。

📝 纪律再加一条：**用"编译器会警告"当护栏之前，先确认那条警告在本项目的警告等级下真的开着。**
与 P3.5 那条「`invalid_argument` 本身就是 `logic_error`」是同一类账 —— 护栏看着在，其实没装上。

#### 三次变异测试

| 变异 | 结果 |
|---|---|
| 给 `OperationKind` 加 `container_exported` | **构建失败** C4062，精确点名新枚举器 |
| 失败的明文导出记一条 failed 事件 | a failed plaintext export left a local trace |
| 迁移导出把目标目录写进索引 | the migration touched the container index |

#### 改动

- `tests/card_login_mount_session_tests.cpp`：新增零痕迹一组用例、`fail_copy` 夹具开关、
  四个枚举值的 `static_assert` 与封闭性 switch（含 C4062 局部提错）。
- **无生产代码改动** —— 这一片是核查，不是实现；查出来的问题都在测试这一侧。

#### 验证

按 P3 测试纪律只跑本片最小目标：`card_login_mount_session_tests` 通过。受前两片接口变更波及的
四个目标一并构建运行，全过。另完成上表三次变异测试。全套留到 P3.17。

- 提交：`9b7b004 [P3.13]ask every export whether it wrote anything down`。

---

### 小切片 P3.14：导出的控制器投影 + WebView 桥

把 P3.11／P3.12 的两个导出入口镜像到控制器，再镜像到宿主对象：
`exportContainerEntry`（4 参，DISPID 42）与 `exportContainerFile`（2 参，DISPID 43）。
与 P3.6 一样是机械镜像，合成一片。

#### 控制器什么都不判

两个入口只做一件事：**路由门**（非 `authenticated` 即 `throw_wrong_route`），然后原样递下去。
位置合不合规、是不是落在已挂载容器里、那个容器是不是挂着 —— **全部由会话判**。理由是会话是
唯一知道这些的层：容器在哪个盘符上、哪个还挂着，都不跨应用边界。

控制器多判一遍只会带来两份可能对不上的规则。**校验写一处**，与 P3.8 那条取名规则的处置一致。

#### 桥这边照旧擦参数

`exportContainerEntry` 的四个参数里有三个字符串：页面正在看的目录、它挑的条目名、
以及**导出目标位置**。前两个是加密卷的内容，第三个是本机路径（8.4 管的东西），
三个都由 `DispatchTextWiper` 在调用返回时清零。已变异实测：去掉那个 wiper，
测试报出「the export location outlived the call」。

#### 目标目录哪来的

复用既有的 `chooseContainerDirectory` —— 建卷表单那个原生文件夹选择器。页面不必新造一个选择器，
且和建卷一样，**选出来的东西回来仍要重新检查**，没有"因为是选的所以可信"这回事。

#### 三次变异测试

| 变异 | 测试报出 |
|---|---|
| 桥把导出的目录与条目名读反 | the plaintext export crossed the bridge out of order |
| 控制器去掉明文导出的路由门 | an unauthenticated route exported an entry in plaintext |
| 去掉导出参数的擦除 | the export location outlived the call |

#### 改动

- `include/usb_vault/ui/card_product_ui_session_controller.h`、`.cpp`：两个入口。
- `src/ui/card_product_webview_host_object.cpp`：两个 DISPID 与名字、两个 invoke 处理。
- `tests/card_product_ui_session_controller_tests.cpp`、
  `tests/card_product_webview_host_object_tests.cpp`：假会话由抛桩改为记录，各新增 2 组用例。
- 无 CMake 改动。

#### 验证

按 P3 测试纪律只跑本片最小目标：两个目标均通过（`/W4 /WX` 编译无警告）。受接口变更波及的
`card_product_ui_page_contract_tests`、`ui_close_guard_tests` 与集成夹具一并构建，前两个运行通过。
另完成上表三次变异测试。全套留到 P3.17。

- 提交：`7ffd22c [P3.14]mirror both exports onto the controller and the bridge`。

---

### 小切片 P3.15：备份中心，以及在那里发生的明文导出

侧栏「备份中心」从占位改成真入口 —— **至此侧栏没有任何占位了**。新增
`dashboard-section-backup`：一条常驻风险提示，加一个明文导出面板（要导出什么／导出到哪／
选择位置／导出）。

#### 🔴 文件管理器只负责"交出去"，导出发生在备份中心

行末新增「导出」按钮，它**不导出**：它把这一项交给备份中心，然后切过去。P3 开篇定的边界
（文件管理器只管容器内部）因此原样成立，页面文案也随之从"不在这里做"改成
"点行末的『导出』会带你去备份中心，那件事在那里做"。

契约测试钉住了这一点：**整个脚本里 `bridge.exportContainerEntry` 只能出现一次**。已变异实测 ——
让文件管理器自己调一次导出，测试报出「the export is reachable from more than the backup
centre」。

#### ⚠️ 没有确认屏，这是产品决定，不是遗漏

Q-04 可验收项 1 是 🔴「**不可跳过的**明文导出风险确认」，项 2 是隐藏盘的二次确认。
**2026-09-09 用户决定：不弹确认。** 我在决定前把冲突讲清楚了（去掉的是两条可验收项，
其中项 1 明标不可跳过；而项 3「不得留任何痕迹」意味着不能记日志，所以确认屏本身就是 Q-04 说的
那个"举证依据"），用户重申继续。决定与冲突已记入 `MILESTONES.md`，本条再记一次。

**留下来的是那条常驻提示**，它把 Q-04 要求明示的话原样写在页面上：

> 导出后的数据不再受本软件保护。……如果导出的内容来自隐藏卷，**这份副本的存在本身就是隐藏卷
> 存在的物证**，可能击穿否认性。普通删除也删不干净：SSD 上覆写不保证有效。

**提示不是确认** —— 它不拦任何东西。但 Q-04 要求"须明示"的那段话，在没有确认屏之后只剩这一处
还说得出来，所以契约测试逐句钉住它。

🔵 **注释里写明了这不是审美选择**：将来若客户要求把确认屏加回来，没有任何测试会反对
（我特意**没有**像 P3.8 的删除那样去钉"页面上不许出现确认屏 id"）—— 那一条是产品自己定的，
这一条是与客户需求冲突的，两者不该用同样的力度锁死。

#### 页面这边也不留痕

导出成功后，`pendingExport` 与 `exportLocation` 都清空。原生侧什么都不写，页面留着上次导出到
哪儿也是一样的痕迹 —— 只是它活在内存里。已变异实测：不清空，测试报出「the page kept the
exported entry or its location afterwards」。

#### 三次变异测试

| 变异 | 测试报出 |
|---|---|
| 提示删掉"物证／击穿否认性"那句 | the backup centre does not say what an export costs |
| 文件管理器自己调导出 | the export is reachable from more than the backup centre |
| 导出完不清空来源与位置 | the page kept the exported entry or its location afterwards |

#### 改动

- `ui/card_session.html`：侧栏入口、`dashboard-section-backup`、风险提示、约 20 行 CSS、
  文件管理器那条文案。
- `ui/card_session.js`：六个元素查找、行末「导出」按钮、`offerEntryForExport`／
  `renderBackupCentre`／`runExport`、两个监听器，`showDashboardSection` 进入 backup 时重绘。
- `tests/card_product_webview_integration.html`：真入口 + 新分区 + 五个 id。
- `tests/card_product_ui_page_contract_tests.cpp`：侧栏占位数 1 改 0，新增一组备份中心契约。
- 无 CMake 改动。

#### 验证

按 P3 测试纪律只跑本片最小目标：`card_product_ui_page_contract_tests` 通过。另完成上表三次变异
测试、`node --check`，并重跑了跨层解析验证。全套留到 P3.17。

- 提交：`21924b6 [P3.15]put the plaintext export where it can be said out loud`。

---

### 小切片 P3.16：页面的密文迁移导出

备份中心加第二个面板：选容器、选位置、导出 —— 把整个容器文件逐字节复制出去。

#### 🔴 顺带修一处 P3.15 留下的歧义：警告放错了地方

P3.15 把那条红色警告放在**分区级**，压在两个面板之上。做这一片时发现它是错的：
**密文迁移导出的副本仍然是加密的**，"导出后的数据不再受本软件保护"对它根本不成立。
一条放错位置的警告教的是错的东西 —— 用户会以为把容器复制到 U 盘等于把内容明文放出去。

现在警告在明文导出面板**里面**，迁移面板有自己那段话。契约测试按位置钉住：
明文面板的 `<h3>` 必须在警告之前，警告必须在迁移面板的 `<h3>` 之前。

#### 迁移面板说的四件事，都是 Q-04 的原话

- **不解密、不重新加密** —— 复制出来的还是那个容器；
- 换机后仍须**原安全卡 + 原口令**；
- **这不是密钥备份，也不是恢复通道：卡丢失或损坏，数据仍然永久不可恢复**（Q-02 不变）；
- **容器必须先卸载**，否则复制到的是一份正在被写入的卷。

#### 已挂载的容器**列出来但说明原因**，不是藏起来

下拉里写「<容器名>（已挂载，需先卸载）」，导出按钮保持禁用。藏起来会让用户以为容器丢了，
或者以为这个功能坏了 —— 与 P3.7「链接照常显示并打标」是同一条调子：**能看见、能理解、
被拒绝**，好过看不见。

#### 📝 这一片的变异测试又一次咬的是我的断言，不是代码

第一版「警告要在明文面板里」写成了「警告出现在『密文迁移导出』这几个字之前」。
把警告挪回分区级做变异 —— **测试照样通过**，因为分区级的警告本来就在那几个字前面。
改成按两个 `<h3>` 的位置夹住之后，同一个变异立刻报出。

这已经是本轮第三次了（P3.5 的 `invalid_argument` 派生自 `logic_error`、P3.13 的 C4062 默认关闭、
本片的位置断言太松）。**规律很清楚：变异测试在这个项目里主要不是在查代码，是在查我写的断言够不够
锋利。** 三次全都是"护栏看着在，其实没装上"。

#### 三次变异测试

| 变异 | 测试报出 |
|---|---|
| 警告挪回分区级（压住两个面板） | the plaintext warning is not scoped to the plaintext panel（改断言后才咬住） |
| 已挂载的容器从下拉里删掉 | a mounted container is hidden rather than explained |
| 迁移完不清空位置 | the page kept the migration location afterwards |

#### 改动

- `ui/card_session.html`：警告移入明文面板，新增迁移面板（含 Q-04 四条文案）。
- `ui/card_session.js`：六个元素查找、迁移状态与选择器渲染、`runMigration`、三个监听器。
- `tests/card_product_webview_integration.html`：新增五个 id。
- `tests/card_product_ui_page_contract_tests.cpp`：新增一组迁移契约；"忙态先上屏"那条计数
  2 改 3，并把消息改成说清这个数字的含义（现在有三处会在绘制线程上搬字节）。
- 无 CMake 改动。

#### 验证

按 P3 测试纪律只跑本片最小目标：`card_product_ui_page_contract_tests` 通过。另完成上表三次变异
测试与 `node --check`。全套留到 P3.17。

- 提交：`7805eec [P3.16]carry a whole container out, still encrypted`。

---

### 小切片 P3.17：导出与迁移的收尾（实机未做）

与 P3.10 同型：**三件事做完两件，第三件我做不了。**

#### ✅ 全套测试：53/53 通过，零警告

`review-clean`（重配后的那棵树）全量构建无警告，`ctest` 53 个目标全过，耗时 339 秒。

#### ✅ 主文档回写

**Q-04 的四条可验收项逐条标了落地状态**，写在需求文档里而不只是 `MILESTONES.md` ——
可验收项的家在 Q-04，状态就该记在那儿：

1. **不可跳过的明文导出风险确认 —— 🔴 未落地。** 2026-09-09 用户决定不弹确认屏。
   决定前已明确告知：去掉的是第 1、2 两条，且第 1 条明标"不可跳过"；而第 3 条"不得留任何
   本地痕迹"意味着不能记日志，**所以这个确认屏本身就是 Q-04 说的那个"出事时的举证依据"**。
   用户重申继续。须明示的那段话以备份中心的常驻提示保留。**建议走书面件**。
2. **隐藏盘的二次确认 —— 🔴 未落地**，随第 1 条一并去掉。另把拆片时发现的那个技术矛盾
   记在了条目里：按字面实现会**泄露档位**（弹几道屏就说出了容器是不是隐藏盘），
   将来若恢复确认屏**不要按字面做**，走"无条件两道"那条。
3. **不得留任何本地痕迹 —— ✅ 已落地**，P3.13 单独成片核查，成功／失败／被拒三种结局各查一遍。
4. **用户手册明示残留问题 —— 🟨 部分落地**：页面已写明"SSD 上覆写不保证有效"，
   但**用户手册本身尚不存在**，手册写出来之前这条不算完成。

**密文迁移导出的五条边界**也逐条对照回写，包括我们额外加的那条"已挂载即拒"（Q-04 没写，
但它是第 4 条"不完整副本不得当成结果"在实现上的必然要求），以及未做的那条"可做当次校验"。

#### ✅ 顺带：写了一份实机验证清单

P3.10 和 P3.17 都卡在实机上，而"实机验证"四个字不是可执行的指令。所以在 `MILESTONES.md` 里
汇总了一份 **14 步清单**，两阶段共用，其中标红的四条是自动化测试**结构上**到不了的：

- 两个真实容器之间拖动（subst 盘会被越界检查拒掉，见 P3.4 的已知边界）；
- 确认没有任何确认屏（这是决定，不是缺陷，实机时要确认它确实如此）；
- 把导出的副本拿到另一台机器，用原卡原口令挂载；
- 导出前后比对本机产物应无差异 —— **这一条同时是 P4 逐字节比对的预演**。

#### ⬜ 实机：没做，也做不了

要真实安全卡、真实建卷、真人在屏幕前拖文件和插另一台机器。**本轮没有执行，不能算完成。**
P3.17 状态因此记为 🟨。

#### 仍然挂着的四项（汇总）

| 事项 | 记在哪 |
|---|---|
| 🔴 明文导出的两道确认屏未做，与 Q-04 可验收项 1、2 冲突，建议走书面件 | Q-04 正文 + `MILESTONES.md` |
| 🔴 异步与取消：跨容器移动与整容器迁移会冻住界面，无法取消 | `MILESTONES.md` 第五节 |
| 🔴 图标自绘（8.7.5 的已定事实之一）至今未做 | 8.7.5 + `MILESTONES.md` |
| ⚠️ Q-04 至今是口头确认，正文自己建议补书面件 | Q-04 正文 |

#### 改动

- `需求梳理清单_v2.md`：Q-04 四条可验收项逐条回写、密文迁移五条边界逐条对照。
- `MILESTONES.md`：P3.17 行、P3.11 起小节标题、第五节阶段状态、阶段总表那一行、
  自 P2 移入的两行补状态列，新增实机验证清单。
- 无代码改动。

#### 验证

全套 `ctest` 53/53 通过、零警告（339 秒）。**实机未执行。**

- 提交：`56b2db5 [P3.17]write the export back into the requirement it came from`。

---

### 四项同型决策的沿革汇总（2026-09-10）

Q-04 与其后三项决定同属"用户自担风险／范围变更"型，此前散在三个文件里、互不相邻，
从未并排看过。本轮把它们的**原决策 → 现决策**汇总一次，便于回头核对改了什么。

不改任何决定，也不改各项的既有记录 —— 完整论述仍以各自正文为准，本条只是索引。

#### 一、明文导出（2026-08-19 拍板，08-24 补充）

- **原**：《基本需求》第 5 条要求"拷贝备份数据明文拷贝出来"；梳理时曾建议改为
  "明文导出／加密备份"二选一；v1 需求书另有 F-037「备份数据完全随机无特征」。
- **现**：仅明文导出，用户自担风险。F-037 与之直接矛盾且不可调和，删除。
  2026-08-24 增设密文迁移导出 —— 原样逐字节复制既有容器，不解密不重加密，
  不新增密钥备份或恢复通道，换机后仍须原卡原口令。
- 记在：`需求梳理清单_v2.md` 第四节 Q-04 全文。

#### 二、本地痕迹的比对范围（2026-09-07）

- **原**：8.4 要求配置／注册表／日志／最近使用记录在档二档三之间逐字节不可区分；
  `MILESTONES.md` 的验收行原文是"全部本地产物……差异必须为零或与档位无关"，**不分作者**。
- **现**：收窄为只约束**本产品自己写下的**产物。Windows 与第三方程序因为用户使用已挂载卷
  而写下的痕迹明示接受，不计入差异 —— thumbcache、shell 最近使用项与跳转列表、
  第三方程序自己的 MRU。**红线不放宽**：我方自己的最近打开列表、上次所在目录、排序方式
  仍一律禁止，复用 shell 组件时其默认 MRU 行为须逐个显式关闭。
- 改这条的直接原因：缩略图缓存与档位有关（档二不会有），照原文那条验收必然失败，
  且会跑出一堆已被接受的差异，做的人分不出哪些是真问题。
- 记在：`需求梳理清单_v2.md` 8.4「本层的边界」。

#### 三、应用内文件管理器的档期（2026-09-08）

- **原**：8.7.5 把应用内文件增删改移降级为 v1.1，v1 只留只读状态展示。
- **现**：提前到 v1，即新设的 P3.1～P3.10。原降级的两条理由里，"泄露面"那条已于
  2026-09-07 随 8.4 的边界自行推翻，只剩工作量与"资源管理器已够用"。
- 代价：工期。第八节"范围已排至工期上限"那条风险**并未解除**，本次提前是直接加在其上的。
- 记在：`需求梳理清单_v2.md` 8.7.5、`MILESTONES.md` 第五节与第七节范围演进史。

#### 四、明文导出的两道确认屏（2026-09-09）

- **原**：Q-04 可验收项 1「**不可跳过的**明文导出风险确认」，须明示"导出后的数据不再受
  本软件保护，且可能击穿隐藏盘的否认性"；可验收项 2「导出隐藏盘内容时单独二次确认」。
- **现**：一道都不弹。须明示的那段话改为「备份中心」页面上的**常驻提示**保留 ——
  常驻提示不是确认，它不拦任何东西。
- 这一项与前三项不同，它是**对可验收项 1、2 的再修改**。原要求在条目里不删，
  现决策以条目下的引用块标注，两条记录并存。
- 拆片时另发现可验收项 2 按字面实现会**泄露档位**：会话知道容器是不是隐藏卷、页面不知道，
  若由会话决定"这个要二次确认"，弹几道屏就说出了容器是不是隐藏盘。将来若恢复确认屏，
  **不要按字面做**，走"无条件两道"那条。
- 记在：`需求梳理清单_v2.md` Q-04 可验收项 1、2。

#### 改动

- `需求梳理清单_v2.md`：Q-04 末尾新增「四项同型决策的沿革索引」一节（表格 + 一条读法说明）。
  各项原有记录一字未动。
- `DEVELOPMENT_LOG.md`：本条目。
- 无代码改动。

- 提交：`f45eee1 [docs]put the four decision changes side by side`。

---

### UI：导出与迁移界面改动提交审查（2026-09-10）

#### 范围与计划核对

- 用户明确要求提交当前工作区改动，使用 `[ui]` 前缀；本轮未追加产品代码改动。
- 已核对 `AGENTS.md`、`MILESTONES.md` 及主需求的 R-03、Q-04、8.6；技术要求仍以主需求为准。
- 本次作为一个界面功能边界提交：导出与迁移页面、配套契约测试与 WebView 测试页面、设计稿提示同步。
- 发现记录与实现的差异：主需求和里程碑仍记载 2026-09-09 不弹确认屏，当前代码已经恢复统一的“明文导出”输入确认。此处记录实际代码状态，不据此将 Q-04 验收项标为完成，也不改写此前决策。

#### 已检查的实现与改动

- `ui/card_session.html`、`ui/card_session.js`：页面更名为“导出与迁移”，切换明文与密文面板；容器行支持挂载、卸载及页面内拖动排序。
- 明文导出支持根目录条目多选、容器全选与文件管理器交接；逐项调用既有导出接口，成功项移出选择，部分失败保留待处理项。
- 明文执行入口增加输入短语确认，密文迁移只允许选择未挂载容器；未修改原生导出、密钥或驱动代码。
- 已检查 `tests/card_product_ui_page_contract_tests.cpp`、`tests/card_product_webview_integration.html` 与 `usb-vault_ui-design.html` 的配套差异。

#### 验证

- `cmake --build --preset review-clean --target card_product_ui_page_contract_tests`：通过，构建树已是最新状态。
- `ctest --preset review-clean -R "^card_product_ui_page_contract_tests$"`：1/1 通过。
- `node --check ui/card_session.js`：通过。
- `git diff --check`：通过；Git 提示工作副本 LF 将按现有配置转为 CRLF。
- 本轮未运行全套测试、WebView 运行时集成测试或真实安全卡交互验证；静态契约通过不等于交互行为已验收。

#### 安全审查、风险与剩余问题

- 新增容器名、文件名和目标路径通过 `textContent` 写入页面；未发现新增动态 HTML 注入入口、持久化写入或档位分支。排序和导出选择仅驻留页面内存，未新增密钥接触路径。
- 原页面关于“可能击穿隐藏盘否认性”和“SSD 上覆写不保证有效”的提示已移除，当前确认弹窗只说明写出的是解密后的原始文件。Q-04 的完整风险提示要求仍有缺口，本次提交不代表消除该差异。
- 批量导出仍串行调用同步复制接口，大数据量会阻塞界面且无法取消；沿用已知风险。
- 选择与目录缓存的跨会话清理、目录截断时全选语义及拖动交互尚未进行运行时验证，后续需覆盖这些边界。

#### 提交状态

- 用户已授权本轮提交，拟提交主题：`[ui]refine export and migration selection flows`。
- 提交正文使用英文，包含主要改动、验证范围与已知限制；不添加共同作者尾注，不推送。

---

### UI：合并容器管理入口与仪表盘共用列表（2026-09-10）

#### 计划、需求与范围审计

- 用户要求以 `usb-vault_ui-design.html` 为 UI 基准：删除独立的旧容器页，将文件管理改为容器管理，并补上仪表盘容器行的左侧拖拽与右侧详情。
- 已检查设计稿的两级容器管理、仪表盘概览与排序说明；对照 `MILESTONES.md` 的 P3 文件管理范围以及主需求 R-03、8.4、8.6、8.7.5。旧文档中的两个侧栏入口由本次用户指令合并，文件操作仍限于容器内部。
- 本片以“仪表盘与容器管理共用列表”为一个可独立审阅的 UI 边界；不修改原生桥、容器文件、加密索引或密钥实现。
- 设计稿要求跨重启保存排序，但这涉及索引条目格式与原生接口。本片沿用页面内排序，已向用户说明；排序持久化另行切片，不能将本片标为该项完成。

#### 检查与实现

- 检查了现有页面、脚本、页面契约测试以及 WebView 测试页面和驱动脚本。
- 删除旧 `containers` 路由；“容器管理”使用原 `files` 路由，第一级复用容器列表，第二级显示容器内文件；去掉重复选择容器的下拉框，面包屑首项返回容器列表。
- 仪表盘与管理列表共用容器行和页面内顺序；左侧六点手柄使用 pointer 事件排序，行体不触发排序，支持 Alt + 上下方向键。挂载状态不决定排序位置。
- 右侧箭头一次展开一个详情面板，展示名称、状态、盘符、已挂载容量以及设计稿规定的两项产品常量。保留浏览、挂载/卸载、导出入口和移除确认流程。
- “在资源管理器中打开”“更新诱饵内容”没有现成桥接接口，统一显示为禁用并解释尚未接入，未伪造可用行为。
- 返回列表后忽略迟到的目录读取结果；会话失效时清空详情、浏览状态和页面内顺序，当前容器被移除时返回管理列表并清空旧文件内容。

#### 最小验证

- 按用户要求只运行直接相关的最小测试，不跑全套。
- 页面契约目标使用 `review-clean` 构建与 CTest 验证：1/1 通过。最初构建缺少 MSVC 标准库搜索路径，加载 `vcvars64.bat` 后构建成功。
- JavaScript 语法检查与 `git diff --check` 通过。
- 纯 DOM 导航回归：18 项通过，覆盖路由合并、详情展开/收起、共用排序、键盘操作、两级导航、未挂载入口、迟到结果及会话重置。
- DOM 模拟使用隔离在 `out/ui-navigation-tools` 的 jsdom 26.1.0；安装时禁用脚本，不新增产品运行依赖。测试桥只提供虚构状态，不访问安全卡、真实容器或文件系统导出接口。
- 提供可重复生成的浏览器测试夹具；浏览器安全策略拒绝本地 HTML 地址，本轮没有执行浏览器视觉验收，也没有绕过该策略。DOM 模拟不覆盖真实布局、指针命中或 Windows WebView 行为。

#### 安全审查与剩余事项

- 新增名称与详情值全部通过 `textContent` 渲染；选择器中的容器 ID 已由原生状态读取边界验证为正整数。未增加动态 HTML、持久化排序、目录记忆、导出记录或档位判断。
- 原有文件操作按容器 ID 与相对路径寻址，未引入盘符寻址或跨卷导出捷径。未改 R-03 所涉及的敏感缓冲区。
- 仍待真实浏览器/应用视觉与拖拽验收；排序持久化和两个未接入的详情操作未实现。此前记录的导出风险提示缺口与同步复制阻塞问题不在本片修复范围内。
- 提交状态：未提交，等待用户决定；未推送。

#### 开发版构建补充

- 按用户要求构建 `out/p2_run_dev_app.ps1` 对应的程序：加载 MSVC 环境后，执行 `cmake --build --preset dev --target usb_vault_vikey_p2_development_app`，构建成功。
- 已核对 `out/build/dev/card_session.html`、`card_session.js` 的 SHA-256 均与当前 `ui/` 源文件一致；启动脚本无需修改。
- 本次只构建与核对部署资源，未追加测试、未启动新应用实例、未提交或推送。

#### 同片 UI 调整：整行展开与移除入口

- 用户进一步指定：仪表盘箭头去掉背景框、放到容器行最右侧；整行点击展开详情，并补回“从列表移除”。沿用此前需求审计与共用列表边界，以本次指令修订交互。
- 编辑前检查容器行生成、详情按钮样式、移除确认流程及 DOM 导航测试；设计稿中“移除只删记录、不删文件且需确认”的边界不变。
- 箭头改为独立的透明无边框控件，固定在行最右侧；名称、状态、容量及行内空白处均可点击展开/收起。共用构件使容器管理列表保持同样行为。
- 整行点击显式排除操作按钮和拖拽手柄；箭头保留键盘可达性及 `aria-expanded`。行内补回移除按钮，已挂载时禁用并提示先卸载，未挂载时进入既有确认流程。
- 最小验证：DOM 导航回归 24 项通过；页面契约 CTest 1/1 通过；JavaScript 语法及差异格式检查通过。未跑全套测试。
- 安全审查：未改移除原生接口与确认门槛，取消不移除记录；未新增文件删除、持久化写入、敏感字段或档位判断。
- `dev` 的 P2 开发应用构建成功，部署 UI 与源文件一致。视觉验收仍未完成，沿用此前浏览器策略限制的记录。
- 此次为上一未提交切片的用户修订；仍未提交、未推送。

#### 同片 UI 调整：详情精简与卸载配色

- 按用户最新指令检查详情面板和全部卸载入口；本次指令优先于设计稿里详情重复提供挂载操作的布局，仍沿用本片的需求与安全边界。
- 删除详情面板中的挂载/卸载按钮，操作保留在容器行内。
- 仪表盘与容器管理行、导出与迁移列表、顶部安全卸载，以及安全组件异常时的“尝试安全卸载”统一使用红色危险操作样式；普通挂载与登录保持原配色，禁用态沿用现有弱化样式。
- 安全审查：仅调整按钮呈现，没有改变挂载/卸载调用、权限检查、忙态或敏感数据处理。
- 最小验证：现有 DOM 导航回归 24 项、页面契约 1/1、JavaScript 语法和差异格式检查通过；未跑全套。开发版构建成功，部署 HTML/JS 与源文件一致。
- 仍未进行视觉验收；未提交、未推送。

#### 同片 UI 调整：移除仪表盘打开与详情占位操作

- 按用户指令再次检查共用容器行、详情操作及现有导航测试；本次修订优先于设计稿先前的按钮安排。
- 仪表盘挂载后不再新增行内“打开”按钮；容器管理列表保留此入口，详情仍可“浏览文件”。删除所有共用详情面板中的“在资源管理器中打开”和“更新诱饵内容”占位按钮。
- 安全审查：页面差异只由所在页面决定，不引入容器类型或档位判断；删除的是未接入的占位入口，没有修改任何原生调用或安全检查。
- 最小验证：现有 DOM 回归 24 项、页面契约 1/1、JavaScript 语法及差异格式检查通过。开发版构建成功，部署脚本与源文件一致。
- 未进行视觉验收；未提交、未推送。

#### 同片 UI 调整：移除展开箭头的黄色焦点框

- 对照用户截图检查全局 `button:focus-visible` 与详情箭头样式，确认黄框来自焦点轮廓，而非按钮背景。
- 按本次指令仅覆盖详情箭头的焦点轮廓：不显示外框，改用箭头本身变色提示键盘焦点；其他控件的焦点样式不变。
- 安全审查：仅一条 CSS 规则，无交互、原生接口或数据处理改动。
- 最小验证：页面契约 1/1、差异格式检查通过；开发版构建成功，部署 HTML 与源文件一致。没有新增或扩大测试范围。
- 未进行视觉验收；未提交、未推送。

#### 本轮提交决定

- 用户已明确批准将本轮未提交的 UI 调整合并为一个提交，包含共用容器列表、两级容器管理、详情/排序交互、后续按钮修订与配套验证文件。
- 提交主题：`[ui]unify container management and dashboard interactions`。正文以英文记录主要改动及最小验证，不添加共同作者尾注。
- 复用本轮已通过的页面契约、24 项 DOM 回归与开发版构建结果，不扩大测试范围。排序仍仅在当前会话有效；真实浏览器视觉验收限制继续保留。
- 本次只创建一个本地提交，不推送。

---

### UI：容器管理文件视图与右键重命名（2026-09-10）

#### 需求审计与切片边界

- 用户要求只调整容器管理页：移除列表中的移除与展开详情入口，单击容器进入文件视图；顶部保留原字号标题，右侧以较小字号显示可点击的逐级路径；文件表格以用户附图为准，仅留导出和红色删除图标，重命名改为右键菜单。
- 已核对既有 P3 文件管理范围、主需求 8.7.5 与现有页面/导航/文件操作测试；设计稿只参考文件表格构件，不引入其余说明。用户本次指令优先于旧的页面安排。
- 本片是容器管理的导航与文件操作呈现调整，不修改原生文件接口；仪表盘继续使用既有展开详情与移除交互。

#### 实现与决策

- 容器管理列表的行体可点击，也可用 Enter/空格进入；挂载操作和拖拽手柄独立工作，不再显示“打开”、移除或详情箭头。
- 顶部统一为“容器管理 > 容器名 > 目录…”；标题保留 1.25rem，路径为 .875rem。包括当前目录在内的每一级都可点击，标题返回容器列表。路径只显示容器内相对层级，不暴露容器文件的绝对路径。
- 文件表格改用独立样式：圆角边界、分隔行、文件夹/文件图标、链接标记、等宽大小及分钟精度的时间；右侧只显示导出和红色垃圾桶图标，通过悬浮提示和可访问名称说明操作。
- 增加仅含“重命名”的右键菜单，支持 Shift+F10/菜单键、Escape 关闭及焦点返回；点击菜单外、滚动、窗口变化、切换目录或会话刷新会关闭菜单。执行前再次比对容器 ID、相对目录和文件名。
- 按用户要求移除文件列表下方的常驻说明；删除仍为永久删除且无二次确认，说明改为图标悬浮提示“永久删除，不进回收站”。这与主需求中既有“常驻提示”呈现记录不同，已在此记录，没有改写删除语义。
- 跨容器拖放目标仅在拖动期间显示；拖出容器仍拒绝并指向导出与迁移。新增菜单不提供任何原生导出捷径。

#### 最小验证与安全审查

- 页面契约目标重新构建后 CTest 1/1 通过；DOM 回归 32 项通过，新增覆盖管理页入口移除、顶部逐级导航、纯图标操作、右键/键盘菜单与导航后菜单失效。
- JavaScript 语法及 `git diff --check` 通过；按用户要求不跑全套测试。P2 开发版重链接被正在运行的 EXE 占用阻止（LNK1168），本轮不能记为构建成功。
- 本次没有原生代码变化，已直接执行与构建后部署相同的两份 UI 资源复制，并核对部署 HTML/JS 的 SHA-256 与源文件一致。未终止正在运行的应用；关闭后重启即可加载新版页面。
- 文件名、路径与菜单目标保持纯文本/内存状态；没有新增持久化记录、档位分支或敏感缓冲区接触。重命名、删除、导出继续走现有受限入口，菜单中的过期目标不会沿用。
- 浏览器视觉与真实指针命中尚未验收，仍受此前本地预览策略限制；DOM 测试不代表像素级验收。
- 提交状态：未提交，等待用户决定；未推送。

#### 容器管理后续调整：工具栏、图标与手动刷新

- 按用户最新指令检查现有工具栏、命名弹窗、标题继承样式、目录读取逻辑及相关最小测试；沿用容器 ID 与相对目录寻址边界，不新增原生接口。
- 新建文件夹移至工具栏左侧；删掉命名弹窗中关于分隔符、冒号等实现约束的说明，原生名称校验与失败反馈保持不变。
- 导出箭头逆时针旋转 90 度朝上；导出和删除改为无背景、无边框图标，删除保留红色和可访问名称。
- “容器管理”标题按钮继承标题字号、字重及字间距，文字颜色与操作日志、导出与迁移标题一致；仍保留返回列表行为。
- 新建文件夹右侧增加带背景的刷新图标，重新请求当前容器的当前相对目录，用于显示系统资源管理器中的外部修改。读取期间禁用刷新并设置忙态，使用请求版本号保护并发导航与迟到结果。
- 最小验证：DOM 回归 35 项、页面契约 1/1、JavaScript 语法和差异格式检查通过；新增模拟外部新增/删除后刷新、保持当前路径与读取期间禁用的验证。未跑全套。
- 安全审查：刷新只调用既有受限目录读取接口；不记录目录或历史，不增加磁盘写入，也未放宽命名校验或文件操作权限。
- 本次仅 UI 资源变化，已同步到 `out/build/dev`，HTML/JS 哈希与源文件一致；未重链接 EXE，未终止正在运行的应用。真实视觉验收仍待执行。
- 此条为未提交切片的用户修订，未提交、未推送。

#### 容器管理后续调整：文件工具栏挂载/卸载

- 按用户指令核对文件工具栏、当前容器状态投影和既有 `runContainerAction` 流程，复用已审查的容器 ID 挂载/卸载入口，不改变原生权限或卷操作策略。
- 工具栏最右侧增加当前容器的挂载/卸载按钮，状态来自原生刷新结果，卸载使用既有红色样式。忙态或会话不可用时禁用。
- 操作后保持当前文件视图，挂载成功重新读取目录，卸载成功清空文件列表并显示就地挂载提示。未挂载时禁用新建文件夹与刷新。
- 点击卷状态切换时使先前目录请求失效，避免卸载期间迟到的目录结果恢复旧列表；失败时继续显示实际原生状态，不提前把按钮乐观切换为成功状态。
- 最小验证：DOM 回归 40 项、页面契约 1/1、JavaScript 语法及差异检查通过，新增覆盖当前容器挂载、忙态、卸载被拒与成功卸载后的列表清理；未跑全套。
- 安全审查：只操作点击时选中的容器 ID，未增加自动挂载、跨容器副作用、持久化信息或档位判断。失败仍经过既有安全卸载与错误反馈流程。
- 已同步开发目录 HTML/JS 并核对哈希；未重链接 EXE、未终止应用。真实视觉/硬件验收待执行；未提交、未推送。

#### 本轮提交确认

- 用户明确要求将本轮容器管理调整合并为一个提交，采用 `[ui]refine container file navigation and toolbar actions`，包含 UI、相关回归测试和开发记录。
- 提交沿用上述已通过的最小验证与安全审查结果，不重复运行全套测试；仅创建本地提交，不推送。真实视觉/硬件验收仍待执行。

### UI：导出与迁移布局和选择交互（2026-09-10）

- 需求审计：核对 `MILESTONES.md` 的 P3 导出迁移范围及 `需求梳理清单_v2.md` Q-04、8.6 约束；本次按用户要求调整页面交互，不改变明文导出、密文复制、挂载限制或密钥边界，无新增需求冲突。
- 实现前检查 `ui/card_session.html` 的导出行与目标位置结构、`ui/card_session.js` 的选择状态及浏览入口，以及现有 DOM 模拟和页面契约测试。确认折叠时勾选显示与切换判断不一致导致无法取消全选。
- 盘符和挂载操作靠右排列，最右勾选框继续与文件行对齐；密文迁移改用双向箭头，列表容器改用立体盒子图标。
- 两种目标位置框改为原生按钮，点击复用浏览入口，支持键盘操作并与浏览按钮保持相同忙态；取消选择保留原位置。
- 统一容器勾选框的显示和切换状态，折叠时可取消该容器的选择，不影响其他容器；展开后的全选与取消行为保持可用。
- 最小验证：DOM 回归 46 项通过，页面契约 1/1 通过，JavaScript 语法与差异检查通过；未跑全套。新增覆盖折叠选择/取消、跨容器隔离、展开取消、两个位置框和取消选择器。
- 安全审查：只复用既有目录选择入口，不自动执行导出、不增加目标路径持久化或日志；选择删除仍按容器 ID 限定，未修改原生权限或档位判断。
- 已同步 HTML/JS 至 `out/build/dev` 并核对哈希一致，未重链接 EXE。DOM 不验证布局，实际视觉与硬件验收待用户检查；本轮未提交、未推送。
- 提交确认：用户已明确批准提交，本轮以 `[ui]refine export and migration controls` 创建一个本地提交，包含 UI、回归测试与开发记录；沿用上述最小验证结果，不推送。

### UI：默认窗口尺寸与设置页尺寸选择（2026-09-10）

- 需求审计：检查 `MILESTONES.md`、`需求梳理清单_v2.md` 的 UI 范围与 R-03、8.6 约束，本次是用户新增显示偏好，无原生密钥、容器或持久化需求变化。按当前指令采用即时调整，不跨启动记忆。
- 实现前检查 `tools/app/main.cpp` 启动窗口、DPI 处理、WebView 安全设置，原生 COM 桥及其测试，以及页面侧栏 760px 折叠规则和设置页结构。原窗口以 900×640 外框创建。
- 默认启动后应用标准预设：1120×760 逻辑像素内容区，按窗口 DPI 换算并计入标题栏边框，居中且限制在当前显示器工作区。通常可完整显示左侧列名；屏幕可用逻辑宽度不足 760 时仍受既有窄屏折叠规则限制。
- 设置页增加标准（1120×760）、宽敞（1280×860）、最大化三个选项，立即生效；从最大化切回预设会恢复普通窗口，失败时恢复选项并显示错误。
- 新增独立窗口尺寸 sink 和 `setWindowSize` 原生桥入口，仅接受 0～2 三个整数预设，不开放任意坐标、尺寸或窗口句柄。保持 WebMessage 禁用与既有导航限制。
- 最小验证：DOM 49 项通过；原生桥测试与页面契约各 1/1 通过；JavaScript 语法和差异检查通过。初次测试暴露测试数字类型不匹配及集成页面缺控件，修正后通过。未运行全套。
- `dev` 开发主程序构建成功，UI 资源已随构建同步并核对哈希；未启动或终止应用。实际多显示器、缩放与视觉验收待执行。
- 安全审查：参数形状、负值、小数、超范围值在桥层拒绝，缺少 sink 与调整失败均不报告成功；调整仅针对主窗口，不新增日志、磁盘状态、秘密数据返回或档位判断。本轮未提交、未推送。

#### 窗口尺寸后续修订：默认宽敞、紧凑与深色菜单

- 按用户最新尺寸与截图要求复核启动预设、侧栏 760px 折叠规则、原生参数边界和相关测试，沿用本节需求审计，无安全范围变化。
- 宽敞改为 1400×860 并成为启动及设置页默认值；保留标准 1120×760 和最大化，新增紧凑 720×640，触发既有侧栏文字收纳。尺寸仍为逻辑内容区大小，并适配屏幕工作区。
- 窗口尺寸选择框显式采用深色配色，选项使用深色背景与浅色文字，修复原生下拉出现白底的问题。
- 最小验证：DOM 51 项、原生桥与页面契约 2/2、开发主程序构建均通过；新增默认预设与紧凑选择验证，并更新原生允许范围为 0～3，非法值仍拒绝。未跑全套。
- 安全复查：未新增任意尺寸入口、持久化或敏感信息；UI 资源随构建同步，真实缩放与菜单视觉仍待检查。本轮修订未提交、未推送。
- 提交确认：用户已批准先提交窗口尺寸功能，以 `[ui]add window size presets and spacious startup` 创建本地提交，沿用本节最小验证，不推送；操作日志开关作为后续独立切片。

### UI：会话设置操作日志开关（2026-09-10）

- 前置提交已完成：`dc11fec`，窗口尺寸功能独立提交，未推送。
- 需求审计：复核需求清单与里程碑的日志分离、会话内日志和否认性边界；用户新增“开启操作日志”设置仅作用于用户可见操作日志，不能关闭安全卡恢复日志或拔卡卸载。无新增持久化要求，采用当前应用运行期间的易失偏好。
- 实现前检查会话核心 `record_operation`、日志环形缓冲区、UI 控制器、COM 布尔设置入口、设置页状态刷新和相关测试。
- 会话设置增加默认开启的操作日志开关；关闭后停止新增操作记录并隐藏侧栏入口，阻止进入隐藏路由；保留已有日志，重新开启后恢复入口与后续记录。开关读写经原生认证状态校验，失败保持原生实际状态。
- 原生偏好只保存在运行内存，复用统一记录入口短路，日志种类与字段白名单不变；未增加任何配置文件、历史或目标路径持久化。应用重新启动恢复默认开启。
- 最小验证：DOM 55 项通过，会话核心、原生桥、页面契约各 1/1 通过；JavaScript 语法和差异检查通过。覆盖默认开启、关闭后操作仍执行但日志不增、重新开启记录恢复、入口隐藏/恢复、失败回退、认证和布尔类型限制。页面契约最初仍限定三个可编辑开关，更新为四个后通过；未运行全套。
- 安全审查：仅影响信息性操作日志，不改变卡恢复事务、安全卸载、卷操作结果、密钥生命周期或导出零痕迹约束；未增加档位判断。默认不支持设置的其他会话实现拒绝写入，不伪报成功。
- 开发主程序构建成功，UI 资源哈希与源文件一致；实际视觉/硬件验收待执行。操作日志开关本轮未提交、未推送。
- 用户后续修订：交换“登录后自动挂载全部容器”与“记住容器位置”，现为记住位置在前；检查现有设置结构后仅调整顺序，控件绑定和默认值不变，无新增安全影响。页面契约 1/1 与差异检查通过，HTML 已同步开发目录。
- 提交确认：用户明确批准本轮操作日志开关和设置排序一并提交，采用 `[ui]add operation logging toggle and reorder session settings`，沿用上述最小验证结果，仅本地提交，不推送。

### UI：含隐藏卷创建前诱饵密码确认（2026-09-10）

- 需求审计：核对需求清单 N-06、8.7.2 及末尾重复建 B 的密码覆盖风险讨论，沿用里程碑当前隐藏卷创建范围。风险是旧密码解锁信息被替换、此前外层数据可能永久无法访问，并非直接覆盖旧容器文件；按用户指令改为创建前独立确认，不改变原生改写机制。
- 实现前检查隐藏卷容量校验、创建按钮门控、密码字节转换与清理、会话失效时关闭创建流程，以及 DOM、页面契约、WebView 集成测试。
- 总容量和内层容量并排，容量提示缩短为“内层卷至多占总容量的 3/4。”；移除原来的卡记录重写整段确认文字。
- 参数页点击创建只打开重要提醒弹窗，固定提醒此前外层数据可能永久无法访问且无法通过本应用恢复，不按登录密码角色分支。展示当前诱饵密码，默认遮挡，眼睛切换显隐；用户勾选永久性后果确认后，才由弹窗按钮调用正式创建。
- 返回修改或重新打开时清除预览和勾选状态并恢复遮挡；确认后清除预览，原始输入仍按原有流程转换并清理；取消创建与会话失效复用统一清理。底层参数页在确认期间不可交互，弹窗支持 Escape 返回和 Tab 循环。
- 最小验证：DOM 61 项、页面契约 1/1、JavaScript 语法和差异检查通过；新增覆盖预确认无原生调用、默认遮挡、显隐、返回清理、重新确认及模拟原生失败后密码清理。DOM 测试补入 TextEncoder，契约更新为新增只读密码预览及第三个显隐按钮；WebView 集成脚本同步新确认步骤但未执行实机集成。未跑全套。
- 安全审查：复用唯一原生建 B 接口，容量与密码校验不变；新增预览只存在当前弹窗内存，不写日志、磁盘或剪贴板，离开即清除。没有新增角色验证、档位状态或恢复入口。
- HTML/JS 已同步开发目录且哈希一致，仅修改 UI，未重链接 EXE。真实布局、弹窗和硬件验收待执行；本轮未提交、未推送。
- 提交前风险修正：用户指出以原诱饵密码登录时，新密码会挤掉日常密码。复核 `CardRecordService::set_second_password` 与 `rewrap_card_record_v1`，确认改写只保留当前登录密码和新密码；前述仅涉及外层数据的表述不完整。最终弹窗统一改为“诱饵密码不一致可能使本卡原有密码失效，导致此前容器中的数据永久无法访问。请务必确认后再继续。”，不按角色分支，也不声称直接覆盖容器数据。
- 用户已明确批准提交本轮创建流程，采用 `[ui]confirm decoy password risks before hidden container creation`；最终文案调整复查页面契约与差异，沿用已通过的其余最小验证，仅本地提交，不推送。

### UI：创建容器动态执行反馈（2026-09-10）

- 需求与设计审计：查看 `usb-vault_ui-design.html` 第 19 页“创建执行中”，核对里程碑的同步调用及取消缺口，以及两种容器显示一致、不得暴露结构的边界。
- 实现前检查页面两种创建调用、COM 同步创建入口、原生 VolumeCreator 的进度回调和 helper 等待流程。底层存在格式化阶段回调，但页面链未接入，且原始阶段包含内外层信息；当前切片不直接暴露这些阶段、不编造百分比，不增加无法兑现的取消按钮。
- 沿用设计稿的步骤列表、活动圆环和进度条视觉，创建时隐藏参数表单与无效操作按钮，显示“准备创建 → 创建容器并挂载 → 确认结果”。前两种容器共用完全一致的步骤和文案；原生调用成功返回后才切换到确认结果。
- 使用无确定百分比的循环进度条、活动圆环与单调时钟耗时，进入原生调用前让渲染器有机会绘制执行页。成功、失败或关闭创建流程时停止计时并清理展示状态，恢复结果与操作按钮；支持系统减少动画偏好。
- 最小验证：DOM 66 项、页面契约 1/1、JavaScript 语法和差异检查通过。模拟延迟原生调用验证两种创建执行视图、等待时耗时更新，以及成功/失败后清理。未运行全套或真实大容量创建。
- 安全审查：未修改原生卷/卡操作、权限或取消机制；不记录名称、路径、密码、创建耗时或阶段到磁盘/日志；没有将内外层原生阶段暴露到页面，既有密码清理流程保持。
- HTML/JS 已同步开发目录并核对哈希，无需重链接 EXE。本切片是渲染器动态反馈，不是原生后台线程改造；真实 helper 同步执行时的窗口响应、动画和耗时更新仍需实机验证，准确进度及可取消创建仍未接通。本轮未提交、未推送。

#### 创建执行页后续修订：就地展示结果

- 按用户最新指令复核执行页收尾、表单显隐及已有测试，保持本节设计和安全边界：结束后不再返回新建参数页。
- 成功后停止动画并保留耗时，标题与步骤直接反馈“容器已创建并挂载／挂载成功”，进度条完成，底部仅留完成按钮。失败也保留执行结果、显示现有错误提示并允许关闭，不重新展示参数表单；关闭或开始新流程时清除结果状态。
- 最小验证：DOM 67 项通过，页面契约 1/1 通过，JavaScript 语法和差异检查通过。契约原来限制清空说明调用仅出现一次，按参数页与结果页两个用途更新。未跑全套。
- 安全复查：结果源自既有原生调用与刷新结果，不新增盘符/路径推测或持久化；会话失效仍关闭并清理流程。HTML/JS 已同步开发目录，实际效果待确认；未提交、未推送。
- 用户批准先提交当前动态执行与结果页，采用 `[ui]show container creation progress and inline results`，沿用上述最小验证；真实原生反馈作为下一独立切片，仅本地提交，不推送。

### UI：真实创建进度与后台执行（2026-09-10）

- 前置提交已完成：`c262314`，仅包含上轮动态执行和就地结果页；未推送。本节为用户随后授权的真实反馈切片。
- 需求审计：复核设计第 19 页、里程碑同步执行缺口、需求清单 R-03 与 8.6/8.7 的密码清理及两种容器一致展示要求。原有 `VolumeCreator` 已有格式化百分比回调，上层未传递；当前 FormatDLL 配置为快速格式化，故显示“格式化容器”，不声称全容量写入随机数据。
- 实现前检查创建 helper IPC、格式化回调、会话建卷/发布/挂载顺序、COM 密码参数消费、窗口 HID 生命周期及已有测试。授权由提权 helper 成功连接确认；文件分配由实际文件长度确认，不用请求容量替代。
- 原生反馈通过独立回调传递到原子快照，再由页面轮询：确认管理员权限、分配容器文件、格式化百分比、挂载与实际盘符。含隐藏卷的内部阶段仅在原生侧合成为按请求容量加权的格式化进度，页面不接收内外层阶段/角色信息；不将阶段百分比当作包含授权和挂载的全流程百分比。
- 新增后台创建 COM 入口，原来的同步入口保留兼容测试。后台线程初始化 COM，独占会话；原生进行中只允许读取进度，其余桥调用返回忙态并清理传入敏感参数。线程结束前清除回调，宿主销毁时等待线程退出；页面在原生开始接收后即清除密码数组，再等待进度结束。
- 创建期间主窗口继续响应绘制和调整大小，阻止直接关闭。经过既有 HID 校验的事件暂存，拔卡设置中断标记，在发布/挂载前拒绝继续并走临时文件清理；创建结束后按拔出、重新插入顺序执行既有会话清理与强制卸载，保留卸载失败告警。完成快照在认证结束后不可读取。
- 最小验证：DOM 69 项通过；创建协议、会话核心、WebView 原生桥、HID 通知、页面契约五个相关测试均通过。新增后台创建实时快照、忙态并发拒绝、认证结束快照拒绝、含隐藏卷后台六参数与清理、发布前中断清理、原生阶段百分比及实际盘符展示验证。初次会话测试暴露无反馈时新增文件大小查询改变顺序，已改为仅反馈启用时查询，复测通过。未跑全套或真实卷创建。
- 安全审查：回调仅带阶段、百分比和已确认盘符，不带路径、卡标识、密码或档位；新位置副本清理，密码仍由 SecureBuffer 和原有消费/清理逻辑管理；无进度/耗时持久化或导出日志。后台不并发修改会话，UI 不乐观宣称权限/分配/挂载成功。
- 开发版首次链接因运行中的 EXE 被占用而失败，用户关闭应用后重新构建成功，包含 helper 与主程序；UI 文件哈希与源一致。实际 UAC、多显示器和真实标准/隐藏卷进度验收待执行。
- 剩余限制：未接通用户主动取消创建；拔卡不能立即中断正在执行的底层格式化，等待该调用结束后在发布/挂载边界阻止继续，并处理会话卸载。这一同步底层调用边界仍需后续取消协议完善，不能将本次改动宣称为即时中止。当前真实反馈切片未提交、未推送。

### UI：创建完成步骤对勾样式（2026-09-10）

- 按用户截图及设计第 19 页复核完成项展示，检查现有步骤状态与样式：原 `.done` 将整行文字改为绿色，序号仍然显示。
- 完成步骤改为淡绿色背景圆形对勾，步骤名称恢复普通文字颜色，右侧状态使用次要文字颜色；仅调整样式，沿用真实完成状态。
- 最小验证：页面契约测试 1/1 通过，差异检查通过；HTML 同步开发版目录。安全复查：不改原生逻辑、敏感数据或权限边界。未提交、未推送。

### 诊断：新建容器无法主动卸载（2026-09-10）

- 复核需求及里程碑的普通卸载重试与拔卡升级边界，检查会话、驱动、COM 桥和页面卸载路径。页面将卸载异常统一显示为文件占用提示，该文案不能单独证明驱动返回 busy。
- 在当前用户身份下只读查询驱动，确认用户指定容器仍挂载；只读枚举文件句柄发现 Windows COM Surrogate 持有对应卷根目录句柄，未发现应用自身的匹配句柄。外部占用是当前最直接线索，但未获取应用本次卸载的原始错误码，尚不能断言唯一根因。
- 命令行非强制卸载探测被自动审批拒绝（当前用户仅授权诊断），未执行；改用只读诊断，未结束进程、关闭用户句柄、强制卸载或改动容器。建议用户关闭相关资源管理器/文件选择窗口后在应用内重试。
- 本次无代码修改，不跑无关测试；不持久化容器路径或文件内容。释放占用后的实机重试待用户确认，未提交、未推送。
- 后续用户明确授权普通安全卸载；核验目标映射后执行非强制卸载，驱动返回 6（volume still has open files），复查仍挂载，确认真实文件占用而非仅页面提示。未升级强制卸载。
- 核对占用 COM Surrogate 的注册，实际承载 Plan9FileSystem（vp9fs.dll），同时系统存在运行中的 WSL 进程；后台文件系统占用比资源管理器窗口更符合当前证据，未关闭 WSL 或相关进程。
- 用户授权关闭 WSL 后重试：`wsl --shutdown` 正常退出，WSL 工作进程结束；重新核验目标映射并执行非强制卸载，驱动返回 0，挂载列表确认目标盘符已消失，并通知 Shell 刷新。关闭 WSL 后占用解除，验证本次失败与 WSL 后台占用有关；未使用强制卸载，未修改容器内容。应用会话可能仍保留外部卸载前的显示状态，本次无代码变更、未提交。

### UI：创建步骤对勾居中与完成态动画（2026-09-10）

- 按用户截图及既有设计第 19 页要求复核进度展示，检查步骤 CSS、轮询状态更新和已有 DOM 测试。原图标采用 grid，隐藏序号文本与伪元素仍参与布局，改为 flex 容器与绝对定位对勾，避免对勾偏下。
- 完成态显式清除动画及旋转，旋转选择器排除完成态。现有轮询已经在同一同步调用中更新状态类、右侧文字与 aria-current，未复现文字完成但仍旋转的情况，故保留真实反馈逻辑并加强样式约束。
- 新增快速创建跳过中间轮询直接进入挂载的回归验证：前置步骤均为 done、无 aria-current、动画为 none，分配/格式化文字完成，仅挂载项 current。DOM 70 项和页面契约 1/1 通过；差异检查通过，HTML 同步开发目录。
- 安全复查：仅样式及测试，不改变创建、挂载、权限或密码处理；原生 WebView 中的实际渲染仍待用户确认。未提交、未推送。
- 用户批准提交当前真实创建反馈及后续完成步骤样式修正，合为一个 `[ui]report real container creation progress in the background` 提交；沿用已通过的开发版构建、五项相关原生测试及最新 DOM 70 项验证，不重复全套测试。已复核差异，不推送。

### 测试版交付第一步：主 EXE 无参数启动（2026-09-10）

- 需求审计：用户选定先替代启动脚本，后续再扩展配置器及制作交付包。本轮复核现有 P2.18 打包记录、里程碑、需求清单 R-03/8.6，以及联网开发模式、凭据管理器和固定本地磁盘的现有边界；不改变生产/开发模式区分或卡记录。
- 实现前检查主程序参数解析、启动脚本固定摘要、打包构建、凭据配置器及 VerifiedLocalArtifact 文件与目录锁定机制。主 EXE 无参数时以实际模块目录定位 helper 和 vendor，不使用工作目录、PATH 或可修改的摘要配置文件；保留原六参数显式开发入口兼容本地脚本。
- 新增构建时生成部署摘要头，依赖 helper、建卷 helper 与页面源文件；将实际构建摘要嵌入主程序，格式化及厂商 DLL 保留已审计固定摘要。无参数入口在加载页面和访问卡之前校验六个组件，整个程序生命周期保留只读验证句柄，拒绝替换、重解析路径及非固定本地磁盘。异常提示完整解压与配置入口，不暴露凭据。
- 打包快捷脚本现在仅转交无参数主 EXE，说明和测试须知改为双击主程序；本轮仍是完整目录部署，不声称真正单文件运行。已将固定摘要匹配的厂商 DLL 复制到开发版 vendor 子目录，可供无参数启动验收。
- 最小验证：首次构建因命令行代码页 936 被仓库检查拒绝，改为既定 UTF-8 代码页后开发版构建成功。配置解析及完整性测试 2/2 通过，新增含中文/空格安装路径、固定子路径、错误目录及摘要拒绝测试；六个部署组件摘要与构建身份核对通过，打包 PowerShell 语法与差异检查通过。未重复全套测试，未操作真实卡或创建容器，无参数图形界面及干净机器验收待执行。
- 安全复查：摘要在构建时固化，运行时不按现有文件自签认可；复用文件/父目录锁定与路径校验，新增路径不含密码或档位。保留的带参数模式仍是原有显式开发接口；程序尚未签名，嵌入摘要不能防止攻击者替换整个 EXE。配置/检测向导与最终发行包属于后续切片，本轮未开展。当前未提交、未推送。

- 用户批准提交无参数启动切片并继续配置器阶段；沿用本节最小验证，以英文 [ui] 提交，不推送。

### 测试版交付第二步 A：配置器只读检测与启动（2026-09-10）

- 前置提交完成：`7f1a2e3 [ui]enable direct launch of the development application`，未推送。用户授权继续下一步；按独立可验收边界先完成配置、只读检测和启动入口，依赖安装自动化及可选临时容器写入测试留在后续切片。
- 需求审计与实现检查：沿用上一节 P2.18、R-03 与 8.6 审计，检查原生配置器字段读取/清空、凭据管理器、严格只读插卡 inspection/router、驱动查询、HID 探测与 WebView2 运行时查询。检测不调用初始化、bootstrap、卡记录更新或挂载/卸载接口。
- 扩展独立配置器：保留三字段掩码及 SecureBuffer 输入路径；保存后清空并提示检测。已保存配置可直接检测，尚未保存输入不会被检测静默丢弃。检测使用后台线程，窗口逐项显示七项结果：系统架构、运行组件、WebView2、驱动、凭据、卡连接、卡访问；全部通过后才启用启动按钮。编辑配置会作废通过状态，运行期间禁用输入/重复检测并阻止提前关闭。
- 运行组件校验覆盖主 EXE 及六个依赖，配置器嵌入依赖主程序构建结果的摘要；检测期间保留验证句柄，启动前再次验证主程序。卡访问仅在组件、凭据、插卡三项均通过后执行一次，复用既有有超时 helper；失败不自动重试。只呈现正式初始化/登录/不可用路由，不导出硬件标识或私有卡证据；区分认证失败、锁定与卡不可用的通用错误。
- 最小验证：初次编译发现缺少显式 COM 头及 WebView2 包含顺序问题，补正后配置器最终构建成功；配置/检测调度、凭据和只读入口路由三个测试通过。新增三项前置失败必须跳过卡访问、全通过可继续、异常消息不透传的确定性检查；配置器所嵌入的主程序摘要与产物一致，差异检查通过。未执行真实卡访问、凭据写入、图形窗口验收或全套测试。
- 安全审查：线程仅共享非敏感显示结果，原子完成状态发布后才读通过标记；窗口生存期覆盖工作线程，UI 定时取结果不阻塞等待 helper。密码字段保持既有掩码和清理流程，不新增密码、路径或卡身份日志。检测报告仅在内存中；检测“通过”只是该次快照，实际主程序仍重新执行自己的校验和认证。
- 使用说明同步为保存、插卡、检测、启动；本轮不自动安装依赖或进行临时容器读写测试，缺失依赖按随包测试须知处理。真实插卡/无卡/缺依赖及 DPI 界面验收待用户测试。当前配置器切片未提交、未推送。
- 用户批准提交配置器只读检测切片并继续下一独立切片：依赖安装入口。沿用已通过验证，本地提交，不推送。

### 测试版交付第二步 B：依赖安装入口（2026-09-10）

- 前置配置器只读检测已按用户授权提交：`8e76dc3 [ui]add configuration diagnostics and application launch`，未推送。本轮为用户授权的下一独立边界：配置器内的依赖安装入口。
- 需求审计与检查：复核 P2.18 打包安装器的固定摘要、签名及来源边界、现有配置器检测线程与启动状态，沿用 R-03/8.6；查阅微软 WebView2 官方分发说明（https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution），采用官方认可的下载页引导方式，不引入未经审计的自动下载执行链。
- 配置器新增“安装 VeraCrypt（管理员）”与“获取 WebView2（微软官网）”。前者仅在用户点击后后台校验随包固定路径安装器的 SHA-256 和 Authenticode 信任链，通过后 runas 启动交互式官方安装器，保留文件/目录验证锁直到进程退出；用户取消 UAC 单独反馈。后者仅打开固定 HTTPS 微软下载页，不下载、不运行浏览器获取的文件。
- 安装期间与检测/保存/启动互斥，窗口保持响应并要求先完成或退出安装器再关闭；安装器退出只显示任务结束并要求重新检测，不将退出码当作安装成功。进入安装或下载入口时作废旧的通过状态。更新说明，正常首次配置不再要求用户手动执行 PowerShell，原验证命令作为可选核查保留。
- 最小验证：配置器构建通过；配置/检测调度及新增安装器校验测试 2/2 通过。安装器测试覆盖文件缺失、未审计字节拒绝；使用已有受审计原版安装器实际验证固定摘要及 Authenticode 成功，未执行安装。已将同一安装器复制到开发目录 prerequisites 并复验。后续收紧测试临时目录所有权，避免碰撞时清理非本次夹具，复建及该测试通过；差异检查通过。
- 安全复查：安装器路径/摘要/官网地址固定，不接受用户命令文本，不拼接 shell 命令，不静默提权；固定摘要也固定受审计签名者对应的原始字节，运行时额外验证证书信任链。验证失败不启动，验证句柄跨启动/等待存活，凭据不传给安装器或浏览器，诊断结果不落盘。
- 剩余验收：本轮没有实际安装/卸载驱动、打开浏览器或修改本机依赖；真实 UAC 取消、安装窗口退出、重启和干净机器验收待执行。WebView2 仍由用户在官网下载安装，临时容器读写自检及最终测试包留待后续切片。当前未提交、未推送。
- 用户批准提交依赖安装入口，并明确最终交付为包含所需内容的单一压缩包；下一切片按完整离线依赖打包推进。

### 测试版交付第三步 A：单 ZIP 与完整安装依赖（2026-09-10）

- 前置依赖入口已提交：`c3e23ab [ui]add verified prerequisite installation entry points`，未推送。用户明确最终采用一个压缩包发给测试客户，按此优先补齐 WebView2 离线安装器与完整包；可选临时容器读写自检仍为后续独立切片。
- 需求审计与检查：复核现有打包白名单、Release 静态运行库配置、P2.18 第三方说明和配置器安装边界。微软官方提供 Evergreen Standalone Installer x64（https://go.microsoft.com/fwlink/p/?LinkId=2124701）。自动下载因连接/传输超时未完成，用户提供完整文件；以用户文件为准，没有将自动下载的部分文件入包。
- 验证用户文件：258614480 字节，Authenticode 有效，签名主体 Microsoft Corporation；固定 SHA-256 为 E7FA35755196AD9223596EF021A1CE6799509142EAA40BA35F634026BE50B831。安装器外层产品元数据为 Microsoft Edge Update 1.3.265.7，不将该版本误报为内含 Runtime 版本。
- 配置器将官网按钮改为随包 WebView2 离线安装入口；与 VeraCrypt 共用先固定摘要/签名校验、保持验证锁、用户点击才请求 UAC、等待退出后要求重新检测的机制。安装器大小上限调整为 512 MiB，仍拒绝超限/缺失/修改文件；没有执行任何真实安装。
- 打包新增必填 WebViewInstaller，读取源码中固定摘要并验证微软可信签名后才收包；静态 MSVC Release 主程序与配置器构建通过。两种安装器缺失/替换拒绝测试及两套真实安装器仅校验测试通过。说明同步为完整 ZIP，明确供应方凭据仍需单独配置，签名验证与厂商开发卡访问可能联网，不承诺整个应用离线运行。
- 候选包：out/packages/usb-vault-p2-test-0.2.0-preview6-full.zip，约 276 MiB；SHA-256 为 7D85301DF6EFC2879C1B32700CCC723E42B1E75AE27B8EF3798AA0CD370EFED3。直接读取 ZIP 逐一验证 20 个清单文件及清单自身的唯一性，无多余条目，没有测试容器、密钥文件或 PDB。构建身份如实标记 c3e23ab + dirty；本轮改动未提交，因此不将候选包描述为已提交发行版。
- 安全/剩余风险：只打包显式白名单，未包含用户凭据或容器；第三方原文件未修改，安装器执行仍需要明确用户点击。尚未实际执行两套安装器、干净机器或完整用户流程验收，也未增加临时容器读写自检。既有 THIRD-PARTY-NOTICES 的 ViKey DLL 再分发授权待确认仍适用，生成候选 ZIP 不等于对外分发获批。当前改动未提交、未推送，未发送给任何客户。

### 交付包精简（2026-09-10）

- 按用户要求检查 preview6 文件清单，移除交付 ZIP 中的 README、测试须知、第三方说明性 README、兼容启动脚本及构建/摘要记录；第三方许可、NOTICE 和补丁保留在 licenses，程序与两套安装器保留。源码说明文档不删除，构建身份及摘要清单改存 ZIP 外用于内部校验。
- 配置器缺依赖文案改为指向安装按钮，不再让用户查已移除的须知。Release 重建通过，重新生成 preview7-clean；ZIP 逐文件摘要校验通过，并确认没有被移除的文档或额外文件。仅作最小构建和包校验，不重复全套测试。
- 安全复核：未移除任何运行时校验或许可文本，不添加凭据/测试容器，不改变安装授权流程。新包 out/packages/usb-vault-p2-test-0.2.0-preview7-clean.zip，SHA-256 A3E85F421C8AE8CB590DE457DC45F2533F882B8EE6DF2D6BBBD471BCEFE1D19F。旧包保留，用户应使用本次新包。当前修改未提交、未推送；既有干净机器验收及厂商再分发授权待确认事项不变。
- 用户批准提交离线 WebView2 接入与交付包精简改动；沿用 Release 构建、安装器校验与 preview7 的 15 文件 ZIP 校验结果，合为一个英文 [ui] 提交。既有 ZIP 不重写，外部构建身份保留生成当时的 dirty 记录；不推送。

### 配置器卡访问错误细分（2026-09-11）

- 用户要求一次提示尽量定位问题；已审计需求的厂商中立错误边界、R-03 与卡状态不可泄露约束，并检查 SDK 映射、helper 协议、配置器及现有测试。工作树开始时干净。
- 追加稳定错误分类，区分 API Key 拒绝/未设置/计算失败/校验失败、UserSN 拒绝、未激活、多卡和响应超时；同步 helper 接收范围，保持原有错误编号不变。厂商原始状态仍限于适配器。
- 开发配置器采用固定中文提示及 CARD 编号，明确失败类别与处理动作；SDK 找不到卡不冒充确定的 UserSN 错误，计算/通信错误不冒充确定的密钥错误。卡内容状态仍保持统一拒绝，不展示内部记录或身份。
- 不输出 exception.what()、密钥、PIN、卡内容或硬件标识，不自动重试、不初始化、不改卡；当前修改待构建及测试，未提交、未推送。

- 验证：MSVC review-clean 下 SDK 映射、会话调用顺序/失败清理、helper 协议边界、配置器错误反馈四组测试通过；Release 配置器、主程序、helper、creator 均构建成功，部署摘要自动重生成。git diff --check 通过。
- 集成验证限制：vikey_helper_process_tests 在 development vendor runtime staging 遇到既有路径安全门（local artifact directory resolves through a redirection），尚未到新增超时断言；改 TEMP 及 Windows 原生参数路径仍复现。未放宽安全检查，未把该套测试记为通过；实际用户卡/API Key 测试待手工完成。
- 完整候选包：out/packages/usb-vault-p2-test-0.2.0-preview8-diagnostics.zip；SHA-256 B61D2CC4C8A4A19983BF5C1054E16119E3D4954CD2B5B33F3DDADFCFB34B8C00。ZIP 全条目与外部摘要清单逐项校验通过，未夹带凭据或测试数据；旧包保留。构建身份为 d8cad6c + dirty，当前未提交、未推送、未向客户发送。

- 2026-09-11 文案复核：按用户要求精简 UserSN、API Key、用户 PIN 校验失败提示，并缩短找不到匹配卡的提示。保留 CARD 编号和不确定性边界，不改错误映射、凭据操作及安全检查；仅固定文本变更，不新增测试。待现有反馈测试验证后发用户确认，暂不重新打包或提交。
- 文案验证：configuration_card_failure_tests 重新构建并运行通过；固定字符串无敏感信息，安全边界不变。修改后的文案已准备供用户确认，preview8 仍为上次构建。

- 用户确认精简文案并要求打包：Release 配置器重建通过，生成完整 preview9-diagnostics ZIP；15 个文件摘要及配置器与本次构建的一致性验证通过。SHA-256：D1EECF3887DD20599729ECE114BB7523609FA92A66F236429D2E1BE6FF45B3CC。沿用上一轮文案测试，未重复无关测试；辅助程序路径集成验证限制及真卡复测待完成事项不变。包无凭据或测试数据，未提交、未推送。

### CARD-1 诊断全链路与反馈审查（2026-09-11，待用户审文案）

- 用户授权细化 CARD-1 及其他粗粒度提示，明确先不打包，完成后提供全部文案。复核既有需求的厂商中立边界、密钥生命周期与卡状态隐私要求；延续未提交的诊断切片，不改初始化或卡操作授权。
- 新增固定诊断阶段及带来源类型的数值码，保持既有 CARD 分类。helper 协议升级为 v12，错误响应保留阶段与系统码，拒绝未知阶段/类型及成功响应夹带诊断数据；不将任意 exception.what()、路径、凭据、卡内容或厂商原始状态新增到界面。
- 覆盖凭据字段读取/格式、文件/摘要/路径、SDK 暂存/加载/接口、安全环境、进程启动/退出、IPC、超时和清理。SDK 操作带阶段，API Key 两个计算错误保留区别；组件提示指出固定组件名，安装器签名/启动/退出分开处理并识别重启与取消。
- 本机集成失败已查明：仅用于 out 下的探针证实 LocalAppData 新建目录实际被重定向至 Codex MSIX 的 LocalCache/Local；这是真实路径差异，未放宽生产路径安全检查，不能据此推断客户也有相同原因。临时探针不属于正式代码或构建目标。
- 初轮六组单元/协议/配置回归通过；正式 Release 二进制编译通过。后续细节变更将完成复验；没有运行真实安装器、读客户凭据或访问真实卡。仍未打包、未提交、未推送。

- 最终验证：八组回归（凭据、安装器、SDK、会话、卡记录 IO、文件校验、协议、反馈）均通过；Release 正式二进制重新构建通过，diff --check 通过。安全复查确认成功响应不能夹带错误元数据，未知阶段/数值来源被拒绝，错误原文不跨到界面，输入缓冲仍按原机制清理，无路径安全放宽、无自动认证重试。完整文案整理至 out/reports/配置器完整文案确认稿-2026-09-11.md，包含 18 类基础错误与 38 个诊断阶段及其余流程提示。按用户要求不打包、不提交；白屏另案本轮未修改。

- 用户授权生成新包：preview10-diagnostics 完整 ZIP 已生成，15 个文件摘要及四个 EXE 与本次 Release 构建一致性校验通过。SHA-256：566BA3CE2B1571BE20D3342772556622DEFBA32777DC23961289335D3CB86E6E。包含 helper v12 诊断阶段传递、细化反馈和安装退出结果；白屏问题未包含修复。既有八组测试通过，普通桌面完整集成与真卡复验仍待完成，未提交、未推送。

- 用户已批准提交当前工作区：将诊断分类、helper v12 元数据传递、配置器文案与安装结果反馈及相关测试纳入本次提交。沿用八组回归、Release 构建及 preview10 包校验结果，本次差异检查通过；白屏修复尚未实施，完整桌面集成与真卡复验仍待完成。不推送，现有 ZIP 保留打包时的 dirty 构建身份。

### 客户白屏定位与建卷取消（2026-09-11）

- 客户设备上配置器七项全通过、主 EXE 白屏。本机复现定位：`tools/app/main.cpp:174` 用 `UrlCreateFromPathW` 生成页面 URI，非 ASCII 字符原样保留，而 WebView2 在 `NavigationStarting` 回报的是 Chromium 规范化后的百分号编码形式；`:475` 的白名单做字符串全等比较，不等即 `put_Cancel(TRUE)`，把自己的首次导航取消掉，窗口类未设背景画刷因而呈纯白。同一套 preview10 文件在 ASCII 目录正常、在中文目录纯白，Edge headless 打印出两种 URI 形式的差异。另测得 `café` 这类拉丁重音路径被编成 ANSI 的 `%E9`，文件根本加载不到，说明 URI 生成本身也要修，不是只放宽比较。配置器七项均与路径编码无关，因此必然是「全绿 + 白屏」。规避：测试包移到纯 ASCII 短路径。本轮未改代码。
- 文件类型图标按 P3.18 对照表落地：`ui/card_session.js` 新增十一类 `kFileTypeIcons` 与 `fileTypePaths` 分类器，复用既有 `exportGlyph` 与 `kFilePaths`，文件浏览与导出选择器共用同一分类；契约测试补五条（每行一个图标、目录优先于扩展名、未知扩展名落兜底、分类不向桥多要字段、页面不加载外部图片）。可执行图标按产品真实尺寸渲染后由用户改判为四根引脚，容器类保留。提交：`36e84e2 [ui]draw a type icon for every file in the list`。
- 建卷取消先查实句柄竞争：`spike/_vcref/src/Common/Format.c:377` 显示格式化期间标准容器以 `dwShareMode = 0` 独占打开，隐藏卷分支也不含 `FILE_SHARE_DELETE`；若父进程靠拆管道取消再删 `.preparing`，必然踩共享冲突。且助手自身的 `delete_target_as_caller` 依赖 `ImpersonateNamedPipeClient`，管道一关就无法模仿，两头都清理不掉，残留的 `.preparing` 又会被建卷前置检查当作 `file_exists`，永久堵死同名容器。故改为带内协作式取消，不拆管道。
- 协议新增 24 字节 `CancelRequest`（nonce 绑定）：父进程在进度回调里发一次并继续读，助手在 `FormatBackend::progress` 每个进度点 `PeekNamedPipe` 查询，命中即回 `FALSE`，落进它已有的 `catch` 走自清理并回报 `operation_cancelled`；父进程新增助手退出等待（上限 30 秒），先关管道、再等退出、最后才清理。提交：`76c856c [creator]let a running creation be called off`。
- 会话层与页面：`CreationCancellation` 贯穿 `CardLoginMountSession` 与 UI 会话控制器；桥新增 `cancelContainerCreation`（DISPID 50），忙态守卫对它与进度查询放行；进度面板新增唯一的取消控件，窗口关闭由「不许关」改为询问；取消是第三种结果（status 4），页面显示「已取消创建」而非失败。助手已写完但取消仍未消费时，会话层在 publish 之前抛出，容器被删而不是发布。提交：`4fa4e03 [ui]offer a way out of a running creation`。
- 验证：协议、页面契约、宿主对象、会话四组目标测试通过，DOM 导航测试通过。WebView2 集成夹具在改动前后同样以 `fail-settings-remember`（退出码 23）失败，属既有问题，与本次无关。真实建卷取消需插卡实机验证，尚未执行；全套测试留待最后一并跑。未推送。
- 环境记录：本机新链接的 EXE 首次运行会被安全软件挡住数十秒，表现为无输出且主线程停在 `Wait/UserRequest`，重跑即恢复；排查时曾据单次结果误判为回归，后续回归对比一律跑够两次。


### P3.19 建卷取消补强切片 1：准确报告取消与清理失败（2026-09-12）

- 用户授权按三个小切片补强建卷取消，每片只跑最小必须测试，不跑全套。本片仅处理结果分类与对应页面反馈，取消截止点及跨层边界回归留给后两片。
- 计划与需求审计：复核 MILESTONES.md 的建卷范围、2026-09-11 取消实现记录，以及需求 R-03、8.6、8.7.2；沿用密钥易失、不得持久化档位、创建反馈不按登录身份分支的要求。里程碑未细列新取消提交，本片以现有实现和开发记录确定进度，不改阶段范围。
- 实现前检查原生后台创建异常出口、会话发布前取消与清理异常、helper 的 cleanup_failed 返回、页面状态范围校验和两类容器反馈，以及宿主对象与 DOM 测试。确认原实现仅凭取消标记把所有失败归为 status 4，可能掩盖清理失败与无关故障。
- 原生桥改为按实际异常分类：仅 creator 错误类别的 operation_cancelled 作为已取消；helper 的 cleanup_failed 和会话的 ContainerCreationCleanupError 统一返回独立清理失败状态 5；其他异常保持普通失败，不读取取消标记来覆盖结果。错误类别与数值同时匹配。
- 页面允许状态 5，两类容器共用固定提示“创建未完成，未能确认临时文件已清理。请检查保存位置中的残留文件后再重试。”只有确认取消状态保留“没有留下容器文件”。不把异常原文、文件路径或内部阶段回传页面。
- 最小验证：重新构建 card_product_webview_host_object_tests 和 card_product_ui_page_contract_tests，2/2 通过；DOM 76 条检查通过（新增两类容器各三种结果）。原生覆盖有/无取消请求时的 helper 清理失败、会话复合清理异常、无关异常和同数值不同错误类别，另覆盖确认取消。DOM 初次发现页面旧上限拒绝状态 5，修正后通过，并复跑页面契约通过；git diff --check 通过。未跑全套、未访问真实卡或执行真实建卷。
- 安全复查：不增加文件删除操作、权限、持久化字段或敏感信息输出；清理异常优先于取消成功展示，失败不再被取消请求掩盖，既有密码清理保持。没有修改底层取消协议及发布/挂载时序。
- 剩余边界：取消截止点、发布竞争与窗口关闭交互留给切片 2；跨层完整边界回归留给切片 3；真实格式化取消、句柄释放及磁盘残留检查仍待实机验收。本片未打包、未提交、未推送，等待用户决定是否提交后再开始下一片。
- 用户已批准提交切片 1 并开始下一片；提交沿用上述最小验证结果，不打包、不推送。

### P3.19 建卷取消补强切片 2：发布前关闭取消入口（2026-09-12）

- 切片 1 已按用户批准提交为 `82614d0 [P3.19]distinguish creation cancellation from cleanup failure`，未推送。用户同时授权开始切片 2，继续执行每片最小必须测试的约束。
- 计划与需求审计：沿用三片边界及 MILESTONES.md 的独立建卷范围、需求 R-03/8.6/8.7.2；本片只明确取消截止点和交互，完整跨层回归留给切片 3。检查两类会话创建路径，均在临时文件清理作用域内、publish_created 之前发送阶段 4；helper 的原始阶段不会直接成为该发布通知。
- 实现前检查 CreationWorkState、原生 cancelContainerCreation 与后台进度回调、WM_CLOSE、页面轮询/取消按钮，以及既有宿主对象、会话和 DOM 检查。原实现的独立取消布尔值与发布前查询存在竞争，窗口确认期间也可能跨过发布边界。
- 引入仅在内存中的原子阶段：空闲、可取消、已请求取消、正在完成。页面和窗口调用同一 request_cancel；发布前阶段 4 用同一原子值转换到完成阶段。取消先到则回调抛出确认取消异常，沿用会话的 staging 清理；发布先开始则拒绝过晚取消，不再修改取消状态。任务完成/启动失败清空阶段，新任务重新开放取消。
- 桥返回取消请求是否接受。页面收到阶段 4/5 或原生拒绝时禁用取消按钮并显示“正在完成创建，已无法取消”；请求通信失败允许重试，收到请求确认仍等待实际结果，不提前宣称取消成功。
- 窗口关闭共用原生取消门槛：已取消时提示等待结果，完成阶段不再询问取消；确认框停留期间错过截止点会明确提示。删除“取消不会留下容器文件”的无条件承诺，保留创建结束后才允许安全关闭的既有规则。
- 最小验证：宿主对象、页面契约、会话测试 3/3 通过；DOM 81 条检查通过，新增请求失败重试、接受后等待结果、下次创建恢复、过晚拒绝和完成阶段禁用。原生新增最后查询之后/发布之前取消，以及发布先开始时页面与窗口均拒绝取消且最终成功的检查；保留切片 1 的异常分类回归。未跑全套。
- 构建验证：review-clean 的主程序目标因缺少 USB_VAULT_FORMAT_DLL 配置被预期部署守卫拒绝；不放宽守卫，改用已有 DLL 配置的 package-release 目录，Release 主程序构建通过（没有制作安装包或 ZIP）。随后仅修正一处 JS 注释，不影响已验证行为。git diff --check 通过。
- 安全复查：取消/发布顺序由同一个原子状态决定，不依赖页面禁用或轮询时机；两类容器共用发布前门槛，不增加档位判断、磁盘字段、敏感输出、强杀进程或文件删除路径；既有失败分类和清理异常保留。取消仍为协作式，不能保证即时打断无进度回调的底层操作。
- 剩余事项：两类容器完整跨层边界与重试回归留给切片 3；真实窗口确认、格式化取消、句柄释放和残留检查待实机验收。本片未提交、未推送，等待用户决定是否提交后开始第三片。

- 编号调整（2026-09-12）：用户要求评估 ui 前缀，不适用则续开 P3 编号。原子取消/发布边界超出纯 UI，因此将三个补强切片编号为 P3.19（结果分类）、P3.20（取消截止点）、P3.21（跨层回归），避开已用于文件类型图标的 P3.18，并同步 MILESTONES.md。P3.19 对应现有提交 7c57bd5，保留历史提交标题，不重写历史；待批准的第二片使用 [P3.20]，第三片使用 [P3.21]。本次仅编号与范围文档调整，差异检查通过，不新增或重跑测试；P3.20 仍待提交决定，P3.21 尚未开工。

- 用户最终编号决定（2026-09-12，覆盖上一条编号安排）：三片全部归入 P3.19，提交统一使用 [P3.19]，不占用 P3.20/P3.21。按明确授权仅改写第一片提交标题，原 7c57bd5 变为 82614d0，代码树未变；同步里程碑与切片 2 标题。用户已批准提交切片 2 并开始切片 3，沿用三组最小目标、81 条 DOM 检查和 Release 主程序构建结果，不跑全套、不推送。

### P3.19 建卷取消补强切片 3：两类容器跨层回归（2026-09-12）

- 用户授权将三片统一使用 [P3.19]，重写第一片、提交第二片并开始本片。第一片已改为 `82614d0 [P3.19]distinguish creation cancellation from cleanup failure`（仅改提交消息，树与旧 7c57bd5 相同），第二片已提交为 `d1ef656 [P3.19]close cancellation before publishing a container`，均未推送。
- 计划与需求审计：按 MILESTONES.md 中 P3.19 切片 3 的取消、清理失败、发布边界与重试范围补回归，保留 R-03、8.6、8.7.2 的易失凭据与对称反馈约束。检查现有会话 FakeRuntime 的创建/发布/清理/挂载事件、两种创建核心、桥与控制器、原生 fake 会话和 DOM 场景；确认原生取消覆盖主要落在标准容器，尚未串联同会话同名重试及残留拒绝。
- 会话回归：两类容器均覆盖格式化完成后的取消检查和发布前回调抛出取消，分别验证清理成功/失败；检查取消错误类别、复合异常保留原因、没有发布/挂载/收录、认证仍有效、临时状态与清理次数。清理成功可在同会话同名重试；清理失败则残留阻止同名创建，模拟显式外部清理后才重试成功（没有加入自动删除）。另验证发布后到达的取消不删除已完成容器。
- 原生桥回归：共用两类 fake 创建的暂停与发布边界，通过实际控制器/COM 接口验证两种创建请求的取消与失败分类、最后查询后的取消、过晚取消被页面/窗口共用入口拒绝；每种结果后在同一宿主、同名和同认证会话重试，确认旧取消/结果不污染下一次成功。含隐藏卷入口的口令数组同步检查已清零。
- 页面回归：两类容器的结果矩阵插入“取消后同名创建成功”，检查成功挂载结果及终态重置；保留失败/清理失败提示与前两片交互检查。
- 最小验证：重新构建并运行 card_login_mount_session_tests、card_product_webview_host_object_tests，2/2 通过；宿主测试整理缩进后重新构建并单独复验通过；DOM 85 条检查通过，git diff --check 通过。不跑全套，不重复未改动的页面契约，不重建产品、不打包。
- 安全复查：本片只改测试，不新增生产路径、权限、落盘字段、日志或敏感输出；夹具使用固定虚构凭据，残留清理仅操作内存 fake 状态；检查确认失败不能被取消标记掩盖、残留不被覆盖、两类入口对称。未发现需继续修改产品代码的问题。
- 验证边界：会话测试模拟文件/驱动，桥测试模拟卷会话，DOM 模拟原生返回；这是分层组合回归，不是实际 helper/DLL/卡/文件系统贯通的端到端验收。真实格式化取消耗时、句柄释放、残留文件及窗口确认仍待实机检查。
- 本片实现与审查完成，未提交、未推送，等待用户决定是否创建第三个 [P3.19] 提交。
- 用户已批准提交切片 3（2026-09-12）：将本片测试与开发记录纳入第三个 [P3.19] 提交，沿用上述两组目标测试与 85 条 DOM 检查结果，提交前差异检查通过；三片代码与自动化回归均已完成，实机验收仍待执行。不重跑全套、不打包、不推送。

### P3.20 挂载与普通卸载的弹窗反馈（2026-09-15）

- 需求与计划审计：用户批准先做挂载／普通卸载弹窗反馈，并指定本片为 3.20；手动强制卸载确认不在本片。复核里程碑 P1.2/P1.7 的普通重试与拔卡升级边界、P3.19 既有后台机制，以及需求清单 R-03、8.4、8.6 的锁页秘密与无档位、无本地持久痕迹约束。此次无需求冲突。
- 实现前检查：检查页面 runContainerAction、统一 unmount、各列表／文件工具栏入口，原生 COM 宿主、控制器与会话卸载路径、窗口插拔／关闭处理，以及现有原生、页面契约和 DOM 测试。确认原有异步 JS 调用仍在原生窗口线程执行驱动操作，单加动画不足以解除阻塞。
- 页面实现：主动挂载已收录容器、单卷卸载和统一安全卸载共用轻量模态弹窗与不确定型动画，不伪造百分比。先让出绘制机会，再启动原生任务；等待期间禁用重复动作，Tab/Escape 保持在进度弹窗；任务完成后关闭、刷新真实状态，失败保留实际卷状态并显示反馈。普通卸载错误不再无条件断言文件占用。补上统一卸载失败在仪表盘的可见提示。
- 原生实现：新增受限开始／结果查询接口，工作线程只携带操作类型和容器 ID，沿用已审查的控制器及普通卸载策略。与建卷共享会话互斥、宿主销毁 join 和窗口插拔延迟处理门槛；运行期间只允许读取对应原子结果，拒绝其他会话调用及建卷取消。处理中的窗口关闭提示等待，已观察到拔卡则结果不报成功，延迟卡事件处理前拒绝新任务。
- 最小验证：宿主对象测试通过，新增真实工作线程与窗口线程分离、三个入口等待、并发读取／重复卸载／创建进度／取消拒绝、失败与重试、拔卡标记和参数验证。页面契约初次发现集成夹具缺少弹窗节点，补齐后通过。DOM 93 条检查通过，含单卷等待／失败／重试、重复点击、Escape、统一卸载失败和处理中会话变化；JavaScript 语法检查通过。未跑全套，未执行真实卷操作。
- 构建：首次使用不存在的短目标名被 Ninja 拒绝；改用项目实际目标 usb_vault_vikey_p2_development_app 后，已有 package-release 配置下 Release 主程序构建通过；不制作安装包或 ZIP。
- 安全复查：原生互斥不依赖页面禁用；后台执行不复制口令、根秘密、路径或档位信息，没有新增磁盘日志、权限、终止进程或强制卸载。不同凭据角色共用相同反馈；错误后重新查询状态，不乐观地把盘符标记为消失。工作结束先发布结果再开放会话；窗口主线程仅在工作结束后处理延迟卡事件，避免与工作线程同时访问会话。
- 限制与实机验收：本片覆盖主动挂载／普通卸载，登录、导入自动挂载沿用现有流程，建卷仍用已有进度；手动强制卸载另片处理。已进入底层驱动的调用不可即时取消，拔卡后的清密钥和卸载仍须等该调用结束；与原同步调用边界一致，不宣称即时中断。DOM 无布局引擎，原生测试使用 fake 会话；真实窗口动画／缩放／关闭、真实忙卷和拔卡仍需实机验收。
- 提交状态：本片尚未提交，等待用户审查后决定是否创建 [P3.20] 提交；未推送。
- 本机构建启动修复：用户实测直接启动 package-release 主 EXE 报组件校验失败。只读核验发现 vendor/ViKey.dll 缺失，其余五项固定组件哈希均与生成的部署头一致；从已有本机 SDK 取白名单 SHA-256 匹配的 ViKey.dll，复制到构建目录 vendor 下并复验哈希后重新启动。此次只补齐被忽略的本机构建产物，不放宽校验、不修改产品代码或凭据，不生成正式测试包。此前仅确认进程创建便报告已启动，验证不足。

#### 同片调整：容器名称与可确认的结果弹窗

- 按用户追加意见调整本片交互：宽度由通用 32rem 收窄为至多 23rem，最小高度 19rem；处理中显示“正在挂载／卸载＋当前容器名”，统一卸载多卷时显示“全部已挂载卷”。长名称允许折行，动画遵循减少动态效果设置。
- 成功后停止动画，显示“挂载成功／卸载成功”及目标名称，提供“确定”并在 2 秒后自动关闭；失败停止动画、显示原因和“确定”，不自动消失。关闭／开始新操作均清除旧计时器，等待态不可提前关闭，终态支持键盘确认、Escape 和焦点恢复。
- 原生结果扩展为固定、不含敏感字段的失败分类：占用／锁定、位置不存在、访问被拒绝、容器无法识别或认证不匹配、读写失败、驱动／磁盘不可用、拔卡，以及原因未知。只读取 error_code，不传递 what()、路径或凭据；未知异常如实显示无法确定原因。测试发现 Windows 共享冲突会被通用条件映射为权限错误，已将精确 Win32 判断移到通用判断之前。
- 安全复查：名称只取已有会话显示事实并通过 textContent 写入，不引入 HTML 拼接或持久化；卡状态变化立即清理目标名，处理中卡状态变化不报成功，结果页也清除旧成功计时器；普通卸载与强制卸载策略未改动，未增加档位区分或秘密复制。
- 验证：宿主对象测试通过，覆盖驱动、通用、Win32 错误映射及未知错误；页面契约通过；DOM 104 项通过，含真实 2 秒计时、显式确定、失败持续显示、旧计时器隔离、各类原因和会话变化。DOM 总截止时间改为 15 秒以容纳真实等待验证。JavaScript 语法、差异检查通过，Release 主程序重新构建通过。真实布局、动画与驱动失败仍需实机检查。本片继续未提交、未推送。

#### 同片调整：弹窗高度、自动关闭时长与遮罩关闭

- 按用户追加意见再调本片弹窗：最小高度由 19rem 降到 13rem，正文内边距 2rem/1.5rem/1rem 收为 1.5rem/1.5rem/.75rem，标题外边距 1.1rem/.7rem 收为 .85rem/.5rem，页脚最小高度 4rem 收为 3.25rem。保留最小高度是为了运行中与终态切换时弹窗不跳动，只是把下限压低。
- 删去成功提示里“此提示将在 2 秒后自动关闭”的自述文案；原先无容器名时该句是全部内容，删后会留空段落，故补“磁盘状态已更新。”兜底，避免 aria-describedby 指向空节点。自动关闭由 2 秒改为 3 秒。
- 成功与失败终态允许点击遮罩关闭，沿用页面既有 `event.target === overlay` 写法；处理中点击遮罩无效，与既有 Escape 处理一致，不给未完成的驱动操作开新的关闭入口。
- 验证：DOM 107 项通过（新增运行中忽略遮罩点击、失败态遮罩关闭、点击弹窗内部不关闭三条，并断言成功文案不再出现“关闭”字样）；失败态等待由 2100ms 提到 3100ms、成功态由 1800ms 提到 2800ms 以匹配新时长。宿主对象测试与页面契约测试各 1/1 通过，JavaScript 语法检查通过。dev 与 package-release 两个配置的主程序均重新构建，部署页面 SHA-256 与源文件及烘入 EXE 的摘要三方一致。
- 手动验证入口统一到 package-release（Release＋静态 CRT，与客户拿到的一致）；dev 仅用于编译和跑 CTest。本机启动脚本 `out/p2_run_dev_app.ps1` 已参数化并改用 SHA-256 比对源与部署副本，该文件在 gitignore 内、不进本次提交。真实布局、动画与驱动失败仍需实机检查。

### P3.21 卸载失败后的手动强制卸载（2026-09-15）

- 基线与范围：用户明确要求开工强制卸载并在失败弹窗提供红色按钮。开工只读检查确认 P3.20 已由此前流程提交为 `0900d7c`，随后有 `b99f1e2` 行尾规则提交，工作区干净；本片作为独立 P3.21 开发，不改写已有提交。开场对“未提交改动”的判断已在检查后向用户纠正。
- 需求与实现审计：沿用需求清单 R-03、8.6、普通卸载重试与拔卡升级约束，审查 P3.20 弹窗／结果分类、COM 后台互斥、窗口卡事件、会话 Runtime 与现有卸载测试。已有 force_unmount_active 专供拔卡，会清除口令、派生秘密与历史，不能借给用户主动强制卸载。新增手动入口复用精确 busy 才升级的底层机制，保留当前登录能力；无需求冲突。
- 页面：普通卸载返回占用／锁定且目标仍处于挂载态时展示红色强制按钮；其他失败与挂载失败不展示。首次点击只切换为目标明确的数据丢失／文件系统损坏确认，取消、Escape、遮罩关闭均不调用原生卸载。第二次明确确认才后台执行，显示“正在强制卸载…”及结果；强制失败仍显示原因、保留真实状态，成功沿用确定／遮罩／3 秒关闭。等待时阻止重复执行；卡状态变化使确认失效并清除目标名称。
- 原生边界：新增无参数 beginForceUnmountLast，只接受最近一次已完成的 busy 卸载结果，目标由原生保存的单卷 ID／全部卷范围确定，不接收页面盘符或新目标。只读刷新不影响资格；任何其他修改操作、结果成功／非 busy、经校验的卡事件代次变化都会阻止旧目标重用。后台运行互斥、窗口等待与拔卡延迟清理沿用 P3.20。
- 会话：单独提供手动单卷和全部卷强制入口，默认不支持的会话实现拒绝调用。仍按现有顺序处理，每个卷先尝试普通卸载，仅精确 driver volume_busy 时才 force；失败保留挂载事实及盘符，成功才清除。单卷不会影响其他卷，全部卷部分失败可准确重试剩余卷。沿用通用卸载历史事件，不新增模式／档位／强制类型的持久标记，保留锁页口令与派生秘密供本次登录重挂载；拔卡路径原有清零行为不变。
- 验证：会话、宿主对象、页面契约 3/3 通过；随后新增部分失败与“挂载 busy 不得强制卸载”回归，宿主与会话分别复验通过。DOM 120 项通过，覆盖红色按钮、错误分类限制、风险确认／取消、重复点击、强制失败与成功、单卷范围、全部卷确认以及卡事件失效。首轮 DOM 夹具未等卡状态刷新便点击下一步，已修正；部分失败测试的驱动状态查询本身记录事件，调整为先验证卸载事件再查询状态后通过。未跑全套，未执行真实卷卸载。
- 构建与环境：review-clean 因行尾基线变化触发 CMake 重配置，默认代码页 936 被既有守卫拒绝；加载 UTF-8 65001 与 MSVC 环境后构建通过。Release 主程序已在 package-release 重建；没有制作安装包或 ZIP，没有关闭或强杀用户进程。
- 安全复查：强制重试资格由原生维护，不依赖按钮显示；不新增任意盘符入口、原始异常文本、秘密传输或持久状态；角色间采用完全一致的交互。取消确认不触发 I/O；未知／权限／读写失败不自动升级，底层在确认后重新尝试普通卸载以避免不必要的 force。真实强制卸载仍可能造成数据或文件系统损坏，无法即时中断已进入驱动的调用；该风险由确认步骤告知，不因验收通过而消失。
- 提交：`c0022c0 [P3.21]offer a way past a volume that will not let go`（用户实机验收通过后提交，未推送）。

#### 同片调整：确认文案、名称留白、按钮排布与结果标识

- 强制卸载确认正文由两句祈使句压缩为一问句「未保存或尚未写入磁盘的数据可能丢失，文件或文件系统可能损坏，仍要强制卸载吗？」。原文末尾的「请先保存文件并关闭使用该卷的程序；仍要继续时，再确认强制卸载」与底部两个按钮重复表达，去掉后与标题「强制卸载 X ？」连读成完整问句。
- 容器名称在三处拼接中前后各加一个空格：处理中标题、成功正文、强制卸载确认标题，避免名称与相邻中文挤在一起（名称为拉丁或中西混排时尤其明显）。成功正文的前导空格按 HTML 文本节点规则会被折叠，不产生缩进，保留只为三处写法一致。
- 强制卸载按钮移到确定左侧：直接调整页脚 DOM 顺序而非用 CSS order，使 Tab 顺序与视觉顺序一致。min-width 规则由确定按钮独有扩为页脚按钮共用 7rem；最长文案「确认强制卸载」约 6.2rem，四种文案组合下按钮等宽不被撑开。
- 新增「重试挂载」按钮：挂载失败且容器仍属本会话、仍未挂载时出现，同样位于确定左侧。与强制卸载有意不同——不限失败码（重试无破坏性）、无二次确认、用默认按钮样式而非危险红。两者按 verb 互斥，页脚恒为至多两个按钮。挂载入口原先只传 verb 与 target、缺 id，已补上以支持重试。
- 结果标识改为纯 CSS 绘制：三态共用同一 2.75rem 圆盘（常驻 3px 透明边框），成功为 --ok 40% 边＋12% 底加两条边旋转而成的勾，失败为 --danger 同配色加一竖一点的感叹号，沿用 `.creation-progress-steps .step-mark` 既有画法。原先 ✓／! 是字体字形，形状由系统回退字体决定；且结果态不画圆环，切换时标识区域塌陷。confirm-force 原本无标识规则会显示空的金色圆盘，现与 error 共用危险盘。标识本就 aria-hidden，语义由标题承担，删除字形无读屏损失。
- 验证：DOM 由 120 增至 123 项全部通过（新增挂载失败给出重试且不出现强制按钮、重试成功后按钮自行撤下，mock 补 failMount 开关）；页面契约、宿主对象、会话测试通过；JavaScript 语法检查通过；package-release 主程序重建，部署页面 SHA-256 与源文件及烘入 EXE 的摘要三方一致。jsdom 无布局引擎，CSS 观感无法由自动化覆盖。
- 实机验收（2026-09-15，用户执行）：结果标识观感、页脚按钮排布与真实忙卷上的强制卸载均通过，未见问题，勾的光学重心与按钮等宽无需再调。本片自动化与实机验收均已完成。

### P3.22 一键挂载与卸载改名（2026-09-15）

- 范围与基线：用户要求把右上角「安全卸载」改为「一键卸载」，并在其左侧加一个同风格的「一键挂载」。开工前只读核查发现两侧并不对称——卸载在原生已有整会话入口（`beginUnmount` → `controller_.unmount()` → `volume_session_.unmount_active()`），挂载只有按 id 的 `beginMountAdoptedContainer`，控制器、会话接口、宿主对象三层均无任何 mount_all。已向用户说明这不是改按钮文字，而是需要补一条与卸载对称的原生链，用户确认走原生方案而非页面循环调单卷接口。
- 挂载顺序核查：原打算把挂载写成卸载的逆序（先 A 后 B），查证后否决。`MILESTONES.md` P1.3 行与需求清单第五节写明 Q-07 固定「先 B 后 A」且失败即中止，理由是避免失败回滚时短暂出现 A 盘符并保持既有事务边界。故 `mount_adopted_all` 与 `unmount_active` 采用同一 `{true, false}` 遍历顺序，而非相反。
- 会话层：新增 `CardLoginMountSessionCore::mount_adopted_all()`，按 B→A 收集未挂载容器的 id 后逐个复用 `mount_adopted_container`，遇失败直接向上抛出、不继续挂剩余卷。先定序再挂载，避免遍历容器表的同时被写回。接口基类给出默认实现：不支持的会话实现抛出拒绝，而不是挂一部分。控制器 `mount_all()` 加认证状态校验。
- 宿主对象：新增 DISPID 56 `beginMountAll`，零参数。原有 `all` 标志同时承担「零参数」与「拔卡后免认证豁免」两个含义，已拆为 `no_id`（参数形状，两者皆是）与 `all`（豁免，仅卸载）——不拆则一键挂载会继承该豁免，在卡已拔出时仍可发起。强制卸载资格判定只认 `kBeginUnmountId` 与 `kBeginUnmountContainerId`，故挂载失败不会解锁强制按钮，已加断言固定。
- 页面：右上角新增 `dashboard-mount`，样式与卸载一致但非危险色，图标为卸载 eject 图标的上下镜像；显隐与卸载互为镜像条件（有已挂载卷显示卸载，有未挂载卷显示挂载），部分挂载时两者同时可见。调用 `bridge.beginMountAll()` 不传任何标识，沿用 P3.20 进度弹窗与结果分类。按钮及全部 10 处状态文案中的「安全卸载」统一改为「一键卸载」。
- 验证：会话测试新增 B→A 顺序、重复调用无副作用、失败即中止不挂子集、部分挂载续挂共四组断言；宿主对象新增拒绝带参、后台线程隔离、挂载完成后强制卸载仍被拒；页面契约新增 `bridge.beginMountAll()` 契约；DOM 新增部分挂载双按钮并存与一键挂载后自行撤下。三个原生套件与 DOM 全部通过。调试中修正两处自造问题：`beginMountAll` 置于宿主测试循环末尾破坏了「循环后无挂载」的不变式，已移至 `beginUnmount` 之前；fake 运行时盘符池仅 3 个且卸载不归还，本用例需 5 个，已就地加大。
- 顺带清理：删除 `ui/card_session.js` 中 2026-09-10 引入即为空、从未被读取的 `kGripPaths`，注释随之落到它解释的 `gripGlyph` 上方。按用户指示不单开提交，并入本片。
- 未跑全套；本片尚未做实机验收，package-release 未重建。

### P3.22 切片 1：所选容器的原生批量执行与进度（2026-09-16）

- 范围与基线：用户批准同一 P3.22 下分三片推进。本轮只做原生选择批量执行／进度／结果，不接入常显菜单和结果重试页面，不做用户主动中断。开工检查工作区干净，已有 `0a47ee6 [P3.22]give mounting the same one-press entry unmounting had`；沿用编号并以切片细分，不重写历史。
- 审计：检查 AGENTS、里程碑、需求清单 R-03／8.6／登录挂载事务，以及现有整会话 mount_all、逐 ID 操作、COM 工作线程／互斥／强制重试资格、卡事件代次和原生测试。发现上一轮把 Q-07 的 B→A／失败即中止沿用到整会话按钮，而最新用户明确要求选择批次中失败后继续。以用户新决定为准，在技术需求表及里程碑明确：新的已认证手动多选入口按选择顺序独立操作；不改登录挂载事务或旧整会话接口。
- 原生契约：新增 beginMountBatch／beginUnmountBatch，接受仅含规范正 uint32 ID 的逗号字符串（不接受路径、盘符、角色或名称）；每批 1～1024 项，与既有索引槽位数量一致。完整检查长度、格式、溢出、重复和会话归属后才执行，空选择或任何非法项均拒绝整批。新增受认证保护的轻量目标快照，只含 ID、显示名和会话挂载事实，不读取卷容量，避免单个不可查询盘符阻断其他目标的选择。
- 执行与反馈：在既有后台线程与会话互斥门槛下按选定顺序执行；单项异常记录现有受限错误分类后继续，已处于目标状态的项记录跳过。批次“完成”不代表每项成功，调用方须读取逐项结果。提供带锁的一致进度快照（状态、操作类型、总数、已处理数、当前序号与 ID）及按索引查询 ID／名称／结果的接口；协议写入宿主公开头注释。下一次批次直接接收本次失败项 ID，实现后续页面重试所需能力。
- 安全边界：执行中仅批次查询可达，不允许同步查询会话、重复操作、旧单卷进度或建卷取消；批次不生成手动 force 资格，普通卸载保持普通策略。线程和结果快照共享锁，卡事件代次变化／拔卡标记在每项前后检查，停止剩余项；旧代次／未认证禁止读取，工作结束清除被中断批次的名称，其他修改操作丢弃旧批次。没有新增口令复制、档位分支、路径输出或磁盘结果缓存；仍依赖既有锁页秘密生命周期。
- 最小验证：宿主对象与会话两个套件 2/2 通过。新增选择顺序、中间失败后继续、挂载与卸载两种方向、已达目标跳过、仅重试指定项、完整请求拒绝（重复／未知／溢出／NUL／超长／错型）、实际后台线程、原生互斥、强制入口拒绝、过程中／完成后拔卡代次失效及新批次恢复。会话补充不可查询容量仍能返回无 I/O 选择快照、未认证不暴露名称。首次编译修正测试中误用 clear() 的问题，改为真实 clear_authentication() 后通过。未跑全套，无页面修改，因此不重复 DOM 测试。
- 限制：已进入驱动的单次调用仍不可立即打断；拔卡等待它返回后停止后续项并沿用窗口生命周期清理。正常结果仅供当前认证／卡代次访问，卡变化不保留旧结果供新登录查看。已达目标判断取会话跟踪状态，最终页面须沿用原生实际状态刷新。真实多卷／忙卷测试尚未执行。
- 构建：package-release 的 Release 主程序构建通过，git diff --check 通过；未启动应用或操作真实卷。
- 提交状态：用户已批准提交切片 1，并进入切片 2；本次提交包含上述已验证原生改动，未推送。其余 UI 仍为原有按钮行为。

### P3.22 切片 2：常显分体按钮、选择菜单与批量进度（2026-09-16）

- 提交与范围：按用户授权先提交切片 1 为 `5c1d3a9 [P3.22]execute selected container batches with per-item progress`，未推送；随后开始本片。用户要求只跑必要最小测试。本片不实现切片 3 的逐项结果列表／选择重试，也不引入主动中断。
- 审计：复核 AGENTS、里程碑 P3.22 三片边界、技术需求清单的显式多选规则及 R-03／8.6；检查现有顶栏按钮、单卷进度与强制确认、卡事件刷新、原生批次协议和页面契约／DOM 测试。新顶栏使用批次接口；非仪表盘的整会话清理保留原有入口，无新需求冲突。
- 交互：认证页面两个按钮常显，主按钮执行勾选项，右侧小箭头展开容器列表。两个方向各有独立的易失选择集，默认不选；顶部全选仅影响可操作项，支持半选状态。已达目标状态的项保持勾选且禁用，左侧分别显示“已挂载”／“未挂载”。无选择时主按钮禁用，箭头仍可查看列表；无容器显示空提示。支持原生复选框键盘操作、Escape 关闭并回焦、外部点击和焦点离开关闭。
- 流程：向原生提交选定 ID，轮询带当前容器名称与 x/y 的批次进度，运行期间互斥。读取逐项结果后刷新真实会话状态，显示成功／失败／已达目标数量与受限失败原因；全成功沿用既有 3 秒关闭与显式确定，含失败保留提示。批次结果不借用单卷强制资格或单卷重试入口。正常完成清空本方向选择；另一方向随新状态剔除不再可操作的项。
- 最小验证：页面契约测试 1/1 通过；DOM 首轮 135 项通过，安全复查补充刷新期间清空下拉名称后定向复验 136 项通过。覆盖默认空选、禁用勾选、全选／半选／清空、多选只提交指定 ID、进度名称／计数、重复操作阻止、部分失败继续、未选项不变、完成计数、卡变化立即清空名称与新会话不继承选择。首次调用运行时依赖目录未找到 jsdom，改用项目已有 `out/ui-navigation-tools/node_modules/jsdom`，未安装新依赖。未重跑原生会话／宿主套件或全套测试。
- 构建与安全复查：package-release 主程序构建通过，git diff --check 通过。新增名称均通过 textContent 写入；选择与结果不持久化、不记录角色／路径／秘密，跨桥只传 ID。卡变化立即清空菜单及选择，异步返回在写入进度前检查卡变化标记，原生代次校验继续作为权限边界；运行／刷新时不重建旧菜单名称。未启动应用或操作真实卷。
- 风险与后续：DOM 模拟没有布局引擎，分体按钮、长名称及小窗口布局待实机验收；真实多卷／忙卷尚未验收。驱动中的单次操作仍不能立即中断。逐项结果页与选择重试留给切片 3。用户已批准先提交本片再进行切片 3，未推送。
