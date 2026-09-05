/*
 * SPDX-License-Identifier: MIT
 * USB-Labs Lab1 —— TinyUSB 配置头
 *
 * 本文件只描述"本设备用到哪些 TinyUSB 能力"。
 * 完整可配置项见 TinyUSB 源码 src/tusb_option.h 与官方文档，
 * 不同版本变量名可能调整（如 0.14 的 CFG_TUSB_RHPORT0_MODE 在 0.15 改为 CFG_TUD_ENABLED），
 * 以所用版本 src/tusb_option.h 为准。
 */
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- MCU / OS 适配 ---------------- */
/* RP2040 设备控制器（含 USB DPRAM 双端口 RAM 架构）。
 * TinyUSB 的构建系统（CMake）通常会在命令行注入 CFG_TUSB_MCU，
 * 这里兜底定义，二选一生效即可。 */
#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU OPT_MCU_RP2040
#endif

/* 使用 pico-sdk 作为 OS 适配层（互斥锁/延时由 SDK 提供） */
#define CFG_TUSB_OS OPT_OS_PICO

/* TinyUSB 动态内存：本例全部静态缓冲，无需特殊段/对齐调整 */
#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN TU_ATTR_ALIGNED(4)
#endif

/* ---------------- Device（设备侧） ---------------- */
/* 启用设备栈（0.15+ 写法；0.14 用 CFG_TUSB_RHPORT0_MODE | OPT_MODE_DEVICE） */
#define CFG_TUD_ENABLED 1

/* EP0 控制传输最大包：FS 允许 8/16/32/64，取上限 64 加快描述符传输 */
#define CFG_TUD_ENDPOINT0_SIZE 64

/* 本设备只用 HID 类，三个接口 → CFG_TUD_HID = 3（同一驱动实例计数） */
#define CFG_TUD_HID 3

/* 每个 HID 中断端点的缓冲大小（字节）。
 * 最大报告 = 键盘 9 字节（Report ID + modifier + reserved + 6 键码），
 * 取 16 留余量（也等于 FS 中断端点合法的 wMaxPacketSize）。 */
#define CFG_TUD_HID_EP_BUFSIZE 16

/* RP2040 专属：设备控制器中断优先级沿用 SDK 默认即可；
 * 如需调整 USB IRQ 相关配置，参考 TinyUSB src/portable/raspberrypi/rp2040/ 目录与
 * hw/bsp/rp2040/family.c（以官方源码为准）。 */

/* ---------------- Host 侧未使用 ---------------- */
/* 本 Lab 不做主机/双角色（OTG），全部 HOST 配置留默认 0；
 * 需要时参考 lab6（2.4G/BLE 接收器）与 TinyUSB host 例程。 */

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
