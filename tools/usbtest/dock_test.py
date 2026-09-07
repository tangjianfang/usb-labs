"""扩展坞产测后端：拓扑核对（hub 层级/billboard/每口枚举）——经 OS USB 树。"""
import subprocess

from usbtest.core import StepResult   # 旧版漏导入：真实后端任一步骤派发即 NameError（#77）

HANDLERS = {}


def handler(t):
    def deco(fn):
        HANDLERS[t] = fn
        return fn
    return deco


def _usb_tree_linux():
    """lsusb -t 的轻量解析（Linux 专用；Windows 产线建议用 UsbTreeView CLI 导出）。"""
    out = subprocess.run(["lsusb", "-t"], capture_output=True, text=True).stdout
    return out.count("5000M") + out.count("480M"), out


@handler("dock_topology")
def topology(ctx, step):
    """核对 dock 下的 hub 层级与高速端口数（Linux lsusb -t）。"""
    hs, raw = _usb_tree_linux()
    want = int((step.get("limits") or {}).get("min_hs_ports", 1))
    return StepResult(step.get("name", "扩展坞拓扑"), hs >= want, {"hs_ports": hs})


@handler("hub_port_cycle")
def port_cycle(ctx, step):
    """对指定下游口做插拔循环（产线机械手/人工按 SOP），统计枚举成功率。"""
    loops = int(step.get("loops", 5))
    ok_n = 0
    for i in range(loops):
        input(f"[{i+1}/{loops}] 拔出后重新插入 dock 下游口，按回车继续…")
        out = subprocess.run(["lsusb"], capture_output=True, text=True).stdout
        if step.get("expect_id", "") in out:
            ok_n += 1
    rate = ok_n / loops * 100
    lo = float((step.get("limits") or {}).get("min_rate", 100))
    return StepResult(step.get("name", "插拔循环"), rate >= lo, {"success_rate": f"{rate:.0f}%"})
