"""UVC 产测后端：格式/描述符核对（pyusb）+ 可选帧采集（OpenCV）。
依赖: pip install pyusb；帧采集另需 opencv-python（可选）。
"""
import functools

from usbtest.core import StepResult   # 旧版漏导入：真实后端任一步骤派发即 NameError（#77）
from usbtest.logbase import setup

log = setup("usbtest.uvc")
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
    import usb.core
    d = usb.core.find(idVendor=dev.get("vid"), idProduct=dev.get("pid"))
    assert d is not None, "未发现 UVC 设备"
    try:
        d.set_configuration()
    except usb.core.USBError as e:
        # Windows 实测（2026-09-10，集成摄像头 13d3:56d5）：设备被系统 UVC 驱动
        # （usbccgp+usbvideo）持有，set/get_active_configuration 报 ENOENT——
        # 但配置描述符树仍可读（产测口径=枚举 VC/VS 接口），采帧走 cv2/媒体管线，
        # 均不依赖 set_configuration。此告警为信息性，不构成失败。
        devlog.info("set_configuration 失败（%s）——Windows 系统驱动持有口径，继续（描述符树可读）", e)
    devlog.info("UVC 设备已打开: VID=0x%04X PID=0x%04X", dev.get("vid"), dev.get("pid"))
    return d


def _uvc_formats(d):
    """从配置描述符扫描 VS 接口的格式线索（简化产测口径：枚举可用分辨率交给 OS 管线）。
    权威做法是解析 VS 描述符的格式/帧描述符——产线通常以 OS 枚举结果比对白名单。"""
    found = {"uvc_itf": 0}
    for cfg in d:
        for itf in cfg:
            if itf.bInterfaceClass == 0x0E:
                found["uvc_itf"] += 1
    log.debug("VC/VS 接口过滤命中 %d 个", found["uvc_itf"])
    return found


@handler("uvc_formats")
def formats(ctx, step):
    d = open_device(ctx["device"])
    f = _uvc_formats(d)
    need = int((step.get("limits") or {}).get("min_uvc_itf", 1))
    return StepResult(step.get("name", "UVC 格式"), f["uvc_itf"] >= need,
                      f, "" if f["uvc_itf"] >= need else "VC/VS 接口缺失")


@handler("uvc_capture_frames")
def capture(ctx, step):
    """经 OS 管线采集 N 帧（依赖 opencv-python；设备索引来自计划）。"""
    try:
        import cv2
    except ImportError:
        return StepResult(step.get("name", "帧采集"), False, note="缺 opencv-python（可选依赖）")
    idx = int(ctx["device"].get("camera_index", 0))
    cap = cv2.VideoCapture(idx)
    if not cap.isOpened():
        return StepResult(step.get("name", "帧采集"), False, note="摄像头打开失败")
    want = int(step.get("frames", 30))
    got, w, h = 0, 0, 0
    for _ in range(want * 3):
        ok, frame = cap.read()
        if ok:
            got += 1
            h, w = frame.shape[:2]
        if got >= want:
            break
    cap.release()
    min_frames = int((step.get("limits") or {}).get("min_frames", want))
    return StepResult(step.get("name", "帧采集"), got >= min_frames,
                      {"frames": got, "size": f"{w}x{h}"})
