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

- 用户已于 2026-08-22 批准提交；本条记录随 P1.0 提交落库。
- 提交哈希：`e0909a2fe0c048bcd316c2062377a9b3de7df54f`。
- 未执行 push。

---

## 2026-08-22 — P1.1 密钥派生

**状态：** 已完成并获准提交

### 计划与需求审计

- 重排后的 P1.1 只覆盖密码学供应链、KDF v1 合约、敏感输入输出和 Argon2
  大工作区生命周期；驱动 ABI、挂载与卸载属于 P1.2，不进入本阶段提交。
- `需求梳理清单_v2.md` 要求 Argon2id 参数固定为 m=512 MiB、t=2、p=1，
  派生耗时须纳入 P-002 五秒预算，且 R-03 的秘密驻留与可靠清零要求继续适用。
- 用户确认 KDF v1：用户输入是 password，32 字节卡材料是 Argon2 secret `K`，
  每容器使用 16 字节公开随机 salt，固定域标识作为 associated data，输出
  64 字节原始 VeraCrypt 卷密码。
- 用户确认 UTF-8 输入不做 Unicode 规范化；小秘密强制锁页，512 MiB 工作区
  使用可分页私有内存并在释放前清零，不因资源不足静默降低参数。

### 已检查的现有代码与上游材料

- 已检查 P1.0 的 `SecureBuffer`、构建预设、负向复制测试和 ABI 断言设施。
- 已检查 RFC 9106 对 password、salt、secret 与 associated data 的输入定义。
- 用户离线提供官方 `phc-winner-argon2` tag `20190702` ZIP；标签对应提交
  `62358ba2123abd17fccf2a108a301d4b52c01a7c`，归档 SHA-256 为
  `506A80B90AC3CA8407636F3B26B7FED87A55F2F1275CDE850CD6698903C4F008`。
- 归档 82 个条目均位于预期顶层目录，不存在路径穿越；许可证允许 Apache-2.0
  或 CC0，项目按用户确认选择 Apache-2.0。
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
  在清零回调执行前捕获内容。当前清零不能追溯擦除这些外部副本，这是用户已接受并
  必须持续记录的威胁模型残余风险。
- 官方 P-H-C 仓库没有安全策略且实现基本停止维护。`20190702` 仍是最新 release，
  但“没有已发布公告”不能证明不存在漏洞；发布前必须重新复核供应链状态。
- 当前使用固定测试卡材料；真实 USB 卡读取属于 P1.6。
- KDF 元数据层尚未实现。后续只允许持久化 KDF 版本、固定参数与 salt，不得记录
  档位、隐藏卷存在性或可推导这些信息的字段。

### 提交决定

- 用户于 2026-08-22 批准按重排后的阶段边界继续拆分提交。
- 本条随 P1.1 提交落库；不执行 push。

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
- 用户于 2026-08-22 批准提交；计划文档提交为 `f691745`。
- 未执行 push。

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

- 用户于 2026-08-22 批准继续按重排后的边界拆分提交。
- P1.2 经独立快照复验后提交；不执行 push。

---

## 2026-08-22 — P1.3 双容器应用编排

**状态：** 实现、测试与安全审计完成，等待用户决定是否提交

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
- 用户于 2026-08-22 同意第 2 项，并同意同时加入延迟 Shell 通知：B 与 A 先以
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
- 用户于 2026-08-23 审阅阶段报告后批准提交 P1.3；本次只创建本地提交，
  不执行 push。

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
- 用户于 2026-08-23 在前台接受 UAC 后，Release `volume_creator_integration_tests`
  退出码为 0：真实创建并验证了普通容器 A、容器 B 隐藏卷、未保护外层卷和受保护
  外层卷，对应 `type=0/1/0/2`，满足阶段要求的 `type=1/0/2` 全路径出口。
- 验收完成后复核无 `p1_4_*.hc` 临时容器，驱动挂载位图为 0；真实建卷路径没有遗留
  文件、盘符或驱动挂载。
- 许可证边界是工程设计而非法律意见；patched FormatDLL/helper 的最终分发义务仍须律师
  确认。P2 若要在隐藏卷创建前引导用户填充诱饵，需要在本协议上增加“对既有外层卷
  添加隐藏卷”的独立瞬时操作，不能把创建选择持久化；该向导能力不属于 P1.4 出口。

### 提交决定

- 用户于 2026-08-23 审阅阶段报告后批准提交 P1.4；本次只创建本地提交，不执行 push。
  实现按协议与客户端 `a1bde93`、提权 helper 与 FormatDLL 边界 `0b964d0`、验证
  `d951ba8` 分开提交，阶段记录另行提交。

---

## 2026-08-23 — P1.5 最小 UI 与应用闭环

**状态：** ✅ 已完成；小切片 1~15 均已按独立可审查边界提交，未执行 push

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
- 用户审阅后批准提交小切片 1，提交为 `e0fd8e8`；只创建本地提交，不执行 push。

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
- 尚未实现 WebView2 宿主、口令桥或真实 UI 闭环。用户审阅后批准提交小切片 2，提交为
  `4e4a1ef`；只创建本地提交，未执行 push。

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
- 用户审阅后批准提交小切片 3，提交为 `8e68661`；只创建本地提交，未执行 push。

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
- 用户审阅后批准提交小切片 4，提交为 `9c09fd5`；只创建本地提交，未执行 push。

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
- 用户审阅后批准提交小切片 5，提交为 `47cb861`；只创建本地提交，未执行 push。

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
- 用户审阅后批准提交小切片 6，提交为 `482bd5d`；只创建本地提交，未执行 push。

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
- 用户审阅后批准提交小切片 7，提交为 `29a00cf`；只创建本地提交，未执行 push。

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
- 用户审阅后批准提交小切片 8，提交为 `4d5be60`；只创建本地提交，未执行 push。

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
  冒烟不等同于 ASan 运行时检查。用户审阅后批准提交小切片 9，提交为 `23c2a7c`；
  只创建本地提交，未执行 push。

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
- 针对用户提出的轻量模型质量担忧，在全新的 `out/build/review-clean` 目录从零配置并编译
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
  记录为后续边界而非本切片阻塞。用户复核并批准提交小切片 10，提交为 `16f8037`；
  只创建本地提交，未执行 push。

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
  仍可绕过 `WM_CLOSE`，这是桌面进程无法完全消除的剩余风险。用户复核并批准提交
  小切片 11，提交为 `a7d328f`；只创建本地提交，未执行 push。

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
  `git diff --check` 通过。用户复核并批准提交小切片 12，提交为 `7276726`；只创建本地
  提交，未执行 push。

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
  `git diff --check` 通过。用户复核并批准提交小切片 13，提交为 `7f8070f`；只创建本地
  提交，未执行 push。

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
  `git diff --check` 通过。用户复核并批准提交小切片 14，提交为 `c37db3d`；只创建本地
  提交，未执行 push。

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
- `git diff --check` 通过。用户复核并批准提交：SAFEARRAY variant 字节数组兼容边界提交为
  `072f79b`，产品页面最小会话状态机、宿主加固与真实 Runtime 闭环提交为 `42000bf`；
  均只创建本地提交，未执行 push。

### 阶段收口（2026-08-24）

- P1.5 出口条件已满足：固定 blob `ICardStore` 桩下能够完成“输入单一密码 → 挂载两个盘 →
  统一状态反馈 → 主动卸载”，且窗口关闭会先尝试卸载并在失败时保留可重试入口。
- 最终验证为 MSVC Debug CTest 15/15、Debug 真实 WebView2 Runtime 连续三次退出 0、正式
  ASan 相关测试与真实 Runtime 闭环退出 0、MSVC `/analyze` 全量通过；安全复核未发现秘密
  日志、本地持久化、模式/档位实体或分类反馈。浏览器与 COM 内部不可控短期副本风险按本阶段
  已知边界保留，后续由安装包完整性和签名阶段继续收口。
- 用户确认 USB 安全卡已经到货。P1.6 可开始真卡型号、接口、驱动/SDK、单 blob 读写及可选
  持久计数器能力的实机审计；到货不等同于能力验收通过，结论须在 P1.6 记录。
