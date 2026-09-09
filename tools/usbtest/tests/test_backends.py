"""cdc/hid/uvc/pd/ble/dock/uac 产测后端离线自测：派发冒烟（假件注入）+ StepResult 导入钉。
T9/T10 增补: uac_record_level（sounddevice 可选依赖——无库环境优雅失败字符串而非裸
ImportError；假件测电平口径与设备名 vid/pid 匹配）；dock Windows 路径（pnputil 假样本
解析 hub/高速行、UsbTreeView 优先/pnputil 回退的平台分派、假 PATH 无工具干净 FAIL）。

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
import pathlib
import sys
import types
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from usbtest import cdc_test, core, dock_test, hid_test, pd_test, uvc_test, ble_test, uac_test, mock_test
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


def _sounddevice_module(amplitude=0.5, devices=None):
    """sounddevice 假件：rec 吐恒幅序列（RMS=amplitude，-6 dBFS@0.5），记录 frames/参数。"""
    mod = types.ModuleType("sounddevice")
    mod._rec = None
    mod.query_devices = lambda: list(devices if devices is not None else [
        {"name": "Microphone (USB Audio CODEC 1234:5678)", "max_input_channels": 2},
        {"name": "麦克风阵列 (Realtek High Definition Audio)", "max_input_channels": 2},
    ])
    mod.default = types.SimpleNamespace(device=(0, 1))

    def rec(frames, samplerate=48000, channels=1, dtype="float32", device=None):
        mod._rec = (frames, samplerate, channels, device)
        return [amplitude] * frames

    mod.rec, mod.wait = rec, (lambda: None)
    return mod


def _ctx(dev=None, sysname=None, which=None, run=None, env=None, **device):
    c = {"dev": dev, "device": dict(device)}
    if sysname is not None:
        c["sysname"] = sysname            # dock 平台分派注入缝（缺省走真实 platform.system()）
    if which is not None:
        c["which"] = which
    if run is not None:
        c["run"] = run
    if env is not None:
        c["env"] = env
    return c


# ---------------- StepResult 导入钉（六后端旧病） ----------------

class TestStepResultImported(unittest.TestCase):
    def test_all_backends_import_stepresult(self):
        for name in ("cdc_test", "hid_test", "uvc_test", "pd_test", "ble_test", "dock_test", "uac_test"):
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


# ---------------- uac ----------------

class TestUac(unittest.TestCase):
    def test_missing_sounddevice_fails_with_install_hint(self):
        # T9 验收口径：无 sounddevice 环境断言优雅失败字符串而非裸 ImportError
        with _module("sounddevice", None):
            r = uac_test.HANDLERS["uac_record_level"](_ctx(vid=0x1234, pid=0x5678), {"duration": 0.1})
        self.assertFalse(r.passed)
        self.assertIn("sounddevice 未安装", r.note)
        self.assertIn("pip install sounddevice", r.note)

    def test_record_level_measures_dbfs_and_matches_device(self):
        # 恒幅 0.5 假件 → RMS 0.5 → -6.0 dBFS；vid/pid 过滤按设备名 "1234:5678" 子串命中
        sd = _sounddevice_module(amplitude=0.5)
        with _module("sounddevice", sd):
            r = uac_test.HANDLERS["uac_record_level"](
                _ctx(vid=0x1234, pid=0x5678), {"duration": 0.1, "limits": {"min_db": -20}})
        self.assertTrue(r.passed)
        self.assertAlmostEqual(r.measured["level_dbfs"], -6.0, delta=0.1)
        self.assertIn("1234:5678", r.measured["device"])
        self.assertEqual(sd._rec[0], 4800)                     # frames = duration × 默认 48000Hz
        self.assertEqual(sd._rec[2], 1)                        # 单声道

    def test_silent_input_fails_below_min_db(self):
        sd = _sounddevice_module(amplitude=0.0)                # 静音 → -120 dBFS 哨兵
        with _module("sounddevice", sd):
            r = uac_test.HANDLERS["uac_record_level"](_ctx(), {"duration": 0.1, "limits": {"min_db": -60}})
        self.assertFalse(r.passed)
        self.assertEqual(r.measured["level_dbfs"], -120.0)

    def test_uac_registered_real_and_mock_unchanged(self):
        # 注册表挂接（core 按 usbtest.<backend>_test 约定 importlib 派发）；mock 路径行为不变
        self.assertIn("uac_record_level", uac_test.HANDLERS)
        self.assertIs(uac_test.StepResult, StepResult)
        self.assertIn("uac_record_level", mock_test.HANDLERS)   # mock 后端仍有同名处理器（--mock 回归口径）


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
        # sysname="Linux" 钉既有 lsusb -t 路径（Windows 产线机分派会被 ctx 缝改道）
        out = types.SimpleNamespace(
            stdout="/: Bus 04.Port 1: 5000M\n/: Bus 05.Port 1: 5000M\nBus 01.Port 1: 480M\n")
        with mock.patch.object(dock_test.subprocess, "run", return_value=out):
            r = dock_test.HANDLERS["dock_topology"](_ctx(sysname="Linux"),
                                                    {"limits": {"min_hs_ports": 2}})
        self.assertTrue(r.passed)
        self.assertEqual(r.measured["hs_ports"], 3)

    def test_port_cycle_full_rate(self):
        out = types.SimpleNamespace(stdout="Bus 001 Device 005: ID 1235:abcd Widget")
        step = {"loops": 2, "expect_id": "1235:abcd", "limits": {"min_rate": 100}}
        with mock.patch.object(dock_test.subprocess, "run", return_value=out), \
             mock.patch("builtins.input", return_value=""):
            r = dock_test.HANDLERS["hub_port_cycle"](_ctx(sysname="Linux"), step)
        self.assertTrue(r.passed)
        self.assertEqual(r.measured["success_rate"], "100%")


class TestDockWindows(unittest.TestCase):
    """T10: dock 后端 Windows 路径——pnputil 假样本解析 + 平台分派（which/run/env 全注入）。"""

    # 中文 Windows 实测版式（字段名本地化）+ 英文设备描述混排，hub 行 2 条、高速线索 1 条
    PNPUTIL_SAMPLE = (
        "Microsoft PnP 工具\n"
        "\n"
        "实例 ID:                USB\\VID_05E3&PID_0610\\5&2f3927&0&2\n"
        "设备描述:         Generic SuperSpeed USB Hub\n"
        "类名:                 USB\n"
        "状态:                 已启动\n"
        "\n"
        "实例 ID:                USB\\VID_2341&PID_0043\\854353236303150\n"
        "设备描述:         USB Serial Device (COM7)\n"
        "类名:                 Ports\n"
        "\n"
        "实例 ID:                USB\\VID_1A40&PID_0101\\5&2f3927&0&1\n"
        "设备描述:         通用 USB 集线器\n"
        "状态:                 已启动\n"
    )

    @staticmethod
    def _which_pnputil_only(name):
        return r"C:\Windows\System32\pnputil.EXE" if name.lower() == "pnputil" else None

    @staticmethod
    def _run_empty(*a, **k):
        return types.SimpleNamespace(stdout="", returncode=1)

    def test_parse_pnputil_counts_hubs_and_hs(self):
        # 假样本文本（模拟 pnputil 输出）解析出 hub/高速行：SuperSpeed Hub + 通用 USB 集线器
        hs, hubs = dock_test._parse_pnputil(self.PNPUTIL_SAMPLE)
        self.assertEqual((hs, hubs), (1, 2))

    def test_decode_cli_gbk_output(self):
        # 真机钉（zh-CN Windows）：pnputil 吐 GBK 字节，text=True 默认 UTF-8 解码炸掉
        # → 必须按字节收 + utf-8/mbcs 双编码尝试；str 直通（假件注入口径）
        self.assertEqual(dock_test._decode_cli(self.PNPUTIL_SAMPLE), self.PNPUTIL_SAMPLE)
        self.assertIn("集线器", dock_test._decode_cli("通用 USB 集线器".encode("gbk")))

    def test_windows_dispatch_usbtv_preferred(self):
        # UsbTreeView 在 PATH → 优先走 /c /f 文本导出（CLI 形状钉死），报告落盘后按内容解析
        report = ("Hub Information\n"
                  "Device Bus Speed  : 0x03 (SuperSpeed)\n"
                  "Device Bus Speed  : 0x02 (High-Speed)\n")

        def fake_run(cmd, **k):
            self.assertEqual(cmd[1:3], ["/c", "/f"])           # 参数以 UsbTreeView 文档为准的保守写法
            pathlib.Path(cmd[3]).write_text(report, encoding="utf-8")
            return types.SimpleNamespace(returncode=0)

        hs, raw, source = dock_test._usb_tree(
            sysname="Windows", which=lambda n: r"C:\tools\UsbTreeView.exe",
            run=fake_run, env={})
        self.assertEqual(source, "usbtv")
        self.assertEqual(hs, 2)
        self.assertIn("Hub Information", raw)

    def test_windows_dispatch_pnputil_fallback(self):
        # 假 PATH 无 UsbTreeView → 回退 pnputil /enum-devices，假输出进解析器
        out = types.SimpleNamespace(stdout=self.PNPUTIL_SAMPLE, returncode=0)
        hs, raw, source = dock_test._usb_tree(
            sysname="Windows", which=self._which_pnputil_only,
            run=lambda *a, **k: out, env={})
        self.assertEqual(source, "pnputil")
        self.assertEqual(hs, 1)

    def test_windows_no_tools_fails_clean(self):
        # 假 PATH 无任何工具（pnputil 也不在）→ 函数级 (0,"","none") 不抛异常；
        # 处理器级干净 FAIL 留痕而非异常穿透
        hs, raw, source = dock_test._usb_tree(
            sysname="Windows", which=lambda n: None, run=self._run_empty, env={})
        self.assertEqual(source, "none")
        self.assertEqual((hs, raw), (0, ""))
        c = _ctx(sysname="Windows", which=lambda n: None, run=self._run_empty, env={})
        r = dock_test.HANDLERS["dock_topology"](c, {"limits": {"min_hs_ports": 1}})
        self.assertFalse(r.passed)
        self.assertIn("无可用", r.note)


if __name__ == "__main__":
    unittest.main(verbosity=1)
