# Lab6 · BLE 双模键鼠（BLE HOGP + USB Dongle）

> 🎯 商业原型：**双模无线键鼠**（2.4G USB dongle + 蓝牙，一拖三通道切换，罗技/雷柏同款形态）。
> 本实验室把 [Lab1 HID 复合设备](../lab1-hid-composite/README.md) 的 USB-HID 报告描述符**直接复用**到 BLE HOGP
> （HID over GATT）上，打通"同一份 HID 报告 → USB 有线 / BLE 无线"两条通道。
>
> 诚实声明：本实验室抓包文件为**基于 Bluetooth Core Spec 重构的教学样本**（非真实采集）；
> 固件以 Nordic nRF Connect SDK 官方示例为基础给出**集成点与骨架**，不重复造轮子。

## 1. 商业场景

- **形态**：键盘（或键盘+鼠标套装）内置 nRF52 级 BLE SoC；随附 USB-A dongle（同款芯片做接收器）。
- **卖点**：
  - 双模三通道：通道 1 = dongle（2.4G 私有协议），通道 2/3 = 直连蓝牙（笔记本、平板、手机）；
  - dongle 免驱（USB HID 类设备，即插即用）；
  - 单节 CR2032 或锂电，续航以"月"为单位做营销口径；
  - BLE 直连走 HOGP 标准协议，无需厂商 App。
- **商业对标**：罗技 K380/MX Keys、雷柏 multi-mode 系列等。价格带决定 BOM 预算：入门 2.4G 单模方案整机 BOM 可压到几十元人民币，双模方案增加一颗 BLE SoC + 天线与认证成本（区间，随行情）。

## 2. 双模架构：一份报告，两条通道

```
              按键矩阵 / 光学传感器 / 滚轮编码器
                          │  扫描去抖
                          ▼
              ┌───────────────────────────┐
              │      HID 报告生成层        │  ← 同一份 HID 报告描述符（Report Map）
              │   keyboard.c / mouse.c    │     生成 8~16 字节标准键鼠报告
              └────────────┬──────────────┘
           ┌───────────────┴────────────────┐
           ▼ 通道选择：蓝牙槽位 / dongle      ▼
  ┌─────────────────────┐        ┌──────────────────────┐
  │  BLE HOGP           │        │  dongle（2.4G 私有链路）│
  │  HID Report →       │        │  HID 报告 → 私有封包    │
  │  ATT Notification   │        │  （Nordic ESB / 或      │
  │  (opcode 0x1B)      │        │   dongle-BLE 白名单链路）│
  └─────────┬───────────┘        └──────────┬───────────┘
            ▼                               ▼
     手机 / 平板 / PC 蓝牙            USB dongle（同款 nRF52840）
     （系统 HOGP 驱动，免 App）             │ USB 全速中断端点
                                          ▼
                                       PC（免驱 HID）
```

**核心复用点（本实验室的"枢纽"知识）**：

1. HOGP 的 **Report Map 特征值**（UUID 0x2A4B）内容 = USB HID 报告描述符字节流。协议层上 HOGP 就是"把 HID 报告描述符塞进 GATT"。
2. 差异点：BLE 侧建议每个报告带 **Report ID**（多报告必须区分），单键盘也建议 ID=1，与多数 USB 键盘一致 → 描述符可以 100% 两侧共用。
3. USB 侧的中断端点轮询间隔 ↔ BLE 侧的连接间隔（Connection Interval），是同一条"报告节奏"在两种物理层上的映射（延迟分析见 [experience.md](experience.md)）。

## 3. 选型（写确信的）

| 方案 | 无线 | USB 设备控制器 | 生态 | 角色建议 | 单价区间（随行情） |
|---|---|---|---|---|---|
| **nRF52840**（Nordic） | BLE 5（2M PHY / 长距 Coded PHY，协议栈随 NCS 演进） | ✅ 内置全速 USB（12 Mbps） | nRF Connect SDK（Zephyr），HOGP 官方示例 | **推荐**：键盘本体与 dongle 用**同一颗料**双份生产，BOM 归一 | 裸片约 US$3–6；nRF52840 Dongle（PCA10059）约 US$10 |
| nRF52832（Nordic） | 同代射频 | ❌ 无 | 同上 | 只做键盘本体；dongle 需另配 USB 芯片 | 约 US$2–4 |
| ESP32-C6（乐鑫） | BLE 5.x + Wi-Fi 6 + 802.15.4 | ❌ 仅 USB-Serial/JTAG（调试口，**不能**做通用 USB HID 设备） | ESP-IDF（Bluedroid/NimBLE），有 BLE HID 设备示例 | 键盘本体可行；USB 侧需换 ESP32-S3 | 模块约 US$1.5–3 |
| ESP32-S3（乐鑫） | BLE 5.x | ✅ USB OTG（可做 HID） | ESP-IDF | 单芯片双模备选，功耗上限不如 nRF52 级别 | 模块约 US$2–4 |
| TI CC2652R（SimpleLink） | BLE 5（协议栈随 SDK） | ❌ 无内置 USB 设备控制器 | TI SimpleLink SDK | 可做键盘本体；dongle 需额外 USB MCU（如 CH552/MSP430 级协处理器） | 约 US$2.5–5 |

> 价格为公开渠道量级区间，随行情浮动，采购前以实际报价为准。"确信"边界：各芯片**是否有 USB 设备控制器**、射频代际、官方 SDK 是否提供 HID 示例，这些按公开数据手册/文档可查；具体蓝牙 minor 版本（5.2/5.3/5.4 认证口径）以各厂商最新数据手册为准，此处不硬写。

**本实验室主线 = nRF52840 双份生产**：一颗跑 BLE/2.4G 键盘，一颗跑 dongle（USB HID + 2.4G），原理图、贴片、库存全部归一，这是商业上最省钱的路径。

## 4. 层级矩阵

| 层级 | 内容 | 本实验室文件 | USBTree 知识库 |
|---|---|---|---|
| RF/物理 | 2.4G PCB 天线、π 匹配、32k 睡眠时钟、电源 | [hardware/](hardware/设计要点.md) | [02-链路层与物理层](https://github.com/tangjianfang/USBTree/blob/main/50-枝干-无线关联/BLE-低功耗蓝牙/02-链路层与物理层.md)、[14-RF物理层参数全表](https://github.com/tangjianfang/USBTree/blob/main/50-枝干-无线关联/BLE-低功耗蓝牙/14-RF物理层参数全表.md) |
| 链路层 | ADV / CONNECT_IND / LLCP 控制过程 | [capture/BLE连接帧分析.md](capture/BLE连接帧分析.md) | [03-广播与连接](https://github.com/tangjianfang/USBTree/blob/main/50-枝干-无线关联/BLE-低功耗蓝牙/03-广播与连接.md)、[12-LLCP控制过程全表](https://github.com/tangjianfang/USBTree/blob/main/50-枝干-无线关联/BLE-低功耗蓝牙/12-LLCP控制过程全表.md) |
| GATT/应用 | HOGP 服务注册、报告映射、描述符复用 | [firmware/src/架构说明.md](firmware/src/架构说明.md) | [04-ATT与GATT](https://github.com/tangjianfang/USBTree/blob/main/50-枝干-无线关联/BLE-低功耗蓝牙/04-ATT与GATT.md)、[07-HOGP-HIDoverGATT](https://github.com/tangjianfang/USBTree/blob/main/50-枝干-无线关联/BLE-低功耗蓝牙/07-HOGP-HIDoverGATT.md) |
| 安全 | SMP 配对、白名单、绑定存储 | firmware/src（配对策略节） | [06-SMP安全与配对](https://github.com/tangjianfang/USBTree/blob/main/50-枝干-无线关联/BLE-低功耗蓝牙/06-SMP安全与配对.md)、[05-GAP与连接管理](https://github.com/tangjianfang/USBTree/blob/main/50-枝干-无线关联/BLE-低功耗蓝牙/05-GAP与连接管理.md) |
| USB（dongle 侧） | 私有 2.4G → USB HID 报告 | firmware/src（usb_hid.c 节） | [HID 概述与定位](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/HID-人机接口设备/00-HID概述与定位.md) |
| 主机调试 | nRF Sniffer / Wireshark / btmon | [host/btmon.md](host/btmon.md) | [01-蓝牙体系架构与HCI](https://github.com/tangjianfang/USBTree/blob/main/50-枝干-无线关联/BLE-低功耗蓝牙/01-蓝牙体系架构与HCI.md) |
| 商业 | 配对体验、延迟、多设备、RF 认证、量产烧录 | [experience.md](experience.md) | —（本仓库"怎么卖"层） |

## 5. 快速开始（推荐从 Nordic 官方 HOGP 键盘示例起步）

1. 安装 **nRF Connect SDK**（NCS，v2.x；推荐用 nRF Connect for Desktop 的 Toolchain Manager 或 `nrfutil toolchain-manager`）。
2. 构建官方 HOGP 键盘示例（NCS 仓库内路径 `nrf/samples/bluetooth/peripheral_hids_keyboard`）：

```bash
cd <ncs>/nrf/samples/bluetooth/peripheral_hids_keyboard
west build -b nrf52840dk/nrf52840     # 旧版 SDK 板名为 nrf52840dk_nrf52840
west flash
```

3. 手机/PC 蓝牙搜索并配对（系统侧即出现标准蓝牙键盘）。
4. 用 **nRF Sniffer for Bluetooth LE** + Wireshark 观察连接建立全程：见 [host/btmon.md](host/btmon.md)。
5. 对照 [capture/BLE连接帧分析.md](capture/BLE连接帧分析.md)（基于 Core Spec 重构的帧序列）读懂每一帧。
6. 修改报告映射、加配对按钮与白名单：见 [firmware/src/架构说明.md](firmware/src/架构说明.md)。

> ESP-IDF 路线（乐鑫）：ESP-IDF 自带 BLE HID 设备示例（`examples/bluetooth/bluedroid/ble/ble_hid_device_demo` 一带），思路一致——一句带过，本实验室主线不走它。

## 6. 目录结构

```
lab6-ble-hid-dongle/
├── README.md               ← 本文件（全栈路线图）
├── hardware/
│   ├── BOM.csv             ← 键盘本体 + dongle 双份 BOM
│   └── 设计要点.md          ← 天线/晶振/电源/dongle USB
├── firmware/
│   ├── README.md           ← NCS 构建说明与示例路径
│   └── src/
│       └── 架构说明.md      ← ble_hid.c / usb_hid.c / 配对白名单 骨架与集成点
├── host/
│   └── btmon.md            ← nRF Sniffer / Wireshark / btmon 调试
├── capture/
│   └── BLE连接帧分析.md     ← 基于 Core Spec 重构的连接序列（含声明）
└── experience.md           ← 配对体验 / 延迟 / 多设备 / RF 认证 / 量产
```

## 7. 全栈路线图（G0 → MP）

| 阶段 | 关键动作 | 本实验室入口 |
|---|---|---|
| G0 选型 | nRF52840 双份生产 vs 双芯片；模块 vs 裸片；认证模块化 | §3 选型表 |
| G1 原理图 | 天线 π 网络 + 净空区、32k 晶振、CR2032/锂电双方案、dongle USB | [hardware/设计要点.md](hardware/设计要点.md) |
| EVT | NCS HOGP 示例跑通 + Sniffer 抓包核对序列 | [firmware/README.md](firmware/README.md)、host/ |
| DVT | 延迟实测（7.5/15/30ms 对比）、功耗实测（PPK2）、多设备切换 | [experience.md](experience.md) |
| PVT | RF 一致性摸底（DTM 传导测试）、烧录夹具、绑定/地址量产策略 | experience.md §4/§5 |
| MP | FCC / CE / **SRRC**（中国大陆强制）/ MIC / KC；产测与老化 | experience.md §4 |

## 参考资源

- 知识库 BLE 篇章：[BLE 概述](https://github.com/tangjianfang/USBTree/blob/main/50-枝干-无线关联/BLE-低功耗蓝牙/00-BLE概述.md) 起的 00–15 全系列（本实验室各文件按需引用）。
- Nordic nRF Connect SDK：https://www.nordicsemi.com/Products/Development-software/nRF-Connect-SDK
- NCS HOGP 键盘示例（官方文档）：https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/samples/bluetooth/peripheral_hids_keyboard/README.html
- nRF Sniffer for Bluetooth LE：https://www.nordicsemi.com/Products/Development-tools/nRF-Sniffer-for-Bluetooth-LE
- Bluetooth Core Spec（SIG 官方下载页）：https://www.bluetooth.com/specifications/specs/
- HOGP（HID over GATT）规范页：https://www.bluetooth.com/specifications/specs/hid-over-gatt-1-0/
- Bluetooth Assigned Numbers（UUID/opcode 总表）：https://www.bluetooth.com/specifications/assigned-numbers/
- Nordic DevZone 社区（配对/功耗问题检索价值极高）：https://devzone.nordicsemi.com/
- TI CC2652R 产品页：https://www.ti.com/product/CC2652R
- 乐鑫 ESP32-C6：https://www.espressif.com/en/products/socs/esp32-c6
