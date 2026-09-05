/* =========================================================================
 * Lab4 教学级固件 —— MJPEG 载荷填充伪代码（UVC 同步流分帧）
 *
 * 定位：伪代码骨架，聚焦 UVC 载荷协议本身；DMA/DCMI/OTG 细节由平台层提供。
 * 知识库对照：USBTree Video-UVC/00-UVC详解.md §5（载荷头/FID）
 * 抓包对照：capture/UVC流建立帧分析.md §3（本文件吐出的每个包头逐字节解读）
 *
 * ⚠ 教学级帧率预期（STM32H743 + OV5640 JPEG 直出 + HS 单事务同步端点 1024B/微帧）：
 *   - 640x480@30fps：帧均 15–25KB（3.6–6 Mbit/s）→ 稳定可达
 *   - 1280x720@30fps：帧均 60KB（14.4 Mbit/s）→ 超上限，实际 15–25fps
 *     （把 OV5640 压到 ≤34KB/帧才能名义 30fps，画质明显下降）
 *   - 这是 USB 2.0 单事务带宽上限（8.19 Mbit/s @1024B/微帧），不是代码 bug；
 *     商用 SoC 用多事务端点（24.6 Mbit/s）或批量端点解决。计算见 hardware/设计要点.md §7。
 * ========================================================================= */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ---------------- 常量：与 uvc_descriptors.c 的 Probe/Commit 闭环 ---------------- */
#define PAYLOAD_BUDGET   1024u              /* = uvc_commit.dwMaxPayloadTransferSize（HS 单事务）*/
#define HDR_MAX          12u                /* 带 PTS+SCR 的完整头 */
#define HDR_MIN          2u                 /* 精简头 */
#define CHUNK_DATA_MAX   (PAYLOAD_BUDGET - HDR_MAX)  /* 首包数据切片上限 */

/* 载荷头第 1 字节 = bHeaderLength；第 2 字节 = bmiHeaderInfo 位图：
 *   D0 FID（帧 ID，每帧翻转）  D1 EOF（本包是帧尾） D2 PTS（带 4B 呈现时间戳）
 *   D3 SCR（带 6B 源时钟）     D5 静态图           D6 ERR（载荷错误）
 *   D7 EOH（头结束标志，恒 1）
 */
#define H_FID   0x01u
#define H_EOF   0x02u
#define H_PTS   0x04u
#define H_SCR   0x08u
#define H_ERR   0x40u
#define H_EOH   0x80u

/* ---------------- 状态机 ---------------- */
typedef enum { VS_IDLE = 0, VS_STREAMING } vs_state_t;
static vs_state_t  vs_state = VS_IDLE;
static bool        fid = false;             /* FID 翻转标志：新帧开始必翻转 */
static uint32_t    frame_seq = 0;
static uint32_t    pts_tick;                /* 1kHz 节拍（SOF 中断维护）*/
static uint64_t    scr_27m;                 /* 27MHz 源时钟计数（dwClockFrequency 时基）*/
static uint16_t    scr_sofc;                /* 帧计数（SCR 低 2 字节，11 位有效）*/

/* 平台层：从 DCMI 环形缓冲取一帧完整 JPEG（OV5640 直出），无帧返回 false */
extern bool     dcmi_pop_jpeg(const uint8_t **buf, uint32_t *len);
extern void     dcmi_pop_done(const uint8_t *buf);
extern void     dcd_iso_video_send(uint8_t ep, const void *buf, uint32_t len);

/* ---------------- VS 接口换挡回调（SET_INTERFACE 由 EP0 事件驱动） ---------------- */
void uvc_vs_alt_changed(uint8_t alt)
{
    if (alt == 0) {                 /* 停流：释放带宽（主机带宽预算随之归还）*/
        vs_state = VS_IDLE;
    } else {                        /* 开流：复位帧边界状态 */
        fid       = false;
        frame_seq = 0;
        vs_state  = VS_STREAMING;
    }
}

/* ---------------- 组头 + 发送一帧（核心伪代码） ---------------- */
static void send_mjpeg_frame(const uint8_t *jpeg, uint32_t jlen)
{
    uint8_t pkt[PAYLOAD_BUDGET];
    uint32_t off = 0;
    bool     first = true;

    while (off < jlen) {
        uint32_t hdr_len;
        uint8_t  flags = (fid ? H_FID : 0) | H_EOH;
        uint32_t room;

        if (first) {
            /* 首包：PTS + SCR（主机用它做音视频对齐与丢帧检测）*/
            flags |= H_PTS | H_SCR;
            pkt[0] = HDR_MAX;
            pkt[1] = flags;
            memcpy(&pkt[2],  &pts_tick, 4);          /* PTS：呈现时间戳 */
            pkt[6]  = (uint8_t)(scr_27m       & 0xFF); /* SCR：4B 时钟 + 2B 帧计数 */
            pkt[7]  = (uint8_t)((scr_27m>> 8) & 0xFF);
            pkt[8]  = (uint8_t)((scr_27m>>16) & 0xFF);
            pkt[9]  = (uint8_t)((scr_27m>>24) & 0xFF);
            pkt[10] = (uint8_t)(scr_sofc & 0x7FF);     /* SOFC 11 位 */
            pkt[11] = (uint8_t)((scr_sofc>>8) & 0x07);
            hdr_len = HDR_MAX;
            first   = false;
        } else {
            pkt[0] = HDR_MIN;
            pkt[1] = flags;
            hdr_len = HDR_MIN;
        }

        room = PAYLOAD_BUDGET - hdr_len;
        uint32_t n = (jlen - off) < room ? (jlen - off) : room;
        memcpy(&pkt[hdr_len], &jpeg[off], n);
        off += n;

        if (off >= jlen)
            pkt[1] |= H_EOF;                         /* 帧尾包：置 EOF */
        dcd_iso_video_send(0x81, pkt, hdr_len + n);  /* 载荷总长（含头）≤ 预算 */
        /* 真实实现：双缓冲轮转，上一包 DMA 未完成前不得改写 pkt —— 此处为伪代码简化 */
    }
}

/* ---------------- 主循环任务 ---------------- */
void uvc_video_task(void)
{
    const uint8_t *jpeg;
    uint32_t jlen;

    if (vs_state != VS_STREAMING) return;
    if (!dcmi_pop_jpeg(&jpeg, &jlen)) return;    /* 无新帧：同步流允许空闲微帧 */

    /* 超长帧保护：单帧超过 Commit 承诺的 dwMaxVideoFrameSize → 整帧丢弃并置 ERR，
     * 但 FID 仍要翻转 —— 主机靠 FID 序列重新对齐帧边界（抓包验证点）*/
    if (jlen > 460800u) {
        dcmi_pop_done(jpeg);
        fid = !fid;
        frame_seq++;
        return;
    }

    send_mjpeg_frame(jpeg, jlen);
    dcmi_pop_done(jpeg);

    fid       = !fid;                            /* FID 每帧必翻转 */
    frame_seq++;
    pts_tick++;                                  /* 教学简化：每帧 1 tick；真实应取采集时刻 */
    scr_sofc = (scr_sofc + 1) & 0x7FF;
}

/* ---------------- 丢帧/错误策略速查 ----------------
 * - DCMI 溢出/半帧   ：整帧丢弃，下一帧首包置 H_ERR 提示主机；
 * - USB 带宽不足掉包 ：同步流无重传，主机按 FID 断档判丢帧（帧率下降的抓包特征）；
 * - 主机切回 Alt0    ：立刻停发，残留半包会污染下一轮流的前几个微帧；
 * - 商用增强（本骨架未含）：Still Image 触发、动态帧率（bmHint）、H.264 载荷（EU 实体）。
 */
