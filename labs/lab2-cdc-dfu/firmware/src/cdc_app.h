/*
 * cdc_app.h —— CDC 应用层：环形缓冲 + DTR 事件 + LINE_CODING 记录
 * USB-Labs Lab2：CDC 虚拟串口 + DFU 固件升级
 */
#ifndef _CDC_APP_H_
#define _CDC_APP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tusb.h"  /* 复用 TinyUSB 的 cdc_line_coding_t（bit_rate/stop_bits/parity/data_bits），勿重复自定义 */

/* 环形缓冲大小：工业日志场景建议 >= 2 个批量端点包 (2 x 64B)，留整数倍减少取模 */
#define CDC_APP_RING_SIZE 1024U

typedef struct {
    uint8_t  buf[CDC_APP_RING_SIZE];
    volatile uint16_t head;   /* 写指针 */
    volatile uint16_t tail;   /* 读指针 */
    volatile uint32_t dropped;/* 溢出丢弃计数（观测指标） */
} cdc_ring_t;

typedef enum {
    CDC_DTR_EDGE_NONE = 0,
    CDC_DTR_EDGE_RISE,        /* 主机打开串口（DTR 0->1） */
    CDC_DTR_EDGE_FALL         /* 主机关闭串口（DTR 1->0） */
} cdc_dtr_edge_t;

void cdc_app_init(void);
/* 应用向主机方向写（入 TX 环，cdc_app_task 里择机搬运到端点）。返回实际入队字节数 */
size_t cdc_app_write(const uint8_t *data, size_t len);
/* 主机方向读（出 RX 环） */
size_t cdc_app_read(uint8_t *out, size_t cap);
/* 周期任务：搬 TX 环 -> IN 端点；在 main 循环里与 tud_task() 并列调用 */
void cdc_app_task(void);
/* 当前 DTR 状态与最近一次边沿（读后清零） */
bool cdc_app_dtr(void);
cdc_dtr_edge_t cdc_app_take_dtr_edge(void);
/* 主机最后一次请求的线路编码（GET_LINE_CODING 必须可回读同值） */
const cdc_line_coding_t *cdc_app_line_coding(void);

#endif /* _CDC_APP_H_ */
