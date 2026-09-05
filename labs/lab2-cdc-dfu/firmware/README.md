# Lab2 固件：CDC + DFU 复合设备（STM32F103 + TinyUSB）

> 概念背景：知识库 [虚拟串口 ACM](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/CDC-通信设备类/01-虚拟串口ACM.md) · [DFU 固件升级](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/其他设备类/01-DFU固件升级.md)
> 源码清单：[src/main.c](src/main.c)（调度与系统初始化）· [src/tusb_config.h](src/tusb_config.h)（TinyUSB 配置）· [src/usb_descriptors.c](src/usb_descriptors.c)（CDC 双接口+IAD+DFU Runtime 描述符）· [src/cdc_app.c](src/cdc_app.c)/[src/cdc_app.h](src/cdc_app.h)（环形缓冲+DTR 回调）· [src/bootloader选型与跳转设计.md](src/bootloader选型与跳转设计.md)（分区/跳转/方案对比）

## 1. 架构总览

```
┌─────────────────── STM32F103C8T6 (64KB Flash / 20KB RAM) ───────────────────┐
│                                                                             │
│  Flash 分区：                                                               │
│  0x08000000 ┌──────────────────────┐                                        │
│             │ Bootloader (12KB)    │ DFU 模式/跳转逻辑（本 Lab 用 ROM DFU，  │
│  0x08003000 ├──────────────────────┤ 自研方案见 bootloader选型与跳转设计.md）│
│             │ App (52KB) ← 本工程  │ TinyUSB 复合设备：                     │
│  0x0800FFFF └──────────────────────┘  Iface0 CDC 通信 + Iface1 CDC 数据       │
│                                       + Iface2 DFU Runtime                  │
│  RAM：主 SRAM 20KB；另划 .noinit 4 字节存放 DFU 复位魔法数（复位不清除）        │
└─────────────────────────────────────────────────────────────────────────────┘
```

运行态（App）对外呈现三个接口：CDC 的通信接口+数据接口（一对，由 IAD 绑定）加一个 DFU Runtime 接口（类码 0xFE/0x01/0x01）。主机 `dfu-util -e` 发 `DFU_DETACH` → 固件在 `tud_dfu_runtime_reboot_to_dfu_cb()` 里把魔法数写入 `.noinit` RAM 并复位 → 若使用自研 bootloader 则停在 DFU 模式重新枚举；本 Lab 的保底路径是 BOOT0 进 ST ROM DFU（见 host/dfu_guide.md）。

## 2. 构建说明

### 2.1 方案 A：STM32Cube + TinyUSB（本工程默认）

依赖：

1. **STM32CubeF1** HAL 库（CubeMX 生成工程，或直接用 [STM32CubeF1 仓库](https://github.com/STMicroelectronics/STM32CubeF1)）；
2. **TinyUSB 0.17+**（`git clone https://github.com/hathach/tinyusb.git`，把 `src/` 加入编译，`-DCFG_TUSB_MCU=OPT_MCU_STM32F1`）；
3. ARM GCC（`arm-none-eabi-gcc`，xpack 或 STM32CubeIDE 自带）。

工程组织（把本目录 `src/` 并入 CubeMX 工程）：

```
lab2_fw/
├── Core/Src/           # CubeMX 生成：main.c 的 SystemClock_Config 部分与本工程 src/main.c 合并
├── Middlewares/tinyusb/src/   # TinyUSB
├── src/                # 本目录四个源文件原样加入
├── stm32f1xx_hal_conf.h # 打开 HAL_PCD_MODULE（或用 TinyUSB dcd_port.c 直驱寄存器，则无需 HAL PCD）
└── CMakeLists.txt / .ioc
```

编译与烧录（**App 链接地址必须改到 0x08003000**，见第 3 节分区表）：

```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-gcc.cmake
cmake --build build
# 首次：SWD 烧录（ST-Link / J-Link / DAPLink）
st-flash write build/lab2_app.bin 0x08003000
# 之后：完全走 DFU，不再需要 SWD（见 ../host/dfu_guide.md）
```

TinyUSB 对 F1 的设备控制器实现（`dcd_dwc2`）直接操作 USB 寄存器，中断向量 `USB_HP_CAN1_TX_IRQHandler` / `USB_LP_CAN1_RX0_IRQHandler` 中调用 `tud_int_handler(0)`（已在 src/main.c 中给出）。

### 2.2 方案 B：CH32V203 生态（低成本替代平台）

- 工具链：MounRiver Studio（WCH 官方）或 `riscv-none-elf-gcc` + WCH-Link 调试器；
- USB 栈两条路：WCH 官方 USBFS 外设库的 CDC 例程（[ch32v20x EVT](https://www.wch.cn/products/CH32V203.html) 资料页下载），或 TinyUSB 社区移植（`OPT_MCU_CH32V203`，成熟度低于 F1，以 TinyUSB 仓库当前状态为准）；
- 描述符/应用层代码与本工程几乎一致（都走 TinyUSB 抽象层），差异集中在时钟初始化与 dcd 移植层。

### 2.3 方案 C：ESP32-S3 / RP2040 速览（换平台时看哪里不一样）

| 平台 | USB 栈 | 本工程代码可复用度 | DFU 差异 |
|---|---|---|---|
| ESP32-S3 | ESP-IDF 内置 TinyUSB | 描述符/应用层完全可复用 | 无 USB DFU：升级走 UART esptool 或 Wi-Fi OTA |
| RP2040 | Pico SDK 内置 TinyUSB | 描述符/应用层完全可复用 | 出厂 BOOTSEL 是 UF2/MSC 不是 DFU；要 DFU 需把 TinyUSB `examples/device/dfu` 移植进 bootloader |

## 3. 内存分区表（64KB Flash 示例）

| 分区 | 起始地址 | 大小 | 用途 |
|---|---|---|---|
| Bootloader | `0x08000000` | **12KB**（`0x08003000` 结束） | DFU 模式 USB 栈+跳转+校验（本 Lab 默认用 ST ROM DFU 时此区是 ST 代码，地址同构） |
| App（本工程） | `0x08003000` | 52KB | TinyUSB CDC+DFU Runtime 复合设备，含中断向量表 |
| （预留思路）A/B 双分区 | — | 64KB 放不下 | 52KB 单 App 已占满；真 A/B 需 128KB+ Flash 或外置 SPI Flash，讨论见 bootloader选型与跳转设计.md |

App 侧三处必须与分区表一致：

1. **链接脚本**：FLASH ORIGIN 改 `0x08003000`，LENGTH `52K`；RAM 保留 `.noinit` 段（放 DFU 魔法数）；
2. **中断向量表**：`VECT_TAB_OFFSET 0x3000`（`system_stm32f1xx.c`）或运行时 `SCB->VTOR = 0x08003000`；
3. **ROM DFU/自研 bootloader 烧写目标地址**：`0x08003000`（host/dfu_guide.md 的 `-s` 参数要与此一致）。

> 提示：12KB 的 bootloader 放"TinyUSB DFU + 校验 + 跳转"属于紧凑但可行的规模（以实际编译结果为准，超了优先精简日志与 CRC 表，其次扩到 16KB 并把 App 起点改为 `0x08004000`——分区表是全栈契约，改一处必须四处同步）。

## 4. 目录内文件速查

| 文件 | 职责 |
|---|---|
| [src/main.c](src/main.c) | 72MHz 时钟初始化、USB 中断向量、TinyUSB 调度循环、DFU 分离回调、心跳/命令演示 |
| [src/tusb_config.h](src/tusb_config.h) | TinyUSB 裁剪：CDC×1 + DFU Runtime×1、缓冲大小 |
| [src/usb_descriptors.c](src/usb_descriptors.c) | 设备/配置/字符串描述符：IAD + CDC 双接口 + DFU 功能描述符；序列号取自芯片 96 位 UID |
| [src/cdc_app.c](src/cdc_app.c) / [src/cdc_app.h](src/cdc_app.h) | TX/RX 环形缓冲、DTR 边沿回调、LINE_CODING 记录、落盘前的丢包策略 |
| [src/bootloader选型与跳转设计.md](src/bootloader选型与跳转设计.md) | 12KB 分区契约、App→BL 跳转时序、ROM DFU / TinyUSB DFU / MCUboot / Open Bootloader 方案对比 |

## 5. 验收自测

```bash
# 枚举
lsusb | grep 1209            # 或 Windows 设备管理器看 COM 口
# 串口功能
python ../host/serial_tool.py --auto    # 收到 1Hz 心跳行，ping→pong，ver→版本+UID
# 断线重连：拔 USB 再插，脚本自动重连不退出
# DFU
dfu-util -e                   # 设备分离；跳线 BOOT0 或 ROM DFU 场景见 ../host/dfu_guide.md
dfu-util -l                   # 确认 DFU 模式枚举（ROM: VID 0483 PID df11）
```

## 参考资源

- [TinyUSB 仓库与文档](https://github.com/hathach/tinyusb) / [docs.tinyusb.org](https://docs.tinyusb.org/)（`examples/device/cdc_msc`、`examples/device/dfu_runtime` 是本工程描述符代码的基准）
- [STM32CubeF1](https://github.com/STMicroelectronics/STM32CubeF1)（HAL 库）
- 知识库：[DFU 固件升级](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/其他设备类/01-DFU固件升级.md)（状态机/错误码/时序全集）· [虚拟串口 ACM](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/CDC-通信设备类/01-虚拟串口ACM.md)
- [MCUboot](https://docs.mcuboot.com/) · [dfu-util](https://dfu-util.sourceforge.net/)
- [WCH CH32V203 产品页（EVT/例程下载）](https://www.wch.cn/products/CH32V203.html)
