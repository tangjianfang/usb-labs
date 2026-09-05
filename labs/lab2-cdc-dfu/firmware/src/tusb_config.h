/*
 * tusb_config.h —— TinyUSB 设备栈裁剪配置
 * USB-Labs Lab2：CDC + DFU Runtime 复合设备（STM32F103 全速）
 *
 * CFG_TUSB_MCU 由构建系统传入：
 *   STM32F103 : -DCFG_TUSB_MCU=OPT_MCU_STM32F1
 *   CH32V203  : -DCFG_TUSB_MCU=OPT_MCU_CH32V203（社区移植，以仓库现状为准）
 */
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 通用 ---------------- */
#define CFG_TUSB_MCU            OPT_MCU_STM32F1
#define CFG_TUSB_OS             OPT_OS_NONE      /* 裸机：tud_task() 在 main 循环里跑 */
#define CFG_TUD_ENABLED         1

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN      __attribute__ ((aligned(4)))
#endif

/* ---------------- 设备（Device）栈 ---------------- */
#define CFG_TUD_ENDPOINT0_SIZE  64               /* 控制端点 0 包长：全速 64B 上限 */

/* 接口数量：CDC 占 2 个（通信 0 + 数据 1）+ DFU Runtime 占 1 个（接口 2） */
#define CFG_TUD_CDC             1
#define CFG_TUD_DFU_RUNTIME     1

/*
 * CDC 端点分配（与 usb_descriptors.c 保持一致）：
 *   EP0x01 OUT / EP0x81 IN  ：CDC 数据（批量，全速 64B）
 *   EP0x82 IN               ：CDC 通知（中断，串口状态 SERIAL_STATE）
 * EP 缓冲大小：全速上限 64；TinyUSB 内部再串一层软件 FIFO
 */
#define CFG_TUD_CDC_EP_BUFSIZE  64
#define CFG_TUD_CDC_RX_BUFSIZE  256              /* EP -> 应用 FIFO */
#define CFG_TUD_CDC_TX_BUFSIZE  256              /* 应用 -> EP FIFO */
#define CFG_TUD_CDC_NOTIF_EPSIZE 8               /* 中断 IN 端点包长（8~16 即可） */

/* DFU Runtime：运行态只响应 DFU_DETACH（0x00），数据搬运发生在 DFU 模式 bootloader */
#define CFG_TUD_DFU_XFER_BUFSIZE 1024            /* 与描述符 wTransferSize 一致 */

/* 断言：接口布局自检（1 个 CDC 功能 = 2 个接口；再加 1 个 DFU Runtime 接口 = 3）
 * 若增删接口，必须同步 usb_descriptors.c 的 ITF_NUM_* 与描述符长度 */
#if CFG_TUD_CDC != 1 || CFG_TUD_DFU_RUNTIME != 1
#error "本工程描述符按 1 个 CDC 功能 + 1 个 DFU Runtime 接口编写，改动需同步 usb_descriptors.c"
#endif

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
