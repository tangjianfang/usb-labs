#!/usr/bin/env python3
"""serial_tool.py —— 工业日志采集串口工具（USB-Labs Lab2）

特性：
  1. 自动发现 CDC 设备：按 VID:PID 过滤（--vidpid），不给参数则自动挑首个串口；
  2. 自动重连：USB 拔出/设备重启/DFU 升级导致的断开都能自愈，进程不退出；
  3. 时间戳：每行加本地时间前缀（%H:%M:%S.mmm），可 --utc 切 UTC；
  4. 日志落盘：按天滚动的 UTF-8 文本，带 stdin 手动打标（现场留证）；
  5. 波特率仅作兼容参数透传——USB CDC 的实际速率与它无关（见 ../experience.md）。

用法示例：
    python serial_tool.py --auto                      # 自动找口 + 重连 + 落盘
    python serial_tool.py --vidpid 0xCAFE:0x4010      # 只认本 Lab 设备
    python serial_tool.py -p COM7 -b 115200 --log-dir ./logs
"""

from __future__ import annotations

import argparse
import datetime as _dt
import os
import sys
import time

import serial  # pip install pyserial
from serial.tools import list_ports

RECONNECT_DELAY_S = 2.0


def parse_vidpid(text: str) -> tuple[int, int] | None:
    """'0xCAFE:0x4010' / 'CAFE:4010' -> (0xCAFE, 0x4010)；解析失败返回 None。"""
    if not text or ":" not in text:
        return None
    vid_s, pid_s = text.split(":", 1)
    try:
        return int(vid_s, 16), int(pid_s, 16)
    except ValueError:
        return None


def find_port(vidpid: tuple[int, int] | None, prefer: str | None) -> str | None:
    """按 VID:PID 精确匹配；找不到时退回指定口名或首个可用串口。"""
    ports = list(list_ports.comports())
    if vidpid is not None:
        vid, pid = vidpid
        for p in ports:
            if p.vid == vid and p.pid == pid:
                return p.device
        return None  # 明确指定了 VID:PID 就不乱抓别的设备
    if prefer:
        return prefer
    return ports[0].device if ports else None


class SerialLogger:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.vidpid = parse_vidpid(args.vidpid)
        self._log = None
        self._log_day = None
        self._ser: serial.Serial | None = None

    # ---------- 日志落盘 ----------
    def _ensure_log(self, now: _dt.datetime) -> None:
        if self._log is not None and now.date() == self._log_day:
            return
        if self._log is not None:
            self._log.close()
        os.makedirs(self.args.log_dir, exist_ok=True)
        name = now.strftime("serial_%Y%m%d.log")
        self._log = open(  # noqa: SIM115 - 常驻日志文件随进程生命周期
            os.path.join(self.args.log_dir, name),
            "a", encoding="utf-8", errors="replace",
        )
        self._log_day = now.date()
        self._write_line(f"=== log opened: {name} ===", to_console=False)

    def _write_line(self, line: str, to_console: bool = True) -> None:
        now = _dt.datetime.now(_dt.timezone.utc if self.args.utc else None)
        self._ensure_log(now.replace(tzinfo=None))
        stamped = f"{now:%H:%M:%S}.{now.microsecond // 1000:03d} {line}" \
            if not self.args.no_timestamp else line
        self._log.write(stamped + "\n")
        self._log.flush()  # 现场断电也要保住已收数据
        if to_console:
            print(stamped, flush=True)

    # ---------- 串口生命周期 ----------
    def open_serial(self) -> bool:
        port = find_port(self.vidpid, self.args.port)
        if port is None:
            return False
        try:
            self._ser = serial.Serial(
                port=port,
                baudrate=self.args.baud,      # 对 USB CDC 仅为兼容参数
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=0.2,                  # 短超时轮询，重连要快
                write_timeout=1.0,
            )
        except (serial.SerialException, OSError) as exc:
            self._ser = None
            print(f"[open failed] {port}: {exc}", file=sys.stderr, flush=True)
            return False
        print(f"[connected] {port} (baud param {self.args.baud})", flush=True)
        self._write_line(f"=== connected: {port} ===", to_console=False)
        return True

    def close_serial(self, note: str) -> None:
        if self._ser is not None:
            try:
                self._ser.close()
            except Exception:  # noqa: BLE001 - 关闭时口可能已消失
                pass
        self._ser = None
        self._write_line(f"=== disconnected: {note} ===", to_console=False)
        print(f"[disconnected] {note}", flush=True)

    # ---------- 主循环 ----------
    def run(self) -> int:
        buf = bytearray()
        while True:
            if self._ser is None:
                if not self.open_serial():
                    time.sleep(RECONNECT_DELAY_S)
                    continue

            try:
                chunk = self._ser.read(512)
                if chunk:
                    buf.extend(chunk)
                    # 按行切分（兼容 \n / \r\n / \r），残包留给下一轮
                    while True:
                        idx = max(buf.find(b"\n"), buf.find(b"\r"))
                        if idx < 0:
                            break
                        line = bytes(buf[:idx]).decode("utf-8", "replace").strip()
                        del buf[: idx + 1]
                        if line:
                            self._write_line(line)
                    if len(buf) > 64 * 1024:  # 无行结束符的流：防内存膨胀
                        self._write_line(bytes(buf).decode("utf-8", "replace"))
                        buf.clear()

            except (serial.SerialException, OSError) as exc:
                self.close_serial(str(exc))
                continue

            # stdin 手动打标（非阻塞式：仅在 Windows/Linux 终端可交互时有效）
            if sys.stdin in select_ready():
                mark = sys.stdin.readline().strip()
                if mark:
                    self._write_line(f"[MANUAL] {mark}")


def select_ready():
    """极简 stdin 可读检测；非交互环境返回空集。"""
    import select

    try:
        ready, _, _ = select.select([sys.stdin], [], [], 0)
        return ready
    except (OSError, ValueError, AttributeError):
        # Windows 无 select(stdin) 支持 -> 手动打标功能降级为不可用
        return []


def main() -> int:
    ap = argparse.ArgumentParser(
        description="pyserial 自动重连 + 时间戳 + 日志落盘（工业日志采集常用）")
    ap.add_argument("-p", "--port", help="指定串口（如 COM7 或 /dev/ttyACM0）")
    ap.add_argument("--auto", action="store_true",
                    help="按 VID:PID 自动发现设备（等价 --vidpid 0xCAFE:0x4010）")
    ap.add_argument("--vidpid", default="0xCAFE:0x4010",
                    help="设备过滤，格式 VID:PID（默认本 Lab 设备）")
    ap.add_argument("--no-filter", action="store_true", help="不过滤，接受任意串口设备")
    ap.add_argument("-b", "--baud", type=int, default=115200,
                    help="波特率兼容参数（USB CDC 实际速率与此无关）")
    ap.add_argument("--log-dir", default="./logs", help="日志目录（按天滚动）")
    ap.add_argument("--no-timestamp", action="store_true", help="关闭行首时间戳")
    ap.add_argument("--utc", action="store_true", help="时间戳用 UTC（默认本地时间）")
    args = ap.parse_args()

    # 过滤策略归一化：
    #   --auto / --no-filter -> vidpid 视为空（parse_vidpid 返回 None = 不过滤）
    #   否则 vidpid 必须能解析，或显式给了 --port
    if args.auto or args.no_filter:
        args.vidpid = ""
    elif parse_vidpid(args.vidpid) is None and args.port is None:
        ap.error("无法解析 --vidpid，且未给 --port；示例：--vidpid 0xCAFE:0x4010 或 --auto")

    try:
        return SerialLogger(args).run()
    except KeyboardInterrupt:
        print("\n[exit] Ctrl+C", flush=True)
        return 0


if __name__ == "__main__":
    sys.exit(main())
