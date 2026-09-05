# Lab1：HID 复合设备（键盘 + 鼠标 + 消费控制）—— 全栈实现

> 面向量产的全栈实战：从 BOM 与 PCB 设计要点、TinyUSB 固件（RP2040 基准平台）、主机侧 hidapi 解析，
> 到抓包分析与商业化经验（回报率营销、NKRO、休眠功耗、三端兼容坑、寿命测试、量产烧录）。

- 基准平台：Raspberry Pi RP2040（USB 1.1 Full-Speed Device）
- 固件栈：pico-sdk + TinyUSB（版本见 `firmware/README.md`，API 细节以官方 SDK 为准）
- 设备形态：一个 USB 设备，三个独立 HID 接口（Interface 0 键盘 / Interface 1 鼠标 / Interface 2 消费控制）

## 1. 商业场景

HID 复合设备不是玩具教程，它对应一批真实量产品类：

| 量产品类 | 与本 Lab 的关系 | 商业要点 |
|---|---|---|
| 2.4G 无线键鼠接收器 OEM | 接收器插到 PC 上就是"键盘 + 鼠标"复合 HID（2.4G 私有协议在 MCU 与无线模块之间传递，对主机完全透明） | BOM 极致成本、免驱动即插即用、Windows 认证（WHQL 可选）、待机功耗 |
| 游戏外设（手柄/方向盘宏键盘） | 高回报率（1000Hz）轮询、NKRO | 回报率是核心卖点（见 `experience.md` 第 1 节），营销话术与实测要诚实 |
| 多媒体遥控器/会议音箱扩展键 | 消费控制接口（Consumer Control，音量/播放/静音） | macOS / Android 对 Usage Page 的支持差异 |
| 工业测试治具、KVM | 键鼠注入 + 主机侧读取 | 可靠性、连接器插拔寿命、量产烧录 |

本 Lab 完成后，`lab6-ble-hid-dongle`（无线接收器）只需把"USB 总线上的报告来源"从按键换成 2.4G/BLE 射频数据，
报告格式、描述符、主机侧解析、抓包方法全部复用。

## 2. 主控选型对比（量产视角）

| 主控 | 内核/主频 | USB 能力 | 常见单价区间(USD，随行情) | 生态/工具链 | 量产关注点 |
|---|---|---|---|---|---|
| **RP2040**（本 Lab 基准） | 双核 Cortex-M0+ @133MHz | USB 1.1 FS Device | 约 0.3~0.7 | pico-sdk + TinyUSB 官方支持；SWD/USB 拖拽烧录；开源调试器生态好 | 必须外挂 QSPI Flash（+0.15~0.35）；QFN-56 需回流焊；Flash 唯一 ID 可做序列号 |
| STM32F103C8T6 | Cortex-M3 @72MHz | USB 1.1 FS Device | 约 0.5~1.5（供货波动大） | HAL/标准库，TinyUSB 有移植 | 市场兼容型号多，量产必须做固件回归测试与来料一致性验证 |
| CH552T | 增强 8051 @24MHz | USB 1.1 FS Device | 约 0.2~0.4 | WCH 官方例程；社区 Arduino/移植框架 | 极致 BOM；片内 Flash 可 OTP；TinyUSB 主线不含 CH552 端口，需用 WCH 例程或社区移植（以官方仓库为准） |
| CH32V203 | RISC-V @144MHz | USB FS Device + 主机 | 约 0.3~0.8 | MounRiver Studio / GCC；TinyUSB 主线有 ch32 端口（以官方仓库为准） | 与 CH55x 同厂供应链；低功耗模式较丰富 |
| ESP32-S3 | 双核 Xtensa LX7 @240MHz | USB OTG FS（仅 FS） | 约 1.5~3.0 | ESP-IDF 内置 TinyUSB；WiFi/BLE 一体 | 单芯片无线键鼠直连方案；但成本与待机功耗（深度睡眠约 7~10μA 量级，以官方 datasheet 为准）高于专用接收器 MCU |

> 价格为公开渠道常见区间，随行情、汇率、量价与原厂策略浮动，**下单前以分销商实时报价为准**。
> 选型结论：纯 USB 键鼠类产品，RP2040（生态/文档最好）与 CH55x/CH32V（成本最优）是两条主流路线；
> 需要无线一体时看 ESP32-S3 / CH32V208 / Nordic nRF52（见 lab6）。

## 3. 全栈层级矩阵（本目录导览）

| 层级 | 文件 | 内容 | 对应知识库（USBTree） |
|---|---|---|---|
| 硬件 | `hardware/BOM.csv` | 1k 量级 BOM 与单价区间 | [30-枝干-接口与供电](https://github.com/tangjianfang/USBTree/blob/main/30-枝干-接口与供电/00-索引.md) |
| 硬件 | `hardware/设计要点.md` | 差分对走线、ESD、晶振、RP2040 最小系统、结构与连接器 | [USB 核心物理层](https://github.com/tangjianfang/USBTree/blob/main/10-树干-USB核心/03-物理层与电气特性.md) |
| 固件 | `firmware/src/usb_descriptors.c/.h` | 设备/配置/HID/报告/字符串描述符，逐字段注释 | [HID 描述符](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/HID-人机接口设备/01-HID描述符.md)、[USB 描述符详解](https://github.com/tangjianfang/USBTree/blob/main/10-树干-USB核心/07-描述符详解.md) |
| 固件 | `firmware/src/tusb_config.h` | TinyUSB 配置（接口数、EP 缓冲） | [TinyUSB 实战](https://github.com/tangjianfang/USBTree/blob/main/60-枝干-主机侧与实现/05-实战-TinyUSB设备固件.md) |
| 固件 | `firmware/src/main.c` | 1kHz 主循环、tud_task、按键演示逻辑 | [四种传输类型](https://github.com/tangjianfang/USBTree/blob/main/10-树干-USB核心/06-四种传输类型.md) |
| 固件 | `firmware/src/hid_app.c` | GET/SET_REPORT 回调、boot 协议、键盘 LED | [HID 传输与类特定请求](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/HID-人机接口设备/04-传输与类特定请求.md) |
| 主机 | `host/hid_monitor.py` | hidapi 打开设备、按 Report ID 解析三种报告 | [鼠标详解](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/HID-人机接口设备/06-鼠标详解.md)、[键盘详解](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/HID-人机接口设备/05-键盘详解.md) |
| 分析 | `capture/HID帧序列分析.md` | 枚举逐包解读、中断 IN 轮询、usbmon/USBPcap 实操 | [枚举流程与标准请求](https://github.com/tangjianfang/USBTree/blob/main/10-树干-USB核心/08-枚举流程与标准请求.md)、[协议分析仪与抓包](https://github.com/tangjianfang/USBTree/blob/main/70-枝干-调试测试与安全/01-协议分析仪与抓包.md) |
| 商业 | `experience.md` | 回报率、NKRO、功耗、三端兼容、寿命、烧录、售后 | [HID 概述与定位](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/HID-人机接口设备/00-HID概述与定位.md) |

## 4. 快速开始

1. **硬件**：手头没有自制板时，代码可直接跑在 Raspberry Pi Pico 上（演示按键用 GP13/14/15 对地短接，
   板载内部上拉；引脚定义见 `firmware/src/main.c` 顶部，可按你的原理图修改）。
2. **固件**：按 `firmware/README.md` 构建，得到 `lab1_hid_composite.uf2`；
   按住 Pico 的 BOOTSEL 插 USB，把 uf2 拖入出现的 U 盘即完成烧录。
3. **验证**：设备应枚举为 "USB-Labs HID Composite"，出现键盘 + 鼠标 + 消费控制三个 HID 顶层集合。
4. **主机侧观察**：
   - `pip install hidapi` 后运行 `python host/hid_monitor.py --list`，再 `--vid 0x1234 --pid 0x0002`（见固件 `usb_descriptors.h`，量产必须换自己的 VID/PID）；
   - 接一个演示按键，观察监视器输出与屏幕上真实的键入/鼠标移动/音量变化。
5. **抓包**：按 `capture/HID帧序列分析.md` 第 3 节，用 usbmon（Linux）或 USBPcap（Windows）抓枚举与轮询，
   Wireshark 过滤 `usb.transfer_type == 0x01`（中断传输）。

## 5. 本 Lab 验收清单（对标量产）

- [ ] 插拔 100 次，Windows/macOS/Linux 均能免驱动枚举（描述符合法）
- [ ] BIOS/UEFI 界面中键盘可用（boot 协议回退正常）
- [ ] 主机休眠唤醒后设备恢复工作（挂起/恢复，见 `experience.md` 第 3 节）
- [ ] Wireshark 抓包确认三个接口的中断 IN 轮询周期符合描述符 bInterval
- [ ] `hid_monitor.py` 能按 Report ID 正确解析三类报告（主机侧与固件描述符一致）

## 参考资源

- USB 2.0 规范（枚举/传输/电气）：https://www.usb.org/document-library/usb-20-specification
- HID 1.11 类规范：https://www.usb.org/document-library/device-class-definition-hid-111
- TinyUSB 仓库与本 Lab 参考例程 hid_composite：https://github.com/hathach/tinyusb
- TinyUSB hid_composite 例程：https://github.com/hathach/tinyusb/tree/master/examples/device/hid_composite
- pico-sdk：https://github.com/raspberrypi/pico-sdk
- RP2040 Datasheet：https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf
- RP2040 硬件设计指南：https://datasheets.raspberrypi.com/rp2040/hardware-design-with-rp2040.pdf
- USB in a NutShell（Beyond Logic）：https://www.beyondlogic.org/usbnutshell/usb1.shtml
- Wireshark USB 抓包指南：https://wiki.wireshark.org/CaptureSetup/USB
- 知识库总图：https://github.com/tangjianfang/USBTree/blob/main/00-知识树总图.md
