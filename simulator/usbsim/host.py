"""usbsim.host —— 主机模型: 事务引擎 + 标准枚举序列 + 传输 API。

主机是唯一发起方（规范原则）：每事务 Token→(Data)→Handshake，
数据触发 DATA0/1 独立维护，CRC 由 packets 层编解码并校验。
"""
from __future__ import annotations
from . import packets as P
from .bus import Bus
from .device import UsbDevice, HidDevice, MscDevice, HubDevice
from .logbase import setup

log = setup("usbsim.host")


class Host:
    def __init__(self, bus: Bus):
        self.bus = bus
        self.addr = 1                          # 下一个可分配地址
        self.in_toggle: dict = {}              # (addr,ep) -> 期望 DATA pid
        self.out_toggle: dict = {}

    # ---------- 事务原语 ----------
    def _toggle_of(self, addr, ep, direction, new=None):
        key = (addr, ep, direction)
        if new is not None:
            self.in_toggle[key] = new
        return self.in_toggle.get(key, P.DATA1)

    def control_transfer(self, addr: int, bmRequestType: int, bRequest: int,
                         wValue: int, wIndex: int, data: bytes | None = None, wlength: int | None = None) -> bytes | None:
        """三阶段控制传输。data 为 None 时按 wLength 收数据（IN 方向）。"""
        bus, ep0 = self.bus, 0
        setup = bytes([bmRequestType, bRequest]) + wValue.to_bytes(2, "little") + \
                wIndex.to_bytes(2, "little") + (len(data) if data is not None else (wlength or 0)).to_bytes(2, "little")
        # Setup 阶段
        self.bus.send_token(addr, ep0, P.SETUP)
        ok, parsed = self.bus.send_data(setup, P.DATA0)
        dev = bus.device_at(addr)
        if not ok or dev is None:
            log.debug("控制传输 addr=%d req=0x%02X 无设备确认 → NAK", addr, bRequest)
            self.bus.send_handshake(P.NAK, "无设备确认"); return None
        dev.ep0_setup(parsed)
        self.bus.send_handshake(P.ACK, "SETUP ACK")
        # Data 阶段（可选）
        result = b""
        if data is not None:                       # OUT 数据阶段
            for i in range(0, len(data), 64):
                chunk = data[i:i + 64]
                self.bus.send_token(addr, ep0, P.OUT)
                self.bus.send_data(chunk, P.DATA1)
                self.bus.send_handshake(P.ACK, "OUT ACK")
        else:                                      # IN 数据阶段（0/多次，短包/ZLP 结束）
            remaining = int.from_bytes(setup[6:8], "little")
            while remaining > 0:
                self.bus.send_token(addr, ep0, P.IN)
                pkt = dev.ep0_data_in()
                if pkt is None:
                    break
                ok, parsed = self.bus.send_data(pkt, P.DATA1)
                if not ok:
                    continue
                result += pkt
                remaining -= len(pkt)
                if len(pkt) < 64:
                    break
        # Status 阶段
        self.bus.send_token(addr, ep0, P.IN if (bmRequestType & 0x80) == 0 else P.OUT)
        self.bus.send_data(b"", P.DATA1)
        self.bus.send_handshake(P.ACK, "状态阶段 ACK")
        return result

    # ---------- 标准枚举序列（对照知识库 08 篇） ----------
    def enumerate(self, dev: UsbDevice, target_addr: int | None = None):
        bus = self.bus
        bus.devices[0] = dev
        bus.reset()
        log.info("枚举: 总线复位完成，进入 Default 态（地址 0）")
        self.bus.log(b"", kind="ENUM", desc="阶段: Default 态, 读取设备描述符前 8 字节")
        d = self.control_transfer(0, 0x80, 0x06, 0x0100, 0, None, wlength=64) or b""
        assert len(d) >= 8 and d[7] == dev.ep0_mps, "EP0 MPS 与描述符不一致"
        log.info("枚举: 首读设备描述符 8 字节，EP0 MPS=%d", d[7])
        self.bus.log(b"", kind="ENUM", desc="阶段: SET_ADDRESS")
        self.control_transfer(0, 0x00, 0x05, target_addr or self.addr, 0, b"")
        dev.address = target_addr or self.addr
        bus.devices[dev.address] = dev
        del bus.devices[0]
        self.bus.log(b"", kind="ENUM", desc=f"阶段: 地址 {dev.address}")
        log.info("枚举: SET_ADDRESS → 地址 %d", dev.address)
        d = self.control_transfer(dev.address, 0x80, 0x06, 0x0100, 0, None, wlength=18)
        assert len(d) == 18, "设备描述符必须 18 字节"
        log.info("枚举: 读完整设备描述符 18 字节（VID=0x%04X PID=0x%04X）",
                 int.from_bytes(d[8:10], "little"), int.from_bytes(d[10:12], "little"))
        self.control_transfer(dev.address, 0x80, 0x06, 0x0200, 0, None, wlength=9)   # 配置头 9 字节
        self.control_transfer(dev.address, 0x80, 0x06, 0x0200, 0, b"\0" * 65535)
        self.control_transfer(dev.address, 0x00, 0x09, 0x0001, 0, b"")    # SET_CONFIGURATION 1
        log.info("枚举: 配置描述符已读，SET_CONFIGURATION=1，枚举完成（地址 %d）", dev.address)
        return dev.address

    # ---------- 数据传输 API ----------
    def bulk_write(self, addr: int, ep: int, data: bytes, mps: int = 64):
        dev = self.bus.device_at(addr)
        for i in range(0, len(data), mps):
            chunk = data[i:i + mps]
            pid = P.DATA1 if self.out_toggle.get((addr, ep), 0) == 0 else P.DATA0
            self.bus.send_token(addr, ep, P.OUT)
            self.bus.send_data(chunk, pid)
            self.bus.send_handshake(P.ACK, "bulk OUT")
            self.out_toggle[(addr, ep)] = self.out_toggle.get((addr, ep), 0) ^ 1
            if dev is not None:
                dev.on_out(ep, chunk)

    def bulk_read(self, addr: int, ep: int, size: int, max_tries: int = 16) -> bytes:
        out = b""
        for _ in range(max_tries):
            if len(out) >= size:
                break
            pid = P.DATA1 if self.in_toggle.get((addr, ep), 0) == 0 else P.DATA0
            self.bus.send_token(addr, ep, P.IN)
            dev = self.bus.device_at(addr)
            pkt = dev.bulk_in_packet(ep) if dev else None
            if pkt is None:
                self.bus.send_handshake(P.NAK, "设备无数据")
                continue
            self.bus.send_data(pkt, pid)
            self.bus.send_handshake(P.ACK, "bulk IN")
            self.in_toggle[(addr, ep)] = self.in_toggle.get((addr, ep), 0) ^ 1
            out += pkt
        return out[:size]

    def interrupt_poll(self, addr: int, ep: int) -> bytes | None:
        pid = P.DATA1 if self.in_toggle.get((addr, ep), 0) == 0 else P.DATA0
        self.bus.send_token(addr, ep, P.IN)
        dev = self.bus.device_at(addr)
        pkt = dev.interrupt_in_packet(ep) if dev else None
        if pkt is None:
            self.bus.send_handshake(P.NAK, "设备暂无报告")
            return None
        self.bus.send_data(pkt, pid)
        self.bus.send_handshake(P.ACK, "中断 IN")
        self.in_toggle[(addr, ep)] = self.in_toggle.get((addr, ep), 0) ^ 1
        return pkt

    # ---------- 场景便捷 ----------
    def attach_tree(self, hub: HubDevice, *devices: UsbDevice):
        """根集线器 + 设备挂载（总线拓扑）。"""
        self.bus.devices[0x01 if not self.bus.devices else max(self.bus.devices) + 1] = hub  # hub 自身占地址
        for i, d in enumerate(devices, 1):
            hub.attach(i, d)
