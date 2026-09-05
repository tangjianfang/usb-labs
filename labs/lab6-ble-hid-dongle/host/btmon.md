# Lab6 主机侧 BLE 调试：nRF Sniffer · Wireshark 过滤 · btmon

> 目标：把 [../capture/BLE连接帧分析.md](../capture/BLE连接帧分析.md) 里"基于 Core Spec 重构"的序列，
> 在你自己的真实抓包里逐帧对上。HCI/链路层背景：知识库
> [01-蓝牙体系架构与HCI](https://github.com/tangjianfang/USBTree/blob/main/50-枝干-无线关联/BLE-低功耗蓝牙/01-蓝牙体系架构与HCI.md)、
> [02-链路层与物理层](https://github.com/tangjianfang/USBTree/blob/main/50-枝干-无线关联/BLE-低功耗蓝牙/02-链路层与物理层.md)。

## 1. nRF Sniffer for Bluetooth LE（外部嗅探，看链路层）

**硬件**：nRF52840 Dongle（PCA10059，约 10 美元量级）或 nRF52840 DK。
**软件**：Wireshark + Nordic 官方嗅探固件与插件。

安装/使用步骤（以官方用户指南为准）：

1. 从 Nordic 官网下载 nRF Sniffer for Bluetooth LE 资源包（固件 + Wireshark 插件 + pyserial 脚本）：
   https://www.nordicsemi.com/Products/Development-tools/nRF-Sniffer-for-Bluetooth-LE
2. 刷入嗅探固件到 Dongle（资源包内含说明，DK 用 SWD 烧写，Dongle 用 USB bootloader）。
3. Wireshark 里安装 `extcap/` 插件脚本，重启 Wireshark，接口列表出现 "nRF Sniffer for Bluetooth LE"。
4. 开始捕获：先在广播列表里看到你的键盘（设备名/地址），点选 → Follow 该连接，之后只抓这条链路（或抓 ALL advertising 广播再跟踪）。
5. 键盘上按一个键 → 分组列表里出现该连接上的 ATT Handle Value Notification。

> 另一条路：**Android 手机开发者选项 → 启用蓝牙 HCI 信息收集日志**，得到 `btsnoop_hci.log`（HCI 层视角，链路层细节少，但配对/GATT 流程足够分析），拷出后直接用 Wireshark 打开。

## 2. Wireshark BLE 常用过滤式（均为真实可用字段）

| 目的 | 过滤式 |
|---|---|
| 只看 HID 报告（Handle Value Notification） | `btatt.opcode == 0x1b` |
| 只看某句柄的报告（先从 Report Map 读到 handle） | `btatt.opcode == 0x1b && btatt.handle == 0x0015`（句柄按实机替换） |
| ATT 写命令（CCCD 使能 / 输出报告） | `btatt.opcode == 0x52` |
| SMP 配对全过程 | `btsmp.opcode` |
| 连接建立事件（HCI LE Connection Complete） | `bthci_evt.code == 0x3e && bthci_evt.le_meta_subevent == 0x01` |
| 广播包中的设备名 | `btcommon.eir_ad.entry.device_name contains "你的设备名"` |
| 链路层控制包（LLCP） | `btll.cb`（LL 控制域非空；版本/特性交换等在此层） |

分析技巧：

- View → Coloring Rules：Wireshark 自带 BLE 配色，ATT 通知一目了然。
- 对 Notification 按句柄 Statistics → conversations 分组，可统计报告节奏（= 连接间隔的真实节奏）。
- 按键延迟测量：抓包时间戳（µs 级）对比"按键动作 → Notification"用高速摄像/录音对齐法（见 [../experience.md](../experience.md) §2）。

## 3. btmon（Linux 主机侧，看 HCI）

`btmon` 是 BlueZ 自带的 HCI 监视工具，直接解码内核与控制器之间收发的 HCI 命令/事件/ACL：

```bash
sudo btmon                       # 实时滚动
sudo btmon -w trace.snoop        # 写成 btsnoop 格式 → 可直接用 Wireshark 打开
```

配合操作（配对、连接、GATT 检查）：

```bash
bluetoothctl                     # 配对/信任/连接管理（现代入口）
scan on
pair XX:XX:XX:XX:XX:XX
trust   XX:XX:XX:XX:XX:XX
```

视角差异（调试时心里要有这张表）：

| 视角 | 工具 | 能看到 | 看不到 |
|---|---|---|---|
| 链路层（空口） | nRF Sniffer | ADV/CONNECT_IND/LLCP 全过程 | 主机协议栈内部状态 |
| HCI（主机-控制器间） | btmon / btsnoop | HCI 命令与事件、GATT 交互 | 空口重传、跳频、广播细节 |

两把抓包交叉着看，是定位"配对成功但键不动"这类问题的标准打法：空口有 Notification 而 UI 无响应 → 主机侧问题；空口连 Notification 都没有 → 固件/链路问题。

## 4. 常见坑

- Sniffer **加密后失明**：连接加密后若无密钥，链路层数据解不开；nRF Sniffer 支持在 GUI 填入 LTK（从固件日志或配对过程抓取）后解密。
- 抓不到 CONNECT_IND：Sniffer 启动晚于连接。键盘重连太快时，先清绑定（长按配对键）再抓完整配对流程。
- `hcitool`/`hciconfig` 已弃用但仍常见于旧资料；新代码用 `bluetoothctl`/`btmgmt`。
- 抓包文件本身可入库对比：Wireshark wiki 样例库有公开 BLE 样例（https://wiki.wireshark.org/SampleCaptures），可作为"别人家真实抓包"的参照。

## 参考资源

- nRF Sniffer for Bluetooth LE（固件/插件/用户指南）：https://www.nordicsemi.com/Products/Development-tools/nRF-Sniffer-for-Bluetooth-LE
- Wireshark 用户文档（BLE 剖析）：https://www.wireshark.org/docs/
- BlueZ（btmon/bluetoothctl 所在套件）：https://www.bluez.org/
- 知识库：[01-蓝牙体系架构与HCI](https://github.com/tangjianfang/USBTree/blob/main/50-枝干-无线关联/BLE-低功耗蓝牙/01-蓝牙体系架构与HCI.md) · [03-广播与连接](https://github.com/tangjianfang/USBTree/blob/main/50-枝干-无线关联/BLE-低功耗蓝牙/03-广播与连接.md) · [12-LLCP控制过程全表](https://github.com/tangjianfang/USBTree/blob/main/50-枝干-无线关联/BLE-低功耗蓝牙/12-LLCP控制过程全表.md)
