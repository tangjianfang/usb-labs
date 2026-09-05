# Lab1：HID 帧序列分析（枚举 + 中断 IN 轮询 + 真实抓包指引）

> **诚实声明**：本文所有帧序列均为**"基于 USB 2.0 规范（usb.org，§8/§9）与 HID 1.11 规范重构"的教学样本**，
> 字段值与本项目固件源码（`firmware/src/usb_descriptors.c`）保持一致；它不是某台分析仪导出的真实逐字节记录。
> 真实总线时序、主机请求顺序因主机控制器而异（例如是否先只取 8 字节设备描述符），**以你自己的抓包为准**——
> 第 3 节给出完整的 usbmon / USBPcap 实操步骤。
>
> 基础概念：[枚举流程与标准请求](https://github.com/tangjianfang/USBTree/blob/main/10-树干-USB核心/08-枚举流程与标准请求.md)、
> [包格式与事务](https://github.com/tangjianfang/USBTree/blob/main/10-树干-USB核心/05-包格式与事务.md)、
> [HID 传输与类特定请求](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/HID-人机接口设备/04-传输与类特定请求.md)

## 0. 预备知识：一次事务 = 三个包

USB 事务（Transaction）由令牌包、数据包、握手包组成（PID 值为规范定义）：

| 包类型 | PID | 说明 |
|---|---|---|
| 令牌 | SETUP=0xB4, IN=0x69, OUT=0xE1 | 指明事务类型与目标（地址+端点） |
| 数据 | DATA0=0xC3, DATA1=0x4B | 数据切换（toggle）实现重传去重 |
| 握手 | ACK=0x4A, NAK=0x5A, STALL=0x5C | NAK="暂时没有数据"，STALL="请求非法/端点挂起" |

控制传输的三个阶段：SETUP（含 8 字节请求）→ 数据阶段（可 IN 可 OUT）→ 状态阶段（反向 ZLP）。

## 1. 枚举序列（基于 USB 2.0 规范重构）

设备插入后的典型顺序。SETUP 包携带的 8 字节请求格式：
`bmRequestType | bRequest | wValue(L, H) | wIndex(L, H) | wLength(L, H)`（均为小端）。

| # | 请求（8 字节 SETUP 数据，hex） | 解读 |
|---|---|---|
| 0 | ——（硬件层） | 插入 → VBUS 5V 供电；设备在 D+ 上拉 1.5kΩ → 主机检测到 Full-Speed 连接；主机复位总线（SE0 持续 ≥10ms），设备地址回到 0 |
| 1 | `80 06 00 01 00 00 08 00` | GET_DESCRIPTOR(Device)，先只要 8 字节——探测 EP0 的 bMaxPacketSize0（多数主机如此，规范亦允许直接取 18 字节） |
| 2 | `00 05 05 00 00 00 00 00` | SET_ADDRESS(5)：给设备分配地址 5。设备在状态阶段以 ZLP 应答后启用新地址 |
| 3 | `80 06 00 01 00 00 12 00` | （地址 5）GET_DESCRIPTOR(Device)，取完整 18 字节 |
| 4 | `80 06 00 02 00 00 09 00` | GET_DESCRIPTOR(Config)，先取 9 字节拿到 wTotalLength |
| 5 | `80 06 00 02 00 00 54 00` | GET_DESCRIPTOR(Config)，取全部 84(0x54) 字节（含 3 个接口 + 3×(HID+端点) 描述符） |
| 6 | `80 06 00 03 00 00 FF 00` | GET_DESCRIPTOR(String 0)：LANGID 列表；随后主机按需取厂商(1)/产品(2)/序列号(3)及接口名(4/5/6) |
| 7 | `81 06 00 22 00 00 FF 00` | **HID 类请求** GET_DESCRIPTOR(Report)：wIndex=0 → 读键盘接口报告描述符；随后 wIndex=1、wIndex=2 各一次 |
| 8 | `21 0A 00 00 00 00 00 00` | SET_IDLE(0,0)：键盘接口，静默期=0（按键变化立即上报）。可选请求，多数 Windows 会发 |
| 9 | `00 09 01 00 00 00 00 00` | SET_CONFIGURATION(1)：激活配置，中断 IN 端点开始被调度 |

### 1.1 关键报文字段解码（与固件源码对照）

**设备描述符（18 字节）**，响应请求 3：

```
12 01 00 02 00 00 00 40 34 12 02 00 00 01 01 02 03 01
│  │  │───┘ │  │  │  │  │───┘ │───┘ │───┘ │  │  │  └─ bNumConfigurations=1
│  │  │     │  │  │  │  └VID=0x1234┘ └PID=0x0002┘ │  │  └ iSerialNumber=3
│  │  │     │  │  │  └ bMaxPacketSize0=64         │  └ iProduct=2
│  │  │     │  │  └ bDeviceProtocol=0             └ iManufacturer=1
│  │  │     │  └ bDeviceSubClass=0                   （以上均为演示占位 VID/PID）
│  │  │     └ bDeviceClass=0（类在接口级声明）
│  │  └ bcdUSB=0x0200（USB 2.0）
│  └ bDescriptorType=0x01（设备）
└ bLength=18
```

**配置描述符头（9 字节）**，响应请求 4/5 的前 9 字节：

```
09 02 54 00 03 01 00 80 32
│  │  │───┘ │  │  │  │  └─ bMaxPower=0x32 → 50×2mA = 100mA（与 BOM/功耗实测一致性见硬件要点）
│  │  │     │  │  │  └─ bmAttributes=0x80：bit7 保留=1，bit6=0 总线供电
│  │  │     │  │  └─ iConfiguration=0
│  │  │     │  └─ bConfigurationValue=1
│  │  │     └─ bNumInterfaces=3 ← 键盘/鼠标/消费控制三个接口
│  │  └─ wTotalLength=0x0054=84 字节 = 9 + 3×(9+9+7) ← 总长算错是"代码10"常见根因
│  └─ bDescriptorType=0x02（配置）
└─ bLength=9
```

**HID 描述符（9 字节，每个接口各一个）**，含在 84 字节内：

```
09 21 11 01 00 01 22 3F 00
│  │  │───┘ │  │  │  │───┘ └ wDescriptorLength=0x003F=63（键盘报告描述符长度，
│  │  │     │  │  └ bDescriptorType=0x22（报告）  鼠标/消费控制各有自己的长度，
│  │  │     └ bNumDescriptors=1                  以编译期 sizeof 与真实抓包为准）
│  └ bDescriptorType=0x21（HID）
└ bLength=9（bcdHID=0x0111 → HID 1.11；bCountryCode=0 不指定布局）
```

**键盘接口描述符（9 字节）**：`09 04 00 00 01 03 01 01 04` →
接口号 0、1 个端点、类 0x03(HID)、子类 0x01(boot 协议)、协议 0x01(键盘)、iInterface=4（"HID Keyboard"）。
鼠标接口为 `09 04 01 00 01 03 01 02 05`（协议 0x02=鼠标）；消费控制为 `09 04 02 00 01 03 00 00 06`（无 boot 语义，协议 0x00）。

**端点描述符（7 字节）**：`07 05 81 03 10 00 01` →
地址 0x81（IN 端点 1）、属性 0x03（中断）、wMaxPacketSize=0x0010=16、bInterval=1（FS：1 帧=1ms → **1000Hz 轮询**）。

**报告描述符（HID Item 流）**，以键盘接口为例（重构，完整 Item 语法见知识库
[02-报告描述符与Item编码](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/HID-人机接口设备/02-报告描述符与Item编码.md)）：

```
85 01                       Report ID = 1
05 01 09 06 A1 01           UsagePage(GenericDesktop) Usage(Keyboard) Collection(Application)
05 07 19 E0 29 E7           UsagePage(Keyboard) UsageMin(E0) UsageMax(E7)  ← 左Ctrl..右GUI
15 00 25 01 75 01 95 08     Logical 0..1, Size 1, Count 8
81 02                       Input(Data,Var,Abs)            ← modifier 位图 1 字节
95 01 75 08 81 01           Count 1 Size 8 Input(Const)    ← 保留字节
95 06 75 08 15 00 25 65     Count 6 Size 8 Logical 0..0x65
19 00 29 65 81 00           UsageMin/Max 0..0x65 Input(Data,Array) ← 6 个键码槽
05 08 19 01 29 05           UsagePage(LEDs) UsageMin 1 UsageMax 5
75 01 95 05 91 02           Size 1 Count 5 Output(Data,Var,Abs)    ← NUM/CAPS/SCROLL/… 灯
95 01 75 03 91 01           Count 1 Size 3 Output(Const)           ← 补齐到整字节
C0                          EndCollection
```

> 排障经验（公开资料共识）：设备能枚举但"按键全无反应/媒体键失灵"，十有八九是报告描述符
> Item 写错（Usage Page、Logical Max、报告位宽与固件实际发送不一致），而不是"缺驱动"。见 `../experience.md` 第 7 节。

## 2. 中断 IN 报告轮询（示意）

配置完成后，主机调度器每 1ms 帧（Full-Speed）向已配置的中断 IN 端点发 IN 令牌。本设备三个端点都被轮询：

```
1ms 帧（Full-Speed 帧时钟，SOF 同步）
├─ IN(0x81) → DATA1: 01 00 00 0B 00 00 00 00 00   ← 键盘：ReportID=1, 按下 'h'(0x0B)
├─ IN(0x82) → NAK                                 ← 鼠标：无新报告，NAK 不占数据阶段
├─ IN(0x83) → DATA1: 03 E9 00                     ← 消费控制：ReportID=3, Usage=0x00E9 音量+
│
├─ 下一帧
├─ IN(0x81) → NAK                                 ← 键盘无变化（SET_IDLE(0) 也不重复上报）
├─ IN(0x82) → DATA0: 02 01 03 00 00               ← 鼠标：Btn1 按下, dx=+3, dy=0, wheel=0
└─ IN(0x83) → NAK
```

- **数据切换（DATA0/DATA1）**在每次成功传输后翻转，主机据此识别重传。
- **NAK 是正常流量**：真实抓包中 1000Hz 轮询的绝大多数事务是 NAK——这正说明轮询速率由主机按 bInterval 保证，
  设备只在有新数据时才真正占用总线。
- **关于"微帧"**：125μs 微帧是 High-Speed 的概念（每 1ms 帧 8 个微帧）。本设备是 Full-Speed，按 1ms 帧轮询。
  若同一描述符跑在 HS 总线上，中断端点 bInterval 的单位变为"微帧的 2^(bInterval-1) 倍"，语义不同，
  见知识库 [13-总线带宽与调度计算](https://github.com/tangjianfang/USBTree/blob/main/10-树干-USB核心/13-总线带宽与调度计算.md)。

## 3. 真实抓包指引（以你自己的总线为准）

### 3.1 Linux（usbmon）

```bash
# 1) 找到设备所在总线号
lsusb                                   # 例如 Bus 001 Device 010: ID 1234:0002

# 2) 加载 usbmon（多数发行版已内置）
sudo modprobe usbmon
ls /sys/kernel/debug/usb/usbmon         # 出现 1u 1t ... 对应各总线

# 3) 用 Wireshark/tshark 抓该总线（ usbmon<总线号> ）
sudo tshark -i usbmon1 -w lab1_hid.pcap
# 或 tcpdump: sudo tcpdump -i usbmon1 -w lab1_hid.pcap

# 4) 插拔设备、按演示按键，然后停止抓包，Wireshark 打开分析
```

官方文档：https://docs.kernel.org/usb/usbmon.html

### 3.2 Windows（USBPcap）

1. 安装 USBPcap（https://desowin.org/usbpcap/ ），安装后重启 Wireshark；
2. 在 Wireshark 接口列表选择 `USBPcap1`，弹出窗口中勾选根集线器与目标设备，开始捕获；
3. 插拔设备触发枚举，按演示键产生中断流量。

### 3.3 Wireshark 过滤器速查

```
usb.transfer_type == 0x01        # 中断传输（0x00 同步/0x01 中断/0x02 控制/0x03 批量）
usb.transfer_type == 0x02        # 控制传输（看枚举）
usb.device_address == 5          # 只看本设备（地址以你的抓包为准）
usb.src == "1.5.1"               # Linux 语法：总线.设备.端点
usb.capdata                      # 原始数据载荷（含 HID 报告）
usbhid                           # HID 协议解析层（Windows/USBPcap 下对报告自动解码）
```

样例捕获文件：Wireshark 官方 Samples 页面收录了 USB 捕获样本（含 HID 流量），
https://wiki.wireshark.org/Samples ；抓包环境搭建详见 https://wiki.wireshark.org/CaptureSetup/USB 。

### 3.4 把真实抓包对回本文

1. 枚举阶段逐条核对 §1 表格：描述符字节应与固件 `usb_descriptors.c` 完全一致（这是最值得逐字节核对的部分）；
2. 中断阶段核对 §2：轮询周期应等于描述符 bInterval=1ms，可用 Wireshark 的 IO Graph 验证"每秒 1000 次轮询"；
3. 顺序差异（如 macOS 不发 SET_IDLE、Linux 枚举顺序不同）属正常主机行为差异。

更多排障路径见知识库 [01-协议分析仪与抓包](https://github.com/tangjianfang/USBTree/blob/main/70-枝干-调试测试与安全/01-协议分析仪与抓包.md) 与
[02-枚举失败排查手册](https://github.com/tangjianfang/USBTree/blob/main/70-枝干-调试测试与安全/02-枚举失败排查手册.md)。

## 参考资源

- USB 2.0 规范（§8 事务/§9 枚举）：https://www.usb.org/document-library/usb-20-specification
- HID 1.11 类规范（§5 请求/§6 描述符/§7 报告）：https://www.usb.org/document-library/device-class-definition-hid-111
- HID Usage Tables（键码/鼠标/消费控制 Usage）：https://usb.org/hid
- Linux usbmon 文档：https://docs.kernel.org/usb/usbmon.html
- USBPcap（Windows 抓包驱动）：https://desowin.org/usbpcap/
- Wireshark USB 抓包指南：https://wiki.wireshark.org/CaptureSetup/USB
- Wireshark 样例捕获：https://wiki.wireshark.org/Samples
- USB in a NutShell（Beyond Logic，控制传输章节）：https://www.beyondlogic.org/usbnutshell/usb4.shtml
- TinyUSB（本固件栈）：https://github.com/hathach/tinyusb
