# CDC 枚举与数据流：规范重构帧序列 + 真实抓包方法

> ⚠️ **诚实声明**：本文第 1~3 节的帧序列**基于 USB 2.0 规范与 CDC/PSTN 1.2 规范重构**（依据本 Lab 描述符在 [firmware/src/usb_descriptors.c](../firmware/src/usb_descriptors.c) 的实际取值逐字段推演），**不是真实抓包导出**；字段的字节序/取值均可对照规范核验。第 4 节给出真实抓包的完整命令，鼓励你抓一份自己的 pcap 存入 `captures/` 与本文对照。
> 概念背景：知识库 [虚拟串口 ACM](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/CDC-通信设备类/01-虚拟串口ACM.md) · [03-CDC规范级-请求与通知全表](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/CDC-通信设备类/03-CDC规范级-请求与通知全表.md)

## 1. 枚举阶段（控制传输，设备地址从 0 → N）

假设本 Lab 设备插入后被分配地址 12（Windows/Linux 均如此，数字随机）。以下为**基于规范重构**的枚举序列（细节如"先取 8 字节再取全长"等时序属于规范允许/常见实现，不同主机有差异）：

| # | 事务 | bmRequestType | bRequest | wValue | wIndex | wLength | 数据（IN/OUT） | 说明 |
|---|---|---|---|---|---|---|---|---|
| 1 | 初始复位 | — | — | — | — | — | — | 复位后设备以地址 0、全速应答（D+ 1.5kΩ 上拉已生效，见 hardware/设计要点.md） |
| 2 | GET_DESCRIPTOR(Device) 前 8 字节 | 0x80 | 0x06 | 0x0100 | 0 | 8 | 设备描述符前 8B（`12 01 00 02 EF 02 01 40`） | 先探 EP0 包长（bMaxPacketSize0=0x40） |
| 3 | USB RESET | — | — | — | — | — | — | 主机再复位一次，规整状态 |
| 4 | SET_ADDRESS | 0x00 | 0x05 | **12** | 0 | 0 | 无 | 设备从此用地址 12 应答 |
| 5 | GET_DESCRIPTOR(Device) 全长 | 0x80 | 0x06 | 0x0100 | 0 | 18 | 18B 设备描述符：VID=0xCAFE, PID=0x4010, bDeviceClass=0xEF（IAD 复合标记）, iSerial=3 | Wireshark 里可核对 VID/PID |
| 6 | GET_DESCRIPTOR(Config) 头 9B | 0x80 | 0x06 | 0x0200 | 0 | 9 | 配置描述符头（wTotalLength=93） | 主机先问"总共多长" |
| 7 | GET_DESCRIPTOR(Config) 全长 | 0x80 | 0x06 | 0x0200 | 0 | 93 | 完整配置：**IAD(0x0B)** + 接口0(通信) + Header/ACM/Union/CallMgmt + 中断IN 0x82 + 接口1(数据) + 批量 0x01/0x81 + **接口2 DFU(0xFE/0x01/0x01)** + DFU 功能描述符 | 见第 2 节逐段拆解 |
| 8 | GET_DESCRIPTOR(String) LangID | 0x80 | 0x06 | 0x0300 | 0 | 4 | `09 04`（0x0409 英语） | |
| 9~11 | GET_DESCRIPTOR(String) i=1/2/3 | 0x80 | 0x06 | 0x0301/0302/0303 | 0x0409 | 适当 | "USB-Labs" / "USB-Labs CDC-DFU Lab2" / **24 位 hex UID 序列号** | 序列号来自芯片 96 位 UID（usb_descriptors.c） |
| 12 | SET_CONFIGURATION | 0x00 | 0x09 | 0x0001 | 0 | 0 | 无 | 配置值 1，接口进入可用态 |

**复合设备看点**：接口 0/1 由 **IAD（Interface Association Descriptor）** 绑定成一个 CDC 功能，设备级 bDeviceClass=0xEF/0x02/0x01 告诉主机"请按 IAD 分组解析"；接口 2 是独立的 DFU Runtime 功能（类码 0xFE 应用特定）。这是所有"HID+CDC 复合调试器"类产品的通用骨架。

## 2. 配置描述符内的 CDC 结构（对应第 1 节 #7 的 93 字节）

```text
09 02 5D 00 03 01 00 A0 FA          配置描述符：wTotalLength=0x005D=93，3 个接口
-- CDC 功能 1（IAD 绑定） ----------
08 0B 00 02 02 02 01 00             IAD: FirstIf=0, IfCount=2, Class=0x02/0x02/0x01
09 04 00 00 01 02 02 01 04          接口0 通信：1 端点（中断 IN），字符串 4
05 24 00 10 01                      CS_INTERFACE Header: bcdCDC=0x0110
04 24 02 02                         CS_INTERFACE ACM: bmCapabilities=0x02
05 24 06 00 01                      CS_INTERFACE Union: Master=0, Slave=1  ← 灵魂所在
05 24 01 00 01                      CS_INTERFACE CallMgmt: bmCap=0x00, DataIf=1
07 05 82 03 08 00 02                端点 0x82 中断 IN，包长 8，bInterval=2ms
09 04 01 00 02 0A 00 00 04          接口1 数据：类码 0x0A，2 端点（批量）
07 05 01 02 40 00 00                端点 0x01 批量 OUT，64B
07 05 81 02 40 00 00                端点 0x81 批量 IN，64B
-- DFU Runtime 功能 ----------------
09 04 02 00 00 FE 01 01 05          接口2：Class=0xFE SubClass=0x01 Protocol=0x01（运行态）
09 21 0F 03E8 0400 10 01            DFU 功能: bmAttr=0x0F, wDetachTimeOut=1000ms,
                                    wTransferSize=1024, bcdDFU=0x0110
```

（字节为小端；`0x5D`=`93`。对照 [usb_descriptors.c](../firmware/src/usb_descriptors.c) 逐字节一致。）

## 3. 打开串口与数据流（应用层 open() 触发的类请求 + 批量传输）

主机执行 `python serial_tool.py --auto` 打开 COM 口时，**基于规范重构**的类请求序列：

| # | 事务 | bmRequestType | bRequest | wValue | wIndex | wLength | 数据 | 说明 |
|---|---|---|---|---|---|---|---|---|
| 13 | SET_CONTROL_LINE_STATE | 0x21 | 0x22 | 0x0003 (DTR\|RTS) | 0 | 0 | 无 | **DTR 置位 = 终端打开**（固件 tud_cdc_line_state_cb 感知） |
| 14 | SET_LINE_CODING | 0x21 | 0x20 | 0 | 0 | 7 | `00 C2 01 00 00 00 08` = 115200,8N1 | "抽象"参数：设备记住、USB 传输不受影响 |
| 15 | GET_LINE_CODING | 0xA1 | **0x21** | 0 | 0 | 7 | 同上 7 字节 | **必须可回读一致**，否则部分驱动拒绝设备 |
| 16 | （可选）SERIAL_STATE 通知 | 0xA1 类通知，经端点 0x82 | 0x20 | 0 | 0 | 2 | `01 00`（DCD 置位） | 设备→主机"线路在位"，状态变化才发 |
| 17 | 批量 OUT | 0x01 → 设备 | — | — | — | — | `ping\n`（5B） | 数据事务：IN 侧 ACK/NAK 握手天然流控 |
| 18 | 批量 IN | 设备 → 0x81 | — | — | — | — | `pong\n`（5B） | 短包即结束；若恰为 64B 整数倍需 ZLP 收尾 |

关于 #18 的 ZLP：当一次 write() 长度恰为 wMaxPacketSize 整数倍时，驱动/固件要发零长度包标记结束，否则主机一直等到短包才认为请求完成——CDC 粘包/延迟的经典成因（知识库 [虚拟串口ACM](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/CDC-通信设备类/01-虚拟串口ACM.md) 第 5 节）。

### 3.1 DFU 触发帧（升级的"第一帧"）

`dfu-util -e` 发出：

```text
bmRequestType=0x21  bRequest=0x00 (DFU_DETACH)  wValue=1000(超时ms)  wIndex=2(DFU接口)  wLength=0
```

设备侧对应 `tud_dfu_runtime_reboot_to_dfu_cb()` 写魔法数并复位（src/main.c）；随后总线重新枚举出 DFU 模式接口（Protocol=0x02）。完整 DFU 下载状态机帧序列见知识库 [01-DFU固件升级](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/其他设备类/01-DFU固件升级.md) 第 8 节的时序图。

## 4. 真实抓包方法（动手把上表复现成 pcap）

### 4.1 Windows：USBPcap + Wireshark

```powershell
# ① 安装 USBPcap（https://desowin.org/usbpcap/），装完重启
# ② 列出可监听的 USB 控制器/Hub（记下设备所在 hub 编号）
USBPcapCMD
# ③ 直接从 Wireshark 抓包界面选择 \\.\USBPcap1（编号按 ②）
#    或命令行录制：
USBPcapCMD -d \\.\USBPcap1 -o lab2_cdc.pcap -A 1024
# ④ 插入设备 → 打开串口 → ping/ver → dfu-util -e …… → Ctrl+C 结束录制
```

Wireshark 显示过滤（验证上表）：

```
usb.device_address == 12              ← 只看本设备（地址以抓包为准，lsusb/设备管理器可查）
usb.bmRequestType == 0x21             ← 所有主机→设备的类请求（SET_LINE_CODING 等）
usb.setup.bRequest == 0x20            ← SET_LINE_CODING
usb.setup.bRequest == 0x22            ← SET_CONTROL_LINE_STATE（找 DTR 边沿）
usb.transfer_type == 0x01             ← 批量传输数据
```

批量数据在 `usb.capdata` 字段；较新版本 Wireshark 能自动按 CDC 解析类请求的参数结构。

### 4.2 Linux：usbmon

```bash
sudo modprobe usbmon                       # 加载抓包模块（多数发行版已内置）
lsusb                                      # 找到 Bus 01 Dev 12
sudo wireshark                             # 抓包接口列表里选 usbmon1（Bus 1）
# 或命令行：
sudo tshark -i usbmon1 -Y "usb.device_address==12" -w lab2_cdc.pcap
# 模块文档：https://docs.kernel.org/usb/usbmon.html
```

### 4.3 Wireshark 官方样例与文档

- USB 抓包总览（各平台方法）：https://wiki.wireshark.org/CaptureSetup/USB
- USBPcap 专页：https://wiki.wireshark.org/USBPcap
- Wireshark 样例抓包库里有现成的 USB pcap（含 CDC/HID 样例）：https://wiki.wireshark.org/SampleCaptures

### 4.4 对照作业

抓到 pcap 后逐条对照本文第 1~3 节：枚举帧的地址分配、IAD 解析、DTR 时机、LINE_CODING 数值、ZLP 是否出现——差异点（比如 Windows 与 Linux 的 GET_DESCRIPTOR 次数不同）就是各主机栈实现差异的鲜活教材。欢迎把脱敏 pcap 提交到仓库 `captures/` 目录。

## 参考资源

- 知识库：[虚拟串口 ACM](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/CDC-通信设备类/01-虚拟串口ACM.md) · [03-CDC规范级-请求与通知全表](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/CDC-通信设备类/03-CDC规范级-请求与通知全表.md) · [DFU 固件升级](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/其他设备类/01-DFU固件升级.md)
- [USB-IF CDC/PSTN 1.2 规范（usb.org 文档库检索 "CDC"）](https://www.usb.org/documents)
- [Wireshark USB 抓包指南](https://wiki.wireshark.org/CaptureSetup/USB) · [USBPcap](https://wiki.wireshark.org/USBPcap) · [USBPcap 官网](https://desowin.org/usbpcap/)
- [Linux 内核 usbmon 文档](https://docs.kernel.org/usb/usbmon.html)
