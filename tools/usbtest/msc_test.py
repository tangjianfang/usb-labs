"""MSC 产测后端：BOT 直写 SCSI（INQUIRY/READ_CAPACITY/写读校验）——产线级盘体检。
依赖: pip install pyusb
注意: write_verify 会破坏 DUT 数据，仅用于产线/授权测试（DESTRUCTIVE 标记）。
"""
import struct

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
    return (b"USBC" + struct.pack("<II", tag, data_len) +
            bytes([0x80 if flags else 0x00, 0, 0, 0, len(cbd)]) + cbd.ljust(16, b"\0"))


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


@handler("msc_capacity")
def capacity(ctx, step):
    status, rx = _scsi(ctx, bytes([0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0]), "IN", b"\0" * 8)
    lba, blk = struct.unpack(">II", rx)
    gb = (lba + 1) * blk / 1e9
    lo = float((step.get("limits") or {}).get("min_gb", 0))
    return StepResult(step.get("name", "容量"), gb >= lo and status == 0,
                      {"gb": round(gb, 2), "block": blk}, "" if gb >= lo else f"低于下限 {lo}GB")


@handler("msc_write_verify")
def write_verify(ctx, step):
    """写读校验（DESTRUCTIVE）：向指定 LBA 写随机图案并回读比对（产线全盘扫描的循环体）。"""
    import os
    lba = int(step.get("lba", 0))
    blocks = int(step.get("blocks", 8))
    blk_size = int(step.get("block_size", 512))
    pattern = os.urandom(blk_size * blocks)
    cbd_r = bytes([0x28]) + (lba).to_bytes(4, "big") + b"\0" + blocks.to_bytes(2, "big") + b"\0"
    cbd_w = bytes([0x2A]) + (lba).to_bytes(4, "big") + b"\0" + blocks.to_bytes(2, "big") + b"\0"
    st1, _ = _scsi(ctx, cbd_w, "OUT", pattern)
    st2, rx = _scsi(ctx, cbd_r, "IN", b"\0" * (blk_size * blocks))
    ok = st1 == 0 and st2 == 0 and rx == pattern
    return StepResult(step.get("name", "写读校验"), ok,
                      {"lba": lba, "bytes": len(rx)}, "" if ok else "数据比对不一致")
