# FydeOS VNC — 测试文档

本文档说明如何测试 FydeOS VNC 远程访问功能，包含两套互补的自动化测试：

- **设备端自动化测试（Tast）** —— `vnc.*` 测试在设备上运行，端到端覆盖
  守护进程：D-Bus 控制 API、RFB/VeNCrypt 协议、TLS、认证、画面捕获、
  输入注入、设置界面、多显示器、noVNC 网页访问、会话切换以及 CPU。
- **外部观察者测试（shell 套件）** —— 一小组只有从 DUT 所在局域网的另一台
  主机才有意义的检查：mDNS 传播、并发查看器、主机侧性能。
`vnc.*` Tast 测试与 `x0*` shell 检查合在一起覆盖全部 VNC 功能 —— 没有手动
用例。第 5 节的覆盖矩阵列出了每个功能对应的测试。

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
| 性能 | 空闲 CPU < 1%、串流 CPU < 15%、局域网输入延迟 < 80 ms（设计指标） |

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
| TigerVNC（`tigervnc-viewer`）+ `xvfb` | 真实客户端检查（`x04`） | `x04` 需要 |

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

结果、每条测试的日志与失败截图由 Tast 收集到其结果目录中。每条测试都会
恢复密码与启用状态，会话切换测试会恢复登录屏；注意 `vnc.LoginScreenInput`
会留下一个本地帐号（下一个基于 Chrome 的测试会清除），两条主机驱动的测试
会重启/休眠设备。

| 测试 | 验证内容 |
|---|---|
| `vnc.Sanity` | 镜像自带 VNC 功能：upstart 任务与守护进程二进制存在 |
| `vnc.Lifecycle` | 两个系统服务均在运行；守护进程以 `fydeos-vnc` 用户沙箱化运行；状态目录权限；持有 D-Bus 名称；强杀后自动重启并重新持有 D-Bus 名称 |
| `vnc.DBusAPI` | `GetStatus` 字段；开关往返会关闭/打开 RFB 端口；`StatusChanged` 信号；`ListDisplays` JSON；非法 `SetCaptureDisplay` 返回带类型的错误；密码文件（0600、bcrypt）；`RegenerateCertificate` |
| `vnc.NetworkBinding` | 绑定地址为真实、在线的局域网接口（RFC-1918/链路本地）；RFB + noVNC 监听；关闭时撤销、开启时恢复；avahi mDNS 公告（启用时存在、禁用时撤销） |
| `vnc.TLSCertificate` | 证书/私钥权限；RSA-4096 自签名；D-Bus 与 RFB 线上指纹一致；拒绝 TLS 1.1、接受 1.2；重新生成无需重启即生效 |
| `vnc.AuthMatrix` | 局域网只提供 VeNCrypt（无密码 X509None、有密码 X509Plain）；错误密码被拒；改密码立即生效；回环 SecTypeNone / VeNCrypt Plain；不安全模式 VncAuth（DES） |
| `vnc.CaptureKMS` | 登录前 KMS 路径：KMS 模式、宽度按 4 对齐、可解码且非空白的帧 |
| `vnc.CaptureChrome` | 登录后 FrameSink 路径：纯色页面在捕获的帧缓冲中被还原（闭环） |
| `vnc.InputInjection` | 经线路注入的 RFB 指针与按键事件在 Chrome 会话中产生预期反应 |
| `vnc.Settings` | os-settings VNC 卡片驱动守护进程：开关往返、指纹显示与 D-Bus 一致、证书重新生成 |
| `vnc.NoVNCWeb` | 以守护进程证书通过 HTTPS 提供 noVNC 页面；websockify 后端；明文 HTTP 策略与 WebSocket 升级为观察性检查（记录、不判失败） |
| `vnc.SessionTransition` | KMS（登录前）→ FrameSink（登录）→ KMS（登出），守护进程全程不掉线且持续提供画面 |
| `vnc.MultiDisplay` | 显示器清单；切换捕获显示器后帧缓冲变为该显示器几何尺寸并可切回；帧内容为该显示器画面且注入点击落在该显示器上；未知 id 错误路径（仅一台显示器时跳过切换检查） |
| `vnc.Performance` | 守护进程空闲与查看器串流时的 CPU 及点击到像素延迟，按随分辨率缩放的回归上限上报 perf 管线（Raw-only 客户端与 Tight 查看器不可直接比较 —— Tight 侧 CPU 数字由 `x04` 提供） |
| `vnc.InputParity` | 经 RFB 注入的右键弹出上下文菜单、中键打开新标签页、滚轮滚动、拖拽选中文本、拖动标题区移动窗口 |
| `vnc.Clipboard` | 查看器的 RFB cut-text 被粘贴进聚焦的输入框；设备侧复制推送到查看器 —— legacy Latin-1 ServerCutText 与 Extended Clipboard（notify→request→provide、UTF-8 完整、连接时同步、约 1 MB 负载） |
| `vnc.AuthAbuse` | 反复错误密码均被拒且守护进程保持健康（未实现按 IP 退避） |
| `vnc.SettingsPassword` | 经 os-settings 密码对话框设置密码后守护进程要求密码 |
| `vnc.LockScreen` | 锁定期间守护进程提供锁屏画面，且经 VNC 注入的击键可驱动锁屏：错误密码被拒、正确密码解锁 |
| `vnc.LoginScreenInput` | 在真正的开机登录屏（无会话）经 VNC 驱动"添加用户 → 使用本地帐号"流程创建本地帐号并登录 |
| `vnc.NoVNCSession` | 内置 noVNC 网页客户端在浏览器标签页中连接并渲染桌面 |
| `vnc.DisplayHotplug` | 强制外接显示器连接器断开/接入后守护进程清单更新且采集优雅回落（需外接显示器） |
| `vnc.OOBESetup` | OOBE 设置屏仅在结束时有查看器连接时出现；启用保持服务监听、禁用关闭服务、无查看器的本地设置静默禁用 |
| `vnc.RebootPersistence` | *(主机驱动)* 启用状态（Chrome pref）跨重启保持 |
| `vnc.SuspendResume` | *(主机驱动)* 休眠/唤醒一轮后守护进程仍在提供服务 |

### 3.2 外部观察者套件（shell）

这些检查需要 DUT 所在局域网内的另一台主机；它们是对 Tast 测试的补充，
而非重复。

```bash
cd tests
./run_all.sh            # x01 + x02 + x03 + x04
./run_all.sh x02        # 只运行子集
```

每项检查输出 `[PASS]`/`[FAIL]`/`[WARN]`/`[SKIP]`；`run_all.sh` 在
`tests/artifacts/` 下输出合并报告。

| 脚本 | 验证内容 |
|---|---|
| `x01_discovery.sh` | mDNS `_rfb._tcp` "FydeOS VNC" 记录对局域网内其他主机可见；RFB/noVNC 端口可从另一台机器访问 |
| `x02_multiclient.sh` | 多个查看器同时连接 —— 各自完成握手与抓帧 —— 之后守护进程仍健康 |
| `x03_performance.sh` | 主机侧整帧往返（连接+TLS+认证+首帧）以及查看器在线路上观察到的更新吞吐/帧率 |
| `x04_tigervnc.sh` | 独立客户端（TigerVNC，Xvfb 无头运行）完成 VeNCrypt/X509 握手、打开真实桌面，且守护进程保持健康；同时在指针驱动的画面变化下采样守护进程 Tight 路径串流 CPU（Tast 客户端仅支持 Raw） |

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

无。全部 VNC 功能均已由第 3 节的自动化套件覆盖 —— 第 5 节的覆盖矩阵列出了
每个功能对应的测试。

明确交给用户验收测试（UAT）而不写成脚本用例的部分：主观画质体验（运动、
视频、清晰度），以及 TigerVNC 与内置 noVNC 之外的客户端兼容性（RealVNC、
移动端应用）。另外不测试：输入法组合与非美式键盘布局；以及网络切换后的
运行时重新绑定 —— 守护进程仅在启动时绑定一次，因此 Wi-Fi/有线切换后查看器
需以新地址重新连接。

---

## 5. 覆盖矩阵

| 功能 | 自动化 | 手动 |
|---|---|---|
| 服务启动/停止/自动重启/沙箱 | `vnc.Lifecycle` | — |
| 设置开关与状态 | `vnc.Settings`、`vnc.DBusAPI` | — |
| 开关对监听器的生命周期控制 | `vnc.DBusAPI`、`vnc.NetworkBinding` | — |
| 启用状态在重启后保持 | `vnc.RebootPersistence` | — |
| 强制 TLS、证书、指纹 | `vnc.TLSCertificate`、`vnc.AuthMatrix`、`vnc.Settings` | — |
| 证书重新生成 | `vnc.DBusAPI`、`vnc.TLSCertificate`、`vnc.Settings` | — |
| 密码认证（bcrypt、实时生效） | `vnc.DBusAPI`、`vnc.AuthMatrix`、`vnc.SettingsPassword` | — |
| 不安全客户端模式（VncAuth/明文） | `vnc.AuthMatrix` | — |
| 回环/网页认证快速通道 | `vnc.AuthMatrix`、`vnc.NoVNCWeb` | — |
| 仅绑定局域网 | `vnc.NetworkBinding` | — |
| mDNS 发现 | `vnc.NetworkBinding`（出现+撤销）；`x01`（外部传播） | — |
| 登录前（KMS）捕获 | `vnc.CaptureKMS`、`vnc.SessionTransition`、`vnc.LoginScreenInput` | — |
| 登录后（合成器）捕获 | `vnc.CaptureChrome` | — |
| 增量更新流、帧率 | `vnc.CaptureChrome`、`vnc.Performance`；`x03` | — |
| 帧缓冲几何/对齐/尺寸变化 | `vnc.CaptureKMS`、`vnc.MultiDisplay` | — |
| 登录前 / 锁屏输入 | `vnc.LockScreen`、`vnc.LoginScreenInput` | — |
| 登录后输入（按键、滚轮、拖拽、键盘） | `vnc.InputInjection`、`vnc.InputParity` | — |
| 剪贴板（双向） | `vnc.Clipboard`（查看器→设备粘贴；设备→查看器 legacy + Extended Clipboard、连接时同步、UTF-8、约 1 MB 负载） | — |
| 多显示器枚举/切换/内容/输入 | `vnc.MultiDisplay` | — |
| 显示器热插拔 | `vnc.DisplayHotplug` | — |
| 登出/登录/崩溃健壮性 | `vnc.SessionTransition` | — |
| 锁屏 | `vnc.LockScreen` | — |
| 休眠/唤醒 | `vnc.SuspendResume` | — |
| 重启持久化 | `vnc.RebootPersistence` | — |
| noVNC 网页访问 | `vnc.NoVNCWeb`、`vnc.NoVNCSession`；`x01`、`x03` | — |
| OOBE 设置屏（门控、启用、禁用） | `vnc.OOBESetup` | — |
| 并发查看器 | `x02` | — |
| 客户端兼容性 | `x04`（TigerVNC：VeNCrypt X509 + 真实桌面）；协议层：`vnc.AuthMatrix` | — |
| CPU 指标 | `vnc.Performance` | — |
| 输入（点击到像素）延迟 | `vnc.Performance`（上报，80 ms 指标） | — |
| 主机侧延迟/吞吐 | `x03` | — |
| 认证滥用防护 | `vnc.AuthAbuse` | — |

说明：
- 第 4 节已为空：所有功能均有自动化测试。交给用户验收测试的部分：主观画质，
  以及 TigerVNC（由 `x04` 覆盖）与内置 noVNC 之外的客户端。
- 明确不测试的项：输入法组合与非美式键盘布局；以及网络切换后的运行时重新
  绑定 —— 守护进程未实现该能力（仅在启动时绑定一次，因此 Wi-Fi/有线切换后
  查看器需以新地址重新连接）。
- 本文档曾列为手动的全部用例 —— 开关、指纹、密码、证书重新生成、登录前与
  锁屏输入、会话切换、休眠/唤醒、mDNS、认证滥用、无明文、显示器热插拔、
  完整 noVNC 会话、指针拖拽、多显示器内容/输入、剪贴板（双向）、OOBE 设置屏、
  点击延迟、TigerVNC 兼容性 —— 现均已自动化；见上方矩阵。
- Tight 编码仅由真实查看器（noVNC 会话与 `x04` 的 TigerVNC）请求；本套件
  自身的 RFB 客户端仅解码 Raw。

---

## 6. 建议的测试轮次

- **冒烟测试（约 4 分钟，自动化）**：`tast run <dut> vnc.Lifecycle
  vnc.DBusAPI vnc.CaptureKMS` —— 服务在线、API 正常、画面可达。
- **完整自动化（25 个测试，约 15–25 分钟）**：对 test-image 的 DUT 运行
  `tast run <dut> 'vnc.*'`（多显示器/热插拔检查需接入第二显示器），再从
  DUT 所在局域网的主机运行 `./run_all.sh` 完成外部检查。包含重启、休眠/唤醒
  与登录屏流程，因此整体耗时长于单条测试之和。
- **版本回归**：先执行上述完整自动化，再安排一次 UAT，覆盖脚本无法判断的
  部分 —— 真实使用中的画质，以及本版本需要支持的其他 VNC 客户端。
