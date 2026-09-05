/* Lab3 MSC —— USB 描述符
 *
 * 枚举期主机按以下顺序来取（对照 USBTree《07-描述符详解》《08-枚举流程与标准请求》）：
 *   GET_DESCRIPTOR(Device)        -> tud_descriptor_device_cb()
 *   SET_ADDRESS / SET_CONFIGURATION(1)
 *   GET_DESCRIPTOR(Config)        -> tud_descriptor_configuration_cb()
 *   GET_DESCRIPTOR(String)        -> tud_descriptor_string_cb()（可选）
 * 之后主机加载 usb-storage/uas 驱动，MSC 世界才开始（BOT 命令见 msc_app.c）。
 *
 * MSC 的关键签名藏在接口描述符里（TUD_MSC_DESCRIPTOR 展开）：
 *   bInterfaceClass    = 0x08  Mass Storage
 *   bInterfaceSubClass = 0x06  SCSI 透明命令集（事实标准，U 盘都用它）
 *   bInterfaceProtocol = 0x50  BOT（Bulk-Only Transport）
 *   + 两个 Bulk 端点（OUT 收 CBW/写数据，IN 发读数据/CSW）
 */
#include "tusb.h"
#include "pico/unique_id.h"

/*//--------------------------------------------------------------------+
//  VID/PID
//--------------------------------------------------------------------+*/
/* 0xCAFE/0x4002 是 TinyUSB 例程惯用的演示占位 VID/PID。
 * 量产提示：USB VID 由 USB-IF 正式授权（会员制，年费数千美元级）或向已持证
 * 模组厂购买子授权；盗用他人 VID/PID 会导致主机驱动误匹配与合规风险。 */
#define USB_VID              0xCAFE
#define USB_PID              0x4002
#define USB_BCD              0x0200   /* USB 2.0（RP2040 仅 Full-Speed） */

/*//--------------------------------------------------------------------+
//  设备描述符（18 字节）
//--------------------------------------------------------------------+*/
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    /* MSC 是"按接口声明类"的设备：设备级三者置 0，签名在接口描述符 */
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,   /* EP0: 64B（FS 上限） */

    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,                   /* 固件版本 1.00 */
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,                     /* U 盘序列号：Windows 挂盘符记忆靠它 */
    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *) &desc_device;
}

/*//--------------------------------------------------------------------+
//  配置描述符（FS 12Mbps）
//--------------------------------------------------------------------+*/
enum {
    ITF_NUM_MSC = 0,
    ITF_NUM_TOTAL
};

/* Bulk 端点号：OUT 收 CBW/写数据，IN 发读数据/CSW（BOT 规定数据与状态都走这两个端点） */
#define EPNUM_MSC_OUT   0x01
#define EPNUM_MSC_IN    0x81

#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)  /* 9 + 23 = 32 字节 */

uint8_t const desc_fs_configuration[] = {
    /* 配置描述符: 1 个配置, ITF_NUM_TOTAL 个接口, iConfiguration=0,
     * bmAttributes=总线供电, bMaxPower=100 (×2mA = 200mA)
     * 供电余量说明：TF 卡连续写峰值 100~200mA + RP2040 ~50mA，
     * 做"大文件导出"产品时建议用自带电源配置（自供电，bmAttributes |= 0x40）。 */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          0x00, 100),

    /* MSC 接口 + 一对 Bulk 端点（FS 单包 64B） */
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void) index;   /* 只有 1 个配置 */
    return desc_fs_configuration;
}

/*//--------------------------------------------------------------------+
//  字符串描述符
//--------------------------------------------------------------------+*/
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CONFIG
};

/* 序列号用 RP2040 出厂唯一 ID（16 个十六进制字符）——每个实体设备不同，
 * 主机用它区分"同一块盘"（驱动器号记忆、UAS/BOT 均衡策略等都可能用到） */
static char serial_str[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];

static uint16_t desc_str[32];   /* UTF-16 编码缓冲（首字为 bLength） */

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    static const char *string_desc[] = {
        [STRID_LANGID]      = NULL,                             /* 0: 语言 ID 特殊处理 */
        [STRID_MANUFACTURER]= "USB-Labs",
        [STRID_PRODUCT]     = "Lab3 MSC Disk",
        [STRID_SERIAL]      = serial_str,
        [STRID_CONFIG]      = "MSC",
    };
    (void) langid;  /* 演示只提供 English (0x0409) */

    uint8_t len;

    switch (index) {
        case STRID_LANGID:
            desc_str[1] = 0x0409;           /* English (United States) */
            len = 1;
            break;

        default:
            if (index >= sizeof(string_desc) / sizeof(string_desc[0]))
                return NULL;
            if (index == STRID_SERIAL && serial_str[0] == 0)
                pico_get_unique_board_id_string(serial_str, sizeof(serial_str));

            const char *str = string_desc[index];
            if (str == NULL) return NULL;

            for (len = 0; str[len] && (len < 31); len++)
                desc_str[1 + len] = (uint16_t) str[len];    /* ASCII -> UTF-16LE */
            break;
    }

    desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return desc_str;
}
