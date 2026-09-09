"""统一日志基座：Python 侧对齐 C++ spdlog 规范（app/log.h，evolve T12）。

usbsim 为独立包（simulator/ 入 sys.path），与 tools/usbtest/logbase.py 同内容双驻——
两包无共同父包，不做跨包 import。格式: [%(asctime)s][%(levelname)s][%(name)s]
%(message)s —— 与 C++ `[时间][级别][模块][线程] 消息` 的前三段一致（Python logging
无线程段，规格允许）。级别语义对齐 C++: trace=逐包 debug=逐操作 info=生命周期
warn=可恢复 err=失败 off=静默。级别开关 USBTS_LOG_LEVEL 与 C++ 侧同名同值域
（trace/debug/info/warn/err/off）。文件 sink 与 C++ 同口径 RotatingFileHandler 5MB×3。
"""
import logging
import logging.handlers
import os

# trace 无内建级别名：取 DEBUG-5（与 C++ spdlog trace < debug 同序），注册后 %(...)s 渲染 "TRACE"
TRACE = logging.DEBUG - 5
logging.addLevelName(TRACE, "TRACE")

_LEVELS = {"trace": TRACE, "debug": logging.DEBUG, "info": logging.INFO,
           "warn": logging.WARNING, "warning": logging.WARNING,
           "err": logging.ERROR, "error": logging.ERROR, "off": logging.CRITICAL + 10}

_FMT = "[%(asctime)s][%(levelname)s][%(name)s] %(message)s"
_DATEFMT = "%Y-%m-%d %H:%M:%S"


def _resolve_level(level):
    """level 参数（int 或 trace/debug/... 字符串）优先，其次 USBTS_LOG_LEVEL，默认 INFO。"""
    if level is None:
        level = os.environ.get("USBTS_LOG_LEVEL")
    if level is None:
        return logging.INFO
    if isinstance(level, int):
        return level
    return _LEVELS.get(str(level).strip().lower(), logging.INFO)


def setup(name, level=None, logfile=None):
    """模块 logger：格式对齐 C++ spdlog 规范 [时间][级别][模块] 消息。

    级别由参数或 USBTS_LOG_LEVEL 环境变量（trace/debug/info/warn/err/off）覆盖，
    默认 INFO。logfile 给出时追加 RotatingFileHandler（5MB×3，utf-8）。
    幂等：同一 name 重复 setup 不重复挂 handler（文件 sink 只在首次生效）。
    """
    lg = logging.getLogger(name)
    if getattr(lg, "_usbts_configured", False):
        return lg
    fmt = logging.Formatter(_FMT, datefmt=_DATEFMT)
    stream = logging.StreamHandler()
    stream.setFormatter(fmt)
    lg.addHandler(stream)
    if logfile:
        fh = logging.handlers.RotatingFileHandler(
            logfile, maxBytes=5 * 1024 * 1024, backupCount=3, encoding="utf-8")
        fh.setFormatter(fmt)
        lg.addHandler(fh)
    lg.setLevel(_resolve_level(level))
    lg.propagate = False          # 模块各自成流，防父 logger（如 "usbsim"）二次emit
    lg._usbts_configured = True
    return lg
