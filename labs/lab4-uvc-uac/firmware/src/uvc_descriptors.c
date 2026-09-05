/* =========================================================================
 * Lab4 教学级固件 —— UVC 1.1 描述符（VC/VS 接口 + Probe/Commit 结构体）
 *
 * 定位：教学骨架。与商用差异：商用 SoC 方案（Linux gadget）由 configfs 生成
 *       描述符；本文件让你逐字节看懂它的每一段（用 `lsusb -v` 对照阅读）。
 * 知识库对照：USBTree 20-枝干-设备类协议/Video-UVC/00-UVC详解.md（实体图/换挡）
 *             01-UVC规范级-控制与格式全表.md（子类型码/请求码全表）
 *
 * 布局（真实复合设备 = 本文件 + uac_mic_descriptors.c 拼进同一个配置）：
 *   if0 = VC 控制接口（实体图：IT1(Camera) -> PU2 -> OT3(Streaming)）
 *   if1 = VS 流接口（Alt0 零带宽 / Alt1 同步 IN 端点）
 *   if2/3 = UAC 的 AC/AS 接口（见 uac_mic_descriptors.c，bNumInterfaces=4）
 * ========================================================================= */
#include <stdint.h>
#include <stdbool.h>

#define UVC_BCD_VERSION   0x0110u   /* UVC 1.10（Windows 7+ 支持 1.1；1.5 需 Win8+）*/
#define VID_USB_LABS      0x1234u   /* 教学 VID（商用需 USB-IF 正式分配）*/
#define PID_UVC_UAC_LAB4  0x5678u

/* ---------------- 设备描述符（IAD 复合设备签名） ---------------- */
static const uint8_t dev_desc[18] = {
    18, 0x01,
    0x00, 0x02,             /* bcdUSB 2.00 */
    0xEF, 0x02, 0x01,       /* 设备类=0xEF/子类0x02/协议0x01：IAD 复合设备签名 */
    64,                     /* EP0 最大包 */
    VID_USB_LABS & 0xFF, VID_USB_LABS >> 8,
    PID_UVC_UAC_LAB4 & 0xFF, PID_UVC_UAC_LAB4 >> 8,
    0x00, 0x01,             /* bcdDevice */
    1, 2, 0,                /* iManufacturer / iProduct / iSerial */
    1,                      /* bNumConfigurations：FS/HS 各备一套配置数组，主机读哪套
                               取决于当前速度（Device Qualifier 描述符声明另一速度的能力）*/
};

/* ---------------- 配置描述符（教学：仅 UVC 段；UAC 段见另一文件） ----------------
 * 总长 = 配置9 + IAD8 + VC接口9 + VC头13 + IT18 + PU13 + OT9
 *      + VS-Alt0接口9 + VS头13 + 格式9 + 帧26 + 帧26 + 色彩6 + VS-Alt1接口9 + 端点7 = 184
 */
#define UVC_CFG_TOTAL_LEN  184

static const uint8_t cfg_uvc_fs[UVC_CFG_TOTAL_LEN] = {
    /* 配置描述符 */
    9, 0x02, UVC_CFG_TOTAL_LEN & 0xFF, UVC_CFG_TOTAL_LEN >> 8,
    2,               /* bNumInterfaces: if0(VC) + if1(VS)（并入复合设备后为 4）*/
    1, 0, 0x80, 50,  /* bmAttributes 总线供电, bMaxPower 100mA */

    /* ---- IAD：把 if0/if1 打包成一个"视频功能"（Windows 依赖它配对驱动） ---- */
    8, 0x0B, 0x00, 2, 0x0E, 0x03, 0x00, 0x00,  /* 视频收集类 Video Collection */

    /* ---- if0：VC 视频控制接口（控制面，零带宽） ---- */
    9, 0x04, 0x00, 0x00, 0x00, 0x0E, 0x01, 0x00, 0x00,
    /* 类特定 VC 头：bcdUVC / wTotalLength(=13+18+13+9=53) / dwClockFrequency(SCR时基) */
    13, 0x24, 0x01,
    0x10, 0x01,             /* bcdUVC 1.10 */
    53, 0x00,               /* wTotalLength */
    0xC0, 0xFC, 0x9B, 0x01, /* dwClockFrequency = 27000000（常见 27MHz 时基；须与固件 SCR 一致）*/
    1, 1,                   /* bInCollection=1, baInterfaceNr[0]=1 (VS 接口号) */
    /* 输入终端 IT1：Camera Terminal（类型 0x0201），支持自动/手动曝光控制 */
    18, 0x24, 0x02, 1,
    0x01, 0x02,             /* wTerminalType = ITT_CAMERA(0x0201) */
    0, 0,                   /* bAssocTerminal, iTerminal */
    0,0, 0,0, 0,0,          /* 目焦距 min/max/ocular（定焦填 0）*/
    3,                      /* bControlSize = 3 字节位图 */
    0x06, 0x00, 0x00,       /* bmControls: D1 自动曝光模式 | D2 手动曝光 */
    /* 处理单元 PU2：亮度/对比度/增益（v4l2-ctl --set-ctrl=brightness 就打到这里）*/
    13, 0x24, 0x05, 2,
    1,                      /* bSourceID = IT1 */
    0,0,                    /* wMaxMultiplier */
    3,                      /* bControlSize */
    0x1C, 0x00, 0x00,       /* bmControls: D2 亮度 | D3 对比度 | D4 增益 */
    0,                      /* iProcessing */
    0x00,                   /* bmVideoStandings（UVC1.1 新增）*/
    /* 输出终端 OT3：USB Streaming（类型 0x0101），挂在 PU2 下游 */
    9, 0x24, 0x03, 3,
    0x01, 0x01,             /* wTerminalType = TT_STREAMING(0x0101) */
    0, 2, 0,                /* bAssocTerminal, bSourceID=PU2, iTerminal */

    /* ---- if1 Alt0：VS 流接口零带宽挡（主机先切这里，相当于"停流"） ---- */
    9, 0x04, 0x01, 0x00, 0x00, 0x0E, 0x02, 0x00, 0x00,
    /* 类特定 VS 头 */
    13, 0x24, 0x01,
    80, 0x00,               /* wTotalLength = VS头13+格式9+帧26*2+色彩6 */
    0x81,                   /* bEndpointAddress（Alt1 的流端点）*/
    0x00,                   /* bmInfo：不支持动态帧率协商的降级位图 */
    3,                      /* bTerminalLink = OT3 */
    0x00,                   /* bStillCaptureMethod：不支持静态拍 */
    0x00, 0x00,             /* bTriggerSupport / bTriggerUsage */
    1, 0x00,                /* bControlSize=1, bmaControls[1]={0} */

    /* ---- 格式描述符：MJPEG（wFormatTag 'MJPG'） ---- */
    9, 0x24, 0x06,
    1,                      /* bFormatIndex=1（Probe 协商用 bFormatIndex 定位到这里）*/
    2,                      /* bNumFrameDescriptors=2 */
    0x02,                   /* bmCapabilities: 固定帧率 */
    0x4D, 0x4A,             /* wFormatTag = 'MJPG'(0x4A4D，小端) */
    0x00,                   /* bmaControls */
    /* 帧描述符 1：1280x720 @30fps */
    26, 0x24, 0x07,
    1,                      /* bFrameIndex=1 */
    0x00,                   /* bmCapabilities */
    0x00, 0x05,             /* wWidth  = 1280 */
    0xD0, 0x02,             /* wHeight = 720 */
    0x80, 0x84, 0x1E, 0x00, /* dwMinBitRate = 2,000,000 */
    0x00, 0x42, 0xF4, 0x00, /* dwMaxBitRate = 16,000,000（对应 §7 带宽账上限）*/
    0x00, 0x08, 0x07, 0x00, /* dwMaxVideoFrameBufferSize = 460800（450KB，压缩最坏帧余量）*/
    0x15, 0x16, 0x05, 0x00, /* dwDefaultFrameInterval = 333333（100ns 单位 = 30fps）*/
    1,                      /* bFrameIntervalType=1：离散帧率 1 个 */
    0x15, 0x16, 0x05, 0x00, /* dwFrameInterval[0] = 333333 */
    /* 帧描述符 2：640x480 @30fps（教学级实测最稳挡）*/
    26, 0x24, 0x07,
    2,
    0x00,
    0x80, 0x02,             /* 640 */
    0xE0, 0x01,             /* 480 */
    0x40, 0x42, 0x0F, 0x00, /* dwMinBitRate = 1,000,000 */
    0x80, 0x8D, 0x5B, 0x00, /* dwMaxBitRate = 6,000,000 */
    0x00, 0x77, 0x01, 0x00, /* dwMaxVideoFrameBufferSize = 96000 */
    0x15, 0x16, 0x05, 0x00,
    1,
    0x15, 0x16, 0x05, 0x00,
    /* 颜色匹配描述符（BT.709/sRGB）*/
    6, 0x24, 0x0D, 1, 1, 1,

    /* ---- if1 Alt1：同步 IN 端点，真实带宽挡 ---- */
    9, 0x04, 0x01, 0x01, 1, 0x0E, 0x02, 0x00, 0x00,
    7, 0x05, 0x81,
    0x05,                   /* bmAttributes: 同步传输 + 异步同步类型（UVC 常见值）*/
    0x00, 0x02,             /* FS: wMaxPacketSize = 512（全速同步上限 1023 以内）*/
    1,                      /* bInterval = 1 */
};

/* HS 版本：仅 Alt1 端点包长不同（单事务 1024B/微帧 = 教学级上限 8.19 Mbit/s）。
 * 多事务编码（商业 720p30 用）：((事务数-1)<<11)|1024，如 3 事务填 0x1400(=5120)，
 * 详见 hardware/设计要点.md §7.2。数组其余字节与 FS 版一致，固件里复制后只改此处。 */
static const uint8_t cfg_uvc_hs[UVC_CFG_TOTAL_LEN] = { /* 同 cfg_uvc_fs，仅端点差异 */ };

/* =========================================================================
 * Probe/Commit 结构体（UVC 1.1 = 34 字节；UVC1.0=26（去掉末 4 字节），
 * UVC1.5=36（再加 bUsage/bmBitRateControls）。GET_LEN 应返回结构体长度。
 * 主机协商流：GET_INFO → GET_MAX/GET_MIN → SET_CUR(PROBE) → GET_CUR(PROBE)
 *           → SET_CUR(COMMIT) → SET_INTERFACE(Alt1) → 开始吐帧
 * ========================================================================= */
typedef struct __attribute__((packed)) {
    uint16_t bmHint;                    /* 主机提示位（D0=dwFrameInterval 可变）*/
    uint8_t  bFormatIndex;              /* 选择格式描述符（1 = MJPEG）*/
    uint8_t  bFrameIndex;               /* 选择帧描述符（1=720p / 2=480p）*/
    uint32_t dwFrameInterval;           /* 帧间隔，100ns 单位：333333=30fps */
    uint16_t wKeyFrameRate, wPFrameRate;/* MJPEG 恒为 0 */
    uint16_t wCompQuality, wCompWindowSize;
    uint16_t wDelay;                    /* 内部延迟（ms），教学填 0 */
    uint32_t dwMaxVideoFrameSize;       /* 单帧最大字节数：主机按它分配接收缓冲！*/
    uint32_t dwMaxPayloadTransferSize;  /* 单包载荷预算（含头）：HS 单事务=1024 */
    uint32_t dwClockFrequency;          /* SCR 时基（须与 VC 头一致，27000000）*/
    uint8_t  bmFramingInfo;             /* D0=支持按 IO 折叠, D1=支持按包大小折叠 */
    uint8_t  bPreferedVersion, bMinVersion, bMaxVersion; /* JPEG 版本协商，填 0 */
} uvc_probe_ctrl_t;

_Static_assert(sizeof(uvc_probe_ctrl_t) == 34, "UVC 1.1 probe = 34 bytes");

static uvc_probe_ctrl_t uvc_probe = { .bFormatIndex = 1, .bFrameIndex = 1,
    .dwFrameInterval = 333333, .dwMaxVideoFrameSize = 460800,
    .dwMaxPayloadTransferSize = 1024, .dwClockFrequency = 27000000 };
static uvc_probe_ctrl_t uvc_commit; /* COMMIT = PROBE 的拷贝快照（主机敲定值）*/

/* ---------------- 类请求处理骨架（EP0 数据阶段） ----------------
 * VS 控制：bmRequestType 0x21(SET)/0xA1(GET)，wValue = (CS<<8)|0，
 *          wIndex = (0<<8)|VS接口号。CS: 1=VS_PROBE_CONTROL, 2=VS_COMMIT_CONTROL。
 * VC 控制（如曝光）：wValue = (CS<<8)|0，wIndex = (实体ID<<8)|VC接口号。
 * 请求码：0x01 SET_CUR / 0x81 GET_CUR / 0x85 GET_LEN / 0x86 GET_INFO。
 */
bool uvc_class_request(uint8_t bmReqType, uint8_t bRequest,
                       uint16_t wValue, uint16_t wIndex,
                       uint8_t *data, uint16_t wLength)
{
    uint8_t cs = wValue >> 8;
    (void)wIndex;
    if (cs == 1 || cs == 2) {                     /* PROBE / COMMIT */
        uvc_probe_ctrl_t *p = (cs == 1) ? &uvc_probe : &uvc_commit;
        switch (bRequest) {
        case 0x01:                                /* SET_CUR：主机写入协商值 */
            if (wLength != sizeof(*p)) return false;
            *p = *(uvc_probe_ctrl_t *)data;
            /* 教学要点：这里应校验 bFormatIndex/bFrameIndex/dwFrameInterval，
             * 并按帧描述符回填 dwMaxVideoFrameSize / dwMaxPayloadTransferSize */
            return true;
        case 0x81: dcd_ep0_send(p, sizeof(*p)); return true;   /* GET_CUR */
        case 0x85: { static uint16_t len = sizeof(uvc_probe_ctrl_t);
                     dcd_ep0_send(&len, 2); } return true;      /* GET_LEN */
        default:   return false;
        }
    }
    return false;   /* Camera/PU 控制（曝光/亮度）按同样套路答 CUR/MIN/MAX/RES */
}

/* 平台层钩子（见 firmware/README.md §1.3） */
void dcd_ep0_send(const void *buf, uint32_t len);
