# 工具用途

本目录同时包含正式程序入口和显式启用的开发工具。名称中的 `p1`、`p2` 或
`development` 不能单独作为废弃判断依据；构建目标与依赖以根目录 `CMakeLists.txt` 为准。

## 正式程序与构建

| 路径 | 用途 |
|---|---|
| `app/` | 主应用入口，当前产物为 `USB Vault.exe` |
| `vikey_helper/` | 隔离供应商 DLL 的卡通信进程 |
| `volume_creator/` | 独立建卷进程 |
| `setup_helper/` | 安装依赖处理进程 |
| `configure-preset.cmd` | 切换 UTF-8 代码页并运行 CMake 配置 |
| `generate-p2-deployment.cmake` | 生成部署完整性信息，供应用构建使用 |
| `package-p2-development.ps1` | 校验依赖、页面和版本信息，制作交付载荷 |
| `build-windows-installer.ps1` | 校验载荷清单并构建 NSIS 安装器；在 `OutputDirectory` 下创建 `installer-yyyyMMdd-HHmmss` 交付文件夹（构建开始时的本机时间，24 小时制），其中安装程序固定为 `USB-Vault-Setup.exe`，摘要记录为 `USB-Vault-Setup.exe.build.txt`；同名文件夹拒绝覆盖 |

## 测试包与正式发布日志

`package-release` 是经过 Release 优化的**测试交付包**，按 2026-09-26 用户决定显式设置
`USB_VAULT_DEBUG_LOG=ON`、`USB_VAULT_PRODUCTION_RELEASE=OFF`，继续用于 P4.9 实机验收。
Release 优化级别本身不代表正式发布。

正式发布使用 `package-production` 配置和构建预设：它设置
`USB_VAULT_PRODUCTION_RELEASE=ON`、`USB_VAULT_DEBUG_LOG=OFF`。
在 x64 MSVC 开发命令行中执行 `tools\configure-preset.cmd package-production`，
另按第三方说明提供已核验的 FormatDLL 路径，再执行 `cmake --build --preset package-production`。
正式发布标志开启而调试日志开启时，CMake 配置直接失败，包括旧缓存或命令行覆盖造成的冲突。
手工配置正式发布构建时也必须开启该标志；此检查不能自动判断一个测试包会被交给谁。
该预设只约束构建日志，不代表已完成签名、安装包审核或实机发布验收。

已有日志不自动删除，关闭编译开关也不会清理旧文件。测试期定期手工整理日志；
交付安装器仍必须遵循仓库规定的 `out/installers/installer-yyyyMMdd-HHmmss` 路径。

## 开发验证工具

以下目标为 `EXCLUDE_FROM_ALL`，需要显式构建；不属于日常启动流程。

| 路径 | 保留原因 |
|---|---|
| `p1_7_development_fixture/` | 容器夹具与挂载诊断；夹具库仍被自动化测试引用 |
| `vikey_development_session_probe/` | 会话、blob 读写探针；随机测试 blob 实现仍被测试引用 |
| `vikey_p1_card_reset/` | 仍声明为独立卡重置目标；不是普通诊断，运行前须确认目标卡与影响 |

2026-09-19 清理了 `out/` 中五个本机临时脚本：`p1_7_diag_acl.ps1`、
`p1_7_diag_pipe.ps1`、`p1_7_step1_fixture.ps1`、`p1_7_step2_dev_app.ps1`、
`p2_run_dev_app.ps1`。它们不是构建输入；旧验收入口依赖已退休的产物，
P2 启动脚本仍指向更名前的 EXE。当前应用应从对应构建或安装目录启动 `USB Vault.exe`。
