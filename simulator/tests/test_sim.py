"""usbsim 全链路自测（无 pytest 依赖，直接 python 运行）。

覆盖: CRC 属性/双实现、枚举序列、描述符、BOT 写读校验、中断报告、PD 协商、BLE GATT。
"""
import json
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from usbsim import packets as P, bus as B, device as D, host as H, pd as PD, ble as BLE


def test_crc():
    def crc5_ref(v, bits=11):
        data = (v & ((1 << bits) - 1)) << 5
        crc = 0x1F
        for _ in range(bits):
            msb, dbit = (crc >> 4) & 1, (data >> 15) & 1
            crc = (crc << 1) & 0x1F
            if msb ^ dbit:
                crc ^= 0x05
            data = (data << 1) & 0xFFFF
        return (~crc) & 0x1F

    def crc16_ref(d):
        # 反射式（USB 软件域通行口径）: init 0xFFFF、0xA001、无最终取反
        crc = 0xFFFF
        for byte in d:
            crc ^= byte
            for _ in range(8):
                crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
        return crc

    random.seed(9)
    for _ in range(100):
        v = random.getrandbits(11)
        assert P.crc5(v) == crc5_ref(v)
    for _ in range(200):
        d = random.randbytes(16)
        assert P.crc16(d) == crc16_ref(d)
    assert P.crc16(b"123456789") == 0x4B37, "CRC-16/USB 目录值 0xB4C8 取反前"
    tok = P.token(0x05, 0x02, P.SETUP)
    assert P.parse(tok)["crc5_ok"]
    dp = P.data_packet(b"USBLabs")
    assert P.parse(dp)["crc16_ok"]
    v = 0x297
    good = P.crc5(v)
    for i in range(11):
        bad = ((v | good << 11) ^ (1 << i))
        assert P.crc5(bad & 0x7FF) != (bad >> 11) & 0x1F
    print("  ✓ CRC 属性/双实现/翻转检测")


def test_enumeration_and_hid():
    bus = B.Bus()
    host = H.Host(bus)
    dev_desc = (bytes([0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40])
                + (0x1234).to_bytes(2, "little") + (0x0002).to_bytes(2, "little")
                + (0x0100).to_bytes(2, "little") + bytes([1, 2, 0, 1]))
    cfg_desc = (bytes([0x09, 0x02, 0x22, 0x00, 0x02, 0x01, 0x00, 0xA0, 0xFA])
                + bytes([0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00])
                + bytes([0x09, 0x21, 0x01, 0x01, 0x00, 0x01, 0x22, 0x22, 0x00])
                + bytes([0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x0A]))
    hid = D.HidDevice(dev_desc, cfg_desc,
                      {1: "USB-Labs Keyboard", 2: "USB-Labs Mouse"})
    addr = host.enumerate(hid, target_addr=5)
    assert addr == 5 and hid.state == "Configured" and hid.configuration == 1
    assert host.interrupt_poll(5, 1) is None
    hid.send_keyboard(0x02, [0x04])
    rep = host.interrupt_poll(5, 1)
    assert rep == bytes([0x02, 0x00, 0x04, 0, 0, 0, 0, 0])
    hid.send_mouse(1, 5, -3)
    rep = host.interrupt_poll(5, 2)
    assert rep[:3] == bytes([1, 5, 253])
    kinds = [e["kind"] for e in bus.capture]
    assert kinds.count("RESET") == 1 and "ENUM" in kinds
    print("  ✓ 枚举 + HID 中断轮询")


def test_msc_bot():
    bus = B.Bus()
    host = H.Host(bus)
    dev_desc = (bytes([0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40])
                + (0xCAFE).to_bytes(2, "little") + (0x4002).to_bytes(2, "little")
                + (0x0100).to_bytes(2, "little") + bytes([1, 2, 0, 1]))
    cfg_desc = (bytes([0x09, 0x02, 0x20, 0x00, 0x01, 0x01, 0x00, 0x80, 0xFA])
                + bytes([0x09, 0x04, 0x00, 0x00, 0x02, 0x08, 0x06, 0x50, 0x00]))
    msc = D.MscDevice(dev_desc, cfg_desc)
    addr = host.enumerate(msc, target_addr=3)

    def cbw(tag, dlen, data_in, cbd):
        return (b"USBC" + tag.to_bytes(4, "little") + dlen.to_bytes(4, "little")
                + bytes([0x80 if data_in else 0x00, 0x00, len(cbd)]) + cbd.ljust(16, b"\x00"))

    # INQUIRY（CDB: 12 00 00 00 24 00）—— 数据与 CSW 都要消费
    host.bulk_write(addr, 1, cbw(1, 36, True, bytes([0x12, 0, 0, 0, 0x24, 0])))
    rx = host.bulk_read(addr, 1, 36)
    assert rx[8:16] == b"USB-Labs", "厂商串位于 INQUIRY 偏移 8"
    csw0 = host.bulk_read(addr, 1, 13)
    assert csw0[:4] == b"USBS" and csw0[12] == 0, "INQUIRY CSW 未消费（流水线错位根因）"

    # WRITE10 写入 + 消费 CSW
    payload = b"USB-Labs SIMULATOR SECTOR" + b"\xAB" * (512 - 25)
    cdb_w = bytes([0x2A, 0x00]) + (0).to_bytes(4, "big") + b"\x00" + (1).to_bytes(2, "big") + b"\x00"
    host.bulk_write(addr, 1, cbw(2, 512, False, cdb_w) + payload)
    csw = host.bulk_read(addr, 1, 13)
    assert csw[:4] == b"USBS" and csw[12] == 0 and csw[4:8] == (2).to_bytes(4, "little")
    assert msc.disk[0][:25] == payload[:25], "RAM 盘应收到写入数据"

    # READ10 读回 + CSW
    cdb_r = bytes([0x28, 0x00]) + (0).to_bytes(4, "big") + b"\x00" + (1).to_bytes(2, "big") + b"\x00"
    host.bulk_write(addr, 1, cbw(3, 512, True, cdb_r))
    rx = host.bulk_read(addr, 1, 512 + 13)
    assert rx[:25] == payload[:25], "读回数据不一致"
    assert rx[512:525] == b"USBS" + (3).to_bytes(4, "little") + b"\x00" * 5, "CSW 应为 tag3/PASS"
    print("  ✓ MSC BOT 写读校验")


def test_pd():
    src = PD.Source()
    snk = PD.Sink(want_v=9, want_i=2000)
    log = []
    ok = PD.negotiate(src, snk, log)
    assert ok and snk.rdo is not None
    assert log[0]["desc"].startswith("GoodCRC") and "3 个 PDO" in log[0]["desc"], "caps 广播的 GoodCRC 应首先出现"
    assert any("Request" in e["desc"] and "RDO" in e["desc"] for e in log)
    assert any("RDO" in e["desc"] for e in log)
    obj_pos = (snk.rdo >> 28) & 0x7
    assert obj_pos == 2, f"应选中第 2 个 PDO(9V)，实际 {obj_pos}"
    print("  ✓ PD 协商（PDO 选择/RDO/时序）")


def test_ble():
    per = BLE.GapPeripheral("USB-Labs Mouse")
    cen = BLE.GapCentral()
    adv = cen.scan(per)
    assert b"USB-Labs Mouse" in adv
    cen.connect(per)
    cen.discover(per)
    assert 0x0003 in cen.discovered and cen.discovered[0x0003][1] == b"USB-Labs Mouse"
    hit = []
    assert cen.subscribe(per, 0x0012, lambda v: hit.append(v))
    assert hit == [b"\x05\x01\x09\x06"]
    print("  ✓ BLE 广播/发现/读取/订阅")


if __name__ == "__main__":
    test_crc()
    test_enumeration_and_hid()
    test_msc_bot()
    test_pd()
    test_ble()
    print("\nusbsim 全部自测通过 ✓")
