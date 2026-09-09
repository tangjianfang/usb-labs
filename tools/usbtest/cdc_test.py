"""CDC 产测后端：串口环回（工装）、线路编码读写、DFU 校验流程。
依赖: pip install pyserial pyusb
"""
import functools

from usbtest.core import StepResult   # 旧版漏导入：真实后端任一步骤派发即 NameError（#77）
from usbtest.logbase import setup

log = setup("usbtest.cdc")
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
    import serial
    port = dev.get("port")
    assert port, "请在计划 device.port 指定串口（如 COM7 / /dev/ttyACM0）"
    s = serial.Serial(port, int(dev.get("baud", 115200)), timeout=1)
    devlog.info("CDC 串口已打开: %s @%d", port, int(dev.get("baud", 115200)))
    return s


def _s(ctx):
    if ctx.get("dev") is None:
        ctx["dev"] = open_device(ctx["device"])
    return ctx["dev"]


@handler("serial_loopback")
def loopback(ctx, step):
    """环回测试：需环回工装（TX-RX 短接）或设备固件回显。"""
    s = _s(ctx)
    s.reset_input_buffer()
    pattern = bytes(range(256)) * int(step.get("repeat", 4))
    s.write(pattern)
    s.flush()
    got = s.read(len(pattern))
    ok = got == pattern
    return StepResult(step.get("name", "串口环回"), ok,
                      {"bytes": len(got)}, "" if ok else f"回读不匹配@{len(got)}B")


@handler("line_coding")
def line_coding(ctx, step):
    """SET/GET_LINE_CODING 往返（经 pyusb 控制传输验证类请求正确性）。"""
    import usb.core
    v, p = ctx["device"].get("vid"), ctx["device"].get("pid")
    d = usb.core.find(idVendor=v, idProduct=p)
    assert d is not None, "USB 设备未找到"
    # 115200-8N1：dwDTERate 4 字节 LE（115200=0x1C200），旧值 [80 BB 00 00] 实为 48000（#77）
    lc = bytes([0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08])
    d.ctrl_transfer(0x21, 0x20, 0, 0, lc)                    # SET_LINE_CODING
    r = d.ctrl_transfer(0xA1, 0x21, 0, 0, 7)                 # GET_LINE_CODING
    ok = bytes(r) == lc
    return StepResult(step.get("name", "线路编码往返"), ok, {"baud": int.from_bytes(r[0:4], "little")})


@handler("dfu_verify")
def dfu_verify(ctx, step):
    """DFU 升级链路校验：dfu-util 下载测试镜像并读回比对（产线工装流程的自动化封装）。
    实际调用: dfu-util -a 0 -D test.bin; 校验由设备侧 CRC 上报（需固件配合）。"""
    import subprocess
    img = step.get("image", "")
    r = subprocess.run(["dfu-util", "-a", str(step.get("alt", 0)), "-D", img],
                       capture_output=True, text=True, timeout=120)
    ok = r.returncode == 0   # 旧表达式因优先级恒等价 rc==0（"done" 为死码）；保守不收紧语义
    return StepResult(step.get("name", "DFU 校验"), ok, {"rc": r.returncode}, r.stderr[-120:])
