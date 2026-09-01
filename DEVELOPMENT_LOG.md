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
- 提交：`022fcc4 [ui]adopt graphite design baseline`。

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
- 提交：`5ce593e [ui]add the multi-container page and bridge`。

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
- 提交：`4a388ff [ui]drop the single-container bridge`。

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
- 提交：待定（`[cleanup]remove the P1 application chain`）。
