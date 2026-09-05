/* Lab3 MSC —— 固件入口
 *
 * 职责：时钟/stdio 初始化 → 块设备后端初始化 → TinyUSB 初始化 → 主循环
 *   主循环三件事：
 *     1) tud_task()          —— TinyUSB 设备栈事件泵（BOT 状态机、MSC 回调都在这里跑）
 *     2) msc_app_task()      —— TF 卡热插拔监测（去抖 + 重新初始化）
 *     3) LED 心跳/IO 指示    —— 模拟 U 盘读写灯
 *
 * 日志走 UART（GPIO0=TX, GPIO1=RX, 115200-8N1）。
 * 注意：本工程禁止同时启用 stdio_usb（会与 TinyUSB 抢 RP2040 USB 控制器），
 *       CMakeLists.txt 已强制 pico_enable_stdio_usb(... 0)。
 */
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "tusb.h"

#include "sd_spi.h"

#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 25
#endif

/* 来自 msc_app.c */
extern volatile uint32_t msc_io_ticks;
extern void msc_app_init(void);
extern void msc_app_task(void);
extern void msc_app_flush(void);

int main(void) {
    stdio_init_all();                       /* UART0 日志口 */
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

#ifdef MSC_USE_SD_CARD
    printf("\n[lab3-msc] boot: backend = SPI TF card (spi%d, SCK=%d CS=%d CD=%d)\n",
           SD_SPI_PORT == spi1 ? 1 : 0, SD_SPI_SCK_PIN, SD_SPI_CS_PIN, SD_CD_PIN);
#else
    printf("\n[lab3-msc] boot: backend = flash ramdisk\n");
#endif

    msc_app_init();                         /* 后端初始化（SD 只装检测脚，卡初始化在主循环） */

    /* SD 后端：插着卡就先初始化，让 MSC 枚举时容量即已就绪。
     * 若卡初始化失败也不阻塞：主机稍后会看到 NOT READY/3Ah（空读卡器行为）。 */
#if defined(MSC_USE_SD_CARD)
    if (sd_card_present() && sd_spi_init())
        printf("[lab3-msc] SD ready, %u sectors (%u MB)\n",
               (unsigned) sd_spi_sector_count(),
               (unsigned) ((uint64_t) sd_spi_sector_count() * 512u >> 20));
    else
        printf("[lab3-msc] SD not ready (empty reader is OK)\n");
#endif

    tusb_init();                            /* TinyUSB 设备栈：RHPort0，FS 12Mbps */
    printf("[lab3-msc] USB MSC device started\n");

    uint32_t blink_t0 = 0, blink_state = 0;
    while (1) {
        tud_task();                         /* 必须尽快喂：BOT 数据阶段节奏由它推进 */
        msc_app_task();                     /* 卡热插拔（内部带 50ms 去抖） */

        uint32_t now = to_ms_since_boot(get_absolute_time());
        /* 读写在动 → LED 快闪（IO 灯）；空闲 → 1s 慢闪心跳（活机指标） */
        if (msc_io_ticks != blink_state) {
            blink_state = msc_io_ticks;
            gpio_put(PICO_DEFAULT_LED_PIN, msc_io_ticks & 1u);
            blink_t0 = now;
        } else if (now - blink_t0 >= 1000) {
            gpio_put(PICO_DEFAULT_LED_PIN, !gpio_get(PICO_DEFAULT_LED_PIN));
            blink_t0 = now;
        }

        tight_loop_contents();              /* 占位：防止编译器把空循环优化掉 */
    }
    return 0;
}
