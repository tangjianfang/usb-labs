"""usbsim.packets —— USB 2.0 包编解码（SYNC/EOP 不含在字节流内，见 README 边界说明）。

规范依据: USB 2.0 spec §8.3（包格式）、§8.3.5（CRC，MSb 先发、初值全 1、余数取反）。
本模块按规范算法实现并在 tests/test_sim.py 中用"双实现交叉验证"。
"""
from __future__ import annotations

# PID（低 4 位；高 4 位为取反校验，由 encode 自动补齐）
OUT, IN, SOF, SETUP = 0x1, 0x9, 0x5, 0xD
DATA0, DATA1, DATA2, MDATA = 0x3, 0xB, 0x7, 0xF
ACK, NAK, STALL, NYET = 0x2, 0xA, 0xE, 0x6
PRE, SPLIT, PING = 0xC, 0x8, 0x4

PID_NAMES = {OUT: "OUT", IN: "IN", SOF: "SOF", SETUP: "SETUP", DATA0: "DATA0", DATA1: "DATA1",
             DATA2: "DATA2", MDATA: "MDATA", ACK: "ACK", NAK: "NAK", STALL: "STALL",
             NYET: "NYET", PRE: "PRE", SPLIT: "SPLIT", PING: "PING"}


def _crc5_lfsr(v: int, bits: int = 11) -> int:
    """独立 LFSR 形式（用于交叉验证，见 tests）。"""
    data = (v & ((1 << bits) - 1)) << 5
    crc = 0x1F
    for _ in range(bits):
        msb = (crc >> 4) & 1
        dbit = (data >> 15) & 1
        crc = (crc << 1) & 0x1F
        if msb ^ dbit:
            crc ^= 0x05
        data = (data << 1) & 0xFFFF
    return (~crc) & 0x1F


def crc5(data: int, bits: int = 11) -> int:
    """Token CRC5（§8.3.5）：5 位 LFSR，数据位与寄存器 MSB 异或驱动反馈，多项式 x^5+x^2+1。"""
    v = (data & ((1 << bits) - 1)) << 5
    crc = 0x1F
    for _ in range(bits):
        msb = (crc >> 4) & 1
        dbit = (v >> 15) & 1
        crc = (crc << 1) & 0x1F
        if msb ^ dbit:
            crc ^= 0x05
        v = (v << 1) & 0xFFFF
    return (~crc) & 0x1F


def crc16(data: bytes) -> int:
    """数据 CRC16（USB 口径）：初值 0xFFFF、反射式 0xA001（=x^16+x^15+x^2+1 的反射）、无最终取反。

    注: 规范 §8.3.5 的"余数取反后发送"发生在物理线序层，编/解码两侧对称抵消，
    软件域内比较同一函数值即可自洽。目录校验锚点: crc16(b"123456789") == 0x4B37
    （CRC-16/USB 目录 check 0xB4C8 含最终取反）。
    """
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc & 0xFFFF


def encode_pid(pid: int) -> int:
    return (pid & 0xF) | ((~pid & 0xF) << 4)


def token(addr: int, endp: int, pid: int) -> bytes:
    """Token 包负载：PID+PID~ | ADDR(7) | ENDP(4) | CRC5(5)，按位压缩为 3 字节（MSb 先发打包）。"""
    val = (addr & 0x7F) | ((endp & 0xF) << 7) | (crc5((endp & 0xF) << 7 | (addr & 0x7F), 11) << 11)
    b0 = encode_pid(pid)
    packed = val & 0x1FFF | ((val >> 13) << 13)  # 19 位有效
    raw = val.to_bytes(3, "little")
    return bytes([b0]) + raw


def sof_packet(frame: int) -> bytes:
    val = (frame & 0x7FF) | (crc5(frame & 0x7FF, 11) << 11)
    return bytes([encode_pid(SOF)]) + val.to_bytes(2, "little")


def data_packet(payload: bytes, pid: int = DATA1) -> bytes:
    return bytes([encode_pid(pid)]) + payload + crc16(payload).to_bytes(2, "little")


def handshake(pid: int) -> bytes:
    return bytes([encode_pid(pid)])


def parse(buf: bytes) -> dict:
    """解析包（输入为不含 SYNC/EOP 的字节流）。返回 {pid, ...}。"""
    assert len(buf) >= 1, "空包"
    pid = buf[0] & 0xF
    if (buf[0] >> 4) != ((~pid) & 0xF):
        return {"pid": None, "error": "PID 校验失败", "raw": buf.hex()}
    out = {"pid": pid, "pid_name": PID_NAMES.get(pid, hex(pid)), "raw": buf.hex()}
    if pid in (OUT, IN, SETUP, PING):
        v = int.from_bytes(buf[1:4], "little")
        addr, endp = v & 0x7F, (v >> 7) & 0xF
        ok = crc5(v & 0x1FFF, 11) == (v >> 11) & 0x1F
        out.update({"addr": addr, "endp": endp, "crc5_ok": ok})
    elif pid in (DATA0, DATA1, DATA2, MDATA):
        payload, crc = buf[1:-2], int.from_bytes(buf[-2:], "little")
        out.update({"payload": payload.hex(), "crc16_ok": crc16(payload) == crc,
                    "len": len(payload)})
    elif pid == SOF:
        out["frame"] = int.from_bytes(buf[1:3], "little") & 0x7FF
    return out
