/* =========================================================================
 * Lab4 教学级固件 —— UAC1.0 麦克风描述符（AC 接口 + Feature Unit + AS 接口）
 *
 * 定位：教学骨架。实体图：IT1(Microphone,0x0201) -> FU2(MUTE/VOLUME) -> OT3(USB Streaming,0x0101)
 * 知识库对照：USBTree 20-枝干-设备类协议/Audio-UAC/01-UAC1.0详解.md（描述符链/请求实战）
 *             03-UAC规范级-实体与请求全表.md（请求码/终端类型全表）
 *
 * 接口号沿用复合设备布局：if2 = AC 控制接口，if3 = AS 流接口（Alt0/Alt1）。
 * 与 UVC（if0/if1）拼进同一个配置：bNumInterfaces=4，wTotalLength 两段相加。
 *
 * 时钟模式（本骨架的诚实声明）：录音方向（设备->主机）用设备本地时钟 = 异步源，
 * 主机自然跟随输入数据率 —— 因此**没有反馈端点**（反馈端点是"播放方向"异步设备的义务，
 * 数值解读见 capture/UVC流建立帧分析.md §5）。描述符与实现必须一致，见 experience.md §4。
 * ========================================================================= */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ---------------- IAD（音频功能）：并入复合配置时紧接 UVC 段之后 ---------------- */
static const uint8_t iad_uac[8] = {
    8, 0x0B,
    0x02,                   /* bFirstInterface = 2（AC 接口号）*/
    2,                      /* bInterfaceCount = AC + AS */
    0x01, 0x00, 0x00,       /* 音频类 / Audio Collection 子类 / 协议 */
    0x00,                   /* iFunction */
};

/* ---------------- if2：AC 音频控制接口（Alt0，零端点） ---------------- */
#define AC_TOTAL_LEN  39    /* = 头9 + IT12 + FU9 + OT9 */

static const uint8_t ac_interface[9 + AC_TOTAL_LEN] = {
    /* 标准接口描述符 */
    9, 0x04, 0x02, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
    /* 类特定 AC 头 */
    9, 0x24, 0x01,
    0x00, 0x01,             /* bcdADC = 1.00（UAC1）*/
    AC_TOTAL_LEN & 0xFF, AC_TOTAL_LEN >> 8,   /* wTotalLength */
    1,                      /* bInCollection = 1 */
    3,                      /* baInterfaceNr[0] = AS 接口号 */
    /* 输入终端 IT1：麦克风 */
    12, 0x24, 0x02, 1,
    0x01, 0x02,             /* wTerminalType = INPUT_MICROPHONE(0x0201) */
    0,                      /* bAssocTerminal */
    1,                      /* bNrChannels = 1（单声道）*/
    0x01, 0x00,             /* wChannelConfig = FRONT_LEFT 占位 */
    0, 0,                   /* iChannelNames, iTerminal */
    /* 特征单元 FU2：MUTE + VOLUME（系统音量条就打到这里）*/
    9, 0x24, 0x06, 2,
    1,                      /* bSourceID = IT1 */
    1,                      /* bControlSize = 1 */
    0x03,                   /* bmaControls[0] 主控: D0 MUTE | D1 VOLUME */
    0x03,                   /* bmaControls[1] 声道1：同上 */
    0,                      /* iFeature */
    /* 输出终端 OT3：USB Streaming */
    9, 0x24, 0x03, 3,
    0x01, 0x01,             /* wTerminalType = OUTPUT_TERMINAL_USB(0x0101) */
    0, 2, 0,                /* bAssocTerminal, bSourceID=FU2, iTerminal */
};

/* ---------------- if3：AS 音频流接口 ----------------
 * 长度 = Alt0接口9 + Alt1接口9 + AS通用7 + TypeI格式13 + 标准端点7 + 类端点7 = 52 */
static const uint8_t as_interface[52] = {
    /* Alt0：零带宽挡（主机采样率切换/停流时切回）*/
    9, 0x04, 0x03, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    /* Alt1：带同步 IN 端点的工作挡 */
    9, 0x04, 0x03, 0x01, 1, 0x01, 0x02, 0x00, 0x00,
    /* 类特定 AS 通用描述符 */
    7, 0x24, 0x01,
    3,                      /* bTerminalLink = OT3 */
    1,                      /* bDelay = 1ms（固件流水线延迟声明）*/
    0x01, 0x00,             /* wFormatTag = PCM(0x0001)（Type I 未压缩）*/
    /* Type I 格式描述符：离散采样率 16k / 48k */
    13, 0x24, 0x02,
    1,                      /* bNrChannels */
    2,                      /* bSubframeSize = 2 字节 */
    16,                     /* bBitResolution = 16 */
    2,                      /* bSamFreqType = 2（离散；连续填 0 并用 Min/Max）*/
    0x80, 0x3E, 0x00,       /* tSamFreq[0] = 16000 Hz（3 字节，LSB first）*/
    0x80, 0xBB, 0x00,       /* tSamFreq[1] = 48000 Hz */
    /* 标准端点：同步 IN（异步源；无 bSynchAddress，因为录音方向不需要反馈端点）*/
    7, 0x05, 0x82,
    0x05,                   /* bmAttributes: ISO + asynchronous */
    0x64, 0x00,             /* FS: wMaxPacketSize = 100（48k×2B=96B + 异步抖动余量）*/
    1,                      /* bInterval = 1（每毫秒一个数据包）*/
    /* 类特定端点描述符 */
    7, 0x25, 0x01,
    0x01,                   /* bmAttributes: D0 = 采样率控制（SET_CUR 切 16k/48k）*/
    0x00,                   /* bLockDelayUnits = 未定义 */
    0x00, 0x00,             /* wLockDelay = 0 */
};

/* =========================================================================
 * Feature Unit 类请求处理（主机必答集，缺一个音量条就失效）
 * 寻址：bmRequestType 0x21(SET)/0xA1(GET)，wValue=(CS<<8)|通道号，
 *       wIndex=(FU_ID<<8)|AC接口号 = (2<<8)|2。
 * CS：0x01 MUTE(1字节) / 0x02 VOLUME(16bit 有符号，单位 1/256 dB)。
 * ========================================================================= */
static uint8_t  fu_mute   = 0;
static int16_t  fu_volume = (int16_t)0xC000;   /* -64 dB */

static const int16_t VOL_MIN = (int16_t)0xA000; /* -96 dB（-24576 = 0xA000 补码）*/
static const int16_t VOL_MAX = 0x0000;          /*   0 dB */
static const int16_t VOL_RES = 0x0040;          /* 0.25 dB 步进 */

static uint16_t fu_sam_freq = 48000;            /* 当前采样率（EP 采样率控制）*/

bool uac_class_request(uint8_t bmReqType, uint8_t bRequest,
                       uint16_t wValue, uint16_t wIndex,
                       uint8_t *data, uint16_t wLength)
{
    uint8_t cs   = wValue >> 8;
    uint8_t unit = wIndex >> 8;
    (void)wLength;
    if (unit != 2) return false;                /* 只实现了 FU2 */

    switch (bRequest) {
    case 0x01:                                  /* SET_CUR */
        if (cs == 0x01) fu_mute   = data[0];
        else if (cs == 0x02) memcpy(&fu_volume, data, 2);
        else return false;
        /* 真实实现：立即生效到 I2S 链路（静音=送零样本/拉低 PA）*/
        return true;
    case 0x81:                                  /* GET_CUR */
        dcd_ep0_send(cs == 0x01 ? (void *)&fu_mute : (void *)&fu_volume,
                     cs == 0x01 ? 1 : 2);
        return true;
    case 0x82: dcd_ep0_send(&VOL_MIN, 2); return true;  /* GET_MIN（VOLUME）*/
    case 0x83: dcd_ep0_send(&VOL_MAX, 2); return true;  /* GET_MAX */
    case 0x84: dcd_ep0_send(&VOL_RES, 2); return true;  /* GET_RES */
    case 0x85: { static uint16_t l = cs == 0x01 ? 1 : 2;
                 dcd_ep0_send(&l, 2); } return true;    /* GET_LEN */
    default:  return false;
    }
}

/* 端点采样率控制（CS EP bmAttributes D0 声明过，主机 SET_INTERFACE Alt1 前下发）：
 * bmRequestType 0x22/0xA2（端点方向），wValue=(0x01<<8)|0，wIndex=EP 地址。*/
bool uac_ep_request(uint8_t bRequest, uint8_t *data)
{
    if (bRequest == 0x01) {                     /* SET_CUR: 3 字节 LSB first */
        uint32_t f = data[0] | (data[1] << 8) | (data[2] << 16);
        if (f != 16000 && f != 48000) return false;
        fu_sam_freq = f;
        /* 真实实现：重配 SAI 分频，注意与 USB 帧节拍的边界对齐 */
        return true;
    }
    if (bRequest == 0x81) {                     /* GET_CUR */
        uint8_t f3[3] = { fu_sam_freq & 0xFF, (fu_sam_freq >> 8) & 0xFF,
                          (fu_sam_freq >> 16) & 0xFF };
        dcd_ep0_send(f3, 3);
        return true;
    }
    return false;
}

/* 每毫秒音频包：I2S DMA 半满/全满回调里把 96 字节（48k×2B×1ch）送入 EP2 */
void uac_audio_task(void)   /* 教学伪代码：真实实现见 mjpeg_frame_fill.c 的节拍思路 */
{
    /* 1) 从 SAI 环形缓冲取 1ms 样本（不足补零，绝不能让同步端点空发）
     * 2) dcd_iso_audio_send(0x82, buf, 96);
     * 3) 48k@HS 时按微帧节拍：每微帧 6 样本 × 2B = 12B
     */
}

/* 平台层钩子 */
void dcd_ep0_send(const void *buf, uint32_t len);
