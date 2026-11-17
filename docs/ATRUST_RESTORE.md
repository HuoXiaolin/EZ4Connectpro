# aTrust 原生免登录恢复核对

核对日期：2026-11-17。目标是已授信终端在电脑重启后通过原生凭据恢复会话，不以 CAS 浏览器 Cookie 恢复代替。

## 已确认

对本机已有的官方 Windows 安装包 2.4.10.20 进行了静态检查，没有安装、启动官方服务或重复下载。客户端具有独立的持久凭据流程：

1. 登录后调用 `/passport/v1/security/passportTokenExchange`。请求含本机 `deviceId`，响应模型含 `username`、`domain` 和 `passportToken`。
2. `SdpMmkv.dll` 的 `SdpcEntity` 提供 `getPassportToken` / `setPassportToken`；启动逻辑会检查是否有已保存令牌。
3. `PassportTokenBusinessHandlerImpl` 依次执行版本和认证配置查询、防中间人检查、登录前环境上报、令牌上线、在线信息和票据获取。
4. 原生上线接口为 `/passport/v1/auth/passportTokenOnline`，字段包含 `passportToken`、`deviceId`、`updateToken`。另有 `/passport/v1/public/passportTokenOffline`。离线与注销需要分别处理。

证据位置：`aTrustCore.dll` 的 `acquirePassportTokenPromise`、`acquireOnlinePromiseV2`、`PassportTokenBusinessHandlerImpl`；`Network.dll` 的虚函数分发表确认令牌交换走异步 `post(URL, HttpFormData)`。这些结果说明接口和客户端流程存在，并不等于已确认当前网关接受这一安装包版本的全部请求格式。

[官方 SDK 的 startAutoTicket 说明](https://bbs.sangfor.com.cn/atrustdeveloper/appsdk/ios/ios_sdk_username_password_auth.html)也区分了本地持有恢复凭据与联网恢复结果。该文档面向移动 SDK，不能直接视作 Windows 的完整协议规范。

## 与当前核心的差异

当前固定版本 zju-connect v1.3.1 的缓存只有设备标识、会话 Cookie 和服务器版本信息，没有持久免密令牌；启动时仅检查旧会话，失效就进入 CAS。2.1.0 初次构建把协议标识改为 Windows 并修改登录环境数据，用户随后反馈必需的 reportEnv 请求被网关以 10000008（接口签名校验失败）拒绝。修正版恢复上游原有的 Linux 兼容协议声明和必需的认证请求格式；本机名称、Windows 版本与架构只在认证成功后独立尝试更新，更新失败不会被当作登录失败。该修复不代表已实现官方 Windows 客户端签名机制或持久令牌恢复，实际网关复测仍需确认。

此前 2.0.1 的持久令牌核对中，官方 `SdpPcTerminalTrustAdapter` 的请求签名属于防中间人模块；当时会话的 `antiMITMAttackData.enable=0`，不能据此认定令牌恢复失败由缺少签名造成。该历史探测与后来 2.1.0 的 `reportEnv` 失败属于不同请求阶段。

## 当前网关验证边界

用户明确授权后，以本机 EZ4Connectpro 自己保存的已登录会话向其配置的网关验证，输出仅保留状态码和字段存在性：

- 认证配置：`code=0`、`isLogin=1`、`passportTokenEnable=0`。
- 已有 JSON 请求诊断记录返回 `75510022`；[官方错误码文档](https://bbs.sangfor.com.cn/atrustdeveloper/pcsdk/docs/6.API接口文档/6.2认证登录-主认证.html)将其定义为认证未启用。
- 按本地官方安装包识别的表单请求验证返回 HTTP 400，未获得令牌。请求版本或环境条件仍需核对。
- 未注销会话、未解绑或添加设备、未更改服务器策略；真实 Cookie、设备标识、票据没有写入源码、测试夹具或本文档。

用户确认另一台电脑上的官方客户端能在同一 CAS 首次认证后，重启直接恢复。因此不能把本次会话的返回值推断为网关整体不支持免登录。

## 尚需的对照证据

需要那台电脑上官方客户端一次成功免登录恢复的日志，至少包含客户端版本、恢复阶段、使用接口和返回状态。可先使用 `scripts/inspect-atrust-restore.ps1` 提取不含凭据的事件摘要；如果日志加密，应从官方客户端的诊断入口导出可读日志后再提取。

在那台电脑重启并由官方客户端恢复连接后，把脚本复制过去，在脚本所在目录运行：

```powershell
powershell -NoProfile -File .\inspect-atrust-restore.ps1
```

默认读取 `%APPDATA%\Sangfor\aTrust\logs`，生成 `%TEMP%\EZ4Connectpro-atrust-restore-check.json`。只需提供该 JSON 和官方客户端版本，无需提供原始会话文件。如果提示没有可读日志，可用 `-LogPath "导出的日志文件路径"` 指定官方诊断导出的可读文件。

在拿到对照证据并完成真实网关验证前，本项目不宣称已实现官方原生免登录恢复。已有浏览器 Cookie 恢复仅作为现有功能保留。本次另外修复了 DNS 接管开关，并补充同一登录浏览器的官方资源页入口。
