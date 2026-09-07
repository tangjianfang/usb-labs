"""MSC 产测后端：BOT 直写 SCSI（INQUIRY/READ_CAPACITY/写读校验）——产线级盘体检。
依赖: pip install pyusb
注意: write_verify 会破坏 DUT 数据，仅用于产线/授权测试（DESTRUCTIVE 标记）。
"""
import struct

from usbtest.core import StepResult   # 旧版漏导入：真实后端任一步骤派发即 NameError（#76）

HANDLERS = {}


def handler(t):
    def deco(fn):
        HANDLERS[t] = fn
        return fn
    return deco


def open_device(dev):
    import usb.core
    d = usb.core.find(idVendor=dev.get("vid"), idProduct=dev.get("pid"))
    assert d is not None, "未发现 MSC 设备"
    d.set_configuration()
    return d


def _d(ctx):
    if ctx.get("dev") is None:
        ctx["dev"] = open_device(ctx["device"])
    return ctx["dev"]


def _bulk_eps(d):
    bo = bi = None
    for cfg in d:
        for itf in cfg:
            for ep in itf:
                if ep.bEndpointAddress & 0x80 and ep.bmAttributes & 2:
                    bi = ep.bEndpointAddress
                elif not (ep.bEndpointAddress & 0x80) and ep.bmAttributes & 2:
                    bo = ep.bEndpointAddress
    assert bo is not None and bi is not None, "未找到批量端点"
    return bo, bi


def _cbw(tag, data_len, flags, cbd):
    # BOT 31 字节：sig4 + tag4 + dlen4 + flags[12] + LUN[13] + cbLen[14] + CDB16[15..30]
    # （与 usbsim 仿真器 data[15:31] 取 CDB 同口径；旧版 flags 段多两个 0 → 33 字节
    # 非规范 CBW，规范设备应拒收，evolve #76 修复）
    return (b"USBC" + struct.pack("<II", tag, data_len) +
            bytes([0x80 if flags else 0x00, 0, len(cbd)]) + cbd.ljust(16, b"\0"))


def _scsi(ctx, cbd, data_dir, data=b""):
    d = _d(ctx)
    bo, bi = _bulk_eps(d)
    tag = 0xDEAD + (data_dir == "IN")
    d.write(bo, _cbw(tag, len(data) if data_dir else 0, 0x80 if data_dir == "IN" else 0x00, cbd), 5000)
    rx = b""
    if data_dir == "IN" and data:
        rx = d.read(bi, len(data), 5000)
        rx = bytes(rx)
    elif data_dir == "OUT" and data:
        d.write(bo, data, 5000)
    csw = bytes(d.read(bi, 13, 5000))
    assert csw[:4] == b"USBS" and struct.unpack("<I", csw[4:8])[0] == tag, "CSW 校验失败"
    status = csw[12]
    return status, rx


@handler("msc_inquiry")
def inquiry(ctx, step):
    status, rx = _scsi(ctx, bytes([0x12, 0, 0, 0, 36, 0]), "IN", b"\0" * 36)
    vendor, product = rx[8:16].decode("ascii", "replace").strip(), rx[16:32].decode("ascii", "replace").strip()
    return StepResult(step.get("name", "INQUIRY"), status == 0, {"vendor": vendor, "product": product})


# ---------------------------------------------------------------------------
# 内核（evolve #76）：与 C++ 同口径收口——apps/win/USBTestStudio/src/usb/msc_scsi.h
# 的 msc_capacity_probe_impl / msc_read_cdb（evolve #75）。scsi 形参为可注入
# callable（cdb, data_dir, data) -> (status, rx)，假件离线自测见 tests/test_msc.py。
# ---------------------------------------------------------------------------

def msc_capacity_probe(scsi):
    """容量探测内核：READ_CAPACITY(10)→0xFFFFFFFF 哨兵→READ_CAPACITY(16)。

    与 C++ msc_capacity_probe_impl 同口径（evolve #75/#76）——旧实现 RC10-only，
    >2TB 盘按 SBC-3 回 0xFFFFFFFF 哨兵后 (last+1)*blk 恒算 2199.02GB@512B
    静默错值。RC16：opcode 0x9E / service action 0x10（byte[1]），分配长度
    32（byte[13]，与验收表 D11 手工 CDB 逐字节一致），last LBA 大端 [0..7]、
    块长大端 [8..11]，块长 0 拒绝。
    """
    status, rx = scsi(bytes([0x25]) + b"\0" * 9, "IN", b"\0" * 8)
    last, blk = struct.unpack(">II", rx[:8])
    if last != 0xFFFFFFFF:
        assert status == 0 and blk > 0, "READ_CAPACITY(10) 失败或块长 0"
        return last + 1, blk
    status, rx = scsi(bytes([0x9E, 0x10]) + b"\0" * 11 + bytes([32, 0, 0]), "IN", b"\0" * 32)
    assert status == 0 and len(rx) >= 12, "READ_CAPACITY(16)（>2TB）失败或短读"
    last = struct.unpack(">Q", rx[:8])[0]
    blk = struct.unpack(">I", rx[8:12])[0]
    assert blk > 0, "READ_CAPACITY(16) 返回块长 0"
    return last + 1, blk


def msc_rw_cdb(op10, op16, lba, blocks):
    """READ/WRITE CDB 选路（纯函数）：最高寻址 LBA（lba+blocks-1）≤0xFFFFFFFF
    用 10 字节 CDB（最大兼容）；越过即 16 字节（LBA 8 字节大端）。

    与 C++ msc_read_cdb 同口径（evolve #75/#76；WRITE 0x2A/0x8A 为对称扩展，
    C++ 上位机本版只读、不实现写路径）。SBC-3 布位：10 字节
    [1]=DPO/FUA 标志、LBA=[2..5]、[6]=GROUP NUMBER、块数=[7..8]；16 字节
    LBA=[2..9]、块数=[12..13]。旧实现 LBA 装在 [1..4]、块数装在 [6..7] 且仅
    9 字节——整体错位一字节：lba≠0 即错扇区（lab3 计划 lba:0 仅掩蔽 LBA 错位），
    块数错位未被掩蔽（blocks:8 会被规范设备读作 0x0800=2048 块，与 CBW 数据长
    失配）；lba≥2^32 另有 to_bytes(4) OverflowError。
    """
    if lba + blocks <= 0x100000000:
        return (bytes([op10, 0]) + lba.to_bytes(4, "big") + b"\0" +
                blocks.to_bytes(2, "big") + b"\0")
    return (bytes([op16, 0]) + lba.to_bytes(8, "big") + b"\0\0" +
            blocks.to_bytes(2, "big") + b"\0\0")


@handler("msc_capacity")
def capacity(ctx, step):
    total, blk = msc_capacity_probe(lambda cbd, d, data: _scsi(ctx, cbd, d, data))
    gb = total * blk / 1e9
    lo = float((step.get("limits") or {}).get("min_gb", 0))
    return StepResult(step.get("name", "容量"), gb >= lo,
                      {"gb": round(gb, 2), "block": blk}, "" if gb >= lo else f"低于下限 {lo}GB")


@handler("msc_write_verify")
def write_verify(ctx, step):
    """写读校验（DESTRUCTIVE）：向指定 LBA 写随机图案并回读比对（产线全盘扫描的循环体）。"""
    import os
    lba = int(step.get("lba", 0))
    blocks = int(step.get("blocks", 8))
    assert blocks > 0, "块数须 ≥ 1（C++ msc_read_blocks_impl 同口径拒绝 0）"
    blk_size = int(step.get("block_size", 512))
    pattern = os.urandom(blk_size * blocks)
    cbd_w = msc_rw_cdb(0x2A, 0x8A, lba, blocks)
    cbd_r = msc_rw_cdb(0x28, 0x88, lba, blocks)
    st1, _ = _scsi(ctx, cbd_w, "OUT", pattern)
    st2, rx = _scsi(ctx, cbd_r, "IN", b"\0" * (blk_size * blocks))
    ok = st1 == 0 and st2 == 0 and rx == pattern
    return StepResult(step.get("name", "写读校验"), ok,
                      {"lba": lba, "bytes": len(rx)}, "" if ok else "数据比对不一致")
