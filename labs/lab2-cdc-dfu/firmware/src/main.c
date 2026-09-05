/*
 * main.c —— 系统初始化 + TinyUSB 调度 + 业务演示（工业日志/命令风格）
 * USB-Labs Lab2：CDC 虚拟串口 + DFU 固件升级（STM32F103C8T6）
 *
 * 链接契约（与 firmware/README.md 分区表一致）：
 *   - FLASH ORIGIN = 0x08003000（App 起点，bootloader 保留 12KB）
 *   - VECT_TAB_OFFSET = 0x3000（system_stm32f1xx.c）或下面 main 里显式设置 VTOR
 *   - .noinit 段：DFU 分离魔法数（复位不清除，bootloader 依据它停留 DFU 模式）
 */
#include <stdio.h>
#include <string.h>

#include "stm32f1xx_hal.h"
#include "tusb.h"
#include "cdc_app.h"

#define APP_VERSION  "Lab2 v1.0.0 (cdc+dfu)"

/* ---------------- DFU 分离魔法数（.noinit 段，复位不清除） ----------------
 * 流程：DFU_DETACH -> 本回调写魔法数 -> NVIC_SystemReset()
 *      -> [bootloader 启动时发现魔法数 => 停留 DFU 模式并清除它]
 * 本 Lab 的保底路径是 ST ROM DFU（BOOT0 进），自研 bootloader 见
 * bootloader选型与跳转设计.md；魔法数机制两者通用。 */
#define DFU_MAGIC_RESET_TO_BL 0x44465531UL /* "DFU1" */
__attribute__((section(".noinit"), used)) uint32_t g_dfu_magic;

/* ---------------- 时钟：HSE 8MHz x9 = 72MHz，USB = 48MHz ---------------- */
static void SystemClock_Config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    __HAL_RCC_PWR_CLK_ENABLE();

    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState       = RCC_HSE_ON;              /* 8MHz 外部晶振：USB 时钟精度的前提 */
    osc.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
    osc.PLL.PLLState   = RCC_PLL_ON;
    osc.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLMUL     = RCC_PLL_MUL9;            /* 8MHz * 9 = 72MHz */
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        Error_Handler();
    }

    clk.ClockType           = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                              RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource        = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider       = RCC_SYSCLK_DIV1;    /* HCLK  = 72MHz */
    clk.APB1CLKDivider      = RCC_HCLK_DIV2;      /* APB1  = 36MHz（上限） */
    clk.APB2CLKDivider      = RCC_HCLK_DIV1;      /* APB2  = 72MHz */
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK) {
        Error_Handler();
    }

    /* USB 时钟 = PLL/1.5 = 48MHz —— 硬性要求，枚举失败的首先排查点 */
    if (HAL_RCCEx_PeriphCLKConfig(&(RCC_PeriphCLKInitTypeDef){
            .PeriphClockSelection = RCC_PERIPHCLK_USB,
            .UsbClockSelection    = RCC_USBCLKSOURCE_PLL_DIV1_5}) != HAL_OK) {
        Error_Handler();
    }
}

/* ---------------- GPIO：LED 心跳 + （可选）USB 软连接 ---------------- */
static void GPIO_Init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* PA11/PA12 为 USB D-/D+：USB 宏单元接管，无需（也不应）配成 GPIO 输出 */

    GPIO_InitTypeDef io = {0};
    /* PB1: 心跳 LED（低电平点亮按板而定） */
    io.Pin   = GPIO_PIN_1;
    io.Mode  = GPIO_MODE_OUTPUT_PP;
    io.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &io);
}

/* ---------------- 业务演示：工业日志 + 简单命令 ---------------- */
static uint32_t s_last_heartbeat_ms;

static void app_handle_rx(void)
{
    uint8_t buf[128];
    size_t n = cdc_app_read(buf, sizeof(buf) - 1);
    if (n == 0) {
        return;
    }
    buf[n] = 0;

    /* 极简行命令解析（工业调试口风格） */
    if (strncmp((char *)buf, "ping", 4) == 0) {
        cdc_app_write((const uint8_t *)"pong\n", 5);
    } else if (strncmp((char *)buf, "ver", 3) == 0) {
        char line[64];
        int len = snprintf(line, sizeof(line), "%s\r\n", APP_VERSION);
        cdc_app_write((const uint8_t *)line, (size_t)len);
    } else if (strncmp((char *)buf, "dfu", 3) == 0) {
        /* 与主机发 `dfu-util -e` 等价的自定义触发通道 */
        cdc_app_write((const uint8_t *)"reboot to bootloader...\r\n", 25);
        g_dfu_magic = DFU_MAGIC_RESET_TO_BL;
        HAL_Delay(10); /* 尽量把 TX 搬完（cdc_app_task 已 flush） */
        NVIC_SystemReset();
    } else {
        cdc_app_write((const uint8_t *)"cmd: ping|ver|dfu\r\n", 19);
    }
}

static void app_heartbeat(uint32_t now_ms)
{
    if (now_ms - s_last_heartbeat_ms >= 1000) {
        s_last_heartbeat_ms = now_ms;
        HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_1);

        if (cdc_app_dtr()) { /* 只在主机打开串口时输出，避免灌日志 */
            char line[48];
            int len = snprintf(line, sizeof(line),
                               "[hb] up=%lus, temp-c=25, in24v=24.1\r\n",
                               (unsigned long)(now_ms / 1000));
            cdc_app_write((const uint8_t *)line, (size_t)len);
        }
    }
}

/* ---------------- TinyUSB DFU Runtime 回调 ---------------- */
void tud_dfu_runtime_reboot_to_dfu_cb(void)
{
    /* 收到 DFU_DETACH (bmRequestType=0x21, bRequest=0x00)：
     * 留下魔法数 -> 复位 -> bootloader/ROM 侧接管进入 DFU 模式重新枚举 */
    g_dfu_magic = DFU_MAGIC_RESET_TO_BL;
    NVIC_SystemReset();
}

/* ---------------- 中断向量对接 ---------------- */
void USB_HP_CAN1_TX_IRQHandler(void)   /* USB 高优先级（送包完成） */
{
    tud_int_handler(0);
}

void USB_LP_CAN1_RX0_IRQHandler(void)  /* USB 低优先级（收包/控制传输） */
{
    tud_int_handler(0);
}

void USBWakeUp_IRQHandler(void)        /* 唤醒（挂起恢复） */
{
    tud_int_handler(0);
}

void SysTick_Handler(void)
{
    HAL_IncTick();
}

void Error_Handler(void)
{
    __disable_irq();
    for (;;) {
        ; /* 量产：记录错误码+复位或进入安全态 */
    }
}

/* ---------------- main ---------------- */
int main(void)
{
    HAL_Init();
    SystemClock_Config();

    /* App 从 0x08003000 运行：向量表偏移（若未在 system_stm32f1xx.c 配置） */
    SCB->VTOR = 0x08003000u;

    GPIO_Init();
    cdc_app_init();

    /* 软连接板型：先把 D+ 上拉开关断开再初始化 USB，主机将看到一次干净的
     * "插入"事件（无软连接电路的板子忽略此步）。 */
    tud_init(0);

    for (;;) {
        tud_task();          /* USB 事件（中断取出、回调在此上下文执行） */
        cdc_app_task();      /* TX 环 -> 批量 IN */
        app_handle_rx();     /* 命令处理 */
        app_heartbeat(HAL_GetTick());
        __WFI();             /* 等 1ms SysTick/USB 中断，降功耗 */
    }
}
