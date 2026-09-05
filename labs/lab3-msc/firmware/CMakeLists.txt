# Lab3 MSC 固件 —— RP2040 + TinyUSB
# 依赖 pico-sdk（含 TinyUSB 子模块），构建说明见同目录 README.md
cmake_minimum_required(VERSION 3.13)

# pico_sdk_import.cmake 从 pico-examples 复制到本目录（见 README.md 第 2 节）
include(pico_sdk_import.cmake)

project(lab3_msc C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(lab3_msc
  src/main.c
  src/usb_descriptors.c
  src/msc_app.c
  src/sd_spi.c        # RAM 盘模式也编译，不占运行路径
)

target_include_directories(lab3_msc PRIVATE src)

# tinyusb_device: TinyUSB 设备栈；tinyusb_board: RP2040 BSP（board_api/引脚定义）
# 注意：CFG_TUSB_MCU=OPT_MCU_RP2040 等由 pico-sdk 的 tinyusb 集成自动注入
target_link_libraries(lab3_msc PRIVATE
  pico_stdlib
  pico_unique_id
  hardware_spi
  hardware_flash
  tinyusb_device
  tinyusb_board
)

# 可选配置透传（cmake -DMSC_USE_SD_CARD=1 ...）：
if(MSC_USE_SD_CARD)
  target_compile_definitions(lab3_msc PRIVATE MSC_USE_SD_CARD=1)
endif()
if(RAMDISK_BLOCK_COUNT)
  target_compile_definitions(lab3_msc PRIVATE RAMDISK_BLOCK_COUNT=${RAMDISK_BLOCK_COUNT})
endif()
if(MSC_EXPORT_READONLY)
  target_compile_definitions(lab3_msc PRIVATE MSC_EXPORT_READONLY=1)
endif()

# 关键：stdio 走 UART，绝不能开 stdio_usb —— 它会和 TinyUSB 抢 USB 控制器
pico_enable_stdio_usb(lab3_msc 0)
pico_enable_stdio_uart(lab3_msc 1)

pico_add_extra_outputs(lab3_msc)   # 生成 .uf2/.hex/.bin/.dis
