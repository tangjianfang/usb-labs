"""BLE 产测后端：扫描/连接/GATT 发现/HID 通知（经标准 BLE 适配器，bleak）。
依赖: pip install bleak
"""
import time

HANDLERS = {}


def handler(t):
    def deco(fn):
        HANDLERS[t] = fn
        return fn
    return deco


def _name_match(dev):
    return dev.get("name_prefix", "")


@handler("ble_scan_connect")
def scan_connect(ctx, step):
    """扫描并按名称前缀连接 DUT（产线：工装屏蔽箱内唯一目标）。"""
    import asyncio
    from bleak import BleakClient, BleakScanner

    async def run():
        prefix = _name_match(ctx["device"])
        target = None
        t0 = time.perf_counter()
        timeout = float(step.get("timeout_s", 10))
        while target is None and time.perf_counter() - t0 < timeout:
            ads = await BleakScanner.discover(timeout=2.0)
            for a in ads:
                if a.name and a.name.startswith(prefix):
                    target = a
                    break
        if target is None:
            return None, False, {}
        async with BleakClient(target) as c:
            return target, c.is_connected, {"mtu": c.mtu_size}

    t, ok, extra = asyncio.run(run())
    return StepResult(step.get("name", "BLE 连接"), bool(ok), extra,
                      "" if t else f"未发现名称前缀 {_name_match(ctx['device'])!r}")


@handler("ble_gatt_discover")
def gatt(ctx, step):
    """GATT 服务发现核对（如 HOGP 0x1812 存在）。"""
    import asyncio
    from bleak import BleakScanner, BleakClient
    want = int(step.get("service", "0x1812"), 16)

    async def run():
        ads = await BleakScanner.discover(timeout=5.0)
        t = next((a for a in ads if a.name and a.name.startswith(_name_match(ctx["device"]))), None)
        if t is None:
            return False, {}
        async with BleakClient(t) as c:
            svcs = c.services
            found = any(s.uuid.startswith(f"{want:04x}") for s in svcs)
            return found, {"services": len(svcs)}

    ok, extra = asyncio.run(run())
    return StepResult(step.get("name", "GATT 发现"), ok, extra)


@handler("ble_hid_notify")
def notify(ctx, step):
    """订阅 HID Report 通知并等待 N 条（需触发设备上报——工装按键机构）。"""
    import asyncio
    from bleak import BleakScanner, BleakClient
    want_n = int(step.get("count", 3))

    async def run():
        ads = await BleakScanner.discover(timeout=5.0)
        t = next((a for a in ads if a.name and a.name.startswith(_name_match(ctx["device"]))), None)
        if t is None:
            return 0
        got = 0
        ev = asyncio.Event()

        def cb(handle, data):
            nonlocal got
            got += 1
            if got >= want_n:
                ev.set()

        async with BleakClient(t) as c:
            hid_svc = next(s for s in c.services if s.uuid.startswith("1812"))
            report_char = next(ch for ch in hid_svc.characteristics
                               if "notify" in ch.properties)
            await c.start_notify(report_char, cb)
            try:
                await asyncio.wait_for(ev.wait(), timeout=float(step.get("timeout_s", 15)))
            except asyncio.TimeoutError:
                pass
            await c.stop_notify(report_char)
        return got

    got = asyncio.run(run())
    return StepResult(step.get("name", "HID 通知"), got >= want_n, {"notifications": got})
