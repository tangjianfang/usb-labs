# Lab2：CDC 虚拟串口 + DFU 固件升级 —— 工业设备的标配组合

> 📦 USB-Labs 全栈实战系列 · Lab2
> 概念知识库：[CDC 通信设备类](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/CDC-通信设备类/) · [DFU 固件升级](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/其他设备类/01-DFU固件升级.md)
> 难度：★★★☆☆ ｜ 预计投入：2~3 周 ｜ 前置：Lab1（HID 设备全栈）

## 1. 为什么是 CDC + DFU

打开任何一台工业网关、示波器、电力集中器的说明书，你几乎总会看到两样东西：一个"调试/配置串口"和一个"固件升级通道"。它们在 USB 世界里的标准答案是：

- **CDC-ACM（Communications Device Class - Abstract Control Model）**：把 USB 批量端点伪装成一条 RS-232 虚拟串口（Virtual COM Port / VCP）。主机枚举后出现 `COMx`（Windows）或 `/dev/ttyACM0`（Linux），上位机、SCADA、终端程序零改造接入。详见知识库 [虚拟串口 ACM](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/CDC-通信设备类/01-虚拟串口ACM.md)。
- **DFU（Device Firmware Upgrade 1.1）**：只用 USB 控制传输就能把固件镜像写进 Flash 的 USB-IF 标准类协议，配合 dfu-util 实现"插线即可刷机"，是 MCU 领域事实上的引导协议标准。详见知识库 [DFU 固件升级](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/其他设备类/01-DFU固件升级.md)。

### 1.1 三个典型商业场景

| 场景 | CDC 的角色 | DFU 的角色 |
|---|---|---|
| **工业网关调试口** | 现场工程师用串口终端/私有 AT 命令做诊断，替代昂贵的 RS232 维护口 | 24V 柜内设备不拆壳升级，故障时回滚 |
| **传感器集中器** | 把 Modbus/LoRa 采集数据以日志流推给上位机，环形缓冲防丢包 | 现场批量部署后远程/本地升级协议栈 |
| **仪器控制** | 仪器面板 USB 口暴露控制串口（高阶用 USBTMC，见知识库 [USBTMC](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/其他设备类/08-USBTMC测试测量类.md)） | 出厂校准数据与固件分离升级，保修期内安全刷机 |

这三类产品有几个共同约束，也正是本 Lab 的设计主线：**7×24 长跑不能挂**（热插拔、断流恢复）、**现场无 SWD 调试器**（升级必须走 USB 自身）、**客户现场 Windows 版本五花八门**（免驱/驱动兼容）、**出了问题要有证据**（抓包+日志+返回件分析）。

## 2. 主控选型

四款主流平台都能做 CDC+DFU，但生态差异很大——这是选型时最容易踩的坑。价格均为单芯片散新/量产区间，**随行情波动，下单前以立创商城等渠道实时报价为准**（下表为 2026 年 9 月查询快照）。

| 平台 | 内核/资源 | USB 能力 | CDC 生态 | DFU 生态 | 价格区间（单芯片） |
|---|---|---|---|---|---|
| **STM32F103C8T6**（本 Lab 参考平台） | Cortex-M3 72MHz / 64KB Flash / 20KB RAM | USB FS 设备（内置 PHY，PA11/PA12，**D+ 需外部 1.5kΩ 上拉**） | TinyUSB / ST 官方 USB 库 / CubeMX 一键生成，Windows 10+ 免驱 | ROM 里自带 USB DFU bootloader（DfuSe 协议，VID 0x0483/PID 0xDF11，BOOT0 引脚进入），业界最成熟的"保底升级通道" | 原装约 ¥8~25；国产兼容（如 GD32F103）约 ¥5~8；散新假货风险见 experience.md |
| **CH32V203C8T6**（WCH） | RISC-V (青稞V4B) 144MHz / 64KB Flash / 20KB RAM | USBFS + USBFS-PHY，**内置上拉可控** | WCH 官方 USBFS 外设库带 CDC 例程；TinyUSB 有社区移植 | 无 ROM USB DFU；需自写/TinyUSB dfu bootloader，或经 SWD/串口（ISP）升级 | 约 ¥3~6，四款中最低 |
| **ESP32-S3** | Xtensa LX7 双核 240MHz / 最高 512KB SRAM+外置 Flash | OTG (USB OTG High-Speed 480Mbps，需外部 PHY 或内置 FS PHY 视型号) | ESP-IDF 内置 TinyUSB，CDC/JTAG-Serial 一等公民 | **无 USB DFU**：官方路线是 UART esptool（ROM bootloader）或 Wi-Fi OTA；USB 口在下载模式下表现为串口/JTAG | 芯片约 ¥10~15；WROOM-1 模组约 ¥18~25 |
| **RP2040**（树莓派） | 双核 Cortex-M0+ 133MHz / 264KB RAM / 外置 QSPI Flash | USB FS 设备/主机（PIO 可扩展） | TinyUSB 官方支持，Pico SDK 例程最完善 | **BOOTSEL 不是 DFU 而是 USB MSC + UF2 拖拽**；另有 picotool（私有 BOOTPROTO）；要 DFU 需自移植 TinyUSB dfu 例程 | 约 ¥4~8 |

**选型结论（本 Lab 采用）**：以 **STM32F103C8T6** 为主平台——它是国内工业小设备的"事实标准"，ROM DFU 提供了不依赖任何自写代码的保底升级路径，且知识库条目最全。CH32V203 作为低成本替代练习（固件层改动最小，硬件差异见 hardware/设计要点.md）；ESP32-S3/RP2040 的差异说明会贯穿各层文档，方便你换平台时知道"哪里不一样"。

## 3. 全栈层级矩阵

本 Lab 覆盖从铜箔到上位机的完整链路，每一层都有独立交付物：

| 层级 | 目录 | 交付物 | 核心学习点 |
|---|---|---|---|
| 硬件 | `hardware/` | BOM.csv、设计要点.md | F103 USB 上拉陷阱、工业 24V 电源、ESD 防护、与 RS485 共存布局 |
| 固件 | `firmware/` | README.md、src/（main.c、tusb_config.h、usb_descriptors.c、cdc_app.c、bootloader 选型） | TinyUSB 复合设备（CDC 双接口+IAD）、环形缓冲、DTR 回调、分区表与跳转 |
| 主机 | `host/` | serial_tool.py、dfu_guide.md | pyserial 自动重连+日志落盘、dfu-util 全流程命令 |
| 抓包 | `capture/` | CDC枚举与数据流.md | 规范重构的枚举帧序列、usbmon/USBPcap 真实抓包方法 |
| 经验 | `experience.md` | 五个工程专题 | COM 号漂移、CH340 假芯片、波特率话术、DFU 回滚、ESD 返回件分析 |

依赖关系：`hardware → firmware → host/capture → experience`。只想快速看协议本质的读者，可以直接读 capture/ 与知识库，不必动烙铁。

## 4. 功能规格（本 Lab 要做到什么程度）

1. **枚举即用**：Windows 10/11、Linux、macOS 插上即出现虚拟串口，无需 INF（Win7 需 INF，方法见 experience.md）；
2. **日志级串口**：批量端点吞吐达到全速理论可用水平（FS 下实测 300~700 KB/s 量级，见知识库 [CDC 概述](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/CDC-通信设备类/00-CDC概述.md)），断线重连后应用不崩、缓冲按策略处理；
3. **DTR 感知**：终端打开/关闭串口时固件感知 DTR 边沿（心跳只在打开时输出）；
4. **软升级**：主机发 `dfu-util -e`（DFU_DETACH）→ 设备复位进 bootloader → DFU 模式重新枚举 → `dfu-util -D` 写入新固件 → 自动跳回应用，全程不拔线、不接 SWD；
5. **失败可恢复**：升级中断电/坏固件，设备仍能通过 BOOT0+ROM DFU 或 bootloader 超时回退被救活（回滚架构讨论见 firmware/src/bootloader选型与跳转设计.md 与 experience.md）。

## 5. 快速开始

```bash
# ① 硬件：按 hardware/BOM.csv 焊接/淘宝蓝pill 均可（注意 R10 上拉陷阱，见 hardware/设计要点.md）
# ② 固件：构建并烧录（首次需 SWD 烧到 0x08003000，或直接用 ROM DFU）
cd firmware/          # 见 firmware/README.md 的构建说明（STM32Cube + TinyUSB）
# ③ 主机工具
python host/serial_tool.py --auto          # 自动发现+自动重连+日志落盘
dfu-util -l                                 # 列出设备（含 ROM DFU 模式）
dfu-util -e                                 # 让运行态设备分离进 bootloader
# ④ 抓包：按 capture/CDC枚举与数据流.md 的 usbmon/USBPcap 命令
# ⑤ 踩坑复盘：experience.md 的五个专题
```

首次没有硬件？`capture/CDC枚举与数据流.md` 的规范重构帧序列 + 知识库可以让你在纸面上完成协议层学习；`host/serial_tool.py` 也可以对着任何现成的 CDC 设备（Arduino Leonardo、树莓派 Pico 等）先用起来。

## 6. 与其他 Lab 的关系

- Lab1（HID）：本 Lab 首次引入**复合设备思维**（IAD+多接口），为 HID+CDC 复合调试器打底；
- Lab3（MSC）及以后：DFU 分区思想将复用到 A/B 双分区与签名升级。

## 参考资源

- 知识库：[CDC-通信设备类](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/CDC-通信设备类/) · [01-DFU固件升级](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/其他设备类/01-DFU固件升级.md)
- 规范：[USB DFU 1.1 (usb.org)](https://www.usb.org/sites/default/files/DFU_1.1.pdf) · USB-IF CDC/PSTN 1.2 规范（[usb.org 文档库](https://www.usb.org/documents) 检索 "CDC"）
- ST：[AN4879 USB 硬件与 PCB 指南](https://www.st.com/resource/en/application_note/an4879-introduction-to-usb-hardware-and-pcb-guidelines-using-stm32-mcus-stmicroelectronics.pdf) · [AN3156 STM32 bootloader 的 USB DFU 协议](https://www.st.com/resource/en/application_note/an3156-usb-dfu-protocol-used-in-the-stm32-bootloader-stmicroelectronics.pdf) · [AN2606 系统存储器启动模式](https://www.st.com/resource/en/application_note/an2606-stm32-microcontroller-system-memory-boot-mode-stmicroelectronics.pdf) · [RM0008 参考手册](https://www.st.com/resource/en/reference_manual/rm0008-stm32f101xx-stm32f102xx-stm32f103xx-stm32f105xx-and-stm32f107xx-advanced-armbased-32bit-mcus-stmicroelectronics.pdf)
- 工具：[dfu-util](https://dfu-util.sourceforge.net/)（[手册](https://dfu-util.sourceforge.net/dfu-util.1.html)）· [TinyUSB](https://github.com/hathach/tinyusb)（[文档](https://docs.tinyusb.org/)）· [USBPcap](https://desowin.org/usbpcap/)
- 元器件行情：[立创商城](https://www.szlcsc.com/)（CH32V203C8T6、RP2040、ESP32-S3 等实时报价）
