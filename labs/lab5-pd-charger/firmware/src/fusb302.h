/**
 * @file fusb302.h
 * @brief onsemi FUSB302（Type-C TCPC / PD PHY）驱动骨架 —— 寄存器与公共 PD 定义
 *
 * 诚实声明：
 *  - 本文件是教学骨架。寄存器地址按 onsemi FUSB302B 数据手册寄存器表与
 *    Linux 内核驱动 drivers/usb/typec/tcpm/fusb302.c 交叉整理；**位域命名与
 *    个别取值在不同手册版本间有过调整，落地前以你手头器件的数据手册为准**
 *    （代码中不确定处已用 `/* 手册核对 *` 标注）。
 *  - GoodCRC 应答 / MessageID / 重传建议启用 FUSB302 硬件自动模式，
 *    软件不要重造该层（tReceive 窗口 0.9~1.1 ms，慢速 MCU 软件做不完）。
 *
 * 知识库：07-USBPD深入-状态机与消息全表（消息类型/Header 位域出处）
 */
#ifndef FUSB302_H
#define FUSB302_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ==========================================================================
 * I²C 与器件识别
 * ========================================================================== */
#define FUSB302_I2C_ADDR_DEFAULT 0x22u /* 7 位地址；ADDR 引脚电平决定 0x22/0x23，手册核对 */

#define FUSB302_REG_DEVICE_ID 0x01u
/* DeviceID 高 4 位为器件族（0x8x/0x9x 段在不同批次/版本上均出现过，手册核对） */
#define FUSB302_DEVICE_ID_FAMILY_MASK 0xF0u

/* ---- 寄存器表（0x02~0x0F 控制组 / 0x40~0x43 状态与 FIFO 组） ---- */
#define FUSB302_REG_SWITCHES0 0x02u /* CC 测量选择 / Rd 端接（PDWN）使能 */
#define FUSB302_REG_SWITCHES1 0x03u /* UFP/DFP/VCONN 开关；AUTO_CRC 使能位在此 */
#define FUSB302_REG_MEASURE   0x04u /* MDAC 比较阈值（CC 电压测量） */
#define FUSB302_REG_SLICE     0x05u /* SDAC 阈值（BMC 接收 sliced） */
#define FUSB302_REG_CONTROL0  0x06u /* HOST_CUR 电流源档位 / TX_FLUSH */
#define FUSB302_REG_CONTROL1  0x07u /* EN_SOPx / RX_FLUSH */
#define FUSB302_REG_CONTROL2  0x08u /* AUTO_RETRY 次数、唤醒等 */
#define FUSB302_REG_CONTROL3  0x09u /* N_RETRIES、AUTO_SOFT/HARDRESET */
#define FUSB302_REG_MASK      0x0Au
#define FUSB302_REG_POWER     0x0Bu /* 上电域：bandgap/振荡器/测量/接收 */
#define FUSB302_REG_RESET     0x0Cu /* SW_RESET */
#define FUSB302_REG_MASKA     0x0Du /* 手册核对：Mask A/B 与 Control4 随版本有差异 */
#define FUSB302_REG_MASKB     0x0Eu
#define FUSB302_REG_CONTROL4  0x0Fu

#define FUSB302_REG_STATUS0   0x40u /* BC_LVL、比较器输出 COMP、VBUSOK */
#define FUSB302_REG_STATUS1   0x41u /* 接收活动/碰撞等 */
#define FUSB302_REG_INTERRUPT 0x42u /* 中断标志（读清除） */
#define FUSB302_REG_FIFOS     0x43u /* TX/RX FIFO 同地址，方向区分 */

/* ---- 常用位（名称即含义；bit 位置以数据手册为准） ---- */
#define FUSB302_POWER_ALL 0x0Fu /* bandgap|osc|measure|receiver 全开 */
#define FUSB302_RESET_SW_RESET 0x01u

#define FUSB302_SWITCHES1_AUTO_CRC 0x04u /* 硬件自动 GoodCRC（强烈建议开启） */

#define FUSB302_CONTROL0_HOST_CUR_MASK 0x0Cu /* bits[3:2]：00=0uA 01=80uA(Default) 10=180uA(1.5A) 11=330uA(3.0A) */
#define FUSB302_CONTROL0_HOST_CUR_SHIFT 2
#define FUSB302_CONTROL0_HOST_CUR(n) (((n) & 0x3u) << FUSB302_CONTROL0_HOST_CUR_SHIFT)

#define FUSB302_STATUS0_COMP   0x20u /* CC 比较器输出：1=CC 电压高于 MDAC 阈值 */
#define FUSB302_STATUS0_VBUSOK 0x40u

/* ---- PHY 发送 token（数据手册 PD Transmitter 章节；取值与内核驱动一致，手册核对） ---- */
#define FUSB302_TKN_TXON    0xA1u
#define FUSB302_TKN_TXOFF   0xFEu
#define FUSB302_TKN_SYNC1   0x12u /* SOP: SYNC1,SYNC1,SYNC1,SYNC2 */
#define FUSB302_TKN_SYNC2   0x14u
#define FUSB302_TKN_SYNC3   0x16u /* SOP' / SOP'' 组合见手册 */
#define FUSB302_TKN_RST1    0x1Au /* Hard Reset / Cable Reset 前导 */
#define FUSB302_TKN_RST2    0x22u
#define FUSB302_TKN_PACKSYM 0x80u /* 低 5 位 = 后续数据字节数 >> 2 */
#define FUSB302_TKN_JAMCRC  0xFFu /* 令硬件接管 CRC 计算 */
#define FUSB302_TKN_EOP     0xE1u

/* ==========================================================================
 * PD 公共定义（与芯片无关，pe_sink.c 复用）
 * 出处：USB PD 规范 §6.2（Header）/ §6.4（PDO）/ §6.5（RDO）；
 *       详见知识库 07-USBPD深入 第四节、08-USBPD消息全表
 * ========================================================================== */

/* 消息类型（4:0 位）—— 仅列骨架用到的 */
typedef enum {
    /* 控制消息 */
    PD_CTRL_GOODCRC       = 1,
    PD_CTRL_ACCEPT        = 3,
    PD_CTRL_REJECT        = 4,
    PD_CTRL_WAIT          = 5,   /* PD Table 6-2 Control type（骨架 pe_sink.c 引用，原缺失） */
    PD_CTRL_PS_RDY        = 6,
    PD_CTRL_GET_SOURCE_CAP = 7,
    PD_CTRL_SOFT_RESET    = 13,
    PD_CTRL_NOT_SUPPORTED = 16, /* PD 3.0+ */
    /* 数据消息 */
    PD_DATA_SOURCE_CAP    = 1,
    PD_DATA_REQUEST       = 2,
    PD_DATA_SINK_CAP      = 4,
} pd_msg_type_t;

/* Header 16 位位段（PD 3.x） */
#define PD_HDR_EXT_MASK      0x8000u /* bit15 扩展消息 */
#define PD_HDR_NDO_SHIFT     12u     /* bit14:12 数据对象个数(0~7) */
#define PD_HDR_NDO_MASK      0x7000u
#define PD_HDR_MSGID_SHIFT   9u      /* bit11:9 MessageID(0~7 循环) */
#define PD_HDR_MSGID_MASK    0x0E00u
#define PD_HDR_PPR_BIT       0x0100u /* bit8 电源角色(SOP)：0=Sink 1=Source */
#define PD_HDR_REV_SHIFT     6u      /* bit7:6 规范版本 01=PD2.0 10=PD3.x */
#define PD_HDR_REV_MASK      0x00C0u
#define PD_HDR_REV_PD2       0x1u
#define PD_HDR_REV_PD3       0x2u
#define PD_HDR_PDR_BIT       0x0020u /* bit5 数据角色：0=UFP 1=DFP */
#define PD_HDR_TYPE_MASK     0x001Fu

/* RDO（Fixed Supply Request Data Object，PD §6.5.1 / 知识库 08 文第五节） */
#define RDO_OBJ_POS_SHIFT    28u /* bit31:28 所选 PDO 序号 1~7 */
#define RDO_GIVE_BACK        (1u << 27)
#define RDO_CAP_MISMATCH     (1u << 26)
#define RDO_USB_COMM         (1u << 25)
#define RDO_NO_USB_SUSPEND   (1u << 24)
#define RDO_OP_CURR_SHIFT    10u /* bit19:10 工作电流，10mA/LSB */
#define RDO_OP_CURR_MASK     0x3FF00u
#define RDO_MAX_CURR_MASK    0x3FFu /* bit9:0 最大电流，10mA/LSB */

/* Fixed PDO（PD §6.4.1 / 知识库 08 文第六节） */
#define PDO_TYPE_FIXED       (0u << 30)
#define PDO_DUAL_ROLE_POWER  (1u << 29)
#define PDO_USB_SUSPEND      (1u << 28)
#define PDO_EXTERNALLY_POWER (1u << 27)
#define PDO_USB_COMM         (1u << 26)
#define PDO_DUAL_ROLE_DATA   (1u << 25)
#define PDO_VOLTAGE_SHIFT    10u /* bit19:10 电压，50mV/LSB */
#define PDO_VOLTAGE_MASK     0x3FF00u
#define PDO_MAX_CURR_MASK    0x3FFu /* bit9:0 最大电流，10mA/LSB */

#define PD_MAX_OBJECTS 7u

typedef struct {
    uint16_t header;              /* PD Header */
    uint32_t objects[PD_MAX_OBJECTS]; /* 0~7 个 32 位数据对象 */
} pd_msg_t;

static inline uint8_t pd_hdr_nobjects(uint16_t h)  { return (h & PD_HDR_NDO_MASK) >> PD_HDR_NDO_SHIFT; }
static inline uint8_t pd_hdr_msgid(uint16_t h)     { return (h & PD_HDR_MSGID_MASK) >> PD_HDR_MSGID_SHIFT; }
static inline uint8_t pd_hdr_type(uint16_t h)      { return (h & PD_HDR_TYPE_MASK); }
static inline uint8_t pd_hdr_rev(uint16_t h)       { return (h & PD_HDR_REV_MASK) >> PD_HDR_REV_SHIFT; }

/* 固定 PDO 解码辅助 */
static inline uint32_t pdo_fixed_mv(uint32_t pdo) { return ((pdo & PDO_VOLTAGE_MASK) >> PDO_VOLTAGE_SHIFT) * 50u; }
static inline uint32_t pdo_fixed_max_ma(uint32_t pdo) { return (pdo & PDO_MAX_CURR_MASK) * 10u; }

/* 构造 Header（教学骨架用；真实实现需按规范填角色/版本并维护 MsgID） */
static inline uint16_t pd_make_header(pd_msg_type_t type, uint8_t nobj, uint8_t msgid,
                                      bool is_source_role, uint8_t rev)
{
    return (uint16_t)(((nobj & 0x7u) << PD_HDR_NDO_SHIFT) |
                      ((msgid & 0x7u) << PD_HDR_MSGID_SHIFT) |
                      (is_source_role ? PD_HDR_PPR_BIT : 0u) |
                      ((rev & 0x3u) << PD_HDR_REV_SHIFT) |
                      (uint16_t)type);
}

/* ==========================================================================
 * CC 状态
 * ========================================================================== */
typedef enum {
    FUSB302_CC_OPEN = 0, /* 未连接 */
    FUSB302_CC_RA,       /* 有源线缆 VCONN 负载（800~1200Ω 特征） */
    FUSB302_CC_RP_DEF,   /* 对端 Rp：Default USB Power */
    FUSB302_CC_RP_1_5,   /* 对端 Rp：1.5 A 宣告 */
    FUSB302_CC_RP_3_0,   /* 对端 Rp：3.0 A 宣告 */
    FUSB302_CC_RD,       /* 对端 Rd（说明对端是 Sink/自测） */
} fusb302_cc_level_t;

typedef struct {
    fusb302_cc_level_t cc1;
    fusb302_cc_level_t cc2;
    bool vbus_ok;   /* VBUS 存在（> vSafe5V 检测窗下限，阈值见手册） */
} fusb302_cc_status_t;

/* ==========================================================================
 * 驱动 API（详见 fusb302.c）
 * ========================================================================== */
int  fusb302_init(void);                          /* 复位+上电+自动 GoodCRC 配置 */
int  fusb302_read_device_id(uint8_t *id);         /* 设备 ID 读取 */
int  fusb302_set_sink_terminations(bool enable);  /* Sink 角色：双侧挂 Rd 端接 */
int  fusb302_get_cc(fusb302_cc_status_t *st);     /* CC 状态测量（HOST_CUR+MDAC 法） */
int  fusb302_flush_fifos(void);
int  fusb302_pd_tx(const pd_msg_t *msg);          /* BMC 发送入口（token 流+FIFO） */
int  fusb302_pd_rx(pd_msg_t *msg, uint8_t *sop);  /* BMC 接收入口（读 FIFO，硬件已剥离 CRC） */

#endif /* FUSB302_H */
