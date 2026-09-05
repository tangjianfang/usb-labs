# Lab7 · USB4 / Type-C 扩展坞（DP Alt Mode + PD + Hub）· 概要级

> 🎯 商业原型：**USB-C/USB4 扩展坞**（上行 Type-C，下行 HDMI/DP + USB-A/C + 2.5G 网卡 + PD 充电直通）。
>
> ⚠️ **诚实声明（本实验室定位）**：商业扩展坞的门槛远高于前六个实验室——USB4/雷电主控授权、
> USB-IF 全栈认证、DP 带宽协商调试、多 PC 兼容性矩阵，均为数人月到数人年的工程量。
> 因此本实验室**只给架构地图、选型起点与调试方向**（概要级），不提供完整实现；
> 深度与完整版实验室（Lab1–Lab6）不同，不要按同一标准期待。

## 1. 架构：一个坞站 = 四个子系统

```
            PC / 手机（Host，全功能 Type-C 口）
                     │  USB4 / USB3.2 Gen2 线缆（E-marker ≥5A，见知识库 E-marker 篇）
                     ▼
┌───────────────────────────────────────────────────────────┐
│ 扩展坞                                                     │
│                                                           │
│ ① PD 控制器（门面）                                        │
│    CC 逻辑 · PD 3.1 SPR/EPR 功率协商（最高 240W 直通）      │
│    SOP' 读线缆 E-marker · VDM 收发 → 进入/退出 DP Alt Mode  │
│         │ Enter Mode（VDM）                                │
│         ▼                                                 │
│ ② 显示子系统（两条技术路线）                                │
│    A. DP Alt Mode 直出：DP/HDMI 转换器 + (可选)MST hub      │
│    B. USB4 路线：USB4 主控隧道 DP，再转 DP/HDMI             │
│         │                                                 │
│ ③ USB3 子系统                                              │
│    USB3 Hub 控制器 → 下行 USB-A×N / USB-C                  │
│    （USB4 路线下为隧道内的 USB3 hub）                       │
│ ④ 网卡 / 读卡器 / 音频（挂 ③ 之下）                         │
│    GbE/2.5GbE USB 网卡 · UASP 读卡 · UAC                   │
│                                                           │
│ ⑤ Billboard 设备（小但必做，见 §5 挑战）                    │
│ ⑥ 电源树：PD 直通给主机 + 坞站自身供电，多轨 DC-DC          │
└───────────────────────────────────────────────────────────┘
```

一句话理解：**扩展坞 = PD 控制器（谈判代表）+ 显示/数据分发芯片（干活的）+ Billboard（翻译官）+ 一堆挂在 hub 下的标准 USB 设备**。主机视角的枚举拓扑解读见 [capture/Dock 枚举拓扑.md](capture/Dock 枚举拓扑.md)。

## 2. 层级矩阵（概要）

| 层级 | 内容 | 深入入口 |
|---|---|---|
| 连接器/线缆 | 全功能 Type-C、E-marker（≥5A 线必带）、插拔寿命 | [05-AlternateMode与E-marker](https://github.com/tangjianfang/USBTree/blob/main/30-枝干-接口与供电/05-AlternateMode与E-marker.md)、[02-USBType-C详解](https://github.com/tangjianfang/USBTree/blob/main/30-枝干-接口与供电/02-USBType-C详解.md) |
| PD/CC | PD 3.1 SPR/EPR、Source/Sink、SOP'/SOP''、VDM | [04-USBPD协议](https://github.com/tangjianfang/USBTree/blob/main/30-枝干-接口与供电/04-USBPD协议.md)、[07-USBPD深入-状态机与消息全表](https://github.com/tangjianfang/USBTree/blob/main/30-枝干-接口与供电/07-USBPD深入-状态机与消息全表.md)、[09-TypeC规范级-状态机与CC时序](https://github.com/tangjianfang/USBTree/blob/main/30-枝干-接口与供电/09-TypeC规范级-状态机与CC时序.md) |
| Alt Mode/显示 | DP Alt Mode 进入/退出、lane 分配、SST/MST、DSC | [05-AlternateMode与E-marker](https://github.com/tangjianfang/USBTree/blob/main/30-枝干-接口与供电/05-AlternateMode与E-marker.md) |
| USB4/隧道 | 路由器/适配层、USB3/DP 隧道、连接管理器（CM） | [02-USB4与雷电整合](https://github.com/tangjianfang/USBTree/blob/main/40-枝干-高速演进/02-USB4与雷电整合.md)、[05-USB4深入-路由隧道与配置](https://github.com/tangjianfang/USBTree/blob/main/40-枝干-高速演进/05-USB4深入-路由隧道与配置.md)、[09-USB4-CM指南要点](https://github.com/tangjianfang/USBTree/blob/main/40-枝干-高速演进/09-USB4-CM指南要点.md) |
| USB 设备 | Hub/网卡/读卡器/Billboard 枚举 | Lab3（MSC）、[20-枝干-设备类协议](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议) |
| 调试/认证 | USB4 CM 日志、分析仪、USB-IF/TB 认证 | [01-协议分析仪与抓包](https://github.com/tangjianfang/USBTree/blob/main/70-枝干-调试测试与安全/01-协议分析仪与抓包.md)、[03-USB-IF合规认证](https://github.com/tangjianfang/USBTree/blob/main/70-枝干-调试测试与安全/03-USB-IF合规认证.md) |
| 商业 | 兼容性名单、发热、返修 | [experience.md](experience.md) |

## 3. 选型起点（厂商/方案级，具体型号以厂商在产为准）

> 以下只写到"方案类别 + 代表厂商"层面。**芯片迭代快、授权与在产状态变化频繁，任何选型决策前必须向厂商确认在产型号与授权条款**。

| 子系统 | 方案类别 | 代表厂商/方案 | 说明 |
|---|---|---|---|
| PD 控制器 | 双口坞站级 PD（EPR/SPR、VDM、SOP'） | Infineon（原 Cypress）EZ-PD CCG 系列坞站型、TI TPS659x 系列等 | 坞站的"谈判代表"，很多方案自带 Billboard 逻辑与 Alt Mode 协同固件 |
| 显示（非 USB4） | DP Alt Mode → DP/HDMI、MST hub | 创溪等国内方案商、VIA（威盛电子 Labs）、Realtek（瑞昱）、Synaptics、Parade/Analogix 类 | 一颗芯片拆多路显示；以厂商在产型号为准 |
| USB4/雷电 | USB4/TB4 主控 | Intel 雷电主控（TB 认证需 Intel 授权链）、ASMedia ASM4242 类等 | 单颗主控 + 外围 PD/hub 组成完整坞站方案；授权与认证成本高 |
| USB3 Hub | 4 口 Gen1/Gen2 hub | VIA Labs VL817（Gen1）/ VL822（Gen2）类、Realtek 等 | 最成熟的一环，便宜稳定 |
| 网卡 | USB3 网卡 | Realtek RTL8153（GbE）/ RTL8156B（2.5GbE） | 驱动生态最省心的选择之一 |
| 直通充电 | EPR 140W/240W 适配器 + 坞内功率分配 | 与 PD 控制器同家配套 | EPR 电压 28V/36V/48V 档，[07-USBPD深入](https://github.com/tangjianfang/USBTree/blob/main/30-枝干-接口与供电/07-USBPD深入-状态机与消息全表.md) |

**学习者的现实起点**（不开发坞站，先当"坞站产品经理/调试工程师"培养）：

1. 手里有 USB4/雷电笔记本 + 市售坞站 → 按 [capture/Dock 枚举拓扑.md](capture/Dock 枚举拓扑.md) 把拓扑读明白；
2. 拆解市售坞站做 BOM 逆向（ tear-down 类公开资料很多，各 EDA/评测站可检索）；
3. 跟踪 USB-IF 认证列表里在售坞站的 VID/PID 与认证状态（https://www.usb.org/ ）。

## 4. BOM 量级表（概要，仅为量级感）

| 部件 | 单价量级（随行情） | 备注 |
|---|---|---|
| PD 控制器 | US$2–5 | 双口坞站级 |
| USB4 主控（USB4/TB 款才有此行） | US$15–35 | 授权费另计，整机成本大头 |
| MST hub/显示转换（非 USB4 款） | US$3–10 | 视 DP 版本与路数 |
| USB3 Hub 控制器 | US$1–2 | |
| 2.5GbE 网卡 | US$2–4 | GbE 更低 |
| E-marker Type-C 座/线缆组件 | US$1–3（座）/ US$5–10（线） | 线缆详见 experience.md §4 |
| PCB（≥4 层，USB4 款 8–12 层常见） | 整机 BOM 百元人民币级起 | 高速布线要求高 |
| 结构（铝壳 + CNC） | 数十元人民币级 | 散热见挑战清单 |
| **整机 BOM 量级** | **非 USB4：百元人民币级；USB4/雷电：数百元人民币级** | 概要，随行情与量产规模浮动 |

## 5. 关键挑战清单（为什么这个品类门槛高）

1. **发热与降速**：USB4 主控 + MST 转换 + 2.5G 网卡挤在小铝壳里，热设计直接决定"拷大文件几分钟掉速"的口碑；铝壳既是结构件也是散热器，BOM 里最不能省的一项。
2. **认证组合拳**：USB-IF 认证（Type-C/PD/Hub/Billboard，过审上 Integrator's List 才能用 USB 商标）、USB4 认证、雷电认证（Intel 授权 + 版税）；加上各目标市场的 EMC/安全。清单见知识库 [03-USB-IF合规认证](https://github.com/tangjianfang/USBTree/blob/main/70-枝干-调试测试与安全/03-USB-IF合规认证.md)。
3. **DP 带宽协商**：Alt Mode 的 lane 分配（4 lane DP 独占 vs 2 lane DP + USB3 共存）、HBR3/HBR2、DSC 压缩、MST 拓扑——"4K60 + 10Gbps USB 同时可用"是产品规格表的第一行，也是翻车第一线。
4. **Windows Billboard**：DP 进不了 Alt Mode 时，Windows 靠 **Billboard 设备**（USB-IF Billboard Device Class）弹出诊断提示。没有 Billboard = 用户面对黑屏，客服只收到"不亮"，无法远程定位。多数坞站方案由 PD 控制器固件兼任 Billboard。
5. **兼容性矩阵**：各品牌 PC 的 PD/USB4/DP 实现差异 + 各版本 Windows/macOS/Linux 行为差异 → "官方兼容列表"是坞站厂商的核心资产（见 [experience.md](experience.md) §1）。

## 6. 快速开始（观察型实验，零风险）

1. 找一台 USB4/雷电笔记本 + 一个坞站，Windows 设备管理器看 "USB4 Host Router / 集线器"，Linux 用 `boltctl list` 与 `/sys/bus/thunderbolt/devices/`；
2. `lsusb -t`（或 USBTreeView/IORegistryExplorer）画出树，对照 [capture/Dock 枚举拓扑.md](capture/Dock 枚举拓扑.md) 认出 hub / Billboard / 网卡 / 显示各在哪一支；
3. 拔掉显示器线，看 Windows 弹出的 USB-C 提示——那就是 Billboard 在工作；
4. 用坏一根杂牌线缆复现"忽连忽断"，理解 E-marker 与线缆等级（知识库 E-marker 篇）；
5. 读完 §5 挑战清单与 [experience.md](experience.md)，你就具备了坞站产品/测试岗的面试语料。

## 参考资源

- USB-IF 官方（规范/认证/Billboard Device Class 规格）：https://www.usb.org/
- USB4 规范下载页（v2.0）：https://www.usb.org/document-library/usb4r-specification-v20
- 雷电技术（Intel 官方站，认证/开发者入口）：https://thunderbolttechnology.net/
- VESA DisplayPort（Alt Mode/DSC/MST 资料）：https://www.displayport.org/
- 知识库：[05-AlternateMode与E-marker](https://github.com/tangjianfang/USBTree/blob/main/30-枝干-接口与供电/05-AlternateMode与E-marker.md) · [02-USB4与雷电整合](https://github.com/tangjianfang/USBTree/blob/main/40-枝干-高速演进/02-USB4与雷电整合.md) · [09-USB4-CM指南要点](https://github.com/tangjianfang/USBTree/blob/main/40-枝干-高速演进/09-USB4-CM指南要点.md) · [03-USB-IF合规认证](https://github.com/tangjianfang/USBTree/blob/main/70-枝干-调试测试与安全/03-USB-IF合规认证.md)
