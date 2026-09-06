"""msc_test 离线自测：容量探测内核（RC10→哨兵→RC16）与 READ/WRITE CDB 选路。

无 pyusb 依赖：内核级用 FakeMsc（scsi callable 假件应答），处理器级用
FakeBotDev（BOT 传输层假件，经 ctx["dev"] 注入——_d 只在 dev 为 None 时
open_device）。对应 C++ 侧模板内核 msc_capacity_probe_impl / msc_read_cdb
的假件自测（apps/win/USBTestStudio，evolve #75）；Python 参考实现同口径
收口（evolve #76）。运行: python tools/usbtest/tests/test_msc.py（任意 CWD）。
"""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from usbtest import msc_test
from usbtest.msc_test import msc_capacity_probe, msc_rw_cdb

RC16_CDB_D11 = bytes.fromhex("9E 10 00 00 00 00 00 00 00 00 00 00 00 20 00 00")


class FakeMsc:
    """内核假件：按 CDB[0] 应答（0x25=RC10 / 0x9E=RC16），记录全部 CDB。"""

    def __init__(self, rc10_last, rc10_blk=512, rc16_last=None, rc16_blk=512, rc10_status=0):
        self.rc10_last, self.rc10_blk = rc10_last, rc10_blk
        self.rc16_last, self.rc16_blk = rc16_last, rc16_blk
        self.rc10_status = rc10_status
        self.seen = []

    def __call__(self, cdb, data_dir, data):
        self.seen.append(bytes(cdb))
        if cdb[0] == 0x25:
            assert data_dir == "IN" and len(data) == 8
            return self.rc10_status, struct.pack(">II", self.rc10_last, self.rc10_blk)
        if cdb[0] == 0x9E:
            assert self.rc16_last is not None, "未配 RC16 应答却收到 RC16"
            assert data_dir == "IN" and len(data) == 32
            return 0, struct.pack(">QI", self.rc16_last, self.rc16_blk) + b"\0" * 20
        raise AssertionError(f"意外 CDB {bytes(cdb).hex()}")


class _Ep:
    def __init__(self, addr):
        self.bEndpointAddress, self.bmAttributes = addr, 2   # 批量端点


_EPS = [_Ep(0x01), _Ep(0x81)]    # OUT / IN（_bulk_eps 遍历形：配置→[接口]→端点）


class FakeBotDev:
    """BOT 假件（处理器级）：31 字节 CBW 解析 + SBC-3 容量/读写应答 + CSW 回签。

    CDB 取自 CBW[15:15+cbLen]（BOT 规范与 usbsim 仿真器 data[15:31] 同口径）；
    读写按 SBC-3 字节位解析（READ/WRITE(10)：LBA[2..5] 块数[7..8]；(16)：
    LBA[2..9] 块数[12..13]），错位 CDB 在此即解不出预期 LBA。
    """

    def __init__(self, total_sectors, block_size=512):
        self.total, self.blk = total_sectors, block_size
        self.cbws = []
        self.written = {}      # lba -> 已写图案（READ 回放）
        self._frames = []      # 待读帧（IN 载荷与 CSW 交替入列）
        self._pending = None

    def __iter__(self):
        return iter([[_EPS]])

    @staticmethod
    def _csw(tag, status):
        return b"USBS" + struct.pack("<II", tag, 0) + bytes([status])

    @staticmethod
    def _check_cbw(cbw, cdb, payload, out_len=None):
        """CBW 头部与命令语义一致性钉（对抗复核 P1：方向位/LUN/cbLen/dlen）。"""
        dlen = struct.unpack("<I", cbw[8:12])[0]
        assert cbw[13] == 0, "bCBWLUN 应为 0"
        assert cbw[12] == (0x80 if payload else 0x00), "bmCBWFlags 与数据相方向不符"
        assert cbw[14] == len(cdb) == (16 if cdb[0] in (0x9E, 0x88, 0x8A) else 10)
        assert dlen == (len(payload) if payload else (out_len or 0)), "dCBWDataTransferLength 失配"

    def write(self, ep, data, timeout=0):
        data = bytes(data)
        if data[:4] != b"USBC":                      # OUT 数据相：待发图案
            lba, blocks = self._pending
            assert len(data) == blocks * self.blk, "OUT 数据长与 CDB 块数不符"
            self.written[lba] = data
            self._frames.append(self._csw(self._pending_tag, 0))
            return
        assert len(data) == 31, f"CBW 须为 31 字节（BOT）, 实得 {len(data)}"
        self.cbws.append(data)
        tag = struct.unpack("<I", data[4:8])[0]
        cdb = data[15:15 + data[14]]
        op = cdb[0]
        wide = op in (0x88, 0x8A)
        if op == 0x25:
            last = min(self.total - 1, 0xFFFFFFFF)
            payload = struct.pack(">II", last, self.blk)
            self._check_cbw(data, cdb, payload)
            self._frames += [payload, self._csw(tag, 0)]
        elif op == 0x9E:
            payload = struct.pack(">QI", self.total - 1, self.blk) + b"\0" * 20
            self._check_cbw(data, cdb, payload)
            self._frames += [payload, self._csw(tag, 0)]
        elif op in (0x28, 0x88):
            lba = int.from_bytes(cdb[2:10] if wide else cdb[2:6], "big")
            blocks = int.from_bytes(cdb[12:14] if wide else cdb[7:9], "big")
            buf = self.written.get(lba)
            assert buf is not None and len(buf) == blocks * self.blk, "READ 未写区或长度不符"
            self._check_cbw(data, cdb, buf)
            self._frames += [buf, self._csw(tag, 0)]
        elif op in (0x2A, 0x8A):
            blocks = int.from_bytes(cdb[12:14] if wide else cdb[7:9], "big")
            self._check_cbw(data, cdb, b"", out_len=blocks * self.blk)
            self._pending = (int.from_bytes(cdb[2:10] if wide else cdb[2:6], "big"), blocks)
            self._pending_tag = tag
        else:
            self._frames.append(self._csw(tag, 1))

    def read(self, ep, length, timeout=0):
        assert self._frames, "无待读帧"
        frame = self._frames.pop(0)
        assert len(frame) == length, f"帧长 {len(frame)} != 请求 {length}"
        return frame

    def ops(self):
        return [c[15] for c in self.cbws]


class TestCapacityKernel(unittest.TestCase):
    def test_rc10_plain_under_sentinel(self):
        f = FakeMsc(rc10_last=0x0FFFFFFF)            # 128GiB 盘，哨兵之下
        self.assertEqual(msc_capacity_probe(f), (0x10000000, 512))
        self.assertEqual(f.seen, [bytes([0x25]) + b"\0" * 9])   # 单发 RC10，不升 16

    def test_sentinel_upgrades_to_rc16(self):
        f = FakeMsc(rc10_last=0xFFFFFFFF, rc16_last=7814037168 - 1)   # 4TB@512B
        self.assertEqual(msc_capacity_probe(f), (7814037168, 512))
        self.assertEqual([c[0] for c in f.seen], [0x25, 0x9E])

    def test_rc16_cdb_matches_acceptance_table(self):
        # 与验收表 D11 手工 CDB（apps/验收-工程师通信控制台.md）逐字节一致
        f = FakeMsc(rc10_last=0xFFFFFFFF, rc16_last=1)
        msc_capacity_probe(f)
        self.assertEqual(f.seen[1], RC16_CDB_D11)

    def test_rc16_block_zero_rejected(self):
        f = FakeMsc(rc10_last=0xFFFFFFFF, rc16_last=7, rc16_blk=0)
        with self.assertRaises(AssertionError):
            msc_capacity_probe(f)

    def test_rc10_status_nonzero_rejected(self):
        with self.assertRaises(AssertionError):
            msc_capacity_probe(FakeMsc(rc10_last=8, rc10_status=1))

    def test_rc10_block_zero_rejected(self):
        with self.assertRaises(AssertionError):
            msc_capacity_probe(FakeMsc(rc10_last=8, rc10_blk=0))

    def test_sentinel_gb_exceeds_rc10_ceiling(self):
        # 旧口径病：哨兵盘恒算 2199.02GB（0x100000000×512/1e9）——真值必须越过它
        total, blk = msc_capacity_probe(FakeMsc(rc10_last=0xFFFFFFFF, rc16_last=7814037168 - 1))
        self.assertGreater(total * blk / 1e9, 2300)


class TestRwCdb(unittest.TestCase):
    def test_read10_layout(self):
        # SBC-3：[1]=DPO/FUA 标志（恒 0）、LBA=[2..5]、[6]=GROUP NUMBER、块数=[7..8]
        self.assertEqual(msc_rw_cdb(0x28, 0x88, 0x12345678, 8),
                         bytes([0x28, 0x00, 0x12, 0x34, 0x56, 0x78, 0x00, 0x00, 0x08, 0x00]))

    def test_write10_symmetric(self):
        # [0]=0x2A，[1]=flags 0，[2..5]=LBA 0，[6]=GROUP 0，[7..8]=块数 0x0004，[9]=CONTROL
        self.assertEqual(msc_rw_cdb(0x2A, 0x8A, 0, 4),
                         bytes([0x2A]) + b"\0" * 7 + bytes([0x04, 0x00]))


    def test_boundary_inclusive_uses_10(self):
        # 最高寻址 LBA = lba+blocks-1；恰含端 0xFFFFFFFF（和=0x100000000）仍 10 字节 CDB
        self.assertEqual(msc_rw_cdb(0x28, 0x88, 0xFFFFFFFF, 1)[0], 0x28)
        self.assertEqual(msc_rw_cdb(0x28, 0x88, 0xFFFFFF00, 0x100)[0], 0x28)
        self.assertEqual(len(msc_rw_cdb(0x28, 0x88, 0xFFFFFF00, 0x100)), 10)

    def test_over_boundary_uses_16(self):
        cdb = msc_rw_cdb(0x28, 0x88, 0xFFFFFFFF, 2)
        self.assertEqual(cdb[0], 0x88)
        self.assertEqual(cdb[2:10], (0xFFFFFFFF).to_bytes(8, "big"))
        self.assertEqual(cdb[10:12], b"\0\0")                    # GROUP NUMBER 段
        self.assertEqual(cdb[12:14], (2).to_bytes(2, "big"))     # 块数 [12..13]
        self.assertEqual(cdb[14:16], b"\0\0")
        self.assertEqual(len(cdb), 16)

    def test_lba_over_32bit_no_overflow(self):
        # 旧口径病：lba≥2^32 时 to_bytes(4) OverflowError（#75 记录）
        cdb = msc_rw_cdb(0x28, 0x88, 2**32, 1)
        self.assertEqual(cdb[0], 0x88)
        self.assertEqual(cdb[2:10], (2**32).to_bytes(8, "big"))

    def test_write16_symmetric(self):
        cdb = msc_rw_cdb(0x2A, 0x8A, 2**40, 16)
        self.assertEqual(cdb[0], 0x8A)
        self.assertEqual(cdb[2:10], (2**40).to_bytes(8, "big"))
        self.assertEqual(cdb[12:14], (16).to_bytes(2, "big"))


class TestHandlers(unittest.TestCase):
    @staticmethod
    def _ctx(dev):
        return {"dev": dev, "device": {"vid": 0, "pid": 0}}

    def test_cbw_is_31_bytes(self):
        dev = FakeBotDev(0x10000000)
        msc_test.HANDLERS["msc_capacity"](self._ctx(dev), {"limits": {"min_gb": 0}})
        self.assertTrue(dev.cbws)
        self.assertTrue(all(len(c) == 31 for c in dev.cbws))    # BOT：CBW 恒 31 字节

    def test_capacity_plain_disk(self):
        dev = FakeBotDev(0x10000000)                 # 128GiB@512B = 137.44GB
        r = msc_test.HANDLERS["msc_capacity"](self._ctx(dev), {"limits": {"min_gb": 100}})
        self.assertTrue(r.passed)
        self.assertEqual(r.measured["gb"], 137.44)
        self.assertEqual(dev.ops(), [0x25])

    def test_capacity_4tb_uses_rc16_and_true_gb(self):
        dev = FakeBotDev(7814037168)                 # 4TB：RC10 哨兵→RC16 真值
        r = msc_test.HANDLERS["msc_capacity"](self._ctx(dev), {"limits": {"min_gb": 3000}})
        self.assertTrue(r.passed)
        self.assertEqual(r.measured["gb"], round(7814037168 * 512 / 1e9, 2))
        self.assertEqual(dev.ops(), [0x25, 0x9E])

    def test_write_verify_cdb_bytes_pinned(self):
        # 旧口径病：LBA 装在 [1..4]、块数装在 [6..7]（错位一字节）——lba≠0 即错扇区
        dev = FakeBotDev(0x10000000)
        r = msc_test.HANDLERS["msc_write_verify"](self._ctx(dev), {"lba": 123, "blocks": 8})
        self.assertTrue(r.passed)
        w, rd = (bytes(c[15:15 + c[14]]) for c in dev.cbws)
        self.assertEqual(w, bytes([0x2A, 0, 0, 0, 0, 0x7B, 0, 0, 8, 0]))
        self.assertEqual(rd, bytes([0x28, 0, 0, 0, 0, 0x7B, 0, 0, 8, 0]))

    def test_write_verify_high_lba_roundtrip(self):
        # 旧口径病：lba≥2^32 直接 OverflowError；此处应选 WRITE16/READ16 完成回读
        dev = FakeBotDev(2**33 + 64)
        r = msc_test.HANDLERS["msc_write_verify"](self._ctx(dev),
                                                 {"lba": 2**32, "blocks": 8, "block_size": 512})
        self.assertTrue(r.passed)
        self.assertEqual(r.measured["bytes"], 8 * 512)
        self.assertEqual(dev.ops(), [0x8A, 0x88])
        self.assertEqual(len(dev.written[2**32]), 8 * 512)

    def test_write_verify_zero_blocks_rejected(self):
        # 与 C++ msc_read_blocks_impl 同口径：blocks=0 直接拒绝（越界 1..65535）
        dev = FakeBotDev(0x10000000)
        with self.assertRaises(AssertionError):
            msc_test.HANDLERS["msc_write_verify"](self._ctx(dev), {"lba": 0, "blocks": 0})


if __name__ == "__main__":
    unittest.main(verbosity=1)
