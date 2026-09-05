/*
 * cdc_app.c —— CDC 应用层：环形缓冲 + DTR 回调 + LINE_CODING 记录
 *
 * 设计要点（与工业日志采集场景对应）：
 *  1. TX/RX 各一条单生产者-单消费者环形缓冲，关中断窗口只保护指针更新；
 *  2. RX 环满时的策略是"丢最旧、计 dropped"，保证日志流不阻塞 USB 中断
 *     （批量端点天然反压，环形缓冲只是解耦批量包边界与业务处理粒度）；
 *  3. DTR 边沿即"终端打开/关闭"事件：心跳与交互逻辑只在 DTR 置位时活跃，
 *     避免无人在场时向日志文件灌心跳；
 *  4. LINE_CODING 按规范记住并可回读（GET_LINE_CODING），但它对 USB 传输
 *     零影响——见 capture/CDC枚举与数据流.md 与 experience.md 的"波特率无关"。
 */
#include "cdc_app.h"

#include <string.h>

#include "stm32f1xx.h"   /* __disable_irq/__enable_irq（CMSIS）；CubeMX 工程亦可换成 "main.h" */
#include "tusb.h"

static cdc_ring_t s_rx;                 /* 主机 -> 设备 */
static cdc_ring_t s_tx;                 /* 设备 -> 主机 */
static volatile bool s_dtr;
static volatile cdc_dtr_edge_t s_dtr_edge;
static cdc_line_coding_t s_line_coding; /* 供 GET_LINE_CODING 回读 */

/* ---------------- 环形缓冲原语（SPSC） ---------------- */

static void ring_reset(cdc_ring_t *r)
{
    r->head = r->tail = 0;
    r->dropped = 0;
}

static inline uint16_t ring_used(const cdc_ring_t *r)
{
    return (uint16_t)((r->head - r->tail) & (CDC_APP_RING_SIZE - 1));
}

static inline uint16_t ring_free(const cdc_ring_t *r)
{
    /* 保留 1 字节区分满/空 */
    return (uint16_t)(CDC_APP_RING_SIZE - 1 - ring_used(r));
}

static size_t ring_put(cdc_ring_t *r, const uint8_t *data, size_t len, bool drop_oldest)
{
    size_t n = 0;
    while (n < len) {
        uint16_t free_now = ring_free(r);
        if (free_now == 0) {
            if (drop_oldest) {
                /* 丢最旧一字节腾位置：日志场景保新数据 */
                __disable_irq();
                if (ring_free(r) == 0) { r->tail = (uint16_t)((r->tail + 1) & (CDC_APP_RING_SIZE - 1)); r->dropped++; }
                __enable_irq();
                continue;
            }
            break;
        }
        size_t chunk = (len - n < free_now) ? (len - n) : free_now;
        uint16_t head = r->head;
        for (size_t i = 0; i < chunk; i++) {
            r->buf[head] = data[n + i];
            head = (uint16_t)((head + 1) & (CDC_APP_RING_SIZE - 1));
        }
        __disable_irq();
        r->head = head;
        __enable_irq();
        n += chunk;
    }
    return n;
}

static size_t ring_get(cdc_ring_t *r, uint8_t *out, size_t cap)
{
    size_t n = 0;
    uint16_t used = ring_used(r);
    if (cap < used) used = (uint16_t)cap;
    for (n = 0; n < used; n++) {
        out[n] = r->buf[r->tail];
        r->tail = (uint16_t)((r->tail + 1) & (CDC_APP_RING_SIZE - 1));
    }
    return n;
}

/* ---------------- 对外 API ---------------- */

void cdc_app_init(void)
{
    ring_reset(&s_rx);
    ring_reset(&s_tx);
    s_dtr = false;
    s_dtr_edge = CDC_DTR_EDGE_NONE;
    memset(&s_line_coding, 0, sizeof(s_line_coding));
    s_line_coding.bit_rate = 115200; /* 兜底显示值，等主机 SET_LINE_CODING */
}

size_t cdc_app_write(const uint8_t *data, size_t len)
{
    return ring_put(&s_tx, data, len, true);
}

size_t cdc_app_read(uint8_t *out, size_t cap)
{
    return ring_get(&s_rx, out, cap);
}

bool cdc_app_dtr(void)
{
    return s_dtr;
}

cdc_dtr_edge_t cdc_app_take_dtr_edge(void)
{
    __disable_irq();
    cdc_dtr_edge_t e = s_dtr_edge;
    s_dtr_edge = CDC_DTR_EDGE_NONE;
    __enable_irq();
    return e;
}

const cdc_line_coding_t *cdc_app_line_coding(void)
{
    return &s_line_coding;
}

void cdc_app_task(void)
{
    /* TX 环 -> 批量 IN 端点。tud_cdc_write 会返回本次实际排队的字节数，
     * 端点写满时（主机未及时取）自动留待下轮，天然背压。 */
    uint8_t chunk[64];
    size_t n;
    while ((n = ring_get(&s_tx, chunk, sizeof(chunk))) > 0) {
        uint32_t sent = tud_cdc_write(chunk, (uint32_t)n);
        if (sent < n) {
            /* 端点缓冲满：把余量塞回环首（丢最旧策略兜底） */
            ring_put(&s_tx, chunk + sent, n - sent, true);
            break;
        }
    }
    tud_cdc_write_flush();
}

/* ---------------- TinyUSB 回调（由 tud_task() 上下文调用） ---------------- */

/* 主机 -> 设备 收到批量 OUT 数据（TinyUSB 已拆包，这里只管入环） */
void tud_cdc_rx_cb(uint8_t itf)
{
    (void)itf;
    uint8_t chunk[64];
    uint32_t n;
    while ((n = tud_cdc_read(chunk, sizeof(chunk))) > 0) {
        ring_put(&s_rx, chunk, n, true);
    }
}

/* DTR/RTS 翻转：终端 open()/close() 时驱动会翻转 DTR —— 这是"主机在场"信号 */
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)rts;
    bool old = s_dtr;
    s_dtr = dtr;
    if (dtr && !old) {
        s_dtr_edge = CDC_DTR_EDGE_RISE;
    } else if (!dtr && old) {
        s_dtr_edge = CDC_DTR_EDGE_FALL;
        /* 热拔/关闭时清 RX，避免给下一位观者留上一场的命令残片 */
        __disable_irq();
        s_rx.tail = s_rx.head;
        __enable_irq();
    }
}

/* SET_LINE_CODING (bRequest=0x20)：记住参数并回读一致；对 USB 传输本身无影响 */
void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *coding)
{
    (void)itf;
    s_line_coding = *coding;
    /* 若业务依赖"波特率"切换设备行为（如 1200bps 触发复位进 bootloader 的
     * "1200bps touch"），在这里判断 coding->bit_rate 后置标志，
     * 由 main 循环执行——不要在中断回调里直接复位。 */
}
