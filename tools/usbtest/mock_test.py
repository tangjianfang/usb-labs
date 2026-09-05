"""模拟后端：无硬件时的确定性结果（CI/培训/演示用）。
所有 mock 处理器按计划里的 limits 返回"刚好通过"的测量值，验证流程与报告链路。
"""
from usbtest.core import StepResult


def _measured(step):
    m = {}
    for k, v in (step.get("limits") or {}).items():
        m[k] = (v.get("min") if isinstance(v, dict) else v) if isinstance(v, (dict, int, float, str)) else "OK"
    return m or {"demo": "OK"}


def _mk(step):
    return StepResult(step.get("name", step["type"]), True, _measured(step), "mock")


HANDLERS = {t: (lambda ctx, s, _t=t: _mk(s)) for t in (
    "enumerate", "descriptor_check", "hid_polling_rate", "hid_output_write",
    "hid_report_loopback", "serial_loopback", "line_coding", "dfu_verify",
    "msc_inquiry", "msc_capacity", "msc_write_verify", "uvc_formats", "uvc_capture_frames",
    "uac_record_level", "pd_attach", "pd_negotiate", "measure_voltage", "ble_scan_connect",
    "ble_gatt_discover", "ble_hid_notify", "dock_topology", "hub_port_cycle",
)}
