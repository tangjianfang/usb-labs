"""扩展坞产测后端：拓扑核对（hub 层级/billboard/每口枚举）——经 OS USB 树。
平台路径:
  Linux:   lsusb -t（既有路径，行为不变）
  Windows: 优先 UsbTreeView.exe CLI 导出文本报告后解析；缺席回退 pnputil /enum-devices
           （Win10 2004+ 内置，输出含 InstanceId/DeviceDescription，中英文本地化均按子串兼容解析）
注入缝: _usb_tree/_device_list 为可注入纯函数（sysname/which/run/env 可覆盖，离线自测用，
  处理器经 ctx 同名键透传）——假 PATH/假输出即可测平台分派，无需真实工具。
"""
import contextlib
import functools
import os
import pathlib
import platform
import shutil
import subprocess
import tempfile

from usbtest.core import StepResult   # 旧版漏导入：真实后端任一步骤派发即 NameError（#77）
from usbtest.logbase import setup

log = setup("usbtest.dock")

HANDLERS = {}

# pnputil 不报设备速率——高速线索按描述词保守近似（精确速率请走 UsbTreeView 报告）
_PNPUTIL_HS_HINTS = ("superspeed", "usb3", "usb 3")


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


def _parse_pnputil(text):
    """pnputil /enum-devices 文本 → (高速线索数, hub 数)。本地化兼容（字段名中英文皆可，
    按内容子串而非表头）：hub = 描述行含 hub/集线器；高速线索 = 描述含 superspeed/usb3。"""
    hubs = hs = 0
    for line in text.splitlines():
        low = line.lower()
        if "hub" in low or "集线器" in line:
            hubs += 1
        if any(h in low for h in _PNPUTIL_HS_HINTS):
            hs += 1
    log.debug("pnputil 解析: %d hub / %d 高速线索", hubs, hs)
    return hs, hubs


def _parse_usbtv(text):
    """UsbTreeView 文本报告 → (高速线索数, hub 段数)。报告版式与字段以 UsbTreeView 官方
    文档为准（https://www.uwe-sieber.de/usbtreeview_e.html）——保守解析：
    hub 段 = "Hub Information" 节；高速线索 = 速度行 (High-Speed)/(SuperSpeed…) 计数。"""
    low = text.lower()
    hubs = low.count("hub information")
    hs = low.count("(high-speed)") + low.count("(superspeed")
    log.debug("UsbTreeView 解析: %d hub 段 / %d 高速线索", hubs, hs)
    return hs, hubs


def _usb_tree_linux(run=None):
    """lsusb -t 的轻量解析（Linux 专用）。"""
    run = subprocess.run if run is None else run
    out = run(["lsusb", "-t"], capture_output=True, text=True).stdout
    return out.count("5000M") + out.count("480M"), out


def _usb_tree_usbtv(exe, run=None):
    """UsbTreeView.exe 导出文本报告再解析。CLI 参数（保守写法，正式接入以 UsbTreeView
    文档为准）: /c 无 GUI 控制台模式、/f <file> 文本报告输出到指定文件。"""
    run = subprocess.run if run is None else run
    fd, path = tempfile.mkstemp(suffix=".txt", prefix="usbtv_")
    os.close(fd)
    try:
        run([exe, "/c", "/f", path], capture_output=True, text=True, timeout=60)
        raw = pathlib.Path(path).read_text(encoding="utf-8", errors="replace")
    finally:
        with contextlib.suppress(OSError):
            os.unlink(path)
    hs, hubs = _parse_usbtv(raw)
    return hs, raw


def _decode_cli(data):
    """Windows CLI 字节输出 → 文本：UTF-8 优先，失败回退本地 ANSI 代码页（zh-CN=GBK/936）。
    pnputil 在中文 Windows 输出 GBK 字节——text=True 的默认解码会直接炸掉（stdout=None），
    表驱动解析需要原始中文 hub/集线器关键词，故按字节收后再选编码。str 直通（假件注入）。"""
    if isinstance(data, str):
        return data
    for enc in ("utf-8", "mbcs"):
        try:
            return data.decode(enc)
        except (UnicodeDecodeError, LookupError):
            pass
    return data.decode("utf-8", "replace")


def _usb_tree_pnputil(run=None):
    run = subprocess.run if run is None else run
    r = run(["pnputil", "/enum-devices"], capture_output=True, timeout=60)   # 字节模式，见 _decode_cli
    raw = _decode_cli(r.stdout or b"")
    hs, _hubs = _parse_pnputil(raw)
    return hs, raw


def _find_usbtv(which=None, env=None):
    """UsbTreeView.exe 查找: PATH 优先，再探常见安装目录 %ProgramFiles%\\USBTreeView。"""
    which = shutil.which if which is None else which
    env = os.environ if env is None else env
    hit = which("UsbTreeView.exe")
    if hit:
        return hit
    for key in ("ProgramFiles", "ProgramFiles(x86)"):
        base = env.get(key, "")
        cand = pathlib.Path(base) / "USBTreeView" / "UsbTreeView.exe" if base else None
        if cand and cand.is_file():
            return str(cand)
    return None


def _usb_tree(sysname=None, which=None, run=None, env=None):
    """平台分派（可注入纯函数）→ (高速端口线索数, 原始文本, 来源)。
    来源 "none" = 无可用枚举工具——调用方干净 FAIL，不抛异常。
    Windows: UsbTreeView CLI 优先（导出失败回退），pnputil /enum-devices 兜底；其余: lsusb -t。
    """
    sysname = platform.system() if sysname is None else sysname
    which = shutil.which if which is None else which
    run = subprocess.run if run is None else run
    if sysname == "Windows":
        exe = _find_usbtv(which, env)
        if exe:
            log.debug("UsbTreeView: %s", exe)
            try:
                hs, raw = _usb_tree_usbtv(exe, run)
            except Exception as e:                 # 权限/版本/参数差异 → 回退 pnputil
                log.warning("UsbTreeView 导出失败（%s），回退 pnputil", e)
            else:
                if raw:
                    return hs, raw, "usbtv"
        if which("pnputil"):
            hs, raw = _usb_tree_pnputil(run)
            if raw:
                return hs, raw, "pnputil"
        return 0, "", "none"
    hs, raw = _usb_tree_linux(run)
    return hs, raw, "lsusb"


def _device_list(sysname=None, which=None, run=None, env=None):
    """在线 USB 设备原始文本（hub_port_cycle 的 expect_id 子串匹配用）。
    POSIX: lsusb（既有行为不变）；Windows: 复用 _usb_tree 的原始文本
    （pnputil InstanceId 含 VID_xxxx、UsbTreeView 报告含 VID/PID 行）。"""
    sysname = platform.system() if sysname is None else sysname
    if sysname == "Windows":
        return _usb_tree(sysname, which, run, env)[1]
    run = subprocess.run if run is None else run
    return run(["lsusb"], capture_output=True, text=True).stdout


@handler("dock_topology")
def topology(ctx, step):
    """核对 dock 下的 hub 层级与高速端口线索数（Linux lsusb -t；Windows UsbTreeView→pnputil）。"""
    hs, raw, source = _usb_tree(sysname=ctx.get("sysname"), which=ctx.get("which"),
                                run=ctx.get("run"), env=ctx.get("env"))
    if source == "none":
        return StepResult(step.get("name", "扩展坞拓扑"), False,
                          note="无可用 USB 枚举工具（Windows: UsbTreeView.exe/pnputil，Linux: lsusb）")
    want = int((step.get("limits") or {}).get("min_hs_ports", 1))
    log.debug("拓扑来源 %s: 高速线索 %d（限值 %d）", source, hs, want)
    return StepResult(step.get("name", "扩展坞拓扑"), hs >= want, {"hs_ports": hs},
                      "" if hs >= want else f"高速端口线索低于下限 {want}（来源 {source}）")


@handler("hub_port_cycle")
def port_cycle(ctx, step):
    """对指定下游口做插拔循环（产线机械手/人工按 SOP），统计枚举成功率。"""
    loops = int(step.get("loops", 5))
    expect = str(step.get("expect_id", ""))
    ok_n = 0
    for i in range(loops):
        input(f"[{i+1}/{loops}] 拔出后重新插入 dock 下游口，按回车继续…")
        raw = _device_list(sysname=ctx.get("sysname"), which=ctx.get("which"),
                           run=ctx.get("run"), env=ctx.get("env"))
        if expect.lower() in raw.lower():          # lsusb "ID 2341:abcd" / pnputil "VID_2341" 皆子串命中
            ok_n += 1
    rate = ok_n / loops * 100
    lo = float((step.get("limits") or {}).get("min_rate", 100))
    return StepResult(step.get("name", "插拔循环"), rate >= lo, {"success_rate": f"{rate:.0f}%"})
