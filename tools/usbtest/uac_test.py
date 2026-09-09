"""UAC 产测后端：默认输入设备电平录制（RMS→dBFS），验证 USB 音频路由与拾音。
依赖: pip install sounddevice（可选依赖——未安装/PortAudio 库缺失时处理器优雅 FAIL，
绝不裸抛 ImportError，与 uvc_capture_frames 缺 opencv 同口径）。

口径: 录制 duration 秒（int16→float32 单声道）→ RMS → dBFS（0=满幅，静音钳 -120 哨兵），
      判 limits.min_db 下限；limits.max_db 为削波上限（可选）。
设备选择: vid/pid 可选过滤（best-effort——PortAudio 设备名不一定含 "vid:pid"，依 OS/后端而定，
      不命中则回退系统默认输入设备并留 warn）；无 vid/pid 直接用默认输入设备。
无 open_device: 音频流是瞬态句柄（录完即关），无持久设备句柄概念（同 ble/dock），
      run_plan 的 getattr(mod, "open_device", …) 回退 None。
"""
import functools
import math

from usbtest.core import StepResult
from usbtest.logbase import setup

log = setup("usbtest.uac")

HANDLERS = {}

_MUTE_DBFS = -120.0     # 静音/空样本哨兵（log10(0) 无定义，钳到可判定的下界）


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


def _level_dbfs(samples):
    """样本序列 → dBFS = 20*log10(RMS)。channels=1 时样本可为一维序列或 (N,1) 嵌套
    （sounddevice.rec 返回 numpy 数组、离线假件返回扁平 list，两者皆容）。"""
    total = n = 0.0
    for blk in samples:
        v = float(blk[0]) if hasattr(blk, "__iter__") else float(blk)
        total += v * v
        n += 1
    if n == 0 or total <= 0.0:
        return _MUTE_DBFS
    return 20.0 * math.log10(math.sqrt(total / n))


def _select_input(sd, device):
    """选输入设备 → (索引, 名称)。vid/pid 过滤按设备名含 "vid:pid" 子串匹配（保守
    best-effort，PortAudio 无 VID/PID 元数据）；不命中回退默认输入设备（warn 留痕）。"""
    vid, pid = device.get("vid"), device.get("pid")
    inputs = [(i, d) for i, d in enumerate(sd.query_devices())
              if d["max_input_channels"] > 0]
    log.debug("输入设备枚举: %d 个", len(inputs))
    if vid and pid:
        want = f"{vid:04x}:{pid:04x}"
        for i, d in inputs:
            if want in str(d["name"]).lower():
                return i, d["name"]
        log.warning("无输入设备名含 %s，回退默认输入设备", want)
    dflt = sd.default.device    # 实库 0.5.x 为 _InputOutputPair（in/out 序列，非 tuple 子类），假件可给 int
    idx = dflt[0] if hasattr(dflt, "__getitem__") and not isinstance(dflt, int) else dflt
    return idx, sd.query_devices()[idx]["name"]


@handler("uac_record_level")
def record_level(ctx, step):
    """录默认输入设备 duration 秒测电平，判 limits.min_db（真机需 OS 音频路由到 DUT 麦克风）。"""
    try:
        import sounddevice as sd
    except ImportError:
        return StepResult(step.get("name", "UAC 电平"), False,
                          note="sounddevice 未安装（pip install sounddevice），真机 UAC 电平测量不可用")
    except OSError as e:                           # PortAudio 动态库缺失时 sounddevice 导入期 OSError
        return StepResult(step.get("name", "UAC 电平"), False,
                          note=f"sounddevice/PortAudio 不可用: {e}")
    duration = float(step.get("duration", step.get("seconds", 2)))
    sr = int(ctx["device"].get("samplerate", 48000))
    try:
        idx, name = _select_input(sd, ctx["device"])
        if int(idx) < 0:
            return StepResult(step.get("name", "UAC 电平"), False, note="无默认输入设备")
        frames = int(duration * sr)
        log.info("UAC 录音: %s %.1fs @%dHz", name, duration, sr)
        data = sd.rec(frames, samplerate=sr, channels=1, dtype="float32", device=idx)
        sd.wait()
    except Exception as e:                         # PortAudioError/设备被占用 → 干净 FAIL 不中断整站
        return StepResult(step.get("name", "UAC 电平"), False, note=f"录音失败: {e}")
    level = _level_dbfs(data)
    limits = step.get("limits") or {}
    lo = float(limits.get("min_db", -60.0))
    hi = float(limits.get("max_db", 0.0))
    ok = lo <= level <= hi
    return StepResult(step.get("name", "UAC 电平"), ok,
                      {"level_dbfs": round(level, 1), "device": name, "duration_s": duration},
                      "" if ok else f"超出 [{lo}, {hi}] dBFS")
