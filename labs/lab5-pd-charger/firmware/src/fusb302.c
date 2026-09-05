/**
 * @file fusb302.c
 * @brief onsemi FUSB302 TCPC 驱动骨架：设备 ID 读取 / CC 状态测量 / BMC 收发入口
 *
 * 架构位置（TCPM + TCPC 分层，见 firmware/README.md）：
 *   pe_sink.c（策略引擎） → fusb302.c（本文件：寄存器与 FIFO） → FUSB302 硬件（BMC/GoodCRC/重试）
 *
 * 诚实声明（务必阅读）：
 *  1. 本文件是**教学骨架**，未做过一致性测试。生产实现请直接看/用：
 *     Linux 内核 drivers/usb/typec/tcpm/fusb302.c（生产级参考实现）。
 *  2. 寄存器位域按 onsemi FUSB302B 数据手册 + 内核驱动交叉整理，
 *     个别位随手册版本有差异，凡标注 `手册核对` 处落地前必须对照你的器件手册。
 *  3. 时序参数（去抖、阈值窗口）引自 Type-C 规范 Table 4-34/4-36（见知识库
 *     09-TypeC规范级 文档），属于规范值；MDAC 步长等器件参数以手册为准。
 *
 * 平台钩子（4 个，接你的 MCU HAL）：
 *   plat_i2c_write / plat_i2c_read / plat_delay_ms / plat_delay_us
 */

#include "fusb302.h"
#include <string.h>

/* ==========================================================================
 * 平台钩子 —— 由移植者实现
 * ========================================================================== */
extern int  plat_i2c_write(uint8_t dev_addr, uint8_t reg, const uint8_t *buf, size_t len);
extern int  plat_i2c_read(uint8_t dev_addr, uint8_t reg, uint8_t *buf, size_t len);
extern void plat_delay_ms(uint32_t ms);
extern void plat_delay_us(uint32_t us);

/* 调试日志钩子：实现为 printf 类函数，不实现可置空 */
__attribute__((weak)) void fusb302_log(const char *s) { (void)s; }

#define LOG(s) fusb302_log(s)

/* ==========================================================================
 * 寄存器读写原语
 * ========================================================================== */
static int reg_write(uint8_t reg, uint8_t val)
{
    return plat_i2c_write(FUSB302_I2C_ADDR_DEFAULT, reg, &val, 1);
}

static int reg_read(uint8_t reg, uint8_t *val)
{
    return plat_i2c_read(FUSB302_I2C_ADDR_DEFAULT, reg, val, 1);
}

static int reg_modify(uint8_t reg, uint8_t mask, uint8_t val)
{
    uint8_t v;
    int ret = reg_read(reg, &v);
    if (ret < 0)
        return ret;
    v = (uint8_t)((v & ~mask) | (val & mask));
    return reg_write(reg, v);
}

/* ==========================================================================
 * 初始化与设备识别
 * ========================================================================== */

/**
 * @brief  读 DeviceID 寄存器
 * @note   高 4 位为器件族，公开资料中 0x8x/0x9x 段的批次均存在（手册核对），
 *         判定"应答且在已知族段"即可，不建议写死单值。
 */
int fusb302_read_device_id(uint8_t *id)
{
    int ret = reg_read(FUSB302_REG_DEVICE_ID, id);
    if (ret < 0)
        return ret;
    switch (*id & FUSB302_DEVICE_ID_FAMILY_MASK) {
    case 0x80:
    case 0x90:
        return 0; /* FUSB302 家族 */
    default:
        LOG("fusb302: unexpected device id\r\n");
        return -1;
    }
}

static int fusb302_sw_reset(void)
{
    int ret = reg_write(FUSB302_REG_RESET, FUSB302_RESET_SW_RESET);
    plat_delay_ms(1); /* 复位恢复，见手册（手册核对） */
    return ret;
}

/**
 * @brief 初始化：复位 → 全功率上电 → 校验 ID → 打开硬件自动 GoodCRC/重试
 *
 * GoodCRC/MessageID/重传交给硬件的原因：tReceive 窗口仅 0.9~1.1 ms
 * （PD 规范 PD Timers 表，知识库 07 文），软件在慢速 MCU 上无法保证。
 */
int fusb302_init(void)
{
    uint8_t id = 0;
    int ret;

    ret = fusb302_sw_reset();
    if (ret < 0)
        return ret;

    /* 上电全部功率域：bandgap | 内部振荡器 | 测量块 | 接收块（手册核对） */
    ret = reg_write(FUSB302_REG_POWER, FUSB302_POWER_ALL);
    if (ret < 0)
        return ret;

    ret = fusb302_read_device_id(&id);
    if (ret < 0)
        return ret;

    /* 硬件自动 GoodCRC（仅对 SOP；AUTO_RETRY/N_RETRIES 档位见手册，手册核对） */
    ret = reg_modify(FUSB302_REG_SWITCHES1, FUSB302_SWITCHES1_AUTO_CRC,
                     FUSB302_SWITCHES1_AUTO_CRC);
    if (ret < 0)
        return ret;

    /* 收发方向：本骨架按 Sink 角色使能 Rd 端接（双侧，等连接判定方向） */
    return fusb302_set_sink_terminations(true);
}

/* ==========================================================================
 * CC 状态测量
 *
 * Sink 视角原理：我方 Rd（5.1kΩ）端接在对端 Rp 电流源之下，CC 电压即
 * "Rp 宣告档位"的编码：
 *   <0.249V  未连接（规范断连阈值）
 *   0.277~0.612V   Default USB Power
 *   0.746~1.164V   1.5A 宣告
 *   1.369~2.042V   3.0A 宣告
 * （Type-C 规范 Table 4-36，转引自知识库 09-TypeC规范级）
 *
 * 骨架用 MDAC 阈值逐级比较实现"读电压"；MDAC 步长与恢复延时以手册为准
 * （公开资料常用值：MDAC LSB 约 42 mV，测量稳定约 275 us——手册核对）。
 * ========================================================================== */
#define MDAC_MV_DEFAULT_MAX 612u /* Def 档上限 */
#define MDAC_MV_15_MAX     1164u /* 1.5A 档上限 */

static int cc_measure_mv(uint8_t cc_meas_sel, uint32_t *out_mv)
{
    /* 从高阈值向低二分：阈值 = LSB(约42mV) * 寄存器值（步长手册核对） */
    static const uint16_t thresholds_mv[] = { 2042u, 1164u, 612u, 249u };
    uint32_t lo = 0;

    reg_write(FUSB302_REG_CONTROL0, FUSB302_CONTROL0_HOST_CUR(0)); /* Sink 不开自源电流 */
    reg_write(FUSB302_REG_SWITCHES0, cc_meas_sel);

    for (unsigned i = 0; i < sizeof(thresholds_mv) / sizeof(thresholds_mv[0]); i++) {
        uint8_t mdac = (uint8_t)(thresholds_mv[i] / 42u); /* 手册核对 */
        uint8_t st0;
        reg_write(FUSB302_REG_MEASURE, mdac);
        plat_delay_us(275); /* 测量建立（手册核对） */
        if (reg_read(FUSB302_REG_STATUS0, &st0) < 0)
            return -1;
        if (st0 & FUSB302_STATUS0_COMP) {
            lo = thresholds_mv[i]; /* CC 电压高于该阈值 */
            break;
        }
    }
    *out_mv = lo; /* 区间下界即可区分档位；精确值可用二分细化 */
    return 0;
}

static fusb302_cc_level_t classify_cc(uint32_t mv, bool vbus_ok)
{
    if (mv == 0)
        return vbus_ok ? FUSB302_CC_RA : FUSB302_CC_OPEN; /* 极低压：Ra 或未连 */
    if (mv <= MDAC_MV_DEFAULT_MAX)
        return FUSB302_CC_RP_DEF;
    if (mv <= MDAC_MV_15_MAX)
        return FUSB302_CC_RP_1_5;
    return FUSB302_CC_RP_3_0;
}

/* SWITCHES0 中选择被测 CC 的组合值（手册核对） */
#define SW_MEAS_CC1 0x01u
#define SW_MEAS_CC2 0x02u
/* SWITCHES0 中使能 Rd 端接（PDWN1/PDWN2）的组合值（手册核对） */
#define SW_RD_CC1   0x04u
#define SW_RD_CC2   0x08u

int fusb302_set_sink_terminations(bool enable)
{
    return reg_modify(FUSB302_REG_SWITCHES0, (uint8_t)(SW_RD_CC1 | SW_RD_CC2),
                      enable ? (uint8_t)(SW_RD_CC1 | SW_RD_CC2) : 0u);
}

int fusb302_get_cc(fusb302_cc_status_t *st)
{
    uint32_t mv1 = 0, mv2 = 0;
    uint8_t st0;
    int ret;

    if (st == NULL)
        return -1;

    if ((ret = cc_measure_mv(SW_MEAS_CC1, &mv1)) < 0)
        return ret;
    if ((ret = cc_measure_mv(SW_MEAS_CC2, &mv2)) < 0)
        return ret;

    if ((ret = reg_read(FUSB302_REG_STATUS0, &st0)) < 0)
        return ret;
    st->vbus_ok = (st0 & FUSB302_STATUS0_VBUSOK) != 0;

    st->cc1 = classify_cc(mv1, st->vbus_ok);
    st->cc2 = classify_cc(mv2, st->vbus_ok);
    return 0;
}

/* ==========================================================================
 * BMC 收发（FIFO 入口）
 * ========================================================================== */
#define FUSB302_CONTROL0_TX_FLUSH 0x40u /* 手册核对 */
#define FUSB302_CONTROL1_RX_FLUSH 0x04u /* 手册核对 */

int fusb302_flush_fifos(void)
{
    int ret = reg_modify(FUSB302_REG_CONTROL0, FUSB302_CONTROL0_TX_FLUSH,
                         FUSB302_CONTROL0_TX_FLUSH);
    if (ret < 0)
        return ret;
    /* flush 位为动作位：置 1 后再清零 */
    ret = reg_modify(FUSB302_REG_CONTROL0, FUSB302_CONTROL0_TX_FLUSH, 0);
    ret |= reg_modify(FUSB302_REG_CONTROL1, FUSB302_CONTROL1_RX_FLUSH,
                      FUSB302_CONTROL1_RX_FLUSH);
    ret |= reg_modify(FUSB302_REG_CONTROL1, FUSB302_CONTROL1_RX_FLUSH, 0);
    return ret;
}

/**
 * @brief PD 消息发送（BMC 入口）
 *
 * FIFO 写入次序（数据手册 PD Transmitter 章节；token 取值见 fusb302.h）：
 *   SYNC1 SYNC1 SYNC1 SYNC2(SOP) | PACKSYM(len>>2) | header+objects | JAMCRC | EOP | TXON
 * 其中 len = 2 + 4×对象数（不含 CRC；JAMCRC 触发硬件计算并追加 CRC-32）。
 * 发送成功/失败经 INT 中断（TXSENT 等）上报，骨架不轮询。
 */
int fusb302_pd_tx(const pd_msg_t *msg)
{
    uint8_t buf[64];
    size_t pos = 0, n;
    size_t len;

    if (msg == NULL || pd_hdr_nobjects(msg->header) > PD_MAX_OBJECTS)
        return -1;

    (void)fusb302_flush_fifos();

    buf[pos++] = FUSB302_TKN_SYNC1; /* SOP Ordered Set（知识库 07 文第二节） */
    buf[pos++] = FUSB302_TKN_SYNC1;
    buf[pos++] = FUSB302_TKN_SYNC1;
    buf[pos++] = FUSB302_TKN_SYNC2;

    n = pd_hdr_nobjects(msg->header);
    len = n * 4u + 2u; /* header(2B) + 对象；与内核驱动同式（手册核对） */
    buf[pos++] = (uint8_t)(FUSB302_TKN_PACKSYM | (len >> 2));

    buf[pos++] = (uint8_t)(msg->header & 0xFF);        /* 小端在前 */
    buf[pos++] = (uint8_t)(msg->header >> 8);
    for (size_t i = 0; i < n; i++) {
        buf[pos++] = (uint8_t)(msg->objects[i] & 0xFF);
        buf[pos++] = (uint8_t)((msg->objects[i] >> 8) & 0xFF);
        buf[pos++] = (uint8_t)((msg->objects[i] >> 16) & 0xFF);
        buf[pos++] = (uint8_t)((msg->objects[i] >> 24) & 0xFF);
    }

    buf[pos++] = FUSB302_TKN_JAMCRC;
    buf[pos++] = FUSB302_TKN_EOP;
    buf[pos++] = FUSB302_TKN_TXON;

    /* 一次写完整 token 流到 FIFO 地址 */
    return plat_i2c_write(FUSB302_I2C_ADDR_DEFAULT, FUSB302_REG_FIFOS, buf, pos);
}

/**
 * @brief PD 消息接收（BMC 入口）
 *
 * FIFO 读出次序：1 字节 SOP 类型标识 → 2 字节 Header（小端）→ 对象 → 4 字节 CRC。
 * 开启硬件自动 GoodCRC 后，CRC 校验失败的消息硬件会丢弃/不回 GoodCRC。
 */
#define FUSB302_RX_SOP 0xE0u /* 首字节 SOP 标识（手册核对；SOP'/SOP'' 另有值） */

int fusb302_pd_rx(pd_msg_t *msg, uint8_t *sop)
{
    uint8_t raw[2 + PD_MAX_OBJECTS * 4 + 4];
    uint8_t ndo;
    size_t need;

    if (msg == NULL)
        return -1;

    /* 先读 1 字节判类型，再按 Header 定长读余下（两次读，避免过读） */
    if (plat_i2c_read(FUSB302_I2C_ADDR_DEFAULT, FUSB302_REG_FIFOS, raw, 3) < 0)
        return -1;
    if (sop)
        *sop = raw[0];
    if (raw[0] != FUSB302_RX_SOP)
        return -2; /* 非 SOP 消息（线缆查询等），骨架不处理 */

    msg->header = (uint16_t)(raw[1] | (raw[2] << 8));
    ndo = pd_hdr_nobjects(msg->header);
    if (ndo > PD_MAX_OBJECTS)
        return -3;
    need = ndo * 4u + 4u; /* 对象 + CRC */
    if (need > 0 &&
        plat_i2c_read(FUSB302_I2C_ADDR_DEFAULT, FUSB302_REG_FIFOS,
                      &raw[3], need) < 0)
        return -1;

    for (uint8_t i = 0; i < ndo; i++) {
        size_t off = 3 + (size_t)i * 4u;
        msg->objects[i] = (uint32_t)raw[off] |
                          ((uint32_t)raw[off + 1] << 8) |
                          ((uint32_t)raw[off + 2] << 16) |
                          ((uint32_t)raw[off + 3] << 24);
    }
    return 0;
}
