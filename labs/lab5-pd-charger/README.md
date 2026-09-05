# Lab5：USB PD 电源——受电端 Sink 与供电源 Source 全栈实战

> 📍 系列位置：[labs/lab5-pd-charger/](./) ｜ 前置：Lab1~Lab4（USB 设备侧）建议先完成，本 Lab 转向**供电侧**
> 🌿 知识库：[04-USBPD协议](https://github.com/tangjianfang/USBTree/blob/main/30-枝干-接口与供电/04-USBPD协议.md) ｜ [07-USBPD深入-状态机与消息全表](https://github.com/tangjianfang/USBTree/blob/main/30-枝干-接口与供电/07-USBPD深入-状态机与消息全表.md) ｜ [09-TypeC规范级-状态机与CC时序](https://github.com/tangjianfang/USBTree/blob/main/30-枝干-接口与供电/09-TypeC规范级-状态机与CC时序.md)

本 Lab 以**商业产品视角**做一遍 USB Power Delivery (PD) 的全栈：从 Type-C 接口与 CC 电气，到 PD 协议（BMC 物理层 → 协议层 → 策略引擎），再到后级 DC-DC / AC-DC 电源、主机侧验证与抓包分析。覆盖两个互补角色：

- **Sink（受电端）**：从 Type-C 口取电并协商电压档位的设备；
- **Source（供电源）**：对外供电、广播 PDO（Power Data Object）的端口，典型即"充电器"。

## 1. 商业场景

| 角色 | 典型产品 | 供电特征 | 工程重点 |
| --- | --- | --- | --- |
| Sink | 电动工具（电池包/主机充电口）、便携显示器、路由器/NUC/NAS | 20 V 档取电，几十瓦级，可能带电池 | VBUS 输入开关与防倒灌、后级宽压 DC-DC、断电行为 |
| Sink | 配件类（小风扇、台灯、桌面设备） | 5 V/9 V 简单档 | 免固件协议芯片即可，成本敏感 |
| Source | GaN 充电器（30W~100W，SPR） | 5/9/12/15/20 V 广播 | AC-DC 安规、协议芯片、恒流恒流保护 |
| Source | 拓展坞/显示器/笔记本电源（100W~240W） | 20 V/5 A（SPR 上限）或 EPR 28/36/48 V | E-marker 线缆、EPR 门槛、多口功率分配（DPM） |

> 注意：**AC-DC 一次侧涉及市电**。本 Lab 的 Source 实操建议先用低压直流（如 24 V 适配器）做"USB Type-C Source 输出板"，完整充电头只做设计要点与方向（见 [hardware/设计要点.md](hardware/设计要点.md)），不强求动手绕变压器。

## 2. 两条技术路线对比

这是本 Lab 最关键的一次选型：**PD 协议栈放在哪里跑？**

| 维度 | 路线 A：硬件协议芯片（免固件 PD 栈） | 路线 B：TCPM + TCPC（MCU 软件栈） |
| --- | --- | --- |
| 代表器件 | CH224K / CH224M（沁恒）、STUSB4500（ST）、HUSB238（慧能泰）；Source 侧如智融/南芯/英集芯协议芯片、Infineon CCGx | FUSB302（onsemi）或 HUSB311（慧能泰）作 TCPC，MCU 跑 TCPM/PE |
| PD 栈在哪 | PHY/协议层/策略层全部在芯片内，上电即协商 | BMC PHY 在 TCPC 内；GoodCRC/重试多数可硬件化；**PE（策略引擎）在你的 MCU 固件里** |
| 灵活性 | 目标档位由引脚/I²C 预先配置，运行中改档=改配置 | 任意 PDO/PPS 策略、运行时动态调整、可做 DPM（多口功率分配） |
| 开发成本 | 几乎零固件，一周出板 | 需移植或实现 PE（数千行状态机），周期以月计 |
| 可观测性 | 只能读状态寄存器/看引脚 | 全链路日志、可注入异常、可单步 |
| 典型风险 | 档位行为黑盒；特殊需求（PPS、DRP、FRS）未必支持 | 自研栈未过一致性测试（CTS）前，兼容性靠自查 |
| 适用 | "只要电"的产品：电动工具、路由器、配件 | 显示器、拓展坞、手机、需要策略的产品 |

一句话：**先问产品要不要"策略"**。只要电→路线 A；要管理电（多口、动态预算、PPS 精调、和系统联动）→路线 B。Linux 内核已有生产级 TCPM 实现（`drivers/usb/typec/tcpm/`，含 FUSB302 驱动），路线 B 不必从零造轮子。

### 器件选型表（价格为散量参考区间，随行情波动，以实时渠道为准）

| 器件 | 角色 | 配置方式 | 特点 | 参考价（元） |
| --- | --- | --- | --- | --- |
| CH224K（沁恒） | Sink 诱骗 | CFG1/2/3 引脚 | 5/9/12/15/20 V，PD 3.0，最高 100 W | 2~5 |
| CH224M / CH224Q（沁恒） | Sink 诱骗 | 引脚 + I²C | I²C 读状态/切档，适配自动化测试 | 3~6 |
| HUSB238（慧能泰） | Sink 诱骗 | I²C/电阻 | 国产低价，市售"PD 诱骗板"常客 | 1~3 |
| STUSB4500（ST） | Sink 自主协商 | I²C + NVM | 上电自动选最优 PDO，免 MCU | 8~20 |
| FUSB302（onsemi） | TCPC PHY | I²C + INT | 裸 PHY，配 TCPM 用，资料最多 | 3~8 |
| HUSB311（慧能泰） | TCPC PHY | I²C + INT | 国产 FUSB302 对标 | 2~5 |
| CCGx（Infineon）/ TI 端口控制器 | Source/Sink 端口管理 | I²C，带固件 SDK | 生态成熟，适合正经产品 | 10~30 |
| 智融 SW2303 / 英集芯 IP2721 / 南芯 SC2001 类 | Source 协议（含反馈） | — | 国产充电头主力，常集成反馈调节 | 1.5~4 |

> Source 侧没有"CH224"对等物——CH224 系列只做 Sink 诱骗。做 Source 需选择支持 Source 角色的协议芯片（上表后两行）。

## 3. 层级矩阵：谁实现了哪层

| 层级 | 路线 A：CH224K | 路线 A：STUSB4500 | 路线 B：FUSB302 + TCPM | Source 协议芯片 |
| --- | --- | --- | --- | --- |
| Type-C 连接/CC 电气（Rp/Rd、方向） | 芯片内 | 芯片内 | 芯片内（TCPC） | 芯片内 |
| PD 物理层（BMC/4b5b/CRC-32） | 芯片内 | 芯片内 | 芯片内（TCPC） | 芯片内 |
| PD 协议层（GoodCRC/重传/MsgID） | 芯片内 | 芯片内 | TCPC 硬件为主 | 芯片内 |
| 策略引擎 PE（何时请求/接受） | 芯片内（固定策略） | 芯片内（NVM 策略） | **你的固件** | 芯片内 |
| 设备策略 DPM（要多少电） | 引脚选择 | I²C/NVM 配置 | **你的固件/系统** | 常为固定广播 |
| 应用层（DC-DC、显示、充电管理） | 你的硬件 | 你的硬件 | 你的硬件 | 你的电源拓扑 |

对应规范分层（DPM → PE → PL → PHY）详见知识库 [04-USBPD协议](https://github.com/tangjianfang/USBTree/blob/main/30-枝干-接口与供电/04-USBPD协议.md) 第九节。

## 4. 全栈路线图（按目录）

```
lab5-pd-charger/
├── hardware/          阶段 1：BOM 与原理图要点（Sink / Source 双栏）
│   ├── BOM.csv
│   └── 设计要点.md
├── firmware/          阶段 2：两条路线的固件
│   ├── README.md
│   └── src/
│       ├── fusb302.c/.h       路线 B：TCPC 驱动骨架
│       ├── pe_sink.c          路线 B：PE_SNK 状态机骨架
│       └── ch224_example.c    路线 A：CH224 引脚/I²C 示例
├── host/              阶段 3：主机/仪器验证
│   └── pd_test.md
└── capture/           阶段 4：协议时序分析
    └── PD协商帧序列.md
```

| 阶段 | 做什么 | 产出 | 文档 |
| --- | --- | --- | --- |
| 0 | 手头充电器摸底 | 每个 Source 的真实档位清单 | [host/pd_test.md](host/pd_test.md) §2 |
| 1 | Sink 板 / Source 概念设计 | 原理图 + BOM | [hardware/BOM.csv](hardware/BOM.csv)、[hardware/设计要点.md](hardware/设计要点.md) |
| 2 | 路线 A 直通 / 路线 B 状态机 | 点亮 + 协商成功日志 | [firmware/README.md](firmware/README.md) |
| 3 | 仪器验证 | 电压转换/限流/效率数据表 | [host/pd_test.md](host/pd_test.md) |
| 4 | 协议时序分析 | 协商帧逐字段解读 | [capture/PD协商帧序列.md](capture/PD协商帧序列.md) |
| — | 踩坑与产品化 | 经验沉淀 | [experience.md](experience.md) |

## 5. 快速开始（先花 30 元，再谈设计）

1. **推荐第一步：买一块 CH224K 诱骗模块**（市售成品约 ¥10~20）+ 一个 USB 测试仪（FNB58 / POWER-Z 类，约 ¥100~300）或万用表。
2. 把模块接到你手头每一个充电器上，拨到 9 V/12 V/20 V，记录：能否锁定、空载电压、带载跌落。这一步就是最朴素的 PD 协商验证，也是 [host/pd_test.md](host/pd_test.md) 的热身。
3. 想深入协议 → 上 FUSB302 开发板 + MCU，跑 [firmware/src/pe_sink.c](firmware/src/pe_sink.c) 骨架，配合 [capture/PD协商帧序列.md](capture/PD协商帧序列.md) 对照日志。
4. 想做产品 → 按 [hardware/BOM.csv](hardware/BOM.csv) 起原理图，读 [experience.md](experience.md) 的兼容性与认证部分再立项。

## 6. 安全与合规红线

- AC-DC 一次侧有市电危险；无隔离经验者只做低压直流侧的 Source/Sink 实验。
- EPR（28/36/48 V，最高 240 W）电压等级和安规要求显著高于 SPR，新手产品定位从 SPR（≤100 W）开始。
- 教学骨架（本目录固件）**未经过 USB-IF 一致性测试**，商用必须换用认证过的协议芯片固件或成熟协议栈。
- 标称 >15 W（5 V/3 A 以上）的对外销售充电器，生态上普遍要求 USB-IF PD 认证，国内销售电源适配器属 CCC 强制认证范围——详见 [experience.md](experience.md)。

## 参考资源

- 知识库：[USBPD协议](https://github.com/tangjianfang/USBTree/blob/main/30-枝干-接口与供电/04-USBPD协议.md) ｜ [USBPD深入-状态机与消息全表](https://github.com/tangjianfang/USBTree/blob/main/30-枝干-接口与供电/07-USBPD深入-状态机与消息全表.md) ｜ [TypeC规范级-状态机与CC时序](https://github.com/tangjianfang/USBTree/blob/main/30-枝干-接口与供电/09-TypeC规范级-状态机与CC时序.md)
- USB-IF 官方 PD 页面（规范下载入口）：<https://www.usb.org/powerdelivery>
- USB-IF 认证产品查询：<https://www.usb.org/compliance>
- Linux 内核 Type-C/TCPM 文档：<https://www.kernel.org/doc/html/latest/driver-api/usb/typec.html>（实现参考 `drivers/usb/typec/tcpm/`）
- 器件资料（各官网产品页/应用笔记）：沁恒 <https://www.wch.cn>（搜 CH224）、ST <https://www.st.com/en/interfaces-and-transceivers/stusb4500.html>、onsemi <https://www.onsemi.com>（搜 FUSB302）、Infineon <https://www.infineon.com>（EZ-PD）、慧能泰 <https://www.hynetek.com>
- 拆解与测试社区（站名提及，不转引具体文句）：Chargerlab <https://www.chargerlab.com>
- 开源 PD 嗅探器 Twinkie（Chromium OS EC 项目）：<https://chromium.googlesource.com/chromiumos/platform/ec>
