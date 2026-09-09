"""logbase 离线自测：幂等（重复 setup 不重复 handler）、USBTS_LOG_LEVEL 环境变量
级别覆盖、spdlog 规范三段方括号格式、文件 sink（RotatingFileHandler 5MB×3）。

对应 C++ 侧 app/log.h 统一规范（evolve T12，[时间][级别][模块] 格式）——Python 侧
logbase 为其对齐实现，本文件钉住三条行为不变量。运行:
python tools/usbtest/tests/test_logbase.py（任意 CWD）。
"""
import io
import logging
import os
import re
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from usbtest import logbase

_FMT_RE = r"^\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\]\[(\w+)\]\[([^\]]+)\] (.+)$"


def _stream_swap(lg):
    """把 setup 挂的 StreamHandler 流换成 StringIO（真管线取证，非重建 Formatter）。"""
    buf = io.StringIO()
    old = lg.handlers[0].stream
    lg.handlers[0].stream = buf
    return buf, old


class TestIdempotent(unittest.TestCase):
    def test_repeated_setup_no_duplicate_handlers(self):
        lg = logbase.setup("usbtest.logtest.idem")
        n = len(lg.handlers)
        for _ in range(3):
            self.assertIs(logbase.setup("usbtest.logtest.idem"), lg)
        self.assertEqual(len(lg.handlers), n)          # 重复 setup 不重复挂 handler
        self.assertTrue(lg._usbts_configured)

    def test_modules_share_named_logger(self):
        # 同名跨模块取回同一 logger（模块级 log = setup(...) 幂等共享的前提）
        self.assertIs(logging.getLogger("usbtest.logtest.shared"),
                      logbase.setup("usbtest.logtest.shared"))


class TestLevels(unittest.TestCase):
    def test_env_override(self):
        cases = {"debug": logging.DEBUG, "warn": logging.WARNING, "err": logging.ERROR,
                 "trace": logging.DEBUG - 5, "off": logging.CRITICAL + 10}
        for raw, want in cases.items():
            with self.subTest(env=raw):
                with mock.patch.dict(os.environ, {"USBTS_LOG_LEVEL": raw}):
                    lg = logbase.setup(f"usbtest.logtest.env_{raw}")
                self.assertEqual(lg.level, want)

    def test_default_info_without_env(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            lg = logbase.setup("usbtest.logtest.default")
        self.assertEqual(lg.level, logging.INFO)

    def test_explicit_level_beats_env(self):
        with mock.patch.dict(os.environ, {"USBTS_LOG_LEVEL": "debug"}):
            lg = logbase.setup("usbtest.logtest.explicit", level="err")
        self.assertEqual(lg.level, logging.ERROR)


class TestFormat(unittest.TestCase):
    def test_three_bracket_sections(self):
        lg = logbase.setup("usbtest.logtest.fmt")
        buf, old = _stream_swap(lg)
        try:
            lg.info("你好 %s", "产测")
        finally:
            lg.handlers[0].stream = old
        m = re.match(_FMT_RE, buf.getvalue().strip())
        self.assertIsNotNone(m, buf.getvalue())
        self.assertEqual(m.group(2), "usbtest.logtest.fmt")   # [时间][级别][模块] 消息
        self.assertEqual(m.group(3), "你好 产测")

    def test_trace_levelname_renders(self):
        lg = logbase.setup("usbtest.logtest.tracefmt", level="trace")
        buf, old = _stream_swap(lg)
        try:
            lg.log(logbase.TRACE, "逐包 %d", 7)
        finally:
            lg.handlers[0].stream = old
        m = re.match(_FMT_RE, buf.getvalue().strip())
        self.assertIsNotNone(m)
        self.assertEqual(m.group(1), "TRACE")


class TestFileSink(unittest.TestCase):
    def test_rotating_file_handler(self):
        td = tempfile.TemporaryDirectory()
        self.addCleanup(td.cleanup)
        path = os.path.join(td.name, "usbts.log")
        lg = logbase.setup("usbtest.logtest.file", logfile=path)
        fh = [h for h in lg.handlers if isinstance(h, logging.handlers.RotatingFileHandler)]
        self.assertEqual(len(fh), 1)
        self.assertEqual(fh[0].maxBytes, 5 * 1024 * 1024)   # 5MB×3（对齐 C++ app/log.h）
        self.assertEqual(fh[0].backupCount, 3)
        try:
            lg.info("落盘一行")
            fh[0].flush()
            with open(path, encoding="utf-8") as f:
                self.assertRegex(f.read().strip(), _FMT_RE)
        finally:
            lg.removeHandler(fh[0])     # Windows: 先关句柄再删临时目录
            fh[0].close()


if __name__ == "__main__":
    unittest.main(verbosity=1)
