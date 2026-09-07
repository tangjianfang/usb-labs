"""PD 产测后端：经 DUT 的 CDC 遥测口读取 PD 状态（固件配合），可选 SCPI 仪器测量。
依赖: pip install pyserial；SCPI 另需 pyvisa + pyvisa-py（可选）。
产线拓扑: [可编程源/电子负载(SCPI)] — [PD DUT] — [DUT 遥测 CDC 口 → 工控机]
"""
import time

from usbtest.core import StepResult   # 旧版漏导入：真实后端任一步骤派发即 NameError（#77）

HANDLERS = {}


def handler(t):
    def deco(fn):
        HANDLERS[t] = fn
        return fn
    return deco


def open_device(dev):
    import serial
    port = dev.get("telemetry_port")
    assert port, "请在计划 device.telemetry_port 指定 DUT 遥测串口"
    return serial.Serial(port, int(dev.get("baud", 115200)), timeout=1)


def _s(ctx):
    if ctx.get("dev") is None:
        ctx["dev"] = open_device(ctx["device"])
    return ctx["dev"]


def _telemetry(ctx, keys, window=2.0):
    """从遥测口收集 'key=value' 形式的状态行（固件输出约定）。"""
    s = _s(ctx)
    s.reset_input_buffer()
    out = {}
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < window and len(out) < len(keys):
        line = s.readline().decode("utf-8", "replace").strip()
        for k in keys:
            if line.startswith(k + "="):
                out[k] = line.split("=", 1)[1]
    return out


@handler("pd_attach")
def attach(ctx, step):
    """CC attach 检测（固件遥测 cc_state=Attached.SRC/SNK）。"""
    t = _telemetry(ctx, ["cc_state"])
    ok = "Attached" in t.get("cc_state", "")
    return StepResult(step.get("name", "CC attach"), ok, t)


@handler("pd_negotiate")
def negotiate(ctx, step):
    """协商结果核对（固件遥测 contract_v/rdo）。触发方式：外部源切换或固件命令（计划参数 command）。"""
    if step.get("command"):
        _s(ctx).write((step["command"] + "\n").encode())
    t = _telemetry(ctx, ["contract_v", "contract_i"])
    v = float(t.get("contract_v", "0") or 0)
    want = float(step.get("expect_v", 5))
    tol = float(step.get("tol_v", 0.5))
    ok = abs(v - want) <= tol
    return StepResult(step.get("name", "PD 协商"), ok, {"contract_v": v},
                      "" if ok else f"期望 {want}±{tol}V")


@handler("measure_voltage")
def measure_voltage(ctx, step):
    """VBUS 实测（可选 SCPI 万用表；无仪器时回退遥测值）。"""
    visa_rm = step.get("scpi_resource")
    if visa_rm:
        try:
            import pyvisa
            rm = pyvisa.ResourceManager("@py")
            meter = rm.open_resource(visa_rm)
            v = float(meter.query(step.get("scpi_query", ":MEAS:VOLT:DC?")))
        except Exception as e:
            return StepResult(step.get("name", "VBUS 实测"), False, note=f"SCPI 异常: {e}")
    else:
        t = _telemetry(ctx, ["vbus_v"])
        v = float(t.get("vbus_v", "0") or 0)
    want = float(step.get("expect_v", 5))
    tol = float(step.get("tol_v", 0.25))
    return StepResult(step.get("name", "VBUS 实测"), abs(v - want) <= tol, {"vbus_v": v})
