# 设计与历史资料

当前技术结论以根目录的[需求梳理清单 v2](../需求梳理清单_v2.md)为准。
这里保存设计参考和需求原件，不作为新的实现指令；与现行需求冲突时以现行需求为准。

| 文件 | 定位 |
|---|---|
| [启动失败与数字错误码对照表](design/startup-failure-codes.md) | 启动链路源码审计、失败出口与拟接入数字码；不代表运行时已实现 |
| [全流程 UI 设计稿](design/usb-vault_ui-design.html) | 视觉与交互参考；不代表所有内容都已实现，也不替代正式页面 |
| [早期 UI 原型](archive/design/usb-vault_ui-disign.html) | 已归档；账号体系和分类导航已作废，保留原文件名以便追溯 |
| [初版需求梳理](archive/requirements/需求梳理清单.txt) | 早期问题清单，已由 v2 技术文档承接 |
| [基本需求图片](archive/requirements/基本需求.png) | 原始需求材料，原样保留 |
| [客户需求说明书](archive/requirements/个人隐私保护产品（桌面式）需求说明书(1).docx) | 原始需求材料，原样保留 |
| [开发日志原文归档](archive/development/README.md) | 2026-09-19 压缩前的完整记录；日常阅读根目录精简版 |

正式界面为 [`ui/card_session.html`](../ui/card_session.html) 和
[`ui/card_session.js`](../ui/card_session.js)。两份设计 HTML 均为独立页面，可直接打开。

这些文件原先位于仓库根目录。开发日志中的历史名称与路径保持原样，查找时按上表对应；
`spike/` 报告及其验证代码继续原位保留，避免破坏技术文档中的证据引用。
