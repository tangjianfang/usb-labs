/**
 * @file pe_sink.c
 * @brief USB PD Sink 策略引擎（PE_SNK）简化状态机 —— 教学骨架
 *
 * 主干流程：Wait_Source_Cap → Evaluate_Capability → Request → (Accept) → PS_RDY → Ready
 *
 * 规范依据与诚实声明：
 *  - 状态名/迁移条件对应 USB PD 3.1 规范第 8 章 Policy Engine 的 Sink 状态集
 *    （PE_SNK_*）。章节号按 PD 3.1 版本书写（如 §8.3.x），**跨版本（2.0/3.0/3.1/3.2
 *    ECN）小节编号会移动**，落地时以你手头规范原文为准；状态名是稳定锚点。
 *  - 定时器取值引自规范 PD Timers 表（转引自知识库 07-USBPD深入"实现者清单"）。
 *  - 骨架假设底层为 fusb302.c + FUSB302 硬件自动 GoodCRC（MessageID 去重、
 *    重传由 TCPC 硬件完成）；正式实现仍需自行维护每方向 MsgID 计数。
 *  - 未实现：Hard Reset 完整分支、PPS 保活循环、扩展消息分块、Get_Source_Cap
 *    应答、EPR 模式。商用请用 Linux TCPM 或厂商已认证协议栈（见 firmware/README.md）。
 */

#include "fusb302.h"
#include <string.h>

/* ==========================================================================
 * 类型与上下文
 * ========================================================================== */
typedef enum {
    /* 状态名对齐规范 PE_SNK_* 命名，见各 case 内注释 */
    SNK_STARTUP,          /* 对应 PE_SNK_Startup / PE_SNK_Discovery 一带 */
    SNK_WAIT_FOR_CAP,     /* PE_SNK_Wait_for_Capabilities */
    SNK_EVALUATE_CAP,     /* PE_SNK_Evaluate_Capability */
    SNK_SELECT_CAP,       /* PE_SNK_Select_Capability（已发 Request，等 Accept/Reject） */
    SNK_TRANSITION_SINK,  /* PE_SNK_Transition_Sink（已 Accept，等 PS_RDY） */
    SNK_READY,            /* PE_SNK_Ready（Explicit Contract） */
    SNK_HARD_RESET,       /* PE_SNK_Hard_Reset（骨架仅复位状态机） */
} pe_sink_state_t;

/* PE → DPM/应用 的契约回调：established=1 契约生效，0=契约失效 */
typedef void (*pe_contract_cb_t)(uint16_t mv, uint32_t ma, bool established);

typedef struct {
    pe_sink_state_t state;

    /* 简易毫秒定时器（正式实现用硬件定时器） */
    uint32_t timer_ms;
    bool timer_running;

    /* 协议变量 */
    uint8_t  tx_msgid;          /* 发送 MessageID（0~7 循环） */
    pd_msg_t src_caps;          /* 最近一次收到的 Source_Capabilities */
    bool     have_caps;

    /* DPM 意愿：目标电压/期望电流（应用经 pe_sink_request_voltage 设置） */
    uint16_t dpm_req_mv;
    uint32_t dpm_req_ma;

    /* 协商结果 */
    uint8_t  sel_index;         /* 选中的 PDO 序号（1 起始，对应 RDO ObjPos） */
    uint16_t sel_mv;
    uint32_t sel_ma;
    bool     contract_active;   /* 契约状态沿（edge）上报用 */

    pe_contract_cb_t on_contract;
    void (*log)(const char *s);
} pe_sink_ctx_t;

/* ==========================================================================
 * PD Timers（规范 PD Timers 表速查；取值出处：知识库 07 文）
 *   tTypeCSendSourceCap 100~200 ms : Source 重播 Source_Cap 的间隔
 *                                    （Sink 侧等待窗口取其上限再加裕量）
 *   SinkRequestTimer    约 24~30 ms: 评估并发出 Request 的应答窗口（手册核对/以规范为准）
 *   tSenderResponse     24~30 ms   : 等 Accept/Reject 的窗口
 *   tPSTransition       450~550 ms : Source 切换电源的窗口（PS_RDY 前电压未保证）
 *   tNoResponseTimer    4.5~5.5 s  : 总无应答超时（骨架未实现）
 * ========================================================================== */
#define T_WAIT_CAP_MS        260u  /* > tTypeCSendSourceCap 上限 + 裕量 */
#define T_SINK_REQUEST_MS     30u
#define T_SENDER_RESPONSE_MS  30u
#define T_PS_TRANSITION_MS   550u

/* ==========================================================================
 * 工具
 * ========================================================================== */
static void pe_log(pe_sink_ctx_t *c, const char *s) { if (c->log) c->log(s); }

static void pe_timer_start(pe_sink_ctx_t *c, uint32_t ms)
{
    c->timer_ms = ms;
    c->timer_running = true;
}

static bool pe_timer_expired(pe_sink_ctx_t *c, uint32_t dt_ms)
{
    if (!c->timer_running)
        return false;
    c->timer_ms = (c->timer_ms > dt_ms) ? (c->timer_ms - dt_ms) : 0;
    return c->timer_ms == 0;
}

static int pe_send(pe_sink_ctx_t *c, pd_msg_type_t type, uint8_t nobj,
                   const uint32_t *objs)
{
    pd_msg_t m;
    /* Sink 角色（PPR=0）默认 UFP（PDR=0）；Rev 按 PD3 起谈，双方取共同版本 */
    m.header = pd_make_header(type, nobj, c->tx_msgid, false, PD_HDR_REV_PD3);
    c->tx_msgid = (uint8_t)((c->tx_msgid + 1u) & 0x7u);
    if (nobj)
        memcpy(m.objects, objs, nobj * sizeof(uint32_t));
    return fusb302_pd_tx(&m);
}

/* ==========================================================================
 * PDO 评估（PD §6.4.1 Fixed PDO；选择策略属于 DPM，此处取"最接近目标电压的
 * 固定档"。PPS/AVS/APDO 选择为 TODO。）
 * ========================================================================== */
static bool pe_evaluate_caps(pe_sink_ctx_t *c)
{
    uint8_t n = pd_hdr_nobjects(c->src_caps.header);
    uint8_t best = 0;
    int32_t best_diff = -1;

    if (n == 0)
        return false;

    for (uint8_t i = 0; i < n; i++) { /* 对象 0 恒为 5V 固定档，同样参与候选 */
        uint32_t pdo = c->src_caps.objects[i];
        if ((pdo >> 30) != 0u)      /* 只处理 Fixed（B31:30=00），APDO 见 TODO */
            continue;
        int32_t diff = (int32_t)pdo_fixed_mv(pdo) - (int32_t)c->dpm_req_mv;
        if (diff < 0) diff = -diff;
        if (best_diff < 0 || diff < best_diff) {
            best_diff = diff;
            best = (uint8_t)(i + 1u); /* RDO ObjPos 从 1 计数 */
        }
    }
    if (best == 0)
        return false;

    {
        uint32_t pdo = c->src_caps.objects[best - 1u];
        uint32_t max_ma = pdo_fixed_max_ma(pdo);
        uint32_t op_ma = (c->dpm_req_ma < max_ma) ? c->dpm_req_ma : max_ma;

        c->sel_index = best;
        c->sel_mv = (uint16_t)pdo_fixed_mv(pdo);
        c->sel_ma = op_ma;
        /* RDO 在 pe_send_request() 构造（PD §6.5.1 / 知识库 08 文第五节）：
         * 电压不出现在 RDO 中，由 ObjPos 指向的 PDO 决定。 */
        return true;
    }
}

static int pe_send_request(pe_sink_ctx_t *c)
{
    uint32_t pdo = c->src_caps.objects[c->sel_index - 1u];
    uint32_t max_ma = pdo_fixed_max_ma(pdo);
    uint32_t rdo = ((uint32_t)c->sel_index << RDO_OBJ_POS_SHIFT) |
                   ((c->sel_ma / 10u) << RDO_OP_CURR_SHIFT) | /* 10mA/LSB */
                   (max_ma / 10u) |
                   RDO_NO_USB_SUSPEND; /* 不接受挂起：教学板常置位 */
    return pe_send(c, PD_DATA_REQUEST, 1, &rdo);
}

/* ==========================================================================
 * 状态机驱动
 * ========================================================================== */
void pe_sink_init(pe_sink_ctx_t *c, pe_contract_cb_t cb, void (*log)(const char *))
{
    memset(c, 0, sizeof(*c));
    c->state = SNK_STARTUP;
    c->dpm_req_mv = 20000; /* 默认诉求：20V（DPM 策略可改） */
    c->dpm_req_ma = 3000;
    c->on_contract = cb;
    c->log = log;
}

/** 收到一条 PD 消息（由主循环/中断调 fusb302_pd_rx 后送入） */
void pe_sink_on_rx(pe_sink_ctx_t *c, const pd_msg_t *m)
{
    if (m == NULL)
        return;

    switch (pd_hdr_type(m->header)) {
    case PD_DATA_SOURCE_CAP:
        c->src_caps = *m;
        c->have_caps = true;
        if (c->state == SNK_WAIT_FOR_CAP || c->state == SNK_READY)
            c->state = SNK_EVALUATE_CAP; /* 契约成立后收到新 Cap = 重协商 */
        break;

    case PD_CTRL_ACCEPT:
        if (c->state == SNK_SELECT_CAP)
            c->state = SNK_TRANSITION_SINK;
        break;

    case PD_CTRL_REJECT:
    case PD_CTRL_WAIT: /* Wait 后可重试，骨架简化为回等待 */
        if (c->state == SNK_SELECT_CAP || c->state == SNK_TRANSITION_SINK)
            c->state = SNK_WAIT_FOR_CAP;
        break;

    case PD_CTRL_PS_RDY:
        if (c->state == SNK_TRANSITION_SINK)
            c->state = SNK_READY;
        break;

    case PD_CTRL_GET_SOURCE_CAP:
        /* Sink 无源能力可报：PD 3.0+ 应答 Not_Supported（骨架 TODO） */
        pe_log(c, "pe: GET_SOURCE_CAP not supported\r\n");
        break;

    default:
        break; /* GoodCRC 由硬件处理，其余消息骨架忽略 */
    }
}

/** 主循环轮询：dt_ms 为距上次调用的毫秒数 */
void pe_sink_run(pe_sink_ctx_t *c, uint32_t dt_ms)
{
    switch (c->state) {
    /* --------------------------------------------------------------
     * PE_SNK_Startup / PE_SNK_Discovery（PD 3.1 §8.3 起始状态）：
     * 保持 Rd 端接，等 VBUS 出现（vSafe5V 检测窗）；VBUS 在即转入等 Cap。
     * -------------------------------------------------------------- */
    case SNK_STARTUP: {
        fusb302_cc_status_t st;
        if (fusb302_get_cc(&st) == 0 && st.vbus_ok) {
            c->state = SNK_WAIT_FOR_CAP;
            pe_timer_start(c, T_WAIT_CAP_MS);
        }
        break;
    }

    /* --------------------------------------------------------------
     * PE_SNK_Wait_for_Capabilities：等 Source_Capabilities。
     * Source 在 attach 后 tTypeCSendSourceCap（100~200ms）内首播，
     * 未获 Request 会在 nCapsCount（上限 50）内重播；Sink 侧超时
     * 正常路径是发 Hard Reset（骨架仅回退重等——完整分支 TODO）。
     * -------------------------------------------------------------- */
    case SNK_WAIT_FOR_CAP:
        if (pe_timer_expired(c, dt_ms)) {
            pe_log(c, "pe: wait cap timeout\r\n");
            pe_timer_start(c, T_WAIT_CAP_MS);
        }
        break;

    /* --------------------------------------------------------------
     * PE_SNK_Evaluate_Capability：按 DPM 意愿与 Power Rule 评估 PDO，
     * 须在 SinkRequestTimer（约 24~30ms，以规范为准）内发 Request。
     * -------------------------------------------------------------- */
    case SNK_EVALUATE_CAP:
        if (!c->have_caps || !pe_evaluate_caps(c)) {
            c->state = SNK_WAIT_FOR_CAP;
            pe_timer_start(c, T_WAIT_CAP_MS);
            break;
        }
        if (pe_send_request(c) == 0) {
            c->state = SNK_SELECT_CAP;
            pe_timer_start(c, T_SENDER_RESPONSE_MS);
        }
        break;

    /* --------------------------------------------------------------
     * PE_SNK_Select_Capability：Request 已发，等 Accept/Reject，
     * 窗口 tSenderResponse（24~30ms）。超时 → Hard Reset（骨架回退）。
     * -------------------------------------------------------------- */
    case SNK_SELECT_CAP:
        if (pe_timer_expired(c, dt_ms)) {
            pe_log(c, "pe: sender response timeout\r\n");
            c->state = SNK_WAIT_FOR_CAP;
        }
        break;

    /* --------------------------------------------------------------
     * PE_SNK_Transition_Sink：Accept 后等 PS_RDY；Source 在
     * tPSTransition（450~550ms）内完成电压调整。
     * **PS_RDY 之前不得假设 VBUS 已切到新电压**（契约生效时点）。
     * -------------------------------------------------------------- */
    case SNK_TRANSITION_SINK:
        if (pe_timer_expired(c, dt_ms + T_SENDER_RESPONSE_MS)) {
            pe_log(c, "pe: ps_rdy timeout\r\n");
            c->state = SNK_WAIT_FOR_CAP;
        }
        break;

    /* --------------------------------------------------------------
     * PE_SNK_Ready：Explicit Contract 成立（知识库 07 文第九节）。
     * 此后可：响应新 Source_Cap 重协商 / 主动 Get_Source_Cap（TODO）/
     * PPS 保活循环（TODO）。应用经 pe_sink_request_voltage() 改诉求后
     * 回 EVALUATE 重新请求（新契约覆盖旧契约）。
     * -------------------------------------------------------------- */
    case SNK_READY:
        break;

    /* --------------------------------------------------------------
     * PE_SNK_Hard_Reset → PE_SNK_Transition_to_default：VBUS 回
     * vSafe0V 再回 5V，契约作废，状态机复位（完整时序 TODO）。
     * -------------------------------------------------------------- */
    case SNK_HARD_RESET:
    default:
        fusb302_set_sink_terminations(true);
        c->have_caps = false;
        c->state = SNK_STARTUP;
        break;
    }

    bool now_active = (c->state == SNK_READY);
    if (c->on_contract && now_active != c->contract_active) {
        c->contract_active = now_active;
        c->on_contract(c->sel_mv, c->sel_ma, now_active);
    }
}

/** DPM 接口：应用层改诉求（如"我要 9V"），在契约态触发重协商 */
int pe_sink_request_voltage(pe_sink_ctx_t *c, uint16_t mv, uint32_t ma)
{
    c->dpm_req_mv = mv;
    c->dpm_req_ma = ma;
    if (c->state == SNK_READY) {
        /* PD 允许 Sink 随时用现有 Source_Cap 重发 Request（新契约覆盖旧契约） */
        c->state = SNK_EVALUATE_CAP;
    }
    return 0;
}
