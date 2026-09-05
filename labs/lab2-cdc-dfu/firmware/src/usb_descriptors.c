/*
 * usb_descriptors.c —— 设备/配置/字符串描述符
 * USB-Labs Lab2：CDC 双接口（IAD 绑定）+ DFU Runtime 复合设备
 *
 * 布局（与 capture/CDC枚举与数据流.md 的重构帧序列一一对应）：
 *   配置描述符
 *   ├─ IAD (FirstInterface=0, Count=2, Class=0x02/0x02/0x01)   ← Union 的"官方"绑定方式
 *   ├─ 接口0 通信 (0x02/0x02/0x01) + Header/ACM/Union/CallMgmt + 中断IN 0x82
 *   ├─ 接口1 数据 (0x0A/0x00/0x00) + 批量 OUT 0x01 / 批量 IN 0x81
 *   └─ 接口2 DFU Runtime (0xFE/0x01/0x01) + DFU 功能描述符
 */
#include "tusb.h"

/* ------------------------------------------------------------------
 * VID/PID 注意：0xCAFE 是 TinyUSB 例程的占位 VID，禁止用于量产！
 * 量产必须申请自己的 VID（usb.org）或经许可使用社区 PID（pid.codes），
 * 否则与别人的驱动/INF 冲突，正是"COM 口漂移/装错驱动"的源头之一。
 * ------------------------------------------------------------------ */
#define USB_VID           0xCAFEu   /* TODO 量产前替换为你司 VID */
#define USB_PID           0x4010u   /* TODO 量产前替换为你司 PID */
#define USB_BCD           0x0200    /* USB 2.0 */

/* 接口编号（与 tusb_config.h 的断言联动） */
enum {
    ITF_NUM_CDC_CTRL = 0,   /* 通信接口 */
    ITF_NUM_CDC_DATA,       /* 数据接口 */
    ITF_NUM_DFU_RUNTIME,
    ITF_NUM_TOTAL
};

/* 接口名字符串索引（枚举续接：4、5） */
enum { STRID_UNUSED = 3, ITF_STR_CDC = 4, ITF_STR_DFU };

#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_DFU_RUNTIME_DESC_LEN)

#define EPNUM_CDC_NOTIF   0x82   /* 中断 IN */
#define EPNUM_CDC_OUT     0x01   /* 批量 OUT */
#define EPNUM_CDC_IN      0x81   /* 批量 IN */

/* DFU 功能描述符属性：可下载 | 可上传 | 会自行分离 | manifestation tolerant */
#define DFU_ATTRS (DFU_ATTR_CAN_DOWNLOAD | DFU_ATTR_CAN_UPLOAD | \
                   DFU_ATTR_WILL_DETACH | DFU_ATTR_MANIFESTATION_TOLERANT)

/* ---------------- 设备描述符 ---------------- */
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = 0xEF,   /* 复合设备 + IAD 的规范取值：Miscellaneous */
    .bDeviceSubClass    = 0x02,   /* Common */
    .bDeviceProtocol    = 0x01,   /* IAD (Interface Association Descriptor) */
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100, /* 产品版本 1.00 */
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,   /* 用芯片 UID 固定序列号 -> Windows COM 口不漂移（见 experience.md） */
    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

/* ---------------- 配置描述符（含全部接口/类描述符/端点） ---------------- */

uint8_t const desc_configuration[] = {
    /* 配置描述符：总接口数 ITF_NUM_TOTAL，总线供电 100mA */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    /* CDC：IAD + 通信接口(中断 IN 0x82, bInterval=2ms) + 数据接口(批量 64B)
     * 宏内部按规范依次展开 Header/ACM/Union/CallMgmt 四个功能描述符 */
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_CTRL, ITF_STR_CDC, EPNUM_CDC_NOTIF,
                       CFG_TUD_CDC_NOTIF_EPSIZE, EPNUM_CDC_OUT, EPNUM_CDC_IN,
                       CFG_TUD_CDC_EP_BUFSIZE),

    /* DFU Runtime 接口：Class 0xFE / SubClass 0x01 / Protocol 0x01（运行态）
     * wDetachTimeOut=1000ms, wTransferSize=1024, bcdDFU=0x0110 */
    TUD_DFU_RUNTIME_DESCRIPTOR(ITF_NUM_DFU_RUNTIME, ITF_STR_DFU, DFU_ATTRS,
                               1000, CFG_TUD_DFU_XFER_BUFSIZE)
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index; /* 只有一套配置 */
    return desc_configuration;
}

/* ---------------- 字符串描述符 ---------------- */

/* 序列号：取 STM32F103 出厂 96 位唯一 ID（基址 0x1FFFF7E8，见 RM0008）
 * -> 每台设备序列号唯一且不变，Windows 按 VID+PID+序列号 记住 COM 号，
 *    换 USB 口也不漂移（工程解释见 ../experience.md 第 1 节）。 */
static char serial_str[25]; /* 24 个 hex 字符 + NUL */

static void serial_str_init(void)
{
    if (serial_str[0] != 0) {
        return;
    }
    const volatile uint32_t *uid = (const volatile uint32_t *)0x1FFFF7E8u;
    char *p = serial_str;
    for (int i = 0; i < 3; i++) {
        for (int nib = 7; nib >= 0; nib--) {
            *p++ = "0123456789ABCDEF"[(uid[i] >> (nib * 4)) & 0xF];
        }
    }
    *p = 0;
}

static char const *string_desc_arr[] = {
    [0] = (const char[]){ 0x09, 0x04 },          /* 0: 语言 English (0x0409) */
    [1] = "USB-Labs",                            /* 1: 厂商 */
    [2] = "USB-Labs CDC-DFU Lab2",               /* 2: 产品 */
    [3] = NULL,                                  /* 3: 序列号（动态生成，见下） */
    [ITF_STR_CDC] = "USB-Labs CDC",              /* 4: CDC 接口名 */
    [ITF_STR_DFU] = "USB-Labs DFU",              /* 5: DFU 接口名 */
};

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid; /* 单语言实现 */

    if (index == 0) {
        /* 语言 ID 描述符 */
        static uint16_t lang_desc = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * 1));
        return &lang_desc;
    }

    if (index == 3) {
        /* 动态序列号（UTF-16） */
        static uint16_t wbuf[1 + 24];
        serial_str_init();
        for (int i = 0; i < 24; i++) {
            wbuf[1 + i] = (uint16_t)serial_str[i];
        }
        wbuf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * (1 + 24)));
        return wbuf;
    }

    size_t n = sizeof(string_desc_arr) / sizeof(string_desc_arr[0]);
    if (index >= n || string_desc_arr[index] == NULL) {
        return NULL; /* 返回 NULL -> 端点 0 STALL：该字符串不存在 */
    }

    /* ASCII -> UTF-16 转换并拼字符串描述符头 */
    static uint16_t conv_buf[32];
    const char *s = string_desc_arr[index];
    uint8_t len = 0;
    while (s[len] != 0 && len < 31) {
        conv_buf[1 + len] = (uint16_t)s[len];
        len++;
    }
    conv_buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * (1 + len)));
    return conv_buf;
}
