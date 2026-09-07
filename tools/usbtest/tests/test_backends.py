"""cdc/hid/uvc/pd/ble/dock 六产测后端离线自测：派发冒烟（假件注入）+ StepResult 导入钉。

旧病（evolve #76 残余 Tier 1）：六后端用而未导入 StepResult——真实后端任一步骤
派发即 NameError（core.run_plan 的 except 吞成 FAIL，整站必挂；CI mock 模式掩蔽）。
顺带钉三处审读缺陷：ble 服务 UUID 全形匹配（SIG 基址 00000000-0000-1000-8000-
00805F9B34FB，16 位 0x1812 全形即 00001812-…，startswith("1812") 恒不中——缓存
Bluetooth-Core-6.0.pdf 核对）；cdc line_coding 115200 编码（dwDTERate 4 字节 LE，
115200=00 C2 01 00，旧值 80 BB 00 00=48000——库内 20/CDC/01-虚拟串口ACM 核对）；
hid open_device 返回未打开设备（enumerate 后丢弃，_h 靠 is_opened 自愈）。
假件经 sys.modules / ctx["dev"] 注入，无硬件依赖。同口径先例 tests/test_msc.py（#76）。
#77 复核驱动硬化三处: run_plan 非 mock 对无 open_device 后端（ble/dock）派发前即
AttributeError→getattr 回退 None；UUID 匹配整串比较（首组撞车 00001812-0000-9999-…
反例驳倒首组转 int 形态）；ble_hid_notify 无 HOGP 服务裸 next() 崩→干净失败留痕。
运行: python tools/usbtest/tests/test_backends.py（任意 CWD）。
"""
import contextlib
import importlib
import os
import sys
import types
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from usbtest import cdc_test, core, dock_test, hid_test, pd_test, uvc_test, ble_test
from usbtest.core import StepResult

BASE = "0000{:04x}-0000-1000-8000-00805f9b34fb"   # 16 位 assigned number 的全形


@contextlib.contextmanager
def _module(name, mod):
    """临时向 sys.modules 注入（mod=None 即制造 ImportError——cv2/pyvisa 缺依赖路径）。"""
    saved = sys.modules.get(name)
    sys.modules[name] = mod
    try:
        yield
    finally:
        sys.modules.pop(name, None) if saved is None else sys.modules.__setitem__(name, saved)


# ---------------- 假件 ----------------

class FakeSerial:
    """pyserial 假件：write 缓存→read 环回（工装 TX-RX 短接口径）；readline 吐遥测行。"""

    def __init__(self, telemetry=()):
        self.written, self.lines = b"", list(telemetry)
        self.nonblocking = None

    def reset_input_buffer(self):
        pass

    def write(self, data):
        self.written += bytes(data)

    def flush(self):
        pass

    def read(self, n):
        d, self.written = self.written[:n], self.written[n:]
        return d

    def readline(self):
        return self.lines.pop(0).encode() if self.lines else b""


class FakeHidDevice:
    """hidapi 假件：open_path/is_opened 真语义；read 两模式（stream=持续回报/echo=回显输出报告）。"""

    def __init__(self, mode="echo"):
        self.mode, self.opened, self.path, self.last_write = mode, False, None, b""

    def open_path(self, path):
        self.opened, self.path = True, path

    def is_opened(self):
        return self.opened

    def set_nonblocking(self, n):
        self.nonblocking = n

    def write(self, data):
        self.last_write = bytes(data)
        return len(data)

    def read(self, n, timeout_ms=0):
        if self.mode == "stream":
            return [0x01] * 8
        return list(self.last_write[:n])


def _hid_module(infos):
    mod = types.ModuleType("hid")
    mod.enumerate = lambda vid, pid: list(infos)
    mod.device = FakeHidDevice
    return mod


class FakeItf:
    def __init__(self, cls):
        self.bInterfaceClass = cls


class FakeCfg:
    def __init__(self, *itfs):
        self.itfs = itfs

    def __iter__(self):
        return iter(self.itfs)


class FakeUsbDevice:
    """pyusb 假件：可迭代配置（UVC 接口扫描）；ctrl_transfer 记录 + IN 长度回线路编码。"""

    def __init__(self, cfgs=(), line_coding=None):
        self.cfgs, self.lc, self.ctrl, self.configured = list(cfgs), line_coding, [], False

    def __iter__(self):
        return iter(self.cfgs)

    def set_configuration(self):
        self.configured = True

    def ctrl_transfer(self, bm, req, val, idx, data):
        self.ctrl.append((bm, req))
        if isinstance(data, int):                      # IN：按长度回读
            return list(self.lc)
        self.last_out = bytes(data)


def _usb_modules(dev):
    usb = types.ModuleType("usb")
    core = types.ModuleType("usb.core")
    core.find = lambda idVendor=None, idProduct=None: dev
    usb.core = core
    return usb, core


class FakeSvc:
    def __init__(self, uuid16, characteristics=()):
        self.uuid, self.characteristics = BASE.format(uuid16), list(characteristics)


class FakeVendorSvc:                                   # 非 SIG 全形（16 位简表不覆盖厂商服务）
    def __init__(self):
        self.uuid, self.characteristics = "6e400001-0000-1000-8000-00805f9b34fb", []


class FakeRawSvc:                                      # 任意原始 UUID（撞车夹具，#77 复核 H2）
    def __init__(self, uuid):
        self.uuid, self.characteristics = uuid, []


class FakeChar:
    def __init__(self, props):
        self.properties = list(props)


def _bleak_module(adverts, services=(), notify_reports=3):
    mod = types.ModuleType("bleak")

    class Scanner:
        @staticmethod
        async def discover(timeout=5.0):
            return list(adverts)

    class Client:
        def __init__(self, target):
            self._svcs = list(services)

        async def __aenter__(self):
            return self

        async def __aexit__(self, *exc):
            return False

        is_connected, mtu_size = True, 23

        @property
        def services(self):
            return self._svcs

        async def start_notify(self, char, cb):
            for _ in range(notify_reports):
                cb(1, b"\x01")

        async def stop_notify(self, char):
            pass

    mod.BleakScanner, mod.BleakClient = Scanner, Client
    return mod


class FakeAdvert:
    def __init__(self, name):
        self.name = name


def _ctx(dev=None, **device):
    return {"dev": dev, "device": dict(device)}


# ---------------- StepResult 导入钉（六后端旧病） ----------------

class TestStepResultImported(unittest.TestCase):
    def test_all_backends_import_stepresult(self):
        for name in ("cdc_test", "hid_test", "uvc_test", "pd_test", "ble_test", "dock_test"):
            with self.subTest(backend=name):
                mod = importlib.import_module(f"usbtest.{name}")
                self.assertIs(mod.StepResult, StepResult)


# ---------------- cdc ----------------

class TestCdc(unittest.TestCase):
    def test_serial_loopback_echo_fixture(self):
        r = cdc_test.HANDLERS["serial_loopback"](_ctx(FakeSerial(), port="COM7"), {"repeat": 4})
        self.assertTrue(r.passed)
        self.assertEqual(r.measured["bytes"], 1024)          # bytes(range(256))×4 环回全中

    def test_line_coding_115200_le_bytes(self):
        # 旧病：lc=[80 BB 00 00…] 实为 48000（0xBB80）——115200 LE 应为 00 C2 01 00
        dev = FakeUsbDevice(line_coding=[0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08])
        usb, core = _usb_modules(dev)
        with _module("usb", usb), _module("usb.core", core):
            r = cdc_test.HANDLERS["line_coding"](_ctx(vid=0x1235, pid=0xABCD), {})
        self.assertTrue(r.passed)
        self.assertEqual(r.measured["baud"], 115200)
        self.assertEqual(dev.ctrl, [(0x21, 0x20), (0xA1, 0x21)])   # SET 后 GET 往返

    def test_dfu_rc0_without_done_still_passes(self):
        # 旧表达式 `rc == 0 and "done" in stderr or rc == 0` 优先级恒等价 rc==0（"done" 死码），
        # 修复取保守语义：退出码即判据，不收紧（防真机 dfu-util 输出变体误 FAIL）
        with mock.patch("subprocess.run", return_value=types.SimpleNamespace(returncode=0, stderr="")):
            r = cdc_test.HANDLERS["dfu_verify"](_ctx(), {"image": "test.bin"})
        self.assertTrue(r.passed)
        self.assertEqual(r.measured["rc"], 0)

    def test_dfu_rc1_fails_with_note(self):
        with mock.patch("subprocess.run", return_value=types.SimpleNamespace(
                returncode=1, stderr="Cannot open device 0483:df11")):
            r = cdc_test.HANDLERS["dfu_verify"](_ctx(), {"image": "test.bin"})
        self.assertFalse(r.passed)
        self.assertIn("Cannot open", r.note)


# ---------------- hid ----------------

class TestHid(unittest.TestCase):
    def test_open_device_returns_opened(self):
        # 旧病：enumerate 断言后丢弃，返回 hid.device() 未打开（靠 _h 的 is_opened 自愈掩盖）
        with _module("hid", _hid_module([{"path": b"P1"}, {"path": b"P2"}])):
            d = hid_test.open_device({"vid": 0x1235, "pid": 0xABCD})
        self.assertTrue(d.is_opened())
        self.assertEqual(d.path, b"P1")

    def test_enumerate_counts_interfaces(self):
        with _module("hid", _hid_module([{"path": b"P1"}, {"path": b"P2"}])):
            r = hid_test.HANDLERS["enumerate"](_ctx(vid=0x1235, pid=0xABCD), {})
        self.assertTrue(r.passed)
        self.assertEqual(r.measured["interfaces"], 2)

    def test_output_write(self):
        d = FakeHidDevice()
        d.open_path(b"P1")
        r = hid_test.HANDLERS["hid_output_write"](_ctx(d), {"report": [0x01, 0x55]})
        self.assertTrue(r.passed)
        self.assertEqual(r.measured["written"], 2)

    def test_polling_rate_stream(self):
        d, s = FakeHidDevice(mode="stream"), {"seconds": 0.05, "limits": {"min_hz": 0}}
        d.open_path(b"P1")
        r = hid_test.HANDLERS["hid_polling_rate"](_ctx(d), s)
        self.assertTrue(r.passed)
        self.assertGreater(r.measured["hz"], 0)
        self.assertEqual(d.nonblocking, 1)

    def test_report_loopback_echo(self):
        d = FakeHidDevice()
        d.open_path(b"P1")
        r = hid_test.HANDLERS["hid_report_loopback"](
            _ctx(d), {"report_id": 1, "pattern": [0x01, 0x55], "timeout_ms": 100})
        self.assertTrue(r.passed)
        self.assertEqual(d.last_write, bytes([1, 0x01, 0x55]))     # Report ID 前缀 + 图案


# ---------------- uvc ----------------

class TestUvc(unittest.TestCase):
    def test_formats_counts_uvc_interfaces(self):
        dev = FakeUsbDevice([FakeCfg(FakeItf(0x0E), FakeItf(0x0E), FakeItf(0x02))])
        usb, core = _usb_modules(dev)
        with _module("usb", usb), _module("usb.core", core):
            r = uvc_test.HANDLERS["uvc_formats"](_ctx(vid=0x1235, pid=0xABCD),
                                                 {"limits": {"min_uvc_itf": 1}})
        self.assertTrue(r.passed)
        self.assertEqual(r.measured["uvc_itf"], 2)
        self.assertTrue(dev.configured)

    def test_capture_missing_opencv_reports_note(self):
        with _module("cv2", None):                             # None in sys.modules → ImportError
            r = uvc_test.HANDLERS["uvc_capture_frames"](_ctx(camera_index=0), {"frames": 1})
        self.assertFalse(r.passed)
        self.assertIn("opencv", r.note)


# ---------------- pd ----------------

class TestPd(unittest.TestCase):
    def test_attach_from_telemetry(self):
        r = pd_test.HANDLERS["pd_attach"](_ctx(FakeSerial(["cc_state=Attached.SRC"])),
                                          {})
        self.assertTrue(r.passed)
        self.assertEqual(r.measured["cc_state"], "Attached.SRC")

    def test_negotiate_voltage_within_tolerance(self):
        r = pd_test.HANDLERS["pd_negotiate"](
            _ctx(FakeSerial(["contract_v=8.9", "contract_i=1.8"])),
            {"expect_v": 9, "tol_v": 0.5})
        self.assertTrue(r.passed)
        self.assertEqual(r.measured["contract_v"], 8.9)

    def test_measure_voltage_telemetry_fallback(self):
        r = pd_test.HANDLERS["measure_voltage"](_ctx(FakeSerial(["vbus_v=5.02"])), {})
        self.assertTrue(r.passed)
        self.assertEqual(r.measured["vbus_v"], 5.02)

    def test_measure_voltage_scpi_exception_fails_soft(self):
        with _module("pyvisa", None):                          # 无 pyvisa → ImportError → 软失败
            r = pd_test.HANDLERS["measure_voltage"](_ctx(), {"scpi_resource": "GPIB0::22"})
        self.assertFalse(r.passed)
        self.assertTrue(r.note.startswith("SCPI 异常"), r.note)


# ---------------- ble ----------------

class TestBle(unittest.TestCase):
    def test_scan_connect_by_name_prefix(self):
        bleak = _bleak_module([FakeAdvert("DUT-7")])
        with _module("bleak", bleak):
            r = ble_test.HANDLERS["ble_scan_connect"](_ctx(name_prefix="DUT"), {})
        self.assertTrue(r.passed)
        self.assertEqual(r.measured["mtu"], 23)

    def test_scan_connect_not_found_fails_with_note(self):
        bleak = _bleak_module([FakeAdvert("OTHER-1")])
        with _module("bleak", bleak):
            r = ble_test.HANDLERS["ble_scan_connect"](_ctx(name_prefix="DUT"),
                                                      {"timeout_s": 0.05})
        self.assertFalse(r.passed)
        self.assertIn("DUT", r.note)

    def test_gatt_discover_full_form_uuid(self):
        # 旧病：startswith("1812") 对全形 00001812-0000-1000-8000-00805f9b34fb 恒 False
        svcs = [FakeSvc(0x1800), FakeSvc(0x1812)]
        bleak = _bleak_module([FakeAdvert("DUT-1")], services=svcs)
        with _module("bleak", bleak):
            r = ble_test.HANDLERS["ble_gatt_discover"](_ctx(name_prefix="DUT"),
                                                       {"service": "0x1812"})
        self.assertTrue(r.passed)
        self.assertEqual(r.measured["services"], 2)

    def test_gatt_discover_vendor_only_not_matched(self):
        # 反向钉：厂商 128 位服务不得被 16 位简写比较误中（防修复过宽）
        bleak = _bleak_module([FakeAdvert("DUT-1")], services=[FakeVendorSvc()])
        with _module("bleak", bleak):
            r = ble_test.HANDLERS["ble_gatt_discover"](_ctx(name_prefix="DUT"),
                                                       {"service": "0x1812"})
        self.assertFalse(r.passed)

    def test_gatt_discover_collide_uuids_not_matched(self):
        # #77 复核 H2 反例钉：首组撞车（00001812-0000-9999-… 非 SIG 基址）与
        # 掩码式低 16 位撞车（feab1812-…）都不得命中——全形整串比较
        svcs = [FakeRawSvc("00001812-0000-9999-8000-00805f9b34fb"),
                FakeRawSvc("feab1812-0000-1000-8000-00805f9b34fb")]
        bleak = _bleak_module([FakeAdvert("DUT-1")], services=svcs)
        with _module("bleak", bleak):
            r = ble_test.HANDLERS["ble_gatt_discover"](_ctx(name_prefix="DUT"),
                                                       {"service": "0x1812"})
        self.assertFalse(r.passed)

    def test_hid_notify_no_hogp_service_fails_clean(self):
        # #77 复核 H3：无 HOGP 服务时裸 next() 在协程内抛 RuntimeError（产线表现为
        # 语义不明异常 FAIL）——修复后干净 FAIL 并留痕
        bleak = _bleak_module([FakeAdvert("DUT-1")], services=[FakeVendorSvc()])
        with _module("bleak", bleak):
            r = ble_test.HANDLERS["ble_hid_notify"](_ctx(name_prefix="DUT"), {"count": 1})
        self.assertFalse(r.passed)
        self.assertIn("HOGP", r.note)

    def test_hid_notify_counts_reports(self):
        svc = FakeSvc(0x1812, [FakeChar(["read"]), FakeChar(["notify"])])
        bleak = _bleak_module([FakeAdvert("DUT-1")], services=[svc], notify_reports=3)
        with _module("bleak", bleak):
            r = ble_test.HANDLERS["ble_hid_notify"](_ctx(name_prefix="DUT"), {"count": 3})
        self.assertTrue(r.passed)
        self.assertEqual(r.measured["notifications"], 3)


# ---------------- dock ----------------

class TestRunPlan(unittest.TestCase):
    def test_ble_backend_dispatches_without_open_device(self):
        # #77 复核 H1：run_plan 非 mock 无条件调 mod.open_device，ble/dock 无此函数——
        # 任何步骤派发之前即 AttributeError（逐步 except 吞不到）；修复后正常派发
        bleak = _bleak_module([FakeAdvert("DUT-7")])
        plan = {"name": "t", "device": {"backend": "ble", "name_prefix": "DUT"},
                "steps": [{"type": "ble_scan_connect", "name": "连接", "timeout_s": 1}]}
        with _module("bleak", bleak):
            rep = core.run_plan(plan, "SN-1", "STN-01", mock=False)
        self.assertTrue(rep.verdict)


class TestDock(unittest.TestCase):
    def test_topology_counts_hs_ports(self):
        out = types.SimpleNamespace(
            stdout="/: Bus 04.Port 1: 5000M\n/: Bus 05.Port 1: 5000M\nBus 01.Port 1: 480M\n")
        with mock.patch.object(dock_test.subprocess, "run", return_value=out):
            r = dock_test.HANDLERS["dock_topology"](_ctx(), {"limits": {"min_hs_ports": 2}})
        self.assertTrue(r.passed)
        self.assertEqual(r.measured["hs_ports"], 3)

    def test_port_cycle_full_rate(self):
        out = types.SimpleNamespace(stdout="Bus 001 Device 005: ID 1235:abcd Widget")
        step = {"loops": 2, "expect_id": "1235:abcd", "limits": {"min_rate": 100}}
        with mock.patch.object(dock_test.subprocess, "run", return_value=out), \
             mock.patch("builtins.input", return_value=""):
            r = dock_test.HANDLERS["hub_port_cycle"](_ctx(), step)
        self.assertTrue(r.passed)
        self.assertEqual(r.measured["success_rate"], "100%")


if __name__ == "__main__":
    unittest.main(verbosity=1)
