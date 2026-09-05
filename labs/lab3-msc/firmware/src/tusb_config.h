/* Lab3 MSC —— TinyUSB 配置
 * 骨架与 TinyUSB 官方示例 examples/device/*/src/tusb_config.h 保持一致（MIT License），
 * 本工程差异点均已注释。
 */
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

/*------------------------- 板级 -------------------------*/
/* RP2040 只有 1 个 USB 控制器，设备模式固定用 RHPort 0 */
#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT      0
#endif

/* RP2040 只有 Full-Speed（12 Mbps）PHY */
#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED   OPT_MODE_FULL_SPEED
#endif

/*------------------------- 通用 -------------------------*/
/* 由 pico-sdk 的 tinyusb 集成注入：CFG_TUSB_MCU=OPT_MCU_RP2040, CFG_TUSB_OS=OPT_OS_PICO */
#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined (由 pico-sdk tinyusb_device 目标提供)
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS           OPT_OS_PICO
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG        0    /* 置 2 可打印 TinyUSB 内部日志（走 UART stdio） */
#endif

/* 使能设备栈（本实验不做 USB 主机） */
#define CFG_TUD_ENABLED       1

/* 实际运行速度 = 板级声明（FS 12Mbps） */
#define CFG_TUD_MAX_SPEED     BOARD_TUD_MAX_SPEED

/* RP2040 的 USB 模块可直接访问全部 SRAM，无需特殊 section；缓冲 4 字节对齐 */
#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN    __attribute__((aligned(4)))
#endif

/*------------------------- 设备 -------------------------*/
/* EP0 最大包长：Full-Speed 固定 64 */
#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE    64
#endif

/*------------- 设备类 -------------*/
#define CFG_TUD_CDC               0
#define CFG_TUD_MSC               1    /* 本实验唯一角色：大容量存储 */
#define CFG_TUD_HID               0
#define CFG_TUD_MIDI              0
#define CFG_TUD_VENDOR            0

/* MSC 数据阶段缓冲大小。
 * FS 批量端点单包最大 64B，但 TinyUSB 以此缓冲为单位搬运整段数据：
 *  - 512  = 一次装一个扇区（教学最小配置）
 *  - 1024 = 双扇区缓冲，配合 tud_msc_read10_cb 分段返回，减少回调往返、提升吞吐
 * 注意：sd_spi 驱动内部还有 512B 块缓冲，此值不需要等于 SD 卡页大小。 */
#define CFG_TUD_MSC_EP_BUFSIZE    1024

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
