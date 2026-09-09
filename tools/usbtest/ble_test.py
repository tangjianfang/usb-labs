"""BLE 产测后端：扫描/连接/GATT 发现/HID 通知（经标准 BLE 适配器，bleak）。
依赖: pip install bleak
"""
import functools
import time

from usbtest.core import StepResult   # 旧版漏导入：真实后端任一步骤派发即 NameError（#77）
from usbtest.logbase import setup

log = setup("usbtest.ble")

HANDLERS = {}


def handler(t):
    def deco(fn):
        @functools.wraps(fn)
        def wrapped(ctx, step):
            log.debug("处理器入口: %s", step.get("name", t))
            r = fn(ctx, step)
            log.debug("处理器出口: %s %s", r.name, "PASS" if r.passed else "FAIL")
            if not r.passed and r.note:
                log.warning("%s 失败原因: %s", r.name, r.note)
            return r
        HANDLERS[t] = wrapped
        return wrapped
    return deco


def _name_match(dev):
    return dev.get("name_prefix", "")


_SIG_BASE = "0000{:04x}-0000-1000-8000-00805f9b34fb"   # 16 位 assigned number 的 SIG 全形


def _svc_is_uuid16(svc, want):
    """SIG 16 位服务精确匹配：全形整串比较（BT Core §2.5.1——16 位别名先扩展回 128 位）。

    旧版 startswith(f"{want:04x}") 对全形恒不中（全形以 0000 前缀开头，#77）；
    首轮修复取首组转 int 比较，被复核以首组撞车反例驳倒（00001812-0000-9999-…
    非 SIG 基址也命中，#77 复核 H2）——整串比较天然排除厂商 128 位与撞车形。
    """
    return svc.uuid.lower() == _SIG_BASE.format(want)


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
        log.debug("广播命中: %s（%.1fs）", target.name, time.perf_counter() - t0)
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
            found = any(_svc_is_uuid16(s, want) for s in svcs)
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
            hid_svc = next((s for s in c.services if _svc_is_uuid16(s, 0x1812)), None)
            if hid_svc is None:
                return None                  # 无 HOGP 服务：干净失败而非协程内 StopIteration（#77 复核 H3）
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
    if got is None:
        return StepResult(step.get("name", "HID 通知"), False, {}, "未发现 HOGP 服务 0x1812")
    return StepResult(step.get("name", "HID 通知"), got >= want_n, {"notifications": got})
