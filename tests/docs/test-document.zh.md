# FydeOS VNC — 测试文档

本文档说明如何测试 FydeOS VNC 远程访问功能，包含三种互补的测试方法：

- **设备端自动化测试（Tast）** —— `vnc.*` 测试在设备上运行，端到端覆盖
  守护进程：D-Bus 控制 API、RFB/VeNCrypt 协议、TLS、认证、画面捕获、
  输入注入、设置界面、多显示器、noVNC 网页访问、会话切换以及 CPU。
- **外部观察者测试（shell 套件）** —— 一小组只有从 DUT 所在局域网的另一台
  主机才有意义的检查：mDNS 传播、并发查看器、主机侧性能。
- **手动测试** —— 需要人工在屏幕前操作或使用真实 VNC 客户端的用例：
  设置界面、OOBE 流程、输入手感、剪贴板、输入法、会话切换、客户端兼容性。

`vnc.*` Tast 测试、`x0*` shell 检查与手动用例（M-\*）合在一起覆盖全部
VNC 功能。第 5 节的覆盖矩阵列出了每个功能与用例的对应关系。

---

## 1. 功能概览（被测对象）

FydeOS VNC 允许同一局域网内任何符合标准的 VNC 客户端查看并控制 FydeOS
设备，无需互联网。被测功能如下：

| 领域 | 功能 |
|---|---|
| 生命周期 | 服务随系统启动、常驻内存、通过设置界面开关；崩溃后自动重启 |
| 控制接口 | D-Bus API `org.fydeos.VncManager`（开关、状态、密码、证书、显示器选择） |
| 画面捕获 | 登录屏 / OOBE / 锁屏（KMS 路径）与登录后的桌面（合成器路径），两者自动切换 |
| 输入 | 键盘、指针、右键/中键、滚轮；登录前与登录后两条注入路径 |
| 编码 | Tight/Raw RFB 编码、按损坏区域（damage）增量更新、帧缓冲尺寸变化处理 |
| 安全 | 局域网强制 TLS（VeNCrypt）、自签名证书 + SHA-256 指纹、可选密码（bcrypt）、可选的兼容旧客户端"不安全连接"模式、内置网页客户端专用的回环快速通道 |
| 网络 | 仅绑定局域网地址、端口 5900（RFB）与 6080（网页）、mDNS `_rfb._tcp` 服务发现 |
| 网页访问 | 通过 HTTPS 在 6080 端口提供内置 noVNC 客户端 |
| 多显示器 | 枚举显示器、选择会话查看哪个显示器 |
| 健壮性 | 登出 / UI 崩溃 / 登录切换时服务不中断；休眠唤醒 |
| 性能 | 空闲 CPU < 1%、串流 CPU < 15%、局域网输入延迟 < 80 ms |

---

## 2. 测试环境

### 2.1 被测设备（DUT）

- 安装了 VNC 功能的 FydeOS 设备，可通过局域网访问。
- 设备端 Tast 测试需要 **test image**（自带 Tast 测试运行器与 `chrome`
  测试支持）。外部 shell 检查在任何带 VNC 功能且可 `root` SSH 的镜像上均可运行。
- 以 `root` 身份的 SSH 访问。
- 若要完整覆盖：需要一个可登录的用户账号，以及一台第二显示器用于多显示器用例。

### 2.2 测试主机

与 DUT 同一局域网内的 Linux 机器（运行设备端套件还需 ChromeOS SDK 的
`tast` 命令）：

| 工具 | 用途 | 是否必需 |
|---|---|---|
| `tast`（ChromeOS SDK） | 设备端 `vnc.*` 套件 | Tast 测试需要 |
| `bash`、`ssh` | shell 套件 | 必需 |
| `python3`（3.8+） | RFB 协议客户端 | 必需 |
| `avahi-browse`（`avahi-utils` 包） | mDNS 传播检查 | 可选 |
| `sshpass` | 免交互的密码 SSH | 可选 |
| TigerVNC（或其他支持 VeNCrypt 的查看器） | 手动用例 | 手动测试需要 |

无需任何第三方 Python 包 —— 随附的 RFB 客户端（`lib/vncrfb.py`）只使用
标准库。

### 2.3 配置

运行 shell 套件时，编辑 `tests/env.sh`（或导出同名环境变量）指向你的 DUT：

```bash
DEVICE_IP=…            # DUT 地址
DEVICE_SSH_PORT=…      # SSH 端口
DEVICE_USER=root
DEVICE_PASSWORD=…      # 或设置 DEVICE_SSH_KEY=/path/to/key
VNC_PORT=5900
NOVNC_PORT=6080
```

> 说明：若 DUT 位于 NAT / 端口转发之后（只有 SSH 端口可达），`x02`/`x03`
> 会自动改走 SSH 隧道访问 VNC 端口；`x01` 的发现/可达性检查需要真实的
> 局域网路径，否则会给出 WARN。

---

## 3. 自动化测试

### 3.1 设备端套件（Tast）—— 主要功能覆盖

`vnc.*` 测试在设备上运行，是主要的自动化覆盖。对一台 test-image 的 DUT
运行整套：

```bash
tast run <dut-ip>[:ssh-port] 'vnc.*'
```

结果、每条测试的日志与失败截图由 Tast 收集到其结果目录中。测试对设备
状态是安全的：每条都会恢复无密码的干净状态，会话切换测试会恢复登录屏。

| 测试 | 验证内容 |
|---|---|
| `vnc.Lifecycle` | 两个系统服务均在运行；守护进程以 `fydeos-vnc` 用户沙箱化运行；状态目录权限；持有 D-Bus 名称；强杀后自动重启并重新持有 D-Bus 名称 |
| `vnc.DBusAPI` | `GetStatus` 字段；开关往返会关闭/打开 RFB 端口；`StatusChanged` 信号；`ListDisplays` JSON；非法 `SetCaptureDisplay` 返回带类型的错误；密码文件（0600、bcrypt）；`RegenerateCertificate` |
| `vnc.NetworkBinding` | 绑定地址为真实、在线的局域网接口（RFC-1918/链路本地）；RFB + noVNC 监听；关闭时撤销、开启时恢复；avahi mDNS 公告（启用时存在、禁用时撤销） |
| `vnc.TLSCertificate` | 证书/私钥权限；RSA-4096 自签名；D-Bus 与 RFB 线上指纹一致；拒绝 TLS 1.1、接受 1.2；重新生成无需重启即生效 |
| `vnc.AuthMatrix` | 局域网只提供 VeNCrypt（无密码 X509None、有密码 X509Plain）；错误密码被拒；改密码立即生效；回环 SecTypeNone / VeNCrypt Plain；不安全模式 VncAuth（DES） |
| `vnc.CaptureKMS` | 登录前 KMS 路径：KMS 模式、宽度按 4 对齐、可解码且非空白的帧 |
| `vnc.CaptureChrome` | 登录后 FrameSink 路径：纯色页面在捕获的帧缓冲中被还原（闭环） |
| `vnc.InputInjection` | 经线路注入的 RFB 指针与按键事件在 Chrome 会话中产生预期反应 |
| `vnc.Settings` | os-settings VNC 卡片驱动守护进程：开关往返、指纹显示与 D-Bus 一致、证书重新生成 |
| `vnc.NoVNCWeb` | 以守护进程证书通过 HTTPS 提供 noVNC 页面；按策略处理明文 HTTP；websockify 后端与 WebSocket 升级 |
| `vnc.SessionTransition` | KMS（登录前）→ FrameSink（登录）→ KMS（登出），守护进程全程不掉线且持续提供画面 |
| `vnc.MultiDisplay` | 显示器清单；切换捕获显示器后帧缓冲变为该显示器几何尺寸并可切回；未知 id 错误路径（仅一台显示器时跳过切换检查） |
| `vnc.Performance` | 守护进程 CPU 空闲（< 1% 指标）与查看器串流时的占用；上报到 perf 管线 |
| `vnc.InputParity` | 经 RFB 注入的右键弹出上下文菜单、中键打开新标签页、滚轮滚动 |
| `vnc.Clipboard` | 查看器的 RFB cut-text 被粘贴进聚焦的输入框（设备→查看器方向未接线） |
| `vnc.AuthAbuse` | 反复错误密码均被拒且守护进程保持健康（未实现按 IP 退避） |
| `vnc.SettingsPassword` | 经 os-settings 密码对话框设置密码后守护进程要求密码 |
| `vnc.LockScreen` | 锁定期间守护进程提供锁屏画面，且经 VNC 注入的击键可驱动锁屏：错误密码被拒、正确密码解锁 |
| `vnc.LoginScreenInput` | 在真正的开机登录屏（无会话）经 VNC 驱动"添加用户 → 使用本地帐号"流程创建本地帐号并登录 |
| `vnc.NoVNCSession` | 内置 noVNC 网页客户端在浏览器标签页中连接并渲染桌面 |
| `vnc.DisplayHotplug` | 强制外接显示器连接器断开/接入后守护进程清单更新且采集优雅回落（需外接显示器） |
| `vnc.RebootPersistence` | *(主机驱动)* 启用状态（Chrome pref）跨重启保持 |
| `vnc.SuspendResume` | *(主机驱动)* 休眠/唤醒一轮后守护进程仍在提供服务 |

### 3.2 外部观察者套件（shell）

这些检查需要 DUT 所在局域网内的另一台主机；它们是对 Tast 测试的补充，
而非重复。

```bash
cd tests
./run_all.sh            # x01 + x02 + x03
./run_all.sh x02        # 只运行子集
```

每项检查输出 `[PASS]`/`[FAIL]`/`[WARN]`/`[SKIP]`；`run_all.sh` 在
`tests/artifacts/` 下输出合并报告。

| 脚本 | 验证内容 |
|---|---|
| `x01_discovery.sh` | mDNS `_rfb._tcp` "FydeOS VNC" 记录对局域网内其他主机可见；RFB/noVNC 端口可从另一台机器访问 |
| `x02_multiclient.sh` | 多个查看器同时连接 —— 各自完成握手与抓帧 —— 之后守护进程仍健康 |
| `x03_performance.sh` | 主机侧整帧往返（连接+TLS+认证+首帧）以及查看器在线路上观察到的更新吞吐/帧率 |

`x01` 的发现与可达性检查需要真实的局域网路径，在 NAT 之后会 WARN。
`x02`/`x03` 在设备不可直连时会自动建立隧道（隧道数值含 SSH 开销）。

### 3.3 RFB 测试客户端

排查缺陷时也可以直接交互使用 `lib/vncrfb.py`：

```bash
python3 lib/vncrfb.py --host <DUT_IP> sectypes                  # 查看认证类型
python3 lib/vncrfb.py --host <DUT_IP> probe                     # 几何尺寸、TLS 指纹
python3 lib/vncrfb.py --host <DUT_IP> screenshot out.png        # 抓取一帧
python3 lib/vncrfb.py --host <DUT_IP> --password X click 500 300
python3 lib/vncrfb.py --host <DUT_IP> watch 10                  # 帧率/带宽
```

---

## 4. 手动测试用例

每个用例记录 通过 / 失败 / 受阻 及备注。除特别说明外，"查看器"指支持
VeNCrypt 的 VNC 客户端（推荐 TigerVNC）。

### 4.1 设置界面

**M-SET-1 —— 开关** *（已自动化 —— 可选的人工抽查）*
已由 `vnc.Settings`（开关往返 + 指纹显示）、`vnc.DBusAPI`（启用/禁用时监听器
开启/关闭）与 `vnc.RebootPersistence`（状态在重启后保持）覆盖。以下步骤仅在需要
人工核对 UI 时执行。
前置条件：已登录。
1. 打开设置，找到 VNC / 远程访问区域。
2. 打开开关。预期：状态变为已启用；显示连接信息（IP:端口）与 TLS 指纹；
   查看器可以连接。
3. 关闭开关。预期：已连接的查看器被断开；新连接被拒绝；重启后状态保持。

**M-SET-2 —— 指纹核对**
1. 启用 VNC 后，读取设置中显示的 SHA-256 指纹。
2. 用能显示服务器证书的查看器连接。
   预期：两处指纹完全一致。

**M-SET-3 —— 设置与清除密码**
1. 在设置中设置密码。预期：查看器此后必须输入密码；错误密码被拒；正确
   密码可连接。
2. 清除密码。预期：查看器无需密码即可连接（TLS 依然生效）。

**M-SET-4 —— 重新生成证书**
1. 记下当前指纹；点击重新生成。
   预期：出现新指纹；信任过旧证书的查看器会警告证书变化；若有活动会话，
   设置界面会给出提醒。

### 4.2 OOBE 流程

**M-OOBE-1 —— 远程开箱设置，保留启用并设密码**
前置条件：设备已恢复出厂，OOBE 期间可通过 VNC 连接。
1. 从查看器远程完成 OOBE 各步骤。
2. OOBE 结束时，预期：出现 VNC 设置屏（因为存在活动的 VNC 会话），
   内容包括功能说明、密码及确认密码输入框、"允许不安全连接"选项、
   启用与禁用按钮。
3. 输入两次一致的密码并点击启用。
   预期：完成设置；VNC 保持启用；重新连接需要新密码。

**M-OOBE-2 —— 远程开箱设置，最后选择禁用**
同 M-OOBE-1，但点击禁用。预期：OOBE 结束后 VNC 服务被禁用；无法再连接；
设置中显示为关闭。

**M-OOBE-3 —— 不设密码直接启用**
同 M-OOBE-1，但密码留空点击启用。
预期：弹出确认对话框，警告未设密码即启用的风险；取消则返回表单；确认后
以无密码（仅 TLS）方式启用。

**M-OOBE-4 —— 本地开箱设置（无 VNC 会话）**
在没有任何 VNC 客户端连接的情况下本地完成 OOBE。
预期：*不*显示 VNC 设置屏；OOBE 结束后 VNC 服务默认被禁用。

### 4.3 查看与输入

**M-INP-1 —— 登录前控制**
前置条件：设备处于登录/锁定屏幕，VNC 已启用。
1. 连接并确认登录屏正常渲染、实时更新。
2. 点击密码框，输入一个错误密码并回车。
   预期：点击与击键位置准确（出现圆点、显示错误提示）；指针位置与本地
   屏幕一致。

**M-INP-2 —— 登录后指针能力对齐**
前置条件：已登录，查看器已连接。
1. 在桌面上右键。预期：弹出上下文菜单。
2. 在浏览器中对链接按中键。预期：新标签页打开。
3. 在长页面中双向滚动滚轮。预期：滚动顺滑。
4. 拖动窗口标题栏移动窗口；拖选文本。预期：拖拽不粘滞、不丢失。

**M-INP-3 —— 键盘布局与输入法**
前置条件：已登录，DUT 上添加了非美式布局或中日韩输入法。
1. 通过 VNC 在文本框中输入字母、符号和需要 Shift 的字符。
   预期：字符与 DUT 当前布局一致。
2. 切换到输入法并进行组合输入（如拼音 → 候选词选择）。
   预期：组合与候选窗口的行为与本地键盘一致。

**M-CLP-1 —— 剪贴板**
前置条件：查看器支持剪贴板（TigerVNC / noVNC 剪贴板面板）。
1. 在查看器一侧复制文本，通过 VNC 粘贴到 DUT 上的编辑器。
2. 在 DUT 上复制文本，确认到达查看器剪贴板。
   预期：双向均能正确传输纯文本（长文本可达约 1 MB；使用扩展剪贴板的
   客户端可保留非 ASCII 字符）。

### 4.4 显示器与会话

**M-DSP-1 —— 显示器选择体验**
前置条件：接入两台显示器，已登录。
1. 通过显示器选择接口（或 `vnc.MultiDisplay` 的自动切换）依次查看每台
   显示器。
   预期：每台显示器画面正确；输入落在正在查看的显示器上；几何尺寸变化
   由查看器正常处理、无花屏。

**M-DSP-2 —— 显示器热插拔**
1. 正在查看外接显示器时将其拔掉。
   预期：会话回落到剩余/主显示器，而不是卡死或断开。
2. 重新接上显示器。预期：它重新出现在显示器列表中并可再次选择。

**M-SES-1 —— 查看期间登录/登出**
1. 在登录屏连接，远程观察登录过程。
   预期：能看到切换到桌面的过程；短暂的画质变化（登录屏模式帧率较低）
   可接受；不出现永久卡死或断开；切换后输入正常。
2. 保持连接时登出。预期：画面回到登录屏并保持实时；登录屏上输入正常。

**M-SES-2 —— 锁屏**
查看器连接期间锁定会话。
预期：远程可看到锁屏；可通过 VNC 输入密码解锁。

**M-SES-3 —— 休眠 / 唤醒**
1. 查看器连接期间休眠 DUT（合盖或电源菜单），然后唤醒。
   预期：唤醒后守护进程重新接管 —— 重新连接查看器可得到实时画面；设备
   不会因 VNC 而无法休眠。

### 4.5 客户端、网页访问、发现

**M-CLI-1 —— 客户端兼容性矩阵**
使用下列每个客户端进行连接、认证（有密码与无密码两种）、查看、输入，
并逐一记录结果：
- TigerVNC（Linux/Windows）—— 首选参考客户端
- RealVNC Viewer
- noVNC（任意现代浏览器，走内置网页访问）
- 一款移动客户端（如 Android 的 AVNC，或其他支持 VeNCrypt 的应用）
预期：支持 VeNCrypt 的客户端功能完整。不支持 VeNCrypt/TLS 的客户端必须
被*拒绝*，除非启用了"允许不安全连接" —— 这种拒绝是正确行为，不是缺陷。

**M-WEB-1 —— 完整 noVNC 会话**
1. 浏览器访问 `https://<DUT_IP>:6080/vnc.html`，接受自签名证书警告。
2. 连接，如设有密码则输入，操作桌面（打字、点击、滚动），使用 noVNC
   剪贴板面板，然后断开。
   预期：无需任何插件即可在浏览器中完成完整会话。

**M-NET-1 —— mDNS 发现**
在支持 zeroconf/Bonjour 浏览的客户端上（如 `avahi-browse -rt _rfb._tcp`
或移动 VNC 应用的发现界面）确认：启用时设备以正确端口公告自身
（"FydeOS VNC on <主机名>"），禁用后公告消失。

**M-NET-2 —— 网络切换**
启用状态下让 DUT 在 Wi-Fi 与有线之间切换（或更换接入点）。
预期：服务重新绑定到新的局域网地址；设置界面显示新地址；查看器可通过
新地址连接。

### 4.6 安全与性能

**M-SEC-1 —— 认证失败行为**
设置密码后，连续多次用错误密码尝试登录，随后再用正确密码登录。
预期：错误尝试始终被拒；服务保持存活（不崩溃、不拒绝服务）；记录快速
重试是否被限流。

**M-SEC-2 —— 线路上无明文**
默认设置下（不安全模式关闭），用只支持非加密 VNC 的客户端尝试连接。
预期：在发送任何屏幕数据之前连接即被拒绝。

**M-PRF-1 —— 延迟与画质体验**
在空闲局域网中，单个查看器、原生分辨率下：
1. 通过 VNC 在终端/编辑器中打字。预期：回显感觉即时（目标 < 80 ms；
   可与物理屏幕并排对比）。
2. 画圈拖动窗口；全屏播放视频。
   预期：运动画面连贯（允许少量模糊/撕裂）；界面保持响应；DUT 不过热、
   前台应用不被拖慢。

---

## 5. 覆盖矩阵

| 功能 | 自动化 | 手动 |
|---|---|---|
| 服务启动/停止/自动重启/沙箱 | `vnc.Lifecycle` | — |
| 设置开关与状态 | `vnc.Settings`、`vnc.DBusAPI` | — |
| 开关对监听器的生命周期控制 | `vnc.DBusAPI`、`vnc.NetworkBinding` | — |
| 启用状态在重启后保持 | `vnc.RebootPersistence` | — |
| 强制 TLS、证书、指纹 | `vnc.TLSCertificate`、`vnc.AuthMatrix` | M-SET-2、M-SEC-2 |
| 证书重新生成 | `vnc.DBusAPI`、`vnc.TLSCertificate`、`vnc.Settings` | M-SET-4 |
| 密码认证（bcrypt、实时生效） | `vnc.DBusAPI`、`vnc.AuthMatrix`、`vnc.SettingsPassword` | — |
| 不安全客户端模式（VncAuth/明文） | `vnc.AuthMatrix` | M-CLI-1 |
| 回环/网页认证快速通道 | `vnc.AuthMatrix`、`vnc.NoVNCWeb` | M-WEB-1 |
| 仅绑定局域网 | `vnc.NetworkBinding` | M-NET-2 |
| mDNS 发现 | `vnc.NetworkBinding`（出现+撤销）；`x01`（外部传播） | — |
| 登录前（KMS）捕获 | `vnc.CaptureKMS`、`vnc.SessionTransition` | M-INP-1 |
| 登录后（合成器）捕获 | `vnc.CaptureChrome` | M-PRF-1 |
| 增量更新流、帧率 | `vnc.CaptureChrome`、`vnc.Performance`；`x03` | M-PRF-1 |
| 帧缓冲几何/对齐/尺寸变化 | `vnc.CaptureKMS`、`vnc.MultiDisplay` | M-DSP-1 |
| 登录前 / 锁屏输入 | `vnc.LockScreen`、`vnc.LoginScreenInput` | — |
| 登录后输入（按键、滚轮、键盘） | `vnc.InputInjection`、`vnc.InputParity` | M-INP-2（拖拽）、M-INP-3 |
| 输入法 / 键盘布局 | — | M-INP-3 |
| 剪贴板（查看器→设备 粘贴） | `vnc.Clipboard` | M-CLP-1（设备→查看器 推送，未实现） |
| 多显示器枚举/切换 | `vnc.MultiDisplay` | M-DSP-1 |
| 显示器热插拔 | `vnc.DisplayHotplug` | — |
| 登出/登录/崩溃健壮性 | `vnc.SessionTransition` | M-SES-1 |
| 锁屏 | `vnc.LockScreen` | — |
| 休眠/唤醒 | `vnc.SuspendResume` | — |
| 重启持久化 | `vnc.RebootPersistence` | — |
| noVNC 网页访问 | `vnc.NoVNCWeb`、`vnc.NoVNCSession`；`x01`、`x03` | — |
| OOBE 设置屏 | — | M-OOBE-1…4 |
| 并发查看器 | `x02` | M-CLI-1 |
| 客户端兼容性 | 协议层：`vnc.AuthMatrix`、`vnc.CaptureChrome` | M-CLI-1 |
| CPU 指标 | `vnc.Performance` | M-PRF-1 |
| 主机侧延迟/吞吐 | `x03` | M-PRF-1 |
| 认证滥用防护 | `vnc.AuthAbuse` | — |

说明：
- 仍为手动的项：输入法/布局（M-INP-3）、指针拖拽（M-INP-2 的一部分）、
  剪贴板设备→查看器方向（M-CLP-1，守护进程未实现）、网络切换（M-NET-2）、
  第三方查看器兼容性（M-CLI-1）、主观延迟/画质体验（M-PRF-1）、
  以及 OOBE 设置屏（M-OOBE-1…4）。OOBE 设置屏仅在 OOBE 结束时有 VNC 客户端
  连接的情况下才出现，因此由 M-OOBE 覆盖，而非自动化套件。

---

## 6. 建议的测试轮次

- **冒烟测试（约 4 分钟，自动化）**：`tast run <dut> 'vnc.Lifecycle
  vnc.DBusAPI vnc.CaptureKMS'` —— 服务在线、API 正常、画面可达。
- **完整自动化（24 个测试，约 15–25 分钟）**：对 test-image 的 DUT 运行
  `tast run <dut> 'vnc.*'`（多显示器/热插拔检查需接入第二显示器），再从
  DUT 所在局域网的主机运行 `./run_all.sh` 完成外部检查。包含重启、休眠/唤醒
  与登录屏流程，因此整体耗时长于单条测试之和。
- **版本回归**：完整自动化 + 第 4 节全部手动用例，包括客户端矩阵与在
  恢复出厂设备上的 OOBE 用例。
