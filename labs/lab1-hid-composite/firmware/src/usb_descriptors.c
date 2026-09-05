/*
 * SPDX-License-Identifier: MIT
 * USB-Labs Lab1 —— 全部 USB 描述符定义（逐字段注释）
 *
 * 概念与字段权威定义：
 *   - USB 2.0 规范第 9 章（设备/配置/接口/端点/字符串描述符）
 *   - HID 1.11 类规范第 5/6 章（HID 描述符、报告描述符）
 *   - 知识库：10-树干-USB核心/07-描述符详解.md
 *             20-枝干-设备类协议/HID-人机接口设备/01-HID描述符.md
 *             20-枝干-设备类协议/HID-人机接口设备/02-报告描述符与Item编码.md
 *
 * 版本：TinyUSB 0.15.0（宏名以 src/class/hid/hid.h、device/usbd.h 为准）
 */
#include "pico/unique_id.h"
#include "tusb.h"
#include "usb_descriptors.h"

/* ==================================================================
 * 1) 设备描述符（18 字节）
 * ================================================================== */
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t), /* 18：描述符自身长度 */
    .bDescriptorType    = TUSB_DESC_DEVICE,           /* 0x01：设备描述符 */
    .bcdUSB             = 0x0200,                     /* USB 2.0（FS 设备；不声明 0x0210+ 以免主机索要 BOS 描述符） */
    .bDeviceClass       = 0x00,   /* 类信息放在接口级（0x03 HID）。HID 复合设备不需要 0xEF/MCD（那是为了 IAD） */
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,     /* EP0 最大包 64 字节（FS 允许 8/16/32/64） */
    .idVendor           = USB_VID,                    /* 演示占位！量产必须换（见 usb_descriptors.h 说明） */
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,                     /* 设备版本 1.00：固件升级时递增，便于主机区分 */
    .iManufacturer      = USB_STRING_IDX_MANUFACTURER,
    .iProduct           = USB_STRING_IDX_PRODUCT,
    .iSerialNumber      = USB_STRING_IDX_SERIAL,      /* 每台设备唯一：Windows 凭 VID+PID+Serial 区分设备实例 */
    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void) {
  return (uint8_t const *) &desc_device;
}

/* ==================================================================
 * 2) 三个接口各自的报告描述符（HID Item 流，主机经 GET_DESCRIPTOR(0x22) 索取）
 *    Item 编码原理见知识库 02-报告描述符与Item编码.md
 *    （TUD_HID_REPORT_DESC_* 宏内部即展开为下方注释中的 Item 序列）
 * ================================================================== */

/* 键盘（63 字节量级，总长以编译期 sizeof 为准）：
 *   REPORT_ID(1)
 *   UsagePage(GenericDesktop) Usage(Keyboard) Collection(Application)
 *     UsagePage(Keyboard) UsageMin(0xE0) UsageMax(0xE7)      ← 8 个修饰键位
 *     Logical 0..1, Size 1, Count 8, Input(Data,Var,Abs)     ← modifier 位图
 *     Size 8, Count 1, Input(Const,Arr,Abs)                  ← 保留字节
 *     Count 6, Logical 0..0x65, UsageMin(0) UsageMax(0x65)
 *     Input(Data,Array)                                      ← 6 个键码槽（数组语义）
 *     UsagePage(LEDs) UsageMin(1) UsageMax(5) Size 1 Count 5 Output(Data,Var)  ← NUM/CAPS/SCROLL… 灯
 *     Count 1 Size 3 Output(Const,Var)                       ← 补齐 1 字节
 *   EndCollection */
uint8_t const desc_hid_report_keyboard[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD))
};

/* 鼠标（约 60 余字节，以 sizeof 为准）：
 *   REPORT_ID(2) + UsagePage(GenericDesktop)/Usage(Mouse)/Application 集合
 *   内含 Physical 集合：3 按键位图 + 5 位填充、X/Y 相对位移(int8)、滚轮(int8) */
uint8_t const desc_hid_report_mouse[] = {
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(REPORT_ID_MOUSE))
};

/* 消费控制（约 20 字节量级）：
 *   REPORT_ID(3) + UsagePage(Consumer)/Usage(ConsumerControl)
 *   Logical 0..0x3FF, Size 16, Count 1, Input(Data,Var,Abs)
 *   → 上报为 2 字节小端 Usage 值（0xE9=音量+, 0xEA=音量-, 0xE2=静音, 0xCD=播放/暂停 …） */
uint8_t const desc_hid_report_consumer[] = {
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(REPORT_ID_CONSUMER))
};

/* TinyUSB 在处理 GET_DESCRIPTOR(Report) 时按"实例号"（= 接口序号 0/1/2）回调本函数 */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
  switch (instance) {
    case ITF_NUM_HID_KEYBOARD: return desc_hid_report_keyboard;
    case ITF_NUM_HID_MOUSE:    return desc_hid_report_mouse;
    case ITF_NUM_HID_CONSUMER: return desc_hid_report_consumer;
    default:                   return desc_hid_report_keyboard;
  }
}

/* ==================================================================
 * 3) 配置描述符（总长 84 字节 = 9 + 25*3，与 CONFIG_TOTAL_LEN 严格一致）
 *
 * 布局（括号内为长度）：
 *   [配置(9)]
 *   [接口0 键盘: 接口(9) + HID(9) + 端点(7)]
 *   [接口1 鼠标: 接口(9) + HID(9) + 端点(7)]
 *   [接口2 消费: 接口(9) + HID(9) + 端点(7)]
 *
 * 其中 HID 描述符（9 字节）字段：bLength=9, bDescriptorType=0x21,
 *   bcdHID=0x0111(类规范1.11), bCountryCode=0(不指定国家键盘布局),
 *   bNumDescriptors=1, bDescriptorType=0x22(报告),
 *   wDescriptorLength=各接口报告描述符长度
 * ================================================================== */
uint8_t const desc_configuration[] = {
    /* bConfigurationValue=1（SET_CONFIGURATION 的参数）；
     * bNumInterfaces=3；iConfiguration=0；
     * bmAttributes=0x80：bit7 保留必须为 1，bit6=0 表示总线供电；
     * bMaxPower=100mA（单位 2mA → 50）。必须与硬件实测一致，见 hardware/设计要点.md */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    /* 键盘接口：
     * bInterfaceClass=0x03(HID)；bInterfaceSubClass=1(支持 boot 协议，BIOS 可用)；
     * bInterfaceProtocol=HID_ITF_PROTOCOL_KEYBOARD(1)：告诉 boot 协议主机"这是键盘"；
     * bInterval=1 → 中断 IN 轮询周期 1ms = 1000Hz（bInterval 单位对 FS 是帧=1ms） */
    TUD_HID_DESCRIPTOR(ITF_NUM_HID_KEYBOARD, USB_STRING_IDX_ITF_KEYBOARD,
                       HID_ITF_PROTOCOL_KEYBOARD, sizeof(desc_hid_report_keyboard),
                       EPNUM_HID_KEYBOARD, CFG_TUD_HID_EP_BUFSIZE, 1),

    /* 鼠标接口：同样声明 boot 协议（协议=2），保证 BIOS 下鼠标可用 */
    TUD_HID_DESCRIPTOR(ITF_NUM_HID_MOUSE, USB_STRING_IDX_ITF_MOUSE,
                       HID_ITF_PROTOCOL_MOUSE, sizeof(desc_hid_report_mouse),
                       EPNUM_HID_MOUSE, CFG_TUD_HID_EP_BUFSIZE, 1),

    /* 消费控制接口：无 boot 协议语义（协议=0）。
     * boot 子类只定义了键盘/鼠标；消费控制本就只在操作系统层使用 */
    TUD_HID_DESCRIPTOR(ITF_NUM_HID_CONSUMER, USB_STRING_IDX_ITF_CONSUMER,
                       HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report_consumer),
                       EPNUM_HID_CONSUMER, CFG_TUD_HID_EP_BUFSIZE, 1),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
  /* 本设备只有一个配置；index 从 0 计 */
  (void) index;
  return desc_configuration;
}

/* ==================================================================
 * 4) 字符串描述符（UTF-16LE）
 * ================================================================== */

/* 序列号缓冲：Flash 唯一 ID 的十六进制串（16 字节 ID → 32 字符），
 * 由 usb_descriptors_init() 在启动时填充 */
static char serial_hex[2 * 8 + 1];

void usb_descriptors_init(void) {
  pico_get_unique_board_id_string(serial_hex, sizeof(serial_hex));
}

#define USB_STR_MANUFACTURER "USB-Labs"
#define USB_STR_PRODUCT      "USB-Labs HID Composite (Kbd+Mouse+Consumer)"

/* 出参缓冲：UTF-16 编码后的字符串描述符（首 2 字节为 bLength/bDescriptorType） */
static uint16_t desc_str[64];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void) langid; /* 只提供一种 LANGID，忽略主机偏好的其他语言 */
  const char *str = NULL;
  uint8_t len = 0;

  if (index == 0) {
    /* 索引 0：LANGID 列表（美式英语 0x0409） */
    desc_str[1] = 0x0409;
    len = 1;
  } else {
    switch (index) {
      case USB_STRING_IDX_MANUFACTURER: str = USB_STR_MANUFACTURER; break;
      case USB_STRING_IDX_PRODUCT:      str = USB_STR_PRODUCT;      break;
      case USB_STRING_IDX_SERIAL:       str = serial_hex;           break;
      case USB_STRING_IDX_ITF_KEYBOARD: str = "HID Keyboard";       break;
      case USB_STRING_IDX_ITF_MOUSE:    str = "HID Mouse";          break;
      case USB_STRING_IDX_ITF_CONSUMER: str = "HID Consumer Control"; break;
      default: return NULL; /* 未定义的索引返回 NULL → 对该请求 STALL */
    }
    /* ASCII → UTF-16LE（本 Lab 不含非 ASCII 字符） */
    while (str[len] != '\0' && len < 63) {
      desc_str[1 + len] = (uint16_t) str[len];
      len++;
    }
  }

  /* 首部：bDescriptorType=0x03(字符串) 在高 8 位，bLength(含首部 2 字节) 在低 8 位 */
  desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (uint16_t) (2 * len + 2));
  return desc_str;
}
