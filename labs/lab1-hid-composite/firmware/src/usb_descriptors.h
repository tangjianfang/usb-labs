/*
 * SPDX-License-Identifier: MIT
 * USB-Labs Lab1 —— 描述符常量与接口编号定义
 *
 * 平台：RP2040 + TinyUSB 0.15.0（pico-sdk 1.5.1 内置）
 * 概念：知识库 10-树干-USB核心/07-描述符详解.md
 *       知识库 20-枝干-设备类协议/HID-人机接口设备/01-HID描述符.md
 */
#ifndef _USB_DESCRIPTORS_H_
#define _USB_DESCRIPTORS_H_

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * VID/PID —— 商业量产注意
 * 本 Lab 使用演示用占位 VID/PID（未在 USB-IF 注册）。
 * 量产必须二选一：
 *   1) 以 USB-IF 会员身份注册自己的 VID（年费制），PID 自行分配；
 *   2) 从有 VID 的方案商/客户处获得 PID 子授权并写入合同。
 * 严禁冒用任何已注册厂商的 VID/PID（会造成驱动误绑定与合规风险）。
 * USB-IF：https://www.usb.org/getting-vendor-id
 * ------------------------------------------------------------------ */
#define USB_VID 0x1234u /* 演示占位，量产必须更换 */
#define USB_PID 0x0002u /* 演示占位，量产必须更换 */

/* 字符串描述符索引 */
#define USB_STRING_IDX_MANUFACTURER 1u
#define USB_STRING_IDX_PRODUCT      2u
#define USB_STRING_IDX_SERIAL       3u
#define USB_STRING_IDX_ITF_KEYBOARD 4u
#define USB_STRING_IDX_ITF_MOUSE    5u
#define USB_STRING_IDX_ITF_CONSUMER 6u

/* 接口号：三个 HID 接口各自独立、顺序编号。
 * 注意：HID 不使用 IAD（那是 CDC/UVC 等复合类把多个接口捆成一个功能的机制），
 * HID 复合设备就是"多个接口、bInterfaceClass 都是 0x03"而已。 */
enum {
  ITF_NUM_HID_KEYBOARD = 0, /* Interface 0：键盘 */
  ITF_NUM_HID_MOUSE,        /* Interface 1：鼠标 */
  ITF_NUM_HID_CONSUMER,     /* Interface 2：消费控制（音量/播放等） */
  ITF_NUM_TOTAL             /* 配置描述符 bNumInterfaces = 3 */
};

/* Report ID（每个接口有独立的报告描述符，ID 在各自接口内是独立命名空间；
 * 仍用不同 ID 是为了主机侧 hidapi 能用第一字节直接区分报告类型，
 * 也与 TinyUSB 官方 hid_composite 例程的编号习惯一致） */
#define REPORT_ID_KEYBOARD 0x01u
#define REPORT_ID_MOUSE    0x02u
#define REPORT_ID_CONSUMER 0x03u

/* 中断 IN 端点：IN 方向端点地址最高位为 1（0x8x） */
#define EPNUM_HID_KEYBOARD 0x81u
#define EPNUM_HID_MOUSE    0x82u
#define EPNUM_HID_CONSUMER 0x83u

/* 配置描述符总长（字节）：
 *   配置描述符     9
 *   键盘接口组     25  = 接口描述符 9 + HID 描述符 9 + 端点描述符 7
 *   鼠标接口组     25
 *   消费控制接口组 25
 *   合计           9 + 25*3 = 84 = 0x0054
 * wTotalLength 必须与此一致，算错是"设备无法启动(代码10)"类售后问题的常见根因。 */
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN * 3)

/* 在 main() 中调用：用板载 Flash 唯一 ID 填充序列号字符串缓冲 */
void usb_descriptors_init(void);

#ifdef __cplusplus
}
#endif

#endif /* _USB_DESCRIPTORS_H_ */
