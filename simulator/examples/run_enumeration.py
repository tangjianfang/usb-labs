import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))) + "/simulator")

from usbsim import packets as P, device as D, bus as B
from usbsim.host import Host

bus = B.Bus()
host = Host(bus)
hid = D.HidDevice(
    bytes([0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40]) + (0x1234).to_bytes(2, "little")
    + (0x0002).to_bytes(2, "little") + (0x0100).to_bytes(2, "little") + bytes([1, 2, 0, 1]),
    bytes([0x09, 0x02, 0x22, 0x00, 0x02, 0x01, 0x00, 0xA0, 0xFA])
    + bytes([0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00])
    + bytes([0x09, 0x21, 0x01, 0x01, 0x00, 0x01, 0x22, 0x22, 0x00])
    + bytes([0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x0A]),
    {1: "USB-Labs Keyboard", 2: "USB-Labs Mouse"})

addr = host.enumerate(hid, target_addr=5)
print(f"枚举完成: 地址 {addr}, 状态 {hid.state}")
hid.send_keyboard(0x02, [0x04])
rep = host.interrupt_poll(5, 1)
print(f"键盘报告: {rep.hex()}")
hid.send_mouse(1, 5, -3)
print(f"鼠标报告: {host.interrupt_poll(5, 2).hex()}")
print(f"\n抓包流共 {len(bus.capture)} 条总线事件，导出 simulator/enum_capture.jsonl")
from usbsim import capture as CAP
CAP.export_jsonl(bus.capture, "enum_capture.jsonl")
print(CAP.render_text(bus.capture)[-800:])
