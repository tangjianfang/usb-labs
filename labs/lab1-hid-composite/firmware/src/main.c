/*
 * SPDX-License-Identifier: MIT
 * USB-Labs Lab1 —— 固件入口：HID 复合设备（键盘 + 鼠标 + 消费控制）
 *
 * 平台：RP2040 + TinyUSB 0.15.0（pico-sdk 1.5.1 内置）
 * 概念：知识库 10-树干-USB核心/06-四种传输类型.md（中断传输）
 *       知识库 60-枝干-主机侧与实现/05-实战-TinyUSB设备固件.md
 *
 * 行为（演示逻辑，量产时替换为真实键阵/传感器/无线数据源）：
 *   SW2 GP13 单击 → 消费控制接口发一次 播放/暂停(0xCD)
 *   SW3 GP14 按住 → 鼠标 1kHz 上报，光标走 24x24 正方形轨迹
 *   SW4 GP15 单击 → 键盘键入 "usb-labs!"
 */
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "tusb.h"

#include "usb_descriptors.h"

/* ---------- TinyUSB 0.15 的 tusb_init 需要角色/速度初始化结构 ----------
 * 0.14 及更早版本是 tusb_init(void)；以所用版本 src/tusb.h 原型为准 */
static tusb_rhport_init_t const dev_init = {
    .role  = TUSB_ROLE_DEVICE,
    .speed = TUSB_SPEED_AUTO, /* RP2040 仅 FS，自动即 FS */
};

/* ---------- 板级引脚（对照 hardware/设计要点.md；可按原理图修改） ---------- */
#define BTN_MEDIA_GPIO 13u
#define BTN_MOUSE_GPIO 14u
#define BTN_KEY_GPIO   15u

#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 25u /* 裸板无板载 LED 时的兜底值 */
#endif

#define APP_TICK_MS 1u /* 主循环节拍：1kHz，与描述符 bInterval=1 对齐 */
#define DEBOUNCE_MS 20u

/* ---------- 简易按键消抖：连续 DEBOUNCE_MS 同态才算有效 ---------- */
typedef struct {
  uint8_t gpio;
  bool stable;    /* 消抖后的稳定状态（true=按下） */
  bool last_sent; /* 上次上报给应用层的状态，用于边沿检测 */
  uint8_t counter;
} debounce_t;

static debounce_t btns[3] = {
    {BTN_MEDIA_GPIO, false, false, 0},
    {BTN_MOUSE_GPIO, false, false, 0},
    {BTN_KEY_GPIO,   false, false, 0},
};

/* ==================================================================
 * 演示动作 1：键盘打字 "usb-labs!"
 * HID 键码（Usage->Keycode）定义见 HID Usage Tables / 知识库 05-键盘详解.md
 * a..z = 0x04..0x1D；'-' = 0x2D；'!' = Shift+1(0x1E)
 * ================================================================== */
typedef struct { uint8_t modifier; uint8_t keycode; } key_step_t;

static key_step_t const demo_text[] = {
    {0, HID_KEY_U}, {0, HID_KEY_S}, {0, HID_KEY_B}, {0, HID_KEY_MINUS},
    {0, HID_KEY_L}, {0, HID_KEY_A}, {0, HID_KEY_B}, {0, HID_KEY_S},
    {KEYBOARD_MODIFIER_LEFTSHIFT, HID_KEY_1}, /* '!' */
};
#define DEMO_TEXT_LEN (sizeof(demo_text) / sizeof(demo_text[0]))

static struct {
  bool active;
  uint8_t idx;      /* 当前第几个字符 */
  bool pressed_now; /* 当前处于按下相还是释放相 */
  uint8_t wait_ms;  /* 相位间隔计时 */
} kbd_demo;

/* ==================================================================
 * 演示动作 2：鼠标正方形轨迹（1kHz，每 tick 一步 3px，32 步一圈 = 96px 边长）
 * ================================================================== */
static int8_t mouse_square_dx(int8_t i) {
  switch (i / 8) { /* 每 8 步一条边 */
    case 0: return 3;
    case 1: return 0;
    case 2: return -3;
    default: return 0;
  }
}

static int8_t mouse_square_dy(int8_t i) {
  switch (i / 8) {
    case 1: return 3;
    case 2: return 0;
    case 3: return -3;
    default: return 0;
  }
}

static struct {
  bool active;
  uint8_t idx;
} mouse_demo;

/* ==================================================================
 * 演示动作 3：消费控制 播放/暂停（按下沿触发，150ms 后自动发"释放"）
 * ================================================================== */
#define CONSUMER_PLAY_PAUSE 0x00CDu
static uint16_t consumer_release_at_ms; /* 0 = 无待释放；否则为释放时刻(相对 tick) */
static uint32_t tick_ms;

/* ---------------- 按键消抖采样（每 1ms 调一次） ---------------- */
static void buttons_poll(void) {
  for (size_t i = 0; i < sizeof(btns) / sizeof(btns[0]); i++) {
    /* 内部上拉，按下读 0 */
    bool raw = gpio_get(btns[i].gpio) == 0;
    if (raw == btns[i].stable) {
      btns[i].counter = 0;
    } else if (++btns[i].counter >= DEBOUNCE_MS) {
      btns[i].stable = raw;
      btns[i].counter = 0;
    }
  }
}

/* ---------------- 应用层任务（每 1ms 调一次） ---------------- */
static void app_task(void) {
  buttons_poll();

  /* --- 消费控制：按下沿 → 播放/暂停；150ms 后补发释放 --- */
  if (btns[0].stable != btns[0].last_sent) {
    btns[0].last_sent = btns[0].stable;
    if (btns[0].stable && hid_app_send_consumer(CONSUMER_PLAY_PAUSE)) {
      consumer_release_at_ms = tick_ms + 150;
    }
  }
  if (consumer_release_at_ms && (int32_t) (tick_ms - consumer_release_at_ms) >= 0) {
    hid_app_release_consumer();
    consumer_release_at_ms = 0;
  }

  /* --- 鼠标：按住 → 1kHz 正方形轨迹（每 tick 走一步 3px，32 步一圈） --- */
  mouse_demo.active = btns[1].stable;
  if (mouse_demo.active) {
    hid_app_send_mouse(0x00, mouse_square_dx(mouse_demo.idx),
                       mouse_square_dy(mouse_demo.idx), 0);
    mouse_demo.idx = (uint8_t) ((mouse_demo.idx + 1) & 0x1F); /* 32 步一圈 */
  }

  /* --- 键盘：按下沿 → 逐字符打字（每字符 30ms 按下 + 30ms 释放） --- */
  if (btns[2].stable != btns[2].last_sent) {
    btns[2].last_sent = btns[2].stable;
    if (btns[2].stable && !kbd_demo.active) {
      kbd_demo.active = true;
      kbd_demo.idx = 0;
      kbd_demo.pressed_now = false;
      kbd_demo.wait_ms = 0;
    }
  }
  if (kbd_demo.active && ++kbd_demo.wait_ms >= 30) {
    kbd_demo.wait_ms = 0;
    if (!kbd_demo.pressed_now) {
      key_step_t const *s = &demo_text[kbd_demo.idx];
      if (hid_app_send_keyboard(s->modifier, (const uint8_t[6]) {s->keycode, 0, 0, 0, 0, 0})) {
        kbd_demo.pressed_now = true;
      }
    } else {
      hid_app_send_keyboard(0, (const uint8_t[6]) {0, 0, 0, 0, 0, 0}); /* 全释放 */
      kbd_demo.pressed_now = false;
      if (++kbd_demo.idx >= DEMO_TEXT_LEN) kbd_demo.active = false;
    }
  }

  /* --- 心跳 LED：1Hz 闪烁（量产可用于产测"设备活着"判据） --- */
#ifdef PICO_DEFAULT_LED_PIN
  gpio_put(PICO_DEFAULT_LED_PIN, (tick_ms & 0x1FF) < 256);
#endif
}

int main(void) {
  /* 1) 板级与 GPIO 初始化：按键内部上拉，LED 默认低 */
  gpio_init(BTN_MEDIA_GPIO);
  gpio_init(BTN_MOUSE_GPIO);
  gpio_init(BTN_KEY_GPIO);
  gpio_set_dir(BTN_MEDIA_GPIO, GPIO_IN);
  gpio_set_dir(BTN_MOUSE_GPIO, GPIO_IN);
  gpio_set_dir(BTN_KEY_GPIO, GPIO_IN);
  gpio_pull_up(BTN_MEDIA_GPIO);
  gpio_pull_up(BTN_MOUSE_GPIO);
  gpio_pull_up(BTN_KEY_GPIO);
  gpio_init(PICO_DEFAULT_LED_PIN);
  gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

  /* 2) 描述符初始化（用 Flash 唯一 ID 生成 USB 序列号） */
  usb_descriptors_init();

  /* 3) TinyUSB 设备栈初始化。
   * 注：TinyUSB 0.15 的 rp2040 端口会在 dcd_init() 内注册 USBCTRL_IRQ 处理器；
   * 若你的版本枚举无响应，参考 tinyusb hw/bsp/rp2040/family.c 是否需要手动注册（以官方源码为准）。 */
  tusb_init(0, &dev_init);

  /* 4) 1kHz 主循环：tud_task() 处理 USB 事件，app_task() 处理应用逻辑。
   * 用绝对时间推进节拍，避免 sleep 累积漂移（长期 1kHz 稳定性 = 回报率真实性）。 */
  absolute_time_t next_tick = make_timeout_time_ms(APP_TICK_MS);
  while (true) {
    tud_task();
    app_task();
    sleep_until(next_tick);
    next_tick = delayed_by_us(next_tick, APP_TICK_MS * 1000u);
    tick_ms++; /* 演示用毫秒时基；量产建议用 SDK 硬件定时器 */
  }
  return 0; /* 不可达 */
}
