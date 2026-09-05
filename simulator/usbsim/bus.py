"""usbsim.bus —— 虚拟总线：主机轮询模型 + 事务路由 + 捕获日志 + 错误注入。

边界声明: 本仿真工作在包/事务逻辑层（不含电气波形与位级 NRZI/位填充），
帧节拍按 1ms 帧模型推进（FS 口径）。
"""
from __future__ import annotations
from . import packets as P


class Bus:
    def __init__(self):
        self.devices: dict[int, object] = {}   # addr -> device（0 = 默认地址上的设备）
        self.frame = 0
        self.capture: list[dict] = []          # {frame, kind, hex, desc}
        self.drop_data_once = False            # 错误注入: 下一个 DATA 包丢弃一次（触发重传）

    # ---------- 总线事件 ----------
    def tick_frame(self):
        """每 1ms 一帧；同步/中断事务由主机调度器在帧内推进。"""
        self.frame += 1
        self.log(P.sof_packet(self.frame), kind="SOF", desc=f"帧 {self.frame}")

    def log(self, packet: bytes, kind: str, desc: str):
        self.capture.append({"frame": self.frame, "kind": kind,
                             "hex": packet.hex(), "desc": desc})

    def reset(self):
        """总线复位（SE0 ≥10ms 的逻辑效果）：所有设备回 Default 态（地址 0）。"""
        self.log(b"", kind="RESET", desc="总线复位 SE0≥10ms")
        for d in self.devices.values():
            d.bus_reset()

    # ---------- 事务路由 ----------
    def device_at(self, addr: int):
        return self.devices.get(addr)

    def send_token(self, addr: int, endp: int, pid: int):
        pkt = P.token(addr, endp, pid)
        parsed = P.parse(pkt)
        self.log(pkt, kind="TOKEN", desc=f"{parsed['pid_name']} addr={addr} ep={endp}")
        return parsed

    def send_data(self, payload: bytes, pid: int) -> tuple[bool, dict]:
        """主机发送 DATA 包。返回 (被接收, 解析)。错误注入时丢弃一次触发重传。"""
        pkt = P.data_packet(payload, pid)
        parsed = P.parse(pkt)
        if self.drop_data_once:
            self.drop_data_once = False
            self.log(pkt, kind="DATA(DROPPED)", desc=f"CRC/电气错误: {parsed['payload']}")
            return False, parsed
        self.log(pkt, kind="DATA", desc=f"{parsed['pid_name']} len={parsed['len']} {parsed['payload'][:24]}")
        return True, parsed

    def send_handshake(self, pid: int, desc: str):
        pkt = P.handshake(pid)
        self.log(pkt, kind="HANDSHAKE", desc=desc)
