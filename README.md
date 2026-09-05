# USB-Labs · USB 全栈实战实验室

> 🧪 配套知识库：[USBTree](https://github.com/tangjianfang/USBTree)（为什么/是什么）→ 本仓库（怎么做/怎么卖）。
> 从**硬件电子电路设计**到**固件、主机驱动、应用层**，再到**测试认证与量产**的商业级全栈实战。
>
> English TL;DR — Full-stack, production-oriented USB labs: hardware design notes, firmware,
> host drivers, applications, packet-capture analysis and go-to-market experience, organized
> as 7 labs covering HID / CDC+DFU / MSC / UVC+UAC / USB PD / BLE-HID dual-mode / docks.

## 实验室矩阵

| 实验室 | 协议族 | 商业场景 | 硬件 | 固件 | 主机/应用 | 抓包 | 深度 |
|---|---|---|---|---|---|---|---|
| [Lab1 HID 复合设备](labs/lab1-hid-composite/README.md) | HID | 无线键鼠接收器/手柄 OEM | ✅ BOM+设计要点 | ✅ TinyUSB 复合设备 | ✅ hidapi 监视器 | ✅ | 完整 |
| [Lab2 CDC+DFU](labs/lab2-cdc-dfu/README.md) | CDC/DFU | 工业网关调试口/固件升级 | ✅ | ✅ CDC+分区方案 | ✅ pyserial/dfu-util | ✅ | 完整 |
| [Lab3 MSC](labs/lab3-msc/README.md) | MSC/BOT/SCSI | U 盘/记录仪导出 | ✅ | ✅ TinyUSB+SD | ✅ f3 鉴伪 | ✅ | 完整 |
| [Lab4 UVC+UAC](labs/lab4-uvc-uac/README.md) | UVC/UAC | 会议摄像头/麦克风 | ✅ 双路线 | 教学级（SoC 为商用主流） | ✅ v4l2/ffmpeg | ✅ | 中级 |
| [Lab5 PD 电源](labs/lab5-pd-charger/README.md) | Type-C/PD | 充电器/受电设备 | ✅ 双路线 BOM | ✅ FUSB302 骨架/硬件方案 | ✅ 仪器验证 | ✅ | 完整 |
| [Lab6 BLE 双模键鼠](labs/lab6-ble-hid-dongle/README.md) | BLE HOGP+HID | 双模键鼠+dongle | ✅ RF/天线 | ✅ NCS 集成点 | ✅ 抓包调试 | ✅ | 完整 |
| [Lab7 扩展坞](labs/lab7-dock-altmode/README.md) | Alt Mode/USB4 | Type-C/USB4 扩展坞 | 概要 | —（门槛说明） | 概要 | 拓扑 | 概要 |

## 快速开始

```bash
git clone https://github.com/tangjianfang/usb-labs.git
cd usb-labs/labs/lab1-hid-composite
cat README.md            # 从选型到量产的全栈路线
```

## 🔬 协议仿真器

[usbsim](simulator/README.md) —— 在纯软件中模拟 USB/BLE/PD 的**协议通信链路**：虚拟主机↔虚拟设备、包真编解码（CRC 实算）、枚举/BOT/PD 协商全状态机、错误注入、抓包流导出。运行 `python simulator/tests/test_sim.py` 全绿。

## 全栈方法论

[docs/00-全栈方法论.md](docs/00-全栈方法论.md) 定义了贯穿所有实验室的六阶段节奏：
**选型(G0) → 原理图(G1) → EVT → DVT → PVT → MP**，每阶段有退出标准；[03-商业实战总纲](docs/03-商业实战总纲.md) 给出认证矩阵、BOM 策略与供应链经验。

## 配套知识库

概念性"为什么"全部沉淀在知识库 [USBTree](https://github.com/tangjianfang/USBTree)：

| 本仓库讲 | USBTree 讲 |
|---|---|
| 怎么做 HID 键盘固件 | [HID 协议为什么这样设计](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/HID-人机接口设备/00-HID概述与定位.md) |
| 怎么调 PD 电源 | [PD 协商怎么走](https://github.com/tangjianfang/USBTree/blob/main/30-枝干-接口与供电/04-USBPD协议.md) |
| 怎么抓包分析 | [包/事务/传输的协议原理](https://github.com/tangjianfang/USBTree/blob/main/10-树干-USB核心/05-包格式与事务.md) |

## 诚实声明（重要）

1. **抓包数据**：`captures/` 中的帧序列为**基于官方规范重构的教学样本**（文件头均有声明），不是真实采集数据；每份文件同时给出真实抓包的工具与方法（usbmon/USBPcap/硬件分析仪），并链接公开真实样例（如 Wireshark wiki 样例库）。
2. **社区经验**：以"公开资料共识"形式概括，附真实参考链接；不编造论坛引言。
3. **价格**：BOM 单价为量级区间（标注"随行情浮动"），采购前以实际报价为准。
4. **代码**：面向"可编译级完整"，标注 SDK 版本；不能保证在你手上直接零修改跑通——那正是实验室的意义。

## 许可

MIT（见 [LICENSE](LICENSE)）。知识库 USBTree 同为 MIT。
