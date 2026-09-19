# USB Vault

Windows 桌面加密容器项目。当前技术约束以[需求梳理清单 v2](需求梳理清单_v2.md)为准；阶段范围见[里程碑](MILESTONES.md)，实施与验证记录见[开发日志](DEVELOPMENT_LOG.md)。

## 目录入口

| 目录 | 用途 |
|---|---|
| `src/`、`include/` | 正式产品实现与接口 |
| `ui/` | 正式 WebView2 页面与脚本 |
| `tests/` | 自动化测试与测试夹具 |
| [`tools/`](tools/README.md) | 应用入口、构建打包与开发验证工具 |
| `packaging/` | 安装器定义、交付说明与第三方声明 |
| `third_party/` | 固定版本依赖源码、补丁与说明 |
| [`docs/`](docs/README.md) | 设计参考与历史需求原件 |
| [`spike/`](spike/README.md) | P0 可行性证据，不是正式产品实现 |
| `out/` | 本机输出与依赖，不纳入 Git |

## 本机构建与清理

构建预设见 `CMakePresets.json`。在 x64 MSVC 开发命令行中通过
`tools\configure-preset.cmd` 配置，以满足 Ninja 的 UTF-8 依赖跟踪要求。
FormatDLL 的准备与路径参数见[第三方说明](third_party/veracrypt_format/README.md)。

`out/` 不能整体视为可删除缓存：`out/local-dependencies/` 保存已核验的构建与安装输入，
`out/deliveries/` 保存交付文件，部分旧构建目录仍可能保存容器。清理前分别核对用途；
不要按 `.exe`、`.dll` 或容器扩展名全局删除。
