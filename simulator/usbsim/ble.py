"""usbsim.ble —— BLE 链路层/GATT 消息级仿真：广播 → 连接 → GATT 发现 → 订阅通知。
层边界: 模拟包序列与 ATT/GATT/LLCP 语义（信道/跳频/调制不在范围），字节布局按 Core Spec 口径重构。
"""
from __future__ import annotations
import struct

from .logbase import setup

log = setup("usbsim.ble")

ADV_IND = 0x00
CONNECT_IND = 0x05


def ad_structure(ad_type: int, data: bytes) -> bytes:
    return bytes([1 + len(data), ad_type]) + data


class GapPeripheral:
    """外设: 广播载荷 + GATT 属性表（handle → (uuid, value, notify_able)）。"""

    def __init__(self, name: str):
        self.name = name
        self.attributes = {
            0x0001: ("0x2800", struct.pack("<H", 0x1800), False),                  # GAP 服务声明
            0x0002: ("0x2803", struct.pack("<BHH", 0x02, 0x0003, 0x2A00), False),  # GAP 特征声明
            0x0003: ("0x2A00", name.encode(), True),                               # Device Name
            0x0010: ("0x2800", struct.pack("<H", 0x1812), False),                  # HID 服务声明
            0x0011: ("0x2803", struct.pack("<BHH", 0x02, 0x0012, 0x1812), False),  # HID 特征声明
            0x0012: ("0x2A4B", b"\x05\x01\x09\x06", False),                        # Report Map（示例头）
        }
        self.connected = False

    def adv_payload(self) -> bytes:
        log.debug("广播载荷: name=%r len=%d", self.name, 5 + len(self.name))
        return ad_structure(0x01, b"\x06") + ad_structure(0x09, self.name.encode())

    def connect(self):
        self.connected = True
        log.info("外设 %r 连接建立", self.name)


class GapCentral:
    """中心设备: 扫描 → 连接 → GATT 发现 → 读/订阅。"""

    def __init__(self):
        self.discovered: dict[int, tuple[str, bytes]] = {}

    def scan(self, periph: GapPeripheral) -> bytes:
        adv = periph.adv_payload()
        log.debug("扫描: 收到广播 name=%r", periph.name)
        return adv

    def connect(self, periph: GapPeripheral):
        periph.connected = True
        log.info("中心设备连接 %r", periph.name)

    def discover(self, periph: GapPeripheral):
        self.discovered = {h: (u, v) for h, (u, v, _) in periph.attributes.items()}
        log.info("GATT 发现: %d 个属性（%r）", len(self.discovered), periph.name)

    def read(self, periph: GapPeripheral, handle: int) -> bytes:
        log.debug("读取 handle=0x%04X", handle)
        return periph.attributes[handle][1]

    def subscribe(self, periph: GapPeripheral, handle: int, callback) -> bool:
        entry = periph.attributes.get(handle)
        if entry is None:
            log.warning("订阅失败: handle=0x%04X 不存在（%r）", handle, periph.name)
            return False
        callback(entry[1])
        log.debug("订阅 handle=0x%04X 成功", handle)
        return True
