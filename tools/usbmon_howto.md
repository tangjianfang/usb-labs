# Linux usbmon 启用速查（各发行版差异）

> usbmon 是 Linux 内核的 USB 抓包机制：把主机控制器收发的 **URB（submit/complete）**
> 以 pcap 格式喂给 Wireshark/tshark。它看到的是主机侧事务语义，看不到线级
> NAK/重试/位填充——需要线级细节时换硬件分析仪（见 `../captures/README.md`）。
> Windows 对应物是 USBPcap；两者产物同为 pcapng，可互换分析。

## 1. 通用三步（99% 的发行版）

```bash
# ① 加载模块（绝大多数发行版内核已带 CONFIG_USB_MON, 只是未自动加载）
sudo modprobe usbmon
ls -l /dev/usbmon*        # usbmonN 对应总线 N（lsusb 第一列）; usbmon0 = 全部总线

# ② 找到目标设备
lsusb                     # 例如 Bus 002 Device 005: ID 2341:0001
                          # → 抓 usbmon2

# ③ 抓包
sudo tshark -i usbmon2 -w cap.pcapng          # 命令行
sudo wireshark                                # 图形界面: 接口列表选 usbmon2
```

内核较老的系统若 debugfs 未挂载（新版仅影响文本接口, 不影响 /dev/usbmonN）：

```bash
ls /sys/kernel/debug 2>/dev/null || sudo mount -t debugfs none /sys/kernel/debug
```

免 root 抓包（Wireshark 图形界面日常使用的正解）：

```bash
sudo groupadd -r usbmon
sudo usermod -aG usbmon $USER
echo 'SUBSYSTEM=="usbmon", KERNEL=="usbmon*", MODE="0640", GROUP="usbmon"' \
  | sudo tee /etc/udev/rules.d/99-usbmon.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
# 重新登录后生效; Wireshark 需以 dumpcap 具备抓包能力为前提(见 §3 排错)
```

## 2. 各发行版差异表

| 发行版 | 模块加载 | 权限/组 | 备注 |
|---|---|---|---|
| **Debian / Ubuntu / Raspberry Pi OS** | `sudo modprobe usbmon`（模块为 `usbmon`, kernel 已编译） | 建 `usbmon` 组 + udev 规则（如上）；Ubuntu 老教程的 `dialout` 组**不是**充分条件 | 开机自动加载：`echo usbmon | sudo tee /etc/modules-load.d/usbmon.conf` |
| **Arch Linux / Manjaro** | 同上；内核默认 `CONFIG_USB_MON=m` | 同上 udev 规则；需安装 `wireshark-qt` + `wireshark-cli`，安装时把用户加入 `wireshark` 组并 `setcap` dumpcap | archwiki "Wireshark" 页与 "USB capture" 页有现成规则 |
| **Fedora / RHEL / CentOS Stream** | `sudo modprobe usbmon`；Fedora 内核已内建或模块化（随版本） | 同上；SELinux 通常无碍（usbmon 是标准设备节点） | RHEL 桌面较少见, 服务器无头抓包用 tshark |
| **openSUSE** | 同上 | 同上 | YaST 内核参数无需改动 |
| **NixOS** | 需声明：`boot.kernelModules = [ "usbmon" ];` | `users.groups.usbmon = {};` + `users.users.<you>.extraGroups = [ "usbmon" ];` + `services.udev.extraRules`（同样规则文本） | 不声明则 /dev/usbmon* 不出现 |
| **Alpine / Buildroot / Yocto 嵌入式** | 内核可能**未编 CONFIG_USB_MON**——需重编内核或选模块 | 设备节点权限手动 chmod 或 mdev 规则 | 抓板卡自身 as host 的场景 |
| **Android（设备端 as host, OTG）** | 量产内核一般**不含** usbmon；root + 自编内核才可行 | — | 调试 OTG 协议建议换 Linux 主机 + usbip |
| **WSL2 / 容器** | **不可用**：WSL2 无真实 USB 主机栈（usbipd-win 是转发不是抓包） | — | 抓 Windows 侧流量用 USBPcap |
| **macOS** | **无 usbmon 等价物** | — | 用硬件分析仪或 usbip 透传到 Linux（见 captures/README） |

> 版本注：老内核（<3.x 时代）的 usbmon 文本接口在 `/sys/kernel/debug/usb/usbmon/Nu`
> （`1u` = 总线1 全端点, `1t` 含完整时间戳格式）；现代发行版直接用 `/dev/usbmonN` 二进制接口即可,
> 文本接口仅在无 Wireshark 的板子上应急（`cat /dev/usbmon1 | xxd`）。

## 3. 排错清单

| 症状 | 根因与解法 |
|---|---|
| `modprobe: FATAL: Module usbmon not found` | 内核未编 CONFIG_USB_MON——换发行版内核或重编（嵌入式常见） |
| Wireshark 接口列表没有 usbmonN | 模块没加载（先 `ls /dev/usbmon*`）；或 Wireshark 是 snap/flatpak 没拿到设备访问——改用发行版原生包 |
| `Permission denied` 抓不了 | udev 规则没生效（`udevadm trigger` 后需重登录）；或 dumpcap 无能力：`sudo setcap cap_net_raw,cap_net_admin+ep $(readlink -f $(which dumpcap))` |
| 抓到了但全是枚举期流量 | 插入发生在抓包之前——拔掉设备再插, 或用集线器保持常连（注意 hub 会引入 TT 延迟） |
| 抓不到某设备流量 | 总线号看错（`lsusb` 热插拔后总线可能变化）；设备挂在 USB3 口以 SS 速率跑时流量同样出现在对应 usbmonN（usbmon 对 xHCI 有效） |
| 大流量丢包 | 提高 snaplen 并落到本地盘：`tshark -i usbmon2 -s 0 -w cap.pcapng`；或事后分析用环形缓冲 |
| 时间戳怪异 | usbmon 用内核时间戳；跨主机对比时统一 `tshark -t r`（相对时间） |

## 4. 一条工作流（可以直接抄）

```bash
# 目标: 抓一枚 U 盘插入后的完整枚举 + 读扇区
sudo modprobe usbmon
sudo tshark -i usbmon2 -s 0 -w usbdisk.pcapng &
# ……插入 U 盘, 操作文件管理器……
sudo kill %1
tshark -r usbdisk.pcapng -Y "usb.transfer_type==0x02" -V | less   # 先看枚举
tshark -r usbdisk.pcapng -Y "ums.bCSWStatus==1" -V | less         # 再找失败命令
```

对照教学样本：枚举段逐包含义见 [`../captures/00-全速枚举逐包标注.md`](../captures/00-全速枚举逐包标注.md)，
读扇区三段见 [`../captures/03-批量BOT读扇区.md`](../captures/03-批量BOT读扇区.md)。
过滤器速查见同目录 [wireshark_filters.md](wireshark_filters.md)。

## 参考资源

- 内核 usbmon 文档: https://www.kernel.org/doc/html/latest/usb/usbmon.html
- Wireshark USB 捕获页（含各发行版权限配置）: https://wiki.wireshark.org/CaptureSetup/USB
- Arch Wiki: https://wiki.archlinux.org/title/Wireshark 与 https://wiki.archlinux.org/title/USB_capture
- USBPcap（Windows 对应物）: https://desowin.org/usbpcap/
- 知识库对照: USBTree `70-枝干-调试测试与安全/01-协议分析仪与抓包.md`
