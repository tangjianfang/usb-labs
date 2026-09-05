#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
USB-Labs Lab1 —— 主机侧 HID 报告监视器（hidapi）

依赖：
    pip install hidapi       # 提供 `hid` 模块（libusb/hidapi 的 Python 绑定）
    https://pypi.org/project/hidapi/   https://github.com/libusb/hidapi

功能：
    按 VID/PID 打开本 Lab 的三个 HID 采集接口（键盘/鼠标/消费控制），
    按报告描述符约定的 Report ID 解析并打印每一条报告：

        接口0 键盘     [0x01][modifier][reserved][k1..k6]
        接口1 鼠标     [0x02][buttons][dx][dy][wheel]
        接口2 消费控制 [0x03][usage_lo][usage_hi]

    报告格式与 firmware/src/usb_descriptors.c 中的报告描述符一一对应；
    位段含义见知识库 20-枝干-设备类协议/HID-人机接口设备/05-键盘详解.md、06-鼠标详解.md、07-消费控制与多媒体.md

用法：
    python hid_monitor.py --list
    python hid_monitor.py --vid 0x1234 --pid 0x0002
    python hid_monitor.py --vid 0x1234 --pid 0x0002 --hex --duration 30
"""

import argparse
import sys
import time

try:
    import hid
except ImportError:
    sys.exit("缺少 hidapi 库：请先执行 pip install hidapi")

# 与 firmware/src/usb_descriptors.h 保持一致（量产请改成你自己的 VID/PID）
DEFAULT_VID = 0x1234
DEFAULT_PID = 0x0002

REPORT_ID_KEYBOARD = 0x01
REPORT_ID_MOUSE = 0x02
REPORT_ID_CONSUMER = 0x03

# ---------------- 解析用码表（节选自 HID Usage Tables，完整表见官方文档） ----------------

MODIFIER_BITS = [
    (0x01, "LCtrl"), (0x02, "LShift"), (0x04, "LAlt"), (0x08, "LGui"),
    (0x10, "RCtrl"), (0x20, "RShift"), (0x40, "RAlt"), (0x80, "RGui"),
]

KEYCODES = {
    0x04: "a", 0x05: "b", 0x06: "c", 0x07: "d", 0x08: "e", 0x09: "f", 0x0A: "g",
    0x0B: "h", 0x0C: "i", 0x0D: "j", 0x0E: "k", 0x0F: "l", 0x10: "m", 0x11: "n",
    0x12: "o", 0x13: "p", 0x14: "q", 0x15: "r", 0x16: "s", 0x17: "t", 0x18: "u",
    0x19: "v", 0x1A: "w", 0x1B: "x", 0x1C: "y", 0x1D: "z",
    0x1E: "1", 0x1F: "2", 0x20: "3", 0x21: "4", 0x22: "5", 0x23: "6",
    0x24: "7", 0x25: "8", 0x26: "9", 0x27: "0",
    0x28: "Enter", 0x29: "Esc", 0x2A: "Backspace", 0x2B: "Tab", 0x2C: "Space",
    0x2D: "-", 0x2E: "=", 0x36: ",", 0x37: ".", 0x38: "/",
    0x4F: "Right", 0x50: "Left", 0x51: "Down", 0x52: "Up",
}

CONSUMER_USAGES = {
    0x0030: "Power",
    0x00B5: "ScanNext",
    0x00B6: "ScanPrev",
    0x00B7: "Stop",
    0x00CD: "Play/Pause",
    0x00E2: "Mute",
    0x00E9: "VolumeUp",
    0x00EA: "VolumeDown",
}

MOUSE_BUTTONS = [(0x01, "Btn1"), (0x02, "Btn2"), (0x04, "Btn3")]


def to_signed8(v: int) -> int:
    """uint8 -> int8（鼠标位移是有符号相对值）"""
    return v - 256 if v >= 0x80 else v


def decode_keyboard(buf):
    modifier = buf[1]
    keys = [b for b in buf[3:9] if b]  # buf[2] 为保留字节
    mods = "+".join(name for bit, name in MODIFIER_BITS if modifier & bit)
    chars = "+".join(KEYCODES.get(k, hex(k)) for k in keys) or "(release)"
    return f"KBD  mods={modifier:#04x}{'[' + mods + ']' if mods else '':<24} keys={chars}"


def decode_mouse(buf):
    buttons = buf[1]
    dx, dy, wheel = to_signed8(buf[2]), to_signed8(buf[3]), to_signed8(buf[4])
    btns = "|".join(name for bit, name in MOUSE_BUTTONS if buttons & bit) or "-"
    return f"MOUSE btns={btns:<9} dx={dx:<5} dy={dy:<5} wheel={wheel}"


def decode_consumer(buf):
    usage = buf[1] | (buf[2] << 8)  # 16 位小端
    return f"CONS usage={usage:#06x} {'(' + CONSUMER_USAGES[usage] + ')' if usage in CONSUMER_USAGES else '(release)' if usage == 0 else '(unknown)'}"


def decode(buf):
    """按第一字节 Report ID 分发。None 表示不是本 Lab 的报告。"""
    if len(buf) >= 9 and buf[0] == REPORT_ID_KEYBOARD:
        return decode_keyboard(buf)
    if len(buf) >= 5 and buf[0] == REPORT_ID_MOUSE:
        return decode_mouse(buf)
    if len(buf) >= 3 and buf[0] == REPORT_ID_CONSUMER:
        return decode_consumer(buf)
    return None


def list_devices():
    print("已枚举到的 HID 设备（按接口分行）：")
    for d in hid.enumerate():
        print(f"  vid=0x{d['vendor_id']:04x} pid=0x{d['product_id']:04x} "
              f"itf={d.get('interface_number', '?')} usage_page=0x{d.get('usage_page', 0):04x} "
              f"usage=0x{d.get('usage', 0):04x}  {d['manufacturer_string']} | {d['product_string']}")


def open_interfaces(vid, pid):
    """打开该 VID/PID 下的所有接口（hidapi 枚举时每个接口一条 path）。"""
    devices = []
    for d in hid.enumerate(vid, pid):
        try:
            h = hid.device()
            h.open_path(d["path"])
            h.set_nonblocking(0)
            devices.append((d.get("interface_number", -1), h))
        except OSError as e:
            print(f"  打开接口 {d.get('interface_number')} 失败：{e}", file=sys.stderr)
    return devices


def monitor(vid, pid, show_hex, duration):
    devices = open_interfaces(vid, pid)
    if not devices:
        print(f"未找到 VID=0x{vid:04x} PID=0x{pid:04x} 的设备；先跑 --list 检查")
        sys.exit(1)
    print(f"已打开 {len(devices)} 个接口，开始监视（Ctrl+C 退出）...")
    deadline = time.time() + duration if duration else None

    try:
        while deadline is None or time.time() < deadline:
            for itf, h in devices:
                buf = h.read(64, timeout_ms=100)  # 阻塞 100ms/次，逐接口轮询
                if not buf:
                    continue
                line = decode(buf)
                if line is None:
                    line = f"RAW  {buf.hex(' ')}"
                ts = time.strftime("%H:%M:%S") + f".{int(time.time() * 1000) % 1000:03d}"
                suffix = f"   [hex] {bytes(buf).hex(' ')}" if show_hex else ""
                print(f"[{ts}] itf{itf} {line}{suffix}")
    except KeyboardInterrupt:
        pass
    finally:
        for _, h in devices:
            h.close()
        print("\n已退出")


def main():
    ap = argparse.ArgumentParser(description="USB-Labs Lab1 HID 报告监视器")
    ap.add_argument("--vid", type=lambda x: int(x, 0), default=DEFAULT_VID)
    ap.add_argument("--pid", type=lambda x: int(x, 0), default=DEFAULT_PID)
    ap.add_argument("--list", action="store_true", help="仅枚举 HID 设备后退出")
    ap.add_argument("--hex", action="store_true", help="同时打印原始字节")
    ap.add_argument("--duration", type=int, default=0, help="监视秒数（0=直到 Ctrl+C）")
    args = ap.parse_args()

    if args.list:
        list_devices()
        return
    monitor(args.vid, args.pid, args.hex, args.duration)


if __name__ == "__main__":
    main()
