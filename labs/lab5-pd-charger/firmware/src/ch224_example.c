/**
 * @file ch224_example.c
 * @brief 路线 A：CH224 系列（南京沁恒 WCH）PD Sink 诱骗芯片使用示例
 *
 * 器件分工（见 README.md 选型表）：
 *   - CH224K ：CFG1/CFG2/CFG3 三引脚组合选择目标电压档（5/9/12/15/20V），
 *              无 I²C；PG（Power Good）引脚指示协商成功。上电即工作，零固件。
 *   - CH224M / CH224Q：在引脚配置之外提供 I²C，可读取协商状态/切换档位，
 *              适合自动化测试或运行时改档的产品。
 *
 * 诚实声明（重要）：
 *   1. CH224K 的 CFG 组合表、CH224M/CH224Q 的 I²C 从机地址与寄存器位定义，
 *      **以沁恒官网最新数据手册为准**（https://www.wch.cn 检索 CH224）。
 *      本示例只给出调用骨架与外设约定，标注 `手册核对` 处不承诺具体数值，
 *      不同封装/版本可能不同。
 *   2. CH224 系列是 Sink（诱骗）角色，**没有对应的 Source 版**；做充电器
 *      请选 Source 协议芯片（README.md 选型表后两行）。
 *
 * 平台钩子与 fusb302.c 相同：plat_i2c_read / plat_delay_ms，
 * 以及 CFG/PG 的 GPIO 读写（plat_gpio_*，由移植者实现）。
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* 平台钩子（与 fusb302.c 共用一套 HAL 约定） */
extern int  plat_i2c_read(uint8_t dev_addr, uint8_t reg, uint8_t *buf, size_t len);
extern void plat_delay_ms(uint32_t ms);
extern void plat_gpio_write(uint8_t pin, int level);   /* 0=低 1=高 2=悬空(高阻) */
extern int  plat_gpio_read(uint8_t pin);

/* ==========================================================================
 * CH224K：纯引脚方案（无 I²C）
 * ========================================================================== */
typedef enum {
    CH224_TARGET_5V = 5,
    CH224_TARGET_9V = 9,
    CH224_TARGET_12V = 12,
    CH224_TARGET_15V = 15,
    CH224_TARGET_20V = 20,
} ch224_target_v_t;

#define CFG1_PIN 0
#define CFG2_PIN 1
#define CFG3_PIN 2
#define PG_PIN   3

/**
 * @brief 设置 CH224K 的目标档位（CFG1/CFG2/CFG3 组合）
 * @note  组合表见数据手册（部分档位可能用到"悬空"电平，故用三态 GPIO）。
 *        本函数故意留 TODO：**不要凭社区转述抄组合**，以手册为准——
 *        档位抄错会静默协商到错误电压，是最常见的"诱骗板翻车"原因。
 */
int ch224k_gpio_apply(ch224_target_v_t v)
{
    int h, l, f = 2;
    (void)f;
    switch (v) {
    /* TODO(手册核对)：按沁恒数据手册填 CFG1/CFG2/CFG3 组合 */
    case CH224_TARGET_5V:  h = 1; l = 0; break; /* 占位，勿用 */
    case CH224_TARGET_9V:  h = 1; l = 0; break;
    case CH224_TARGET_12V: h = 1; l = 0; break;
    case CH224_TARGET_15V: h = 1; l = 0; break;
    case CH224_TARGET_20V: h = 1; l = 0; break;
    default: return -1;
    }
    plat_gpio_write(CFG1_PIN, h);
    plat_gpio_write(CFG2_PIN, l);
    plat_gpio_write(CFG3_PIN, h);
    plat_delay_ms(10); /* 允许芯片重新发起请求（时序见手册，手册核对） */
    return 0;
}

/** PG 引脚 = 协商成功指示（Power Good）。读电平即可，零 I²C。 */
bool ch224k_power_good(void)
{
    return plat_gpio_read(PG_PIN) == 1;
}

/* ==========================================================================
 * CH224M / CH224Q：I²C 读状态骨架
 * ========================================================================== */
#define CH224M_I2C_ADDR 0x50u /* 7 位地址示例——**手册核对**，勿直接使用 */
/* 寄存器地址/位定义：见沁恒数据手册（手册核对）。下方结构体只给"想要什么" */
#define CH224M_REG_STATUS 0x10u /* 占位地址——手册核对 */

typedef struct {
    bool     pd_active;    /* 是否已建立 PD 契约（对应 PG 概念） */
    uint16_t active_mv;    /* 当前锁定档位电压 */
} ch224_status_t;

int ch224m_read_status(ch224_status_t *out)
{
    uint8_t st = 0;
    if (out == NULL)
        return -1;
    if (plat_i2c_read(CH224M_I2C_ADDR, CH224M_REG_STATUS, &st, 1) < 0)
        return -1;

    /* TODO(手册核对)：按数据手册位域解析。示例仅示意解析思路。 */
    out->pd_active = (st & 0x80u) != 0;
    out->active_mv = 5000u * (1u + (st & 0x07u)); /* 占位换算，勿用 */
    return 0;
}

/* ==========================================================================
 * 演示：手头充电器档位摸底（对应 host/pd_test.md 的第 0 步）
 * ========================================================================== */
void ch224_demo_run(void)
{
    static const ch224_target_v_t ladder[] = {
        CH224_TARGET_5V, CH224_TARGET_9V, CH224_TARGET_12V,
        CH224_TARGET_15V, CH224_TARGET_20V,
    };

    for (unsigned i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
        ch224k_gpio_apply(ladder[i]);
        plat_delay_ms(300); /* 等协商稳定（经验值，量程见手册） */
        /* 此处接万用表/USB 测试仪读 VBUS 实际电压，
         * PG=1 表示该充电器支持此档并已锁定；记录表模板见 host/pd_test.md */
        (void)ch224k_power_good();
    }
}
