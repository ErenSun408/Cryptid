USB Vault P2 内部开发测试包
============================

这不是生产发行版。程序尚未代码签名，第三方再分发与安装边界仍在评审，只能用于受控的内部演示和测试。

运行条件
--------

1. Windows 11 x64。
2. 已安装 VeraCrypt 1.26.29，并确保其驱动已加载。官方安装程序已放在
   prerequisites\VeraCrypt Setup 1.26.29.exe；安装前请按“测试须知.txt”核验摘要和签名。
   安装时选择 Install，不要选择 Extract。如果挂载时报驱动不可用，可先启动一次 VeraCrypt。
3. 已安装 Microsoft Edge WebView2 Runtime。Windows 11 通常已自带。
4. 一张与本开发版本匹配的 ViKeyPRO 安全卡。
5. 开发模式允许厂商运行库联网；防火墙不得静默替换或拦截厂商 DLL。

首次使用
--------

1. 双击 usb_vault_vikey_development_credentials_config.exe。
2. 输入供应方提供的 32 位十六进制 UserSN、128 位十六进制 API Key 和 8 位字母数字用户 PIN。
3. 出现“已保存并复读验证”后关闭配置器。凭据只写入当前 Windows 用户、当前机器的凭据管理器。
4. 插入安全卡。
5. 双击 usb_vault_vikey_p2_development_app.exe，无需命令行参数或启动脚本。新建容器时 Windows 会显示 UAC 提示，这是当前建卷 helper 的正常行为。

重要限制
--------

- 请只使用可丢弃的测试数据并另行备份。此版本面向演示，不是生产数据保护承诺。
- “重新初始化安全卡”会销毁当前卡身份；使用旧身份加密的容器随后无法解开。
- 忘记密码决策仍处于暂缓状态，本版本不提供密码恢复或忘记密码重置。
- 凭据属于当前 Windows 用户。更换账户或电脑后需要重新运行配置器。
- Windows 凭据管理器只能防止普通明文文件落盘；同一用户上下文中的其他程序仍可能读取这些开发凭据。
- 请将整个 ZIP 解压到本机固定磁盘；不能仅复制主 EXE，也不支持直接在网络盘或可移动盘运行。组件缺失、修改或版本不匹配时，主程序会拒绝启动。

主要文件
--------

- Start USB Vault.cmd：兼容快捷入口，仅转交主 EXE；正常使用不需要它。
- 测试须知.txt：最短安装流程、所需条件及 VeraCrypt/WebView2 验证指令。
- prerequisites\VeraCrypt Setup 1.26.29.exe：未经修改的 VeraCrypt 官方安装程序。
- usb_vault_vikey_p2_development_app.exe：可直接双击的 P2 开发主程序；以 EXE 所在目录定位组件，按构建时嵌入的 SHA-256 校验 helper、建卷后端、厂商 DLL 及页面。仍为允许厂商运行库联网的开发模式。
- usb_vault_vikey_development_credentials_config.exe：开发凭据配置器。
- usb_vault_vikey_helper.exe：隔离访问 ViKey 的 helper。
- usb_vault_volume_creator.exe：需要 UAC 的建卷 helper。
- VeraCryptFormat.dll：已审计补丁构建的建卷后端。
- vendor\ViKey.dll：供应商 x64 运行库。
- SHA256SUMS.txt：本包逐文件摘要。
- licenses\：第三方许可、补丁和来源说明。
