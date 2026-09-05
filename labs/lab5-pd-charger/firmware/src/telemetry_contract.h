"""telemetry 契约（固件↔产测工具）：向调试/CDC 口周期打印 key=value 行。
与 tools/usbtest/pd_test.py 的 _telemetry() 采集格式一一对应；只加字段不改名。
嵌入点：PE_SNK Ready 状态（每次协议事件/每 500ms 节拍）与 VBUS ADC 采样处。
契约版本: 1（2026-09-05）
"""
#pragma once
#include <stdint.h>

typedef struct {
    const char *cc_state;   /* "Attached.SNK" / "Attached.SRC" / "Unattached" ... */
    float       contract_v; /* 当前合同电压 V（无合同=0） */
    float       contract_i; /* 当前合同电流 A */
    float       vbus_v;     /* VBUS ADC 实测 */
} pd_telemetry_t;

/* 每次调用输出一行/字段（产测窗口 2s 内至少各出现一次）：
 *   printf("cc_state=%s\n", t.cc_state);
 *   printf("contract_v=%.2f\n", t.contract_v);
 *   printf("contract_i=%.2f\n", t.contract_i);
 *   printf("vbus_v=%.2f\n", t.vbus_v);
 */
void pd_telemetry_emit(const pd_telemetry_t *t);
