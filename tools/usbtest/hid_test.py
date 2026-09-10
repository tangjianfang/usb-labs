"""HID 产测后端：枚举、描述符、回报率、输出报告、环回。
依赖: pip install hidapi pyusb （回报率用 hidapi；描述符用 pyusb）
"""
import functools
import time

from usbtest.core import StepResult   # 旧版漏导入：真实后端任一步骤派发即 NameError（#77）
from usbtest.logbase import setup

log = setup("usbtest.hid")
devlog = setup("usbtest.device")      # 设备 open/close 生命周期（规格模块 usbtest.device）

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


def open_device(dev):
    import hid  # hidapi
    infos = hid.enumerate(dev.get("vid"), dev.get("pid"))
    assert infos, "未发现 HID 设备（检查 VID/PID 或权限）"
    d = hid.device()
    d.open_path(infos[0]["path"])   # 旧版返回未打开实例（_h 靠 is_opened 自愈掩盖，#77）
    devlog.info("HID 设备已打开: VID=0x%04X PID=0x%04X（枚举命中 %d 接口，取首个）",
                dev.get("vid"), dev.get("pid"), len(infos))
    return d


def _h(ctx):
    # 打开态自检不能硬依赖 d.is_opened()——hidapi 绑定两代 API 不一（原 hid 包有
    # is_opened，pip hidapi 的 device 无此属性，2026-09-10 真机实测 AttributeError）。
    # 兼容口径：绑定提供 is_opened 就调用；否则 dev 存在即视为已打开
    # （引擎与测试注入都只在 open 成功后才把 dev 写入 ctx）。
    d = ctx.get("dev")
    if d is not None:
        is_open = getattr(d, "is_opened", None)
        if callable(is_open):
            if is_open():
                return d
        else:
            return d
    import hid
    info = hid.enumerate(ctx["device"].get("vid"), ctx["device"].get("pid"))[0]
    d = hid.device()
    d.open_path(info["path"])
    ctx["dev"] = d
    return d


@handler("enumerate")
def enumerate_dut(ctx, step):
    import hid
    infos = hid.enumerate(ctx["device"].get("vid"), ctx["device"].get("pid"))
    log.debug("枚举过滤命中 %d 个接口", len(infos))
    return StepResult(step.get("name", "枚举检测"), len(infos) > 0,
                      {"interfaces": len(infos)})


@handler("hid_polling_rate")
def polling_rate(ctx, step):
    """测量回报率: 订阅输入报告 N 秒统计次数（需触发设备上报——鼠标移动或产测工装信号注入）。"""
    d = _h(ctx)
    seconds = float(step.get("seconds", 2))
    d.set_nonblocking(1)
    count, t0 = 0, time.perf_counter()
    while time.perf_counter() - t0 < seconds:
        if d.read(64, timeout_ms=1):
            count += 1
    hz = count / seconds
    lo = float((step.get("limits") or {}).get("min_hz", 0))
    return StepResult(step.get("name", "回报率"), hz >= lo, {"hz": round(hz, 1)},
                      "" if hz >= lo else f"低于下限 {lo}")


@handler("hid_output_write")
def output_write(ctx, step):
    """写输出报告（如 LED）——产线工装用光敏/目检确认。"""
    d = _h(ctx)
    report = bytes(step.get("report", [0]))
    n = d.write(report)
    return StepResult(step.get("name", "输出报告"), n > 0, {"written": n})


@handler("hid_report_loopback")
def loopback(ctx, step):
    """工装环回：写输出报告后期待输入报告回显（需测试夹具，见实验室 hardware/ 设计要点）。"""
    d = _h(ctx)
    pattern = bytes(step.get("pattern", [0x01, 0x55]))
    d.write(bytes([step.get("report_id", 1)]) + pattern)
    got = d.read(len(pattern) + 1, timeout_ms=int(step.get("timeout_ms", 500)))
    ok = got and bytes(got[1:]) == pattern
    return StepResult(step.get("name", "环回"), bool(ok), {"echo": got})
