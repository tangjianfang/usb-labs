# Lab3 固件：TinyUSB MSC（RP2040 + TF 卡）

RP2040 + TinyUSB 设备栈，对外呈现一个 USB 大容量存储设备（Mass Storage Class，接口签名 `08h/06h/50h`）。
块设备后端二选一，由编译开关切换：

| 后端 | 编译开关 | 容量上报 | 用途 |
|---|---|---|---|
| **Flash RAM 盘（默认）** | 不定义 `MSC_USE_SD_CARD` | `RAMDISK_BLOCK_COUNT`（默认 1024 扇区 = 512 KB） | 零硬件依赖起步，先跑通 BOT/SCSI 全链路 |
| **SPI TF 卡（真盘）** | `-DMSC_USE_SD_CARD=1` | 从卡 CSD 寄存器解析真实扇区数 | 接上 hardware/ 的卡座后的量产形态 |

## 1. 目录结构

```
firmware/
├── CMakeLists.txt
└── src/
    ├── main.c              # 入口：初始化、热插拔监测、主循环（tud_task）
    ├── tusb_config.h       # TinyUSB 配置（FS 12Mbps、MSC 缓冲 2×512B）
    ├── usb_descriptors.c   # 设备/配置/字符串描述符（08h/06h/50h 接口）
    ├── msc_app.c           # TinyUSB MSC 回调 + 块设备层（RAM盘缓存策略 / SD 对接）
    ├── sd_spi.h            # SPI 模式 SD 卡驱动接口（引脚可在编译期覆盖）
    └── sd_spi.c            # SPI 模式 SD 卡驱动（CMD0/8/41/58/9/16/17/24）
```

## 2. 环境准备

- **pico-sdk ≥ 1.5.0**（含 TinyUSB 子模块，clone 时要 `--recursive`，或进 `pico-sdk/` 后 `git submodule update --init`）
- CMake ≥ 3.13、ARM GNU 工具链（`arm-none-eabi-gcc`，Windows 推荐 MSYS2 `pacman -S mingw-w64-x86_64-arm-none-eabi-gcc` 或 VS Code 树莓派官方插件）
- 把 pico-examples 里的 [`pico_sdk_import.cmake`](https://github.com/raspberrypi/pico-examples/blob/master/pico_sdk_import.cmake) 复制到 `firmware/`（sdk 导入样板，遵循其 BSD-3 许可，不入库）

> **关键**：本工程用 USB 跑 MSC，**必须使用 UART 看日志**（GPIO0 TX / GPIO1 RX，115200）。
> `CMakeLists.txt` 已执行 `pico_enable_stdio_usb(... 0)`——如果同时开 `stdio_usb`，pico-sdk 和 TinyUSB 会争抢同一个 USB 控制器，MSC 无法枚举。

## 3. 编译与烧录

```bash
cd labs/lab3-msc/firmware
# RAM 盘（默认，无需卡）
cmake -S . -B build -DPICO_SDK_PATH=~/pico-sdk -DPICO_BOARD=pico
cmake --build build -j

# SPI TF 卡真盘
cmake -S . -B build-sd -DPICO_SDK_PATH=~/pico-sdk -DPICO_BOARD=pico -DMSC_USE_SD_CARD=1
cmake --build build-sd -j
```

烧录：按住板上 BOOTSEL 键插 USB → 出现 `RPI-RP2` 盘 → 拖入 `build/lab3_msc.uf2` → 设备自动重启并枚举为 U 盘。

## 4. 首次上电预期现象

1. **RAM 盘版**：主机识别出新磁盘但提示"未格式化/需要格式化"（Flash 初态全 0xFF）。
   - Linux：`sudo mkfs.vfat -F16 /dev/sdX`；
   - Windows：小于 33 MB 的盘 Windows 不提供 FAT32 格式化（其 FAT32 最小簇数限制），512 KB RAM 盘请用 Linux 格式化，或把 `RAMDISK_BLOCK_COUNT` 调大后用 `format /FS:FAT`（FAT12/16）。
2. **SD 卡版**：直接主机可见真实容量；插拔卡 `TEST UNIT READY` 返回 NOT READY/3Ah（Medium Not Present），主机端表现为盘暂时不可用——这正是读卡器行为。

## 5. 编译期配置项

| 宏 | 默认 | 说明 |
|---|---|---|
| `MSC_USE_SD_CARD` | 未定义=RAM 盘 | =1 时启用 SPI SD 后端 |
| `RAMDISK_BLOCK_COUNT` | 1024（512 KB） | RAM 盘扇区数；≥4096 建议在 4 MB Flash 板上使用 |
| `SD_SPI_PORT/_SCK/_MOSI/_MISO/_CS_PIN` | spi1 / 10/11/12/13 | SPI 引脚 |
| `SD_CD_PIN` / `SD_CD_ACTIVE_LOW` | 9 / 1 | 卡检测脚及极性（按卡座实测调整） |
| `MSC_EXPORT_READONLY` | 未定义 | **记录仪导出模式**：置 1 后设备只读（is_writable=false），见 msc_app.c |

## 6. 代码导读顺序（与知识库对照）

1. `usb_descriptors.c`：枚举时主机看到的"你是谁" → USBTree《MSC 概述与 BOT》接口签名、《描述符详解》
2. `capture/BOT读扇区帧分析.md`：跑起来之后总线上的字节长什么样 → USBTree《四种传输类型》批量传输
3. `msc_app.c`：CBW/CSW 如何落在 5 个回调上、INQUIRY/READ10 的对接点、预读与写缓存策略 → USBTree《SCSI 命令与 UFI》《Sense 全表》
4. `sd_spi.c`：块设备层的另一半（SD 规范侧）

## 参考资源

- TinyUSB 官方 MSC 示例（本工程回调骨架的出处）— https://github.com/hathach/tinyusb/tree/master/examples/device
- pico-sdk — https://github.com/raspberrypi/pico-sdk ；pico-examples flash_program 示例 — https://github.com/raspberrypi/pico-examples/tree/master/flash/program
- RP2040 Datasheet（USB 控制器/SPI/PIO 章节）— https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf
- SD Physical Layer Simplified Spec（命令/响应/CSD 字段）— https://www.sdcard.org/downloads/pls/
- USBTree 知识库 — https://github.com/tangjianfang/USBTree/tree/main/20-枝干-设备类协议/MSC-大容量存储
