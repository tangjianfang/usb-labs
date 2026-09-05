# Lab3：MSC 大容量存储（U 盘 / 读卡器 / 记录仪导出模式）

> 系列定位：USB-Labs 第 3 个全栈实验。把一台 MCU 变成主机眼里的**块设备（Block Device）**：
> 主机 `lsblk`/`磁盘管理` 里出现一块盘，可以格式化、拷文件、安全弹出——全链路可解释、可抓包、可量产化改造。
>
> 知识库对照阅读：
> - [MSC 概述与 BOT 传输](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/MSC-大容量存储/00-MSC概述与BOT.md)
> - [SCSI 命令与 UFI](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/MSC-大容量存储/01-SCSI命令与UFI.md)
> - [SCSI 操作码与 Sense 全表](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/MSC-大容量存储/02-SCSI操作码与Sense全表.md)
> - [四种传输类型（批量 Bulk）](https://github.com/tangjianfang/USBTree/blob/main/10-树干-USB核心/06-四种传输类型.md)

---

## 1. 这个实验解决什么商业问题

| 商业形态 | 场景描述 | 本实验覆盖点 |
|---|---|---|
| **U 盘 / 读卡器** | 最经典的 USB 外设：主机把 TF 卡/闪存当本地盘读写 | MSC 描述符、BOT 状态机、读卡器热插拔（TEST UNIT READY + Sense 3Ah） |
| **行车记录仪导出模式** | 记录仪平时用 TF 卡循环录像；插上电脑 USB 口后**临时切换成 MSC 设备**，让 PC 直接拷视频 | "双主共享一张卡"的并发冲突、导出前 STOP 录制 + SYNCHRONIZE CACHE、只读导出策略（is_writable=false） |
| **打印机桥接存储** | 打印机把读卡槽/内部存储**桥接（Bridge）**给电脑（打印照片直读 U 盘的同源能力），即"USB 桥接读卡器" | 多 LUN（`tud_msc_get_maxlun_cb`）、单卡座单 LUN 起步、PREVENT/ALLOW MEDIUM REMOVAL |

一句话架构：**主机文件系统 → usb-storage/uas 驱动 → SCSI 命令（CDB）→ BOT 封装 → USB 批量端点 → 固件 TinyUSB MSC 回调 → 块设备层 → TF 卡（SPI/SDIO）或 RAM 盘**。

## 2. 全栈路线图（本目录文件）

| 层 | 文件 | 内容 |
|---|---|---|
| 硬件 | [hardware/BOM.csv](hardware/BOM.csv) | 主控、TF 卡座、ESD、电源、PCB 全料单（含价格区间） |
| 硬件 | [hardware/设计要点.md](hardware/设计要点.md) | 卡座布线/上拉、热插拔检测、浪涌与电源开关、ESD 阵列选型 |
| 固件 | [firmware/README.md](firmware/README.md) | pico-sdk + TinyUSB 构建说明 |
| 固件 | [firmware/src/](firmware/src/) | `main.c` / `tusb_config.h` / `usb_descriptors.c` / `msc_app.c` / `sd_spi.c|h`，RAM 盘起步 + SD 真盘对接 |
| 主机 | [host/msc_linux.md](host/msc_linux.md) | Linux 挂载（dmesg/lsblk）、**f3 鉴伪工具**、性能基准 |
| 抓包 | [capture/BOT读扇区帧分析.md](capture/BOT读扇区帧分析.md) | CBW(READ10)→数据→CSW 逐字段（规范重构）+ usbmon 抓真实包指引 |
| 经验 | [experience.md](experience.md) | FAT 掉电、安全弹出、假容量卡黑产、Windows 写缓存、UAS 兼容、NAND 寿命 |

## 3. 主控选型

| 主控 | USB | 存储接口 | 关键优势 | 注意点 | 芯片/板价格区间（参考，以行情为准） |
|---|---|---|---|---|---|
| **RP2040**（推荐起步） | USB 1.1 FS 12 Mbps | SPI（≤1 MB/s 级）/ **PIO 自制 4-bit SDIO（10 MB/s 级）** | 双核：核 0 跑 USB，核 1 预读/写回；PIO 不占 CPU 做媒体接口 | 仅 Full Speed，吞吐天花板 ~1.1 MB/s；Flash XIP 写入要停总线 | 芯片约 ¥8–15；树莓派 Pico 板约 ¥25–40 |
| **STM32F407/F411** | FS 12 Mbps（HS 需外挂 ULPI PHY，成本高） | **硬件 SDMMC 4-bit + DMA（20 MB/s 级）** | SDMMC 外设现成、CubeMX 生态 | HS USB 需 ULPI 芯片（如 USB3300，¥10–20）；FS 仍是瓶颈 | 芯片约 ¥12–30；F407 板约 ¥35–60 |
| **CH32V307** | **HS 480 Mbps 片上 PHY** | SDIO 4-bit | 全链路 480 Mbps 无瓶颈，MSC 真正能跑 20–30 MB/s | RISC-V 工具链（MounRiver/官方 GCC）；HS PCB 布线要求提高 | 芯片约 ¥10–20；官方 EVT 板约 ¥50–70 |

> 结论：**学习期 RP2040 + SPI SD 足够**（FS 端点带宽只有 ~1 MB/s，SPI SD 不是瓶颈，反而是"预读/写缓存"的最佳教学样本）；**产品化若要速度，CH32V307 的 HS + SDIO 性价比最高**；STM32F4 适合已有 ST 存量平台。PIO SDIO 方案参考社区成熟的 no-OS-FatFS（见参考资源）。

## 4. 存储介质选型（固件视角的"盘"）

| 方案 | 容量区间 | 接口 | 顺序读/写（典型） | 寿命管理 | 价格区间（参考） | 适用 |
|---|---|---|---|---|---|---|
| **TF 卡（microSD）** | 8 GB–1 TB | SPI ≤2 MB/s / 4-bit SDIO 10–25 MB/s | 由卡内部 FTL 决定 | 卡内 FTL 自带磨损均衡（黑盒） | 32 GB A1 约 ¥20–35；128 GB 约 ¥60–100 | 消费产品首选，可更换 |
| **SPI NAND（如 W25N01G 1 Gb）** | 128–512 MB | QSPI | 1–8 MB/s | **FTL 要自己写**（坏块表+磨损均衡），推荐用 littlefs 或现成 FTL | 芯片约 ¥5–10 | 固件盘、日志盘，不可换卡产品 |
| **eMMC（BGA 焊接）** | 8–64 GB | 4/8-bit MMC | 50–300 MB/s | 封装内 FTL + 可选 pSLC 模式 | 8 GB 约 ¥15–25；32 GB 约 ¥30–60 | 数据记录仪量产形态，BGA 工艺 |
| **SPI NOR（如 W25Q64）** | 1–16 MB | QSPI | 0.5–2 MB/s | 自己管理擦写循环 | 芯片约 ¥2–5 | 小容量配置/标定数据，不适合当 U 盘 |

> 本实验固件同时提供两种块设备后端：**Flash RAM 盘（默认，零硬件依赖）** 与 **SPI TF 卡**（`MSC_USE_SD_CARD=1` 编译开关）。

## 5. 层级 × 知识点矩阵

|  | 核心知识点 | 本实验落点 |
|---|---|---|
| 电气/硬件 | 卡座热插拔、浪涌、ESD | 设计要点 + BOM |
| 协议 | Bulk 批量传输、BOT 三段式（CBW/数据/CSW）、SCSI 最小命令集（INQUIRY/TEST UNIT READY/REQUEST SENSE/READ CAPACITY/READ10/WRITE10/SYNCHRONIZE CACHE） | capture 逐字段重构 |
| 枚举 | 接口描述符 `08h/06h/50h`、双 Bulk 端点 | usb_descriptors.c 注释 |
| 固件工程 | TinyUSB 回调与数据阶段拆包（512B/64B 包）、预读与写缓存、掉电 flush | msc_app.c 注释 |
| 主机 | 块设备 vs 字符设备（MSC **没有串口**！）、挂载、f3 鉴伪 | host/msc_linux.md |
| 商业经验 | 掉电损坏、假容量卡、UAS 兼容、保修现实 | experience.md |

## 6. 快速开始

```bash
# ① 固件（RAM 盘起步，不需要任何卡）
cd labs/lab3-msc/firmware
mkdir build && cd build
cmake .. -DPICO_SDK_PATH=/path/to/pico-sdk -DPICO_BOARD=pico
make -j            # 生成 lab3_msc.uf2
# 按住 BOOTSEL 插 USB → 拖入 lab3_msc.uf2 → 重新插拔

# ② 主机验证（PC 会提示"发现新磁盘/需要格式化"）
# Linux:
dmesg | tail          # usb-storage ... Direct-Access
lsblk -f              # 出现新块设备
sudo mkfs.vfat -F16 /dev/sdX   # RAM 盘用 FAT16（Windows 不给小盘格式化 FAT32，原因见 host 文档）

# ③ 看协议（可选）
sudo modprobe usbmon && wireshark   # 选 usbmonX，过滤 usb.transfer_type == 0x01
```

接真卡：焊好 TF 卡座（见 hardware/），`cmake .. -DMSC_USE_SD_CARD=1` 重新编译，固件自动用 CSD 上报真实容量（`READ CAPACITY(10)` 返回 `LAST_LBA = 扇区数 - 1`，这是最容易搞错的对接点）。

## 7. 做完本实验你能回答

1. 主机第一次读一个文件，固件最少要正确响应哪 4 条 SCSI 命令？
2. `dCBWDataTransferLength` 与 CDB 里的 Transfer Length 有什么区别？单位分别是什么？
3. 为什么"安全弹出"要发 `SYNCHRONIZE CACHE` + `START STOP UNIT`？
4. 一张 128 GB 的卡标价 ¥39，`f3probe` 会告诉你什么？
5. 记录仪导出模式切 USB 时，为什么必须先停录像再枚举？

（答案分别藏在 capture/、firmware/src/msc_app.c、experience.md 里。）

## 参考资源

- USB Implementers Forum: *Universal Serial Bus Mass Storage Class Bulk-Only Transport (BOT) Rev.1.0* — https://www.usb.org/document-library/mass-storage-bulk-only-10
- USB-IF: *Mass Storage Class Specification Overview* — https://www.usb.org/documents
- TinyUSB 官方仓库（本实验固件基准）— https://github.com/hathach/tinyusb
- 树莓派 Pico SDK — https://github.com/raspberrypi/pico-sdk ；pico-examples（flash 编程示例）— https://github.com/raspberrypi/pico-examples
- no-OS-FatFS（RP2040 PIO SDIO + FatFS 社区成熟方案）— https://github.com/carlk3/no-OS-FatFS-SimpleDemo
- f3（Fight Flash Fraud）— https://github.com/Digint/f3
- USBPcap（Windows 抓包）— https://github.com/desowin/usbpcap
- Wireshark 样例抓包库 — https://wiki.wireshark.org/SampleCaptures
- SD Association 简化规范（SD Physical Layer Simplified Specification）— https://www.sdcard.org/downloads/pls/
- 沁恒 CH32V307 — https://www.wch-ic.com/products/CH32V307.html
- USBTree 知识库 MSC 分枝 — https://github.com/tangjianfang/USBTree/tree/main/20-枝干-设备类协议/MSC-大容量存储
