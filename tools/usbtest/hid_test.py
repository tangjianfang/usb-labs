"""HID 产测后端：枚举、描述符、回报率、输出报告、环回。
依赖: pip install hidapi pyusb （回报率用 hidapi；描述符用 pyusb）
"""
import time

HANDLERS = {}


def handler(t):
    def deco(fn):
        HANDLERS[t] = fn
        return fn
    return deco


def open_device(dev):
    import hid  # hidapi
    infos = hid.enumerate(dev.get("vid"), dev.get("pid"))
    assert infos, "未发现 HID 设备（检查 VID/PID 或权限）"
    return hid.device()


def _h(ctx):
    d = ctx.get("dev")
    if d is None or not d.is_opened():
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
