# Lab4：UVC 摄像头 + UAC 麦克风 —— USB 会议/采集设备全栈实现（UVC Camera + UAC Microphone）

> USB-Labs 系列 Lab4。目标：从硬件选型到固件描述符、主机调试、总线抓包，完整走通一台"USB 会议设备"（视频 UVC + 音频 UAC 复合设备）的全栈链路。
> 本 Lab 的帧序列解读均为**基于 UVC 1.5 / UAC 1.0 规范重构的示意**（非真实抓包数据），真实抓包方法见 [capture/UVC流建立帧分析.md](capture/UVC流建立帧分析.md)。

---

## 1. 商业场景：为什么值得学

UVC（USB Video Class）+ UAC（USB Audio Class）是消费与商用市场上出货量最大的两类 USB 设备协议之一，三者共享同一套技术底座（UVC 视频 + UAC 麦克风 + UVC/H.264 或裸流）：

| 商业产品 | USB 身份 | 典型形态 | 市场参考价区间* |
|---|---|---|---|
| USB 会议摄像头 | UVC（复合 UAC 麦克风） | 罗技 C920/C930e 级 ~ 会议全景一体机 | ¥100–1,000（桌面级）/ ¥2,000–20,000+（会议一体机） |
| USB 会议麦克风 | UAC（录音 + 回放） | 全向麦/播客麦（如 Jabra、Poly 级） | ¥150–1,500 |
| HDMI→USB 采集棒 | UVC（MJPEG/YUY2 批量流） | 直播推流采集卡 | ¥50–500 |
| 教学记录/可视门铃模组 | UVC（输出给上位机） | USB 相机模组 | ¥20–150 |

\* 价格为 2025–2026 公开电商渠道的大致区间（人民币），随市场波动，仅供参考。

一句话产品逻辑：**能 UVC 则 UVC**——免驱、全平台即插即用，厂商私有协议只留给 Extension Unit 的增强通道（知识库：[UVC 详解 §7](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/Video-UVC/00-UVC详解.md)）。

---

## 2. 方案分级现实：MCU 方案 vs SoC 方案（必读）

"用 STM32 做一个 1080p60 摄像头"是不现实的目标。商业 UVC 设备的主流实现分两个世界，**本 Lab 两者都讲清楚，固件代码只做教学级 MCU 方案，商用请直接看 SoC 方案**：

### 2.1 对比表

| 维度 | 教学级：MCU 方案 | 商业主流：SoC 方案 |
|---|---|---|
| 主控 | STM32H743（Cortex-M7 480MHz）+ OTG HS | 全志 V3s、瑞芯微 RV1103/RV1106、海思 Hi3516EV200、君正 T31 等专用 ISP SoC |
| 摄像头接口 | DVC 并行口（DCMI/DVP）+ OV5640（自带 JPEG 编码） | MIPI CSI-2（2-lane 起）+ 片上 ISP |
| 视频 | **低帧率 MJPEG**：640×480@30 稳，720p 实际 15–25fps（见 §2.2 带宽账） | 1080p@30 MJPEG/H.264、4K@30（H.264/265 机型） |
| 音频 | I2S MEMS 麦克风（INMP441/ICS-43434），UAC1 | 音频 codec（ES8311/ALC5651 等），UAC1/UAC2，支持回放+录音 |
| 软件栈 | 裸机/RTOS 手写描述符 + DMA 管线 | Linux（UVC gadget + ALSA gadget）或厂商 RTOS 原生 UVC 栈 |
| 裸片价格* | H743 约 ¥45–75 | V3s/RV1106 约 ¥10–40（核心板 ¥50–150） |
| 整机 BOM* | 开发板路线 ¥300–500 | 核心板+sensor+codec+铝壳 ¥150–400 |
| 生态/量产 | 教学、原型、低速工业相机 | 商业摄像头、会议设备、采集卡的事实标准 |
| 上手难度 | 需吃透描述符与带宽（教学价值最高） | 需 Linux 设备树/驱动知识（工程价值最高） |

\* 2025–2026 公开渠道区间，仅供参考，以实时行情为准。

### 2.2 一条关键带宽账（为什么 MCU 方案帧率上不去）

- USB 2.0 高速同步端点**单事务**每微帧（125µs）最多 1024 字节 → 8.192 Mbit/s；
- 1280×720 MJPEG 一帧中位约 60 KB（画质/场景相关，30–110 KB 均常见）→ 30fps 需 ≈14.4 Mbit/s；
- 所以单事务 MCU 方案 720p30 装不下：要么压画质（平均帧 ≤34 KB），要么降到 15–25fps；
- 商业方案用**高带宽端点**（每微帧 2–3 个事务，24.6 Mbit/s）或**批量传输**（采集卡）解决——计算细节见 [hardware/设计要点.md §6](hardware/设计要点.md)。

### 2.3 选型速断

- **学 USB 协议本身** → MCU 方案（本 Lab firmware/ 的教学代码）；
- **做商业产品** → SoC 方案：Linux + UVC gadget 是开源生态最全的路线（全志 V3s 有主线支持、RV1106 生态便宜量大）；海思/君正偏封闭但出货量大；
- **纯主机侧学习** → 一颗几十元的现成 UVC 摄像头 + 本 Lab 的 host/ 与 capture/ 文档即可开跑，零硬件门槛。

---

## 3. 层级矩阵

| 层级 | 教学级（MCU） | 商业主流（SoC） |
|---|---|---|
| hardware | H743 核心板 + OV5640 DVP 模组 + I2S MEMS 麦 + ULPI PHY + ESD（[BOM.csv](hardware/BOM.csv)） | SoC 核心板 + MIPI sensor FPC + ES8311 codec + 铝壳（[BOM.csv](hardware/BOM.csv) 两栏对照） |
| firmware | 手写 UVC/UAC1 复合描述符 + MJPEG 管线（[firmware/src/](firmware/src/)，教学级） | Linux configfs UVC/UAC gadget + uvc-gadget 用户态（[firmware/README.md](firmware/README.md)） |
| host | v4l2-ctl / ffmpeg / arecord / Windows 相机（[host/预览与调参.md](host/预览与调参.md)） | 同左 + 认证测试项（[experience.md](experience.md) §5） |
| capture | usbmon / USBPcap + Wireshark（[capture/UVC流建立帧分析.md](capture/UVC流建立帧分析.md)） | 硬件分析仪（Ellisys / Beagle USB 480）抓同步流与反馈端点 |

---

## 4. 目录结构

```
labs/lab4-uvc-uac/
├── README.md                     ← 本文件：路线图与方案分级
├── hardware/
│   ├── BOM.csv                   ← 教学级/商业级两栏物料对照（含价格区间）
│   └── 设计要点.md               ← 并行口/MIPI 布线、I2S 布局、散热、带宽预算计算
├── firmware/
│   ├── README.md                 ← MCU 方案构建 + SoC/Linux uvc-gadget 框架说明
│   └── src/
│       ├── uvc_descriptors.c     ← VC/VS 接口描述符 + Probe/Commit 结构体
│       ├── uac_mic_descriptors.c ← UAC1 麦克风描述符 + Feature Unit
│       └── mjpeg_frame_fill.c    ← MJPEG 载荷填充/分帧伪代码（含教学级帧率预期）
├── host/
│   └── 预览与调参.md             ← v4l2-ctl/ffmpeg/Windows 相机；arecord/audacity
├── capture/
│   └── UVC流建立帧分析.md        ← 换挡→PROBE→COMMIT→同步流（FID）+ UAC 反馈 10.14
└── experience.md                 ← 踩坑：DHT 缺失、Hub 带宽、隐私开关、时钟、认证成本
```

---

## 5. 快速开始

> **推荐顺序：先跑通"现成 Linux uvc-gadget 演示"建立全局感，再进入 MCU 描述符细节，最后主机侧调参与抓包。**

### 路线 A（推荐，零焊接）：Linux uvc-gadget 演示

在一台 Linux 单板机（树莓派 Zero 2 W / Luckfox Pico / LicheePi Zero 等，需 USB device 口）上把设备模拟成 UVC 摄像头，PC 作主机验证：

```bash
# 设备端（Linux ≥ 5.x，内核需 CONFIG_USB_CONFIGFS_F_UVC）
# 1) 用 configfs 组一个 UVC gadget（内核文档 gadget-testing §UVC 有完整 shell 示例）
#    https://docs.kernel.org/usb/gadget-testing.html
# 2) 用户态跑 uvc-gadget，把本地视频源（vivid 虚拟设备或真实 sensor）桥接到 gadget：
#    https://gitlab.freedesktop.org/camera/uvc-gadget
# 3) USB 线连接 PC
# PC 端（主机）
v4l2-ctl --list-devices        # 应出现新的 UVC 摄像头
cheese                          # 或任意相机应用直接出图
```

### 路线 B：MCU 教学固件（本 Lab 提供骨架代码）

硬件按 [hardware/BOM.csv](hardware/BOM.csv) 教学级栏搭建，构建说明见 [firmware/README.md](firmware/README.md)。验收预期（诚实标注）：**640×480@30fps MJPEG 稳定，720p 约 15–25fps，麦克风 16k/48k 16bit 单声道**——这是 USB 2.0 单事务同步端点的物理上限，不是代码 bug。

### 路线 C：纯主机侧实验

任购一颗 UVC 摄像头（¥30–100）+ 带 UAC 麦克风的耳机，直接按 [host/预览与调参.md](host/预览与调参.md) 与 [capture/UVC流建立帧分析.md](capture/UVC流建立帧分析.md) 做调参与抓包，配合 `lsusb -v` 反读描述符，与本 Lab firmware/src/ 的代码逐字段对照。

---

## 6. 完成标志（验收清单）

- [ ] 能说清 UVC 复合设备的接口拓扑：IAD → VC 接口（实体图）+ VS 接口（Alt0/Alt1 换挡）+ UAC1 AC/AS 接口；
- [ ] 能手写/改写 VS MJPEG 格式与帧描述符，并解释 `dwFrameInterval=333333` 与 30fps 的关系；
- [ ] 能计算 720p30 MJPEG 的带宽预算并正确填写 HS 同步端点 `wMaxPacketSize`（含多事务编码）；
- [ ] 用 Wireshark/usbmon 抓到一次真实的 SET_INTERFACE→PROBE→COMMIT→IN 流序列，并按 FID 分帧；
- [ ] 能解读 UAC 反馈端点的 10.14 格式数值（或解释录音方向为何不需要反馈端点）；
- [ ] 在 Linux 上用 v4l2-ctl 修改曝光/白平衡并落到对应 UVC 实体控制上。

---

## 7. 知识库链接（USBTree）

- 视频：[00-UVC详解](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/Video-UVC/00-UVC详解.md) · [01-UVC规范级-控制与格式全表](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/Video-UVC/01-UVC规范级-控制与格式全表.md)
- 音频：[00-UAC概述](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/Audio-UAC/00-UAC概述.md) · [01-UAC1.0详解](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/Audio-UAC/01-UAC1.0详解.md) · [02-UAC2与UAC3](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/Audio-UAC/02-UAC2与UAC3.md) · [03-UAC规范级-实体与请求全表](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/Audio-UAC/03-UAC规范级-实体与请求全表.md)
- 前置：树干[四种传输类型](https://github.com/tangjianfang/USBTree/blob/main/10-树干-USB核心/06-四种传输类型.md)（同步传输带宽预算）、[描述符详解/IAD](https://github.com/tangjianfang/USBTree/blob/main/10-树干-USB核心/07-描述符详解.md)；同系列 Lab1（HID 复合设备）已练过 IAD 复合描述符。

---

## 参考资源

- Linux UVC driver & tools（uvcvideo 官方站，设备兼容列表 + FAQ）: https://www.ideasonboard.org/uvc/
- Linux 内核 USB gadget 测试文档（UVC/UAC1/UAC2 gadget configfs 完整示例）: https://docs.kernel.org/usb/gadget-testing.html
- uvc-gadget（freedesktop 官方用户态库+示例）: https://gitlab.freedesktop.org/camera/uvc-gadget
- Microsoft Learn：USB Video Class Driver Overview（Windows 内置 Usbvideo.sys 的能力矩阵）: https://learn.microsoft.com/en-us/windows-hardware/drivers/stream/usb-video-class-driver-overview
- USB-IF 官方文档库（UVC/UAC 类规范原文下载入口）: https://www.usb.org/documents
- TinyUSB（开源 USB 设备栈，UAC1/UAC2 官方示例）: https://github.com/hathach/tinyusb
- Espressif usb_device_uvc 组件（ESP32-S2/S3 UVC 设备开源实现，支持同步/批量）：https://components.espressif.com/components/espressif/usb_device_uvc
- esp32-usb-uvc-experiments（社区 ESP32-S3 UVC 网络摄像头实验）: https://github.com/atomic14/esp32-usb-uvc-experiments
