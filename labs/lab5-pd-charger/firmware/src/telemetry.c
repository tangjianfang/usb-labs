/**
 * @file telemetry.c
 * @brief PD 遥测契约实现（固件↔产测工具，契约版本 1，见 telemetry_contract.h）
 *
 * 产测工具 pd_test.py 在串口上开 2 秒窗口采集 key=value 行；本模块按契约
 * 输出四行（cc_state/contract_v/contract_i/vbus_v），pd_test._telemetry()
 * 的解析器逐字段取首个命中。节流：SNK_READY 下每 500ms 一轮（契约头约定），
 * 状态沿（Unattached→Attached 等）立即 emit 一轮，保证窗口内至少命中一次。
 *
 * VBUS 采样：板级 ADC 不可移植，应用启动时经 pd_telemetry_set_vbus_sampler()
 * 注入采样函数（返回伏特）；未注入时按 0.0 上报（工具侧 vbus_v 仅为参考值，
 * 判定以 contract_v 为准——pd_test 处理器口径）。
 */
#include <stdio.h>

#include "telemetry_contract.h"

static float (*s_vbus_sampler)(void) = 0;

void pd_telemetry_set_vbus_sampler(float (*fn)(void))
{
    s_vbus_sampler = fn;
}

float pd_telemetry_sample_vbus(void)
{
    return s_vbus_sampler ? s_vbus_sampler() : 0.0f;
}

void pd_telemetry_emit(const pd_telemetry_t *t)
{
    if (!t)
        return;
    /* 契约锁定：只加字段不改名；数值格式 %.2f（与 pd_test 解析的 float 口径一致） */
    printf("cc_state=%s\n", t->cc_state ? t->cc_state : "Unknown");
    printf("contract_v=%.2f\n", (double)t->contract_v);
    printf("contract_i=%.2f\n", (double)t->contract_i);
    printf("vbus_v=%.2f\n", (double)t->vbus_v);
}

/* 主会话宿主自检钩（仅 HOST_TELEMETRY_SELFTEST 编译时生效；固件构建不包含） */
#ifdef HOST_TELEMETRY_SELFTEST
int main(void)
{
    pd_telemetry_t t = {"Attached.SNK", 9.0f, 2.25f, 9.02f};
    pd_telemetry_emit(&t);
    return 0;
}
#endif
