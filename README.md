# EZ4Connectpro

EZ4Connectpro 是面向深信服 aTrust 和 EasyConnect 的轻量 VPN 工作台。Windows x64 版集成为一个 **27.0 MiB 的便携 EXE**，比上游发布包小 **82.1%**。常用 VPN 配置可以集中管理，也可以按需同时连接。

[项目主页](https://github.com/HuoXiaolin/EZ4Connectpro) · [发布页](https://github.com/HuoXiaolin/EZ4Connectpro/releases) · [使用说明](docs/VPN_WORKSPACE.md)

## Windows x64 体积对比

| 版本 | 发布文件 | 展开后的运行文件 |
| --- | ---: | ---: |
| EZ4Connect v1.9.1 | ZIP，150.9 MiB | 354.4 MiB |
| EZ4Connectpro 2.0.0 | 单文件 EXE，27.0 MiB | 56.5 MiB |

同为 Windows x64，发布文件减少约 **123.9 MiB**。Pro 版改用外部浏览器完成单点登录，移除了内置的 Qt WebEngine。便携 EXE 仍包含 Qt 运行库、zju-connect 和 Wintun，启动时将运行文件展开到临时目录，正常退出后清理。 [上游 v1.9.1 发布附件](https://github.com/chenx-dust/EZ4Connect/releases/expanded_assets/v1.9.1)

### 与深信服官方 aTrust 对比

深信服 aTrust **2.5.16.50 标准版**的 Windows 安装包约 **201.2 MiB**。与它相比，EZ4Connectpro 的便携 EXE 小 **86.6%**。 [深信服官方下载页](https://atrustcdn.sangfor.com/tools/clientVersion/index.html)

运行时，Pro 的 EXE 与展开的程序文件合计约 **83.5 MiB**。以官方客户端安装后占用 **300 MiB** 或 **1 GiB** 的部署为例，Pro 的程序文件分别少约 **72%** 和 **92%**。官方客户端的实际安装占用随版本和部署组件变化。

## 主要功能

- **多配置工作台**。集中管理 aTrust 和 EasyConnect，按名称、协议或网关查找配置。每条连接各自显示状态和代理端口，活动日志带有配置标记。
- **配置备份和开机连接**。在「文件」导入或导出 INI，在「设置 → 启动设置」统一管理静默启动和开机连接线路。 [工作台使用说明](docs/VPN_WORKSPACE.md)
- **外部浏览器登录**。aTrust 的 CAS 和 HTTPS OAuth2 默认使用临时浏览器配置，自动接回登录票据。也可选择系统默认浏览器，借助回传扩展使用已有浏览器配置。 [浏览器登录说明](docs/VPN_WORKSPACE.md#单点登录的两种浏览器方式)
- **内网分流和本机代理**。Windows 为开启 TUN 的配置分配独立虚拟网卡，按网关路由访问内网。遇到虚拟 IP、下发网段或 DNS 劫持冲突时，会阻止新连接并显示原因。关闭 TUN 后可使用该配置的 HTTP 或 SOCKS5 代理。 [高级使用方式](docs/ADVANCED_USAGE.md)

## 界面截图

### 连接工作台

左侧选择或新建配置，右侧查看连接阶段、代理端口和活动日志。「更多操作」提供重命名、删除及适用的登录维护操作。截图使用示例网关。

![EZ4Connectpro 工作台，左侧为办公内网和测试环境配置，右侧为连接状态及活动日志](docs/main.png)

### 浏览器登录设置

选中配置，在「编辑配置 → 认证」中选择登录浏览器。该设置按连接配置保存。

![aTrust CAS 认证设置，登录浏览器选为临时浏览器配置](docs/browser-login.png)

## 快速开始

1. 双击 `EZ4Connectpro-windows-x64.exe`。从源码构建时，默认成品在 `out` 目录。
2. 在工作台点击「新建配置」，选择 aTrust 或 EasyConnect，填写网关和认证信息。已有配置可直接在左侧选中。
3. 点击「连接 VPN」，按网关要求完成认证。首次启动 TUN 时按提示授予 Windows 管理员权限。

配置和日志保存在 EZ4Connectpro 的应用数据目录。系统代理由其他软件管理时，浏览器的内网流量需要直连，才能进入本机 VPN 路由。 [浏览器访问内网页面](docs/VPN_WORKSPACE.md#浏览器访问内网页面)

## 开发

项目的分层、依赖方向和新增代码归属规则见
[架构说明](docs/ARCHITECTURE.md)。

### 构建 Windows x64 便携版

便携包使用 Qt 6.10.3、对应的 C++ 编译器、CMake、Ninja 和 Go 构建。将 Qt 的 `bin`、编译器、Go 和 Git 加入 `PATH`，Qt 需包含 `qt5compat` 和 `qtwebsockets` 模块。便携启动器使用 `windres.exe` 写入图标和版本信息。Windows 打包使用固定的 zju-connect v1.3.1 源码和 [多网卡补丁](scripts/windows-core/zju-connect-v1.3.1-tun.patch)，复用 Go 模块缓存。也可用 `-CoreArchive` 指定已经编译好的核心包。

```powershell
cmake -S . -B build/windows-x64 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/windows-x64 --parallel
.\scripts\package-windows-single-file.ps1 -BuildDir build/windows-x64
```

## 许可证

本项目遵循 [GNU General Public License Version 3](LICENSE) 开源。

## 致谢

- [chenx-dust/EZ4Connect](https://github.com/chenx-dust/EZ4Connect)
- [Mythologyli/ZJU-Connect-for-Windows](https://github.com/Mythologyli/ZJU-Connect-for-Windows)
- [Mythologyli/zju-connect](https://github.com/Mythologyli/zju-connect)
- [lyc8503/EasierConnect](https://github.com/lyc8503/EasierConnect)

> 欢迎加入 HITSZ 开源技术协会 [@hitszosa](https://github.com/hitszosa)
