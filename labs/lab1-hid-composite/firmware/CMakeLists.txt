# SPDX-License-Identifier: MIT
# USB-Labs Lab1 —— RP2040 + TinyUSB 设备固件构建脚本
# 依赖：pico-sdk v1.5.1+（其中 lib/tinyusb 子模块必须已初始化：
#       cd $PICO_SDK_PATH && git submodule update --init）
# 若此脚本与你的 SDK/TinyUSB 版本组合不兼容（CMake 变量随版本有差异），
# 请改用 TinyUSB 官方 examples 的 family.cmake 构建方式，以官方仓库为准。

cmake_minimum_required(VERSION 3.13)

# 标准 pico-sdk 引导（见 firmware/pico_sdk_import.cmake）
include(pico_sdk_import.cmake)

project(lab1_hid_composite C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# 初始化 pico-sdk（会做工具链探测）
pico_sdk_init()

# ---------------- TinyUSB ----------------
# 复用 pico-sdk 内置的 TinyUSB 源码树。
# FAMILY/BOARD 是 TinyUSB CMake 体系的环境变量：
#   FAMILY=rp2040 选择设备控制器端口（src/portable/raspberrypi/rp2040）
#   BOARD 只影响其 bsp 的板级初始化例程（本项目自管 GPIO，可任选官方 RP2040 板名）
set(FAMILY rp2040 CACHE STRING "TinyUSB family")
set(BOARD raspberrypi-pico CACHE STRING "TinyUSB board")
add_subdirectory(${PICO_SDK_PATH}/lib/tinyusb tinyusb_build)

# ---------------- 应用固件 ----------------
add_executable(lab1_hid_composite
    src/main.c
    src/usb_descriptors.c
    src/hid_app.c
)

target_include_directories(lab1_hid_composite PRIVATE src)

target_link_libraries(lab1_hid_composite PRIVATE
    pico_stdlib        # GPIO/时间函数（sleep_until 等）
    pico_unique_id     # 读取 Flash 唯一 ID 作为 USB 序列号
    tinyusb_dev        # TinyUSB 设备栈
)

# TinyUSB 会按 tusb_config.h 编译；这里显式声明其位置
target_include_directories(lab1_hid_composite PRIVATE
    ${PICO_SDK_PATH}/lib/tinyusb/src
)

# 本固件不用 stdio 占用 USB/UART（调试按需打开 uart stdio）
pico_enable_stdio_usb(lab1_hid_composite 0)
pico_enable_stdio_uart(lab1_hid_composite 0)

# 生成 .uf2（BOOTSEL 拖拽烧录用）等附加产物
pico_add_extra_outputs(lab1_hid_composite)
