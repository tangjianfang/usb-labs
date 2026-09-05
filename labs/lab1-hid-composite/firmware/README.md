# Lab1 固件：HID 复合设备（RP2040 + TinyUSB）

## 1. 版本基准

| 组件 | 版本 | 说明 |
|---|---|---|
| pico-sdk | 建议 v1.5.1 或 v2.x | v1.5.x 内置 TinyUSB 0.15.0，v2.x 内置 TinyUSB 0.17+；**API 细节以你实际拉取的官方 SDK/TinyUSB 为准** |
| TinyUSB | 0.15.0（随 pico-sdk 内置） | 0.15 起 `tusb_init()` 带参数（`tusb_rhport_init_t`），0.14 及更早为 `tusb_init(void)`，`main.c` 中已标注 |
| 工具链 | arm-none-eabi-gcc 12+ 或 pico-sdk 指定版本 | Windows 建议 VS Code + "Raspberry Pi Pico" 官方扩展 |

## 2. 目录说明

```
firmware/
├── CMakeLists.txt            # 工程构建（复用 pico-sdk 内置 TinyUSB）
├── pico_sdk_import.cmake     # pico-sdk 官方引导脚本（复制自 SDK external/ 目录）
└── src/
    ├── main.c                # 入口：硬件初始化、1kHz 主循环 tud_task、按键演示
    ├── tusb_config.h         # TinyUSB 配置（MCU/OS、接口数、EP 缓冲大小）
    ├── usb_descriptors.h     # VID/PID、接口号、Report ID、EP 编号定义
    ├── usb_descriptors.c     # 设备/配置/HID/报告/字符串描述符（逐字段注释）
    └── hid_app.c             # HID 类回调：GET/SET_REPORT、boot 协议切换、LED
```

## 3. 构建

```bash
# 1) 获取 pico-sdk（注意 tinyusb 是其 git submodule，必须拉全）
git clone -b 1.5.1 https://github.com/raspberrypi/pico-sdk.git
cd pico-sdk && git submodule update --init && cd ..
export PICO_SDK_PATH=$(pwd)/pico-sdk

# 2) 编译
cd labs/lab1-hid-composite/firmware
mkdir build && cd build
cmake ..
make -j$(nproc)          # Windows 下: cmake --build . -j
```

产物：`build/lab1_hid_composite.uf2`

> 若 `add_subdirectory(pico-sdk/lib/tinyusb)` 在某个 SDK/TinyUSB 版本组合上报错（CMake 变量随版本有差异），
> 请改用 TinyUSB 官方 examples 的 family.cmake 构建方式，以 https://github.com/hathach/tinyusb 官方仓库说明为准。

## 4. 烧录

- **开发/小批量**：按住板上 BOOTSEL（SW1）插 USB → 电脑出现名为 "RPI-RP2" 的 U 盘 → 把 `.uf2` 拖进去，
  设备自动重启并枚举为 HID 复合设备。
- **量产**：见 `../experience.md` 第 6 节（SWD 治具 + picotool / openocd，或出厂预烧）。
  `picotool`：https://github.com/raspberrypi/picotool

## 5. 固件行为（演示逻辑）

| 输入 | 行为 |
|---|---|
| SW2（GP13）按一下 | 通过消费控制接口发送一次 播放/暂停（Usage 0xCD） |
| SW3（GP14）按住 | 鼠标接口以 1kHz 上报，光标走正方形轨迹（循环） |
| SW4（GP15）按一下 | 键盘接口逐字符键入 `usb-labs!`（含 Shift 组合） |
| 主机侧键盘 LED | 收到 SET_REPORT(OUTPUT) 后更新到板载 LED（如 Pico 板载 LED 反映 CapsLock） |

按键均为内部上拉、1ms 主循环内做 20ms 消抖；所有上报只在 `tud_ready()` 且上次报告发送完成后发出。

## 6. 与主机的协议约定（host/ 目录解析用）

| 接口 | Report ID | 报告格式（报告协议模式，含 1 字节 Report ID 前缀） |
|---|---|---|
| 键盘（ITF 0） | 0x01 | `[0x01][modifier][reserved][k1..k6]` 共 9 字节 |
| 鼠标（ITF 1） | 0x02 | `[0x02][buttons][dx][dy][wheel]` 共 5 字节 |
| 消费控制（ITF 2） | 0x03 | `[0x03][usage_lo][usage_hi]` 共 3 字节（16 位 Usage，小端） |

> boot 协议（BIOS 场景）下主机通过 SET_PROTOCOL 切换，TinyUSB 内部按 boot 兼容格式发送（无 Report ID），
> 实现细节以 TinyUSB `src/class/hid/hid_device.c` 为准。

## 参考资源

- TinyUSB（官方仓库）：https://github.com/hathach/tinyusb
- TinyUSB hid_composite 例程（本固件的参照模板）：https://github.com/hathach/tinyusb/tree/master/examples/device/hid_composite
- pico-sdk 入门文档：https://github.com/raspberrypi/pico-sdk
- 知识库：[实战-TinyUSB设备固件](https://github.com/tangjianfang/USBTree/blob/main/60-枝干-主机侧与实现/05-实战-TinyUSB设备固件.md)、
  [实战完整报告描述符](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/HID-人机接口设备/11-实战完整报告描述符.md)
