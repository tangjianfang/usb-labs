/*
 * SPDX-License-Identifier: MIT
 * USB-Labs Lab1 —— HID 类应用回调（hid_app.c）
 *
 * TinyUSB 设备栈把 HID 类协议细节（SETUP 解析、SET_IDLE、SET_PROTOCOL…）
 * 封装在 src/class/hid/hid_device.c 中，应用层只需实现下列弱函数回调。
 * 回调签名以所用 TinyUSB 版本的 src/class/hid/hid_device.h 为准（本文按 0.15.0）。
 *
 * 概念：知识库 20-枝干-设备类协议/HID-人机接口设备/04-传输与类特定请求.md
 *       20-枝干-设备类协议/HID-人机接口设备/05-键盘详解.md（LED 输出报告）
 */
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "usb_descriptors.h"

/* ---------------- 主机可见状态（供 main.c / 调试用） ---------------- */
volatile uint8_t hid_app_kbd_leds = 0;   /* 主机最后一次设置的键盘 LED 位图 */
volatile bool    hid_app_boot_mode[CFG_TUD_HID]; /* 各接口是否处于 boot 协议 */

/* 最近一次成功发出的报告缓存：响应 GET_REPORT 用 */
static uint8_t last_kbd_report[CFG_TUD_HID_EP_BUFSIZE];
static uint16_t last_kbd_len;

/* ---------------- 报告发送封装 ---------------- */

/* 键盘：modifier 位图 + 6 键码（boot/报告协议两种模式由 TinyUSB 内部适配） */
bool hid_app_send_keyboard(uint8_t modifier, const uint8_t keycode[6]) {
  if (!tud_ready()) return false;
  return tud_hid_keyboard_report(REPORT_ID_KEYBOARD, modifier, keycode);
}

/* 鼠标：3 按钮位图 + X/Y 位移 + 滚轮（int8 相对值） */
bool hid_app_send_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel) {
  if (!tud_ready()) return false;
  return tud_hid_mouse_report(REPORT_ID_MOUSE, buttons, dx, dy, wheel, 0);
}

/* 消费控制：16 位 Usage，小端（如 0xCD 播放/暂停、0xE9 音量+） */
bool hid_app_send_consumer(uint16_t usage) {
  uint8_t report[2] = { (uint8_t) (usage & 0xFF), (uint8_t) (usage >> 8) };
  if (!tud_ready()) return false;
  return tud_hid_report(REPORT_ID_CONSUMER, report, sizeof(report));
}

/* 消费控制按键抬起：报告 0x0000（对该键的"释放"） */
bool hid_app_release_consumer(void) {
  return hid_app_send_consumer(0x0000);
}

/* ---------------- TinyUSB 必选回调 ---------------- */

/* 主机 GET_REPORT(输入/输出/特性) 时回调。
 * 多数操作系统日常不主动 GET_REPORT，但Windows/macOS 在特定场景（快速用户切换、
 * 休眠恢复、查询当前状态）会调用——返回 0 会导致该请求 STALL，
 * 生产经验（公开资料共识）：缓存最近报告并正确返回，可避免偶发兼容问题。 */
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
  (void) report_id;
  (void) report_type;
  if (instance == ITF_NUM_HID_KEYBOARD && reqlen > 0) {
    uint16_t n = last_kbd_len < reqlen ? last_kbd_len : reqlen;
    memcpy(buffer, last_kbd_report, n);
    return n;
  }
  /* 其余接口不提供 GET_REPORT 语义 → 返回 0（该请求被 STALL，属正常响应） */
  return 0;
}

/* 主机 SET_REPORT / SET_IDLE(带数据) 等下发数据时回调。
 * 对键盘接口，主机用 SET_REPORT(OUTPUT) 推送 1 字节 LED 位图：
 *   bit0 NumLock, bit1 CapsLock, bit2 ScrollLock, bit3 Compose, bit4 Kana */
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
  if (instance == ITF_NUM_HID_KEYBOARD &&
      report_type == HID_REPORT_TYPE_OUTPUT && bufsize >= 1) {
    hid_app_kbd_leds = buffer[0];
    /* 演示：把 CapsLock 状态显示到板载 LED（量产看板子实际指示灯电路） */
#ifdef PICO_DEFAULT_LED_PIN
    gpio_put(PICO_DEFAULT_LED_PIN, (hid_app_kbd_leds & KEYBOARD_LED_CAPSLOCK) != 0);
#endif
  }
  (void) report_id;
}

/* 每次中断 IN 报告真正发上总线后回调（可用于流控/续传）。
 * 这里缓存键盘报告，供 GET_REPORT 应答。 */
void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len) {
  if (instance == ITF_NUM_HID_KEYBOARD && len <= sizeof(last_kbd_report)) {
    memcpy(last_kbd_report, report, len);
    last_kbd_len = len;
  }
}

/* 主机 SET_PROTOCOL（boot/report 协议切换）时回调。
 * BIOS 场景：主机切到 boot 协议，TinyUSB 内部按 boot 固定格式发送（键盘 8 字节无 Report ID）。
 * 注意：回调名与参数随 TinyUSB 版本可能调整（0.15 为 tud_hid_boot_protocol_cb），
 * 以所用版本 hid_device.h 为准。 */
void tud_hid_boot_protocol_cb(uint8_t instance, bool boot_mode) {
  if (instance < CFG_TUD_HID) {
    hid_app_boot_mode[instance] = boot_mode;
  }
}
