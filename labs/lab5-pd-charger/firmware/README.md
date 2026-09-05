# Lab5 固件：两条路线的实现说明

> 📍 [lab5-pd-charger/firmware/](./) ｜ 知识库：[07-USBPD深入-状态机与消息全表](https://github.com/tangjianfang/USBTree/blob/main/30-枝干-接口与供电/07-USBPD深入-状态机与消息全表.md)（PE 状态机/定时器/消息全表）、[04-USBPD协议](https://github.com/tangjianfang/USBTree/blob/main/30-枝干-接口与供电/04-USBPD协议.md)
>
> **诚实声明**：本目录 `src/` 下的代码是**教学骨架**（能编译的思路地图，不是可量产固件）：
> - 没有经过 USB-IF 一致性测试（CTS），协议边角（Hard Reset 全分支、扩展消息分块、BIST、EPR）未实现；
> - 寄存器位域与芯片地址**以各家最新数据手册为准**，代码中已逐处标注不确定点；
> - **商用产品请使用**：Linux 内核 TCPM（`drivers/usb/typec/tcpm/`）、厂商 SDK/端口控制器固件，或购买已认证的协议芯片方案。

## 路线 A：硬件协议芯片（免固件 PD 栈）

PD 栈全部在芯片内，你的"固件"只有两件事：**配置**（目标档位）与**观测**（读状态）。

- **CH224K**（沁恒）：上电用 CFG1/CFG2/CFG3 三个引脚选目标电压档（组合表见数据手册），PG（Power Good）引脚指示协商成功。无需任何代码。
- **CH224M / CH224Q**：在引脚配置之外提供 I²C，可读协商状态/切换档位，适合自动化测试或需要运行时改档的产品。
- **STUSB4500**：NVM 保存 PDO 偏好，上电自动协商，无 MCU 也可工作；接 MCU 可动态改策略。
- **HUSB238**（慧能泰）：国产低价诱骗方案，市售 PD 触发板主力。

示例代码：[src/ch224_example.c](src/ch224_example.c)——CH224K 引脚配置思路 + CH224M/CH224Q 的 I²C 状态读取骨架。

适用：电动工具、路由器、桌面配件等"只要电"的产品；以及本 Lab 的**第一步验证**（见 [../README.md](../README.md) 快速开始）。

## 路线 B：TCPM + TCPC（MCU 软件栈）

分工（即知识库 07 文"实现者清单"的架构）：

```
应用/DPM（你的业务：要多少电、何时改）
   ↓
PE  策略引擎（pe_sink.c：PE_SNK 状态机）
   ↓
TCPM/TCPC 接口层（fusb302.c：I²C 驱动 + FIFO 收发）
   ↓
TCPC 硬件（FUSB302：BMC PHY、CC 测量、GoodCRC/重试硬件化）
```

关键设计：**把 GoodCRC 应答、MessageID、重传交给 FUSB302 硬件**（自动应答模式），MCU 只处理"收到什么消息、处于什么状态、下一发什么"。这正是 Linux 内核 TCPM 的分层方式。

| 文件 | 内容 | 状态 |
| --- | --- | --- |
| [src/fusb302.h](src/fusb302.h) | 寄存器定义、常用位、消息/Header 结构 | 骨架，位定义以数据手册为准 |
| [src/fusb302.c](src/fusb302.c) | 设备 ID 读取、CC 状态测量（HOST_CUR+MDAC 法）、BMC FIFO 收发入口 | 骨架，含平台 HAL 抽象 |
| [src/pe_sink.c](src/pe_sink.c) | PE_SNK 简化状态机：Wait_Source_Cap → Evaluate → Request → PS_RDY → Ready | 骨架，逐状态注释对应 PD 规范章节 |

### 移植要点

1. `src/fusb302.c` 头部列出了 4 个平台钩子（`plat_i2c_read/write`、`plat_delay_ms`、INT 引脚），接到你的 MCU HAL 即可。
2. CC 测量用"电流源 + 比较器阈值扫描"法（数据手册 CC 测量章节），骨架给出流程，阈值二分细节可对照内核 `fusb302.c` 实现。
3. PE 状态机是**协作式**：`pe_sink_run()` 放主循环或定时器里跑，收到消息时调 `pe_sink_on_rx()`。骨架未做 RTOS 封装。
4. 定时器取值（tSenderResponse、tTypeCSendSourceCap、tPSTransition 等）见知识库 07 文"实现者清单"表；骨架用 `pd_ctx.timer_*` 字段模拟，正式实现请用硬件定时器。
5. 联调必备：INT 引脚中断 + UART 日志；再配 PD 分析仪（[../host/pd_test.md](../host/pd_test.md)）看线上真实行为。

### 什么时候别自研

- 产品要做 **PPS/DRP/FRS/EPR**、多口功率分配、与 USB 数据角色联动 → 直接用内核 TCPM（Linux 系统）或厂商端口控制器（Infineon CCGx/TI 等，固件已过认证）。
- 需要 USB-IF 认证的充电器/配件 → 自研未认证栈的整改成本远高于买认证方案。

## 与知识库的对照阅读顺序

1. [04-USBPD协议](https://github.com/tangjianfang/USBTree/blob/main/30-枝干-接口与供电/04-USBPD协议.md)：PDO/RDO/消息类型，看懂报文字段；
2. [07-USBPD深入](https://github.com/tangjianfang/USBTree/blob/main/30-枝干-接口与供电/07-USBPD深入-状态机与消息全表.md)：PE 状态机、定时器表、可靠传输三件套；
3. [../capture/PD协商帧序列.md](../capture/PD协商帧序列.md)：把骨架日志与线上帧序列对上。

## 参考资源

- Linux 内核 Type-C/TCPM 文档：<https://www.kernel.org/doc/html/latest/driver-api/usb/typec.html>（实现参考 `drivers/usb/typec/tcpm/`，含 `fusb302.c` 生产级驱动）
- USB PD 规范原文：<https://www.usb.org/powerdelivery>
- 器件手册：沁恒 <https://www.wch.cn>（CH224 系列）、onsemi <https://www.onsemi.com>（FUSB302）、ST <https://www.st.com/en/interfaces-and-transceivers/stusb4500.html>、慧能泰 <https://www.hynetek.com>
- 开源参考：Chromium OS EC 项目（Twinkie 嗅探器固件/驱动）：<https://chromium.googlesource.com/chromiumos/platform/ec>
