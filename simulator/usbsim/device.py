"""usbsim.device —— 设备模型: EP0 状态机 + 描述符 + HID/MSC/Hub 功能模型。"""
from __future__ import annotations
from . import packets as P

GET_STATUS, CLEAR_FEATURE, SET_FEATURE = 0x00, 0x01, 0x03
SET_ADDRESS, GET_DESCRIPTOR, SET_CONFIGURATION = 0x05, 0x06, 0x09
GET_INTERFACE, SET_INTERFACE = 0x0A, 0x0B
DESC_DEVICE, DESC_CONFIG, DESC_STRING = 0x01, 0x02, 0x03


class Endpoint:
    def __init__(self, number: int, direction: str, mps: int, kind: str):
        self.number, self.direction, self.mps, self.kind = number, direction, mps, kind
        self.toggle = 0
        self.halted = False
        self.rx_buffer = bytearray()
        self.tx_queue: list[bytes] = []

    def next_in_packet(self) -> bytes | None:
        if self.halted or not self.tx_queue:
            return None
        return self.tx_queue.pop(0)

    def flip(self):
        self.toggle ^= 1


class UsbDevice:
    def __init__(self, device_desc: bytes, config_desc: bytes,
                 strings: dict[int, str] | None = None, vid: int = 0x1234, pid: int = 0x0001):
        self.device_desc = device_desc
        self.config_desc = config_desc
        self.strings = strings or {}
        self.address = 0
        self.state = "Default"
        self.configuration = 0
        self.ep0_mps = device_desc[7]
        self.vid, self.pid = vid, pid
        self._setup: dict | None = None
        self._data_tx = bytearray()
        self._data_rx = bytearray()
        self.endpoints: dict[tuple[int, str], Endpoint] = {}

    def _descriptor(self, wValue: int) -> bytes | None:
        dtype, idx = (wValue >> 8) & 0xFF, wValue & 0xFF   # 高字节=类型, 低字节=索引（§9.4.3）
        if dtype == DESC_DEVICE:
            return self.device_desc
        if dtype == DESC_CONFIG:
            return self.config_desc
        if dtype == DESC_STRING:
            return self.strings.get(idx, "").encode("utf-16-le") if idx in self.strings else None
        return None

    def ep0_setup(self, packet: dict):
        req = bytes.fromhex(packet["payload"])
        assert len(req) == 8, "SETUP 数据必须 8 字节"
        bmRequestType, bRequest = req[0], req[1]
        wValue, wIndex, wLength = (int.from_bytes(req[2:4], "little"),
                                   int.from_bytes(req[4:6], "little"),
                                   int.from_bytes(req[6:8], "little"))
        self._setup = {"type": bmRequestType, "req": bRequest,
                       "wValue": wValue, "wIndex": wIndex, "wLength": wLength}
        self._data_tx, self._data_rx = bytearray(), bytearray()
        if bRequest == SET_ADDRESS and self.state in ("Default", "Addressed"):
            self.address = wValue
            self.state = "Addressed"
        elif bRequest == SET_CONFIGURATION:
            self.configuration = wValue
            self.state = "Configured" if wValue else "Addressed"
        elif bRequest == GET_DESCRIPTOR and bmRequestType & 0x80:
            desc = self._descriptor(wValue)
            if desc is None:
                self._setup["stall"] = True
            else:
                self._data_tx = bytearray(desc[:wLength])

    def ep0_data_in(self) -> bytes | None:
        if not self._setup or self._setup.get("stall"):
            return None
        chunk, self._data_tx = self._data_tx[:self.ep0_mps], self._data_tx[self.ep0_mps:]
        return bytes(chunk)

    def ep0_data_out(self, data: bytes):
        self._data_rx += data

    # ---------- 端点交互钩子（功能子类覆盖） ----------
    def on_out(self, ep: int, data: bytes):
        ep_obj = self.endpoints.get((ep, "OUT"))
        if ep_obj is not None:
            ep_obj.rx_buffer += data

    def bulk_in_packet(self, ep: int) -> bytes | None:
        ep_obj = self.endpoints.get((ep, "IN"))
        return ep_obj.next_in_packet() if ep_obj else None

    def interrupt_in_packet(self, ep: int) -> bytes | None:
        return self.bulk_in_packet(ep)

    def bus_reset(self):
        self.address, self.state, self.configuration = 0, "Default", 0
        self._setup = None
        for ep in self.endpoints.values():
            ep.toggle, ep.halted = 0, False


class HubDevice(UsbDevice):
    def __init__(self, ports: int = 4):
        super().__init__(
            bytes([0x12, 0x01, 0x00, 0x02, 0x09, 0x00, 0x01, 0x40]) + bytes(10),
            bytes(0), {1: "USB-Labs Root Hub"}, vid=0x1D6B, pid=0x0001)
        self.ports: dict[int, UsbDevice | None] = {i: None for i in range(1, ports + 1)}

    def attach(self, port: int, device: UsbDevice):
        self.ports[port] = device
        device.state = "Attached"

    def reset_port(self, port: int):
        dev = self.ports.get(port)
        if dev:
            dev.bus_reset()


class HidDevice(UsbDevice):
    def __init__(self, device_desc, config_desc, strings=None):
        super().__init__(device_desc, config_desc, strings)
        self.endpoints = {(1, "IN"): Endpoint(1, "IN", 8, "interrupt"),
                          (2, "IN"): Endpoint(2, "IN", 4, "interrupt")}
        self.ep1_in = self.endpoints[(1, "IN")]
        self.ep2_in = self.endpoints[(2, "IN")]

    def send_keyboard(self, modifiers: int, keys: list[int]):
        self.ep1_in.tx_queue.append(bytes([modifiers, 0] + keys + [0] * (6 - len(keys))))

    def send_mouse(self, buttons: int, dx: int, dy: int):
        self.ep2_in.tx_queue.append(bytes([buttons, dx & 0xFF, dy & 0xFF]))


class MscDevice(UsbDevice):
    INQUIRY = bytes([0x00, 0x80, 0x04, 0x02, 0, 0, 0, 0]) + b"USB-Labs ".ljust(8) + b"RAM Disk".ljust(16) + b"1.0 ".ljust(4)  # 36B: 头4B+保留4B+厂商8+产品16+版本4

    def __init__(self, device_desc, config_desc, blocks: int = 64, strings=None):
        super().__init__(device_desc, config_desc, strings)
        self.endpoints = {(1, "OUT"): Endpoint(1, "OUT", 64, "bulk"),
                          (1, "IN"): Endpoint(1, "IN", 64, "bulk")}
        self.ep_out = self.endpoints[(1, "OUT")]
        self.ep_in = self.endpoints[(1, "IN")]
        self.blocks = blocks
        self.disk = {lba: bytearray(512) for lba in range(blocks)}
        self.bot_tag = 0
        self._bot = "CBW"
        self._cbd = None
        self._rx = bytearray()
        self._expect = 0
        self._in_queue: list[bytes] = []

    def on_out(self, ep: int, data: bytes):
        if self._bot == "CBW":
            assert data[:4] == b"USBC", "CBW 签名错误"
            self.bot_tag = int.from_bytes(data[4:8], "little")
            dlen = int.from_bytes(data[8:12], "little")
            self._cbd = data[15:31]
            data_ret, status = self._scsi(self._cbd)
            if self._cbd[0] == 0x2A and dlen:
                self._bot = "DATA_OUT"
                self._rx, self._expect = bytearray(data[31:]), dlen   # BOT 允许 CBW 与数据首段同包
                return
            if dlen and data_ret:
                self._in_queue.extend([data_ret, self._csw_bytes(status)])
                return
            self._in_queue.append(self._csw_bytes(status))
        elif self._bot == "DATA_OUT":
            self._rx += data
            if len(self._rx) >= self._expect:
                lba = int.from_bytes(self._cbd[2:6], "big")
                for i in range(self._expect // 512):
                    self.disk[lba + i] = bytearray(self._rx[i * 512:(i + 1) * 512])
                self._in_queue.append(self._csw_bytes(0))
                self._bot = "CBW"

    def bulk_in_packet(self, ep: int) -> bytes | None:
        if self._in_queue:
            return self._in_queue.pop(0)
        return None

    def _csw_bytes(self, status: int) -> bytes:
        return b"USBS" + self.bot_tag.to_bytes(4, "little") + (0).to_bytes(4, "little") + bytes([status])

    def _scsi(self, cbd: bytes) -> tuple[bytes, int]:
        op = cbd[0]
        if op == 0x00:
            return b"", 0
        if op == 0x12:
            return self.INQUIRY[:36], 0
        if op == 0x25:
            return (self.blocks - 1).to_bytes(4, "big") + (512).to_bytes(4, "big"), 0
        if op in (0x28, 0x2A):
            lba = int.from_bytes(cbd[2:6], "big")
            n = int.from_bytes(cbd[7:9], "big")
            data = b"".join(bytes(self.disk.get(lba + i, b"\0" * 512)) for i in range(n))
            return data, 0
        return b"", 0x01


def struct_pack(lba: int, blk: int) -> bytes:
    return lba.to_bytes(4, "big") + blk.to_bytes(4, "big")
