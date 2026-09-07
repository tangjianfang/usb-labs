"""核心：测试计划加载、步骤分发、报告生成。"""
from __future__ import annotations
import argparse
import datetime
import importlib
import json
import pathlib
import sys

DEFAULT_STATION = "STN-01"


class StepResult:
    def __init__(self, name: str, passed: bool, measured: dict | None = None, note: str = ""):
        self.name, self.passed, self.measured, self.note = name, passed, measured or {}, note

    def row(self):
        m = " ".join(f"{k}={v}" for k, v in self.measured.items())
        return f"{'PASS' if self.passed else 'FAIL'} | {self.name:<24} | {m} {self.note}".rstrip()


class TestReport:
    def __init__(self, plan: dict, dut_sn: str, station: str):
        self.plan, self.dut_sn, self.station = plan, dut_sn, station
        self.steps: list[StepResult] = []
        self.started = datetime.datetime.now()

    def add(self, r: StepResult):
        self.steps.append(r)
        print(r.row())

    @property
    def verdict(self):
        return all(x.passed for x in self.steps) and bool(self.steps)

    def save_json(self, outdir: str):
        outdir = pathlib.Path(outdir)
        outdir.mkdir(parents=True, exist_ok=True)
        ts = self.started.strftime("%Y%m%d_%H%M%S")
        f = outdir / f"report_{self.plan.get('name', 'plan')}_{self.dut_sn}_{ts}.json"
        data = {
            "plan": self.plan.get("name"), "station": self.station, "dut_sn": self.dut_sn,
            "verdict": "PASS" if self.verdict else "FAIL",
            "started": self.started.isoformat(timespec="seconds"),
            "steps": [{"name": x.name, "pass": x.passed, "measured": x.measured, "note": x.note}
                      for x in self.steps],
        }
        f.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"\n报告: {f}")
        return f


def load_plan(path: str) -> dict:
    try:
        import yaml
    except ImportError:
        sys.exit("缺少依赖: pip install pyyaml")
    plan = yaml.safe_load(pathlib.Path(path).read_text(encoding="utf-8"))
    assert "device" in plan and "steps" in plan, "计划缺少 device/steps"
    return plan


def run_plan(plan: dict, dut_sn: str, station: str, mock: bool) -> TestReport:
    """按 backend 加载对应协议模块并执行 steps。
    模块约定: usbtest/<backend>_test.py 内有 HANDLERS = {<type>: callable(ctx, params)->StepResult}
    ctx 携带 device 匹配信息与 mock 标志，供真实后端/模拟后端选择。
    """
    backend = "mock" if mock else plan["device"].get("backend", "mock")
    mod = importlib.import_module(f"usbtest.{backend}_test")
    handlers = getattr(mod, "HANDLERS", {})
    ctx = {"device": plan["device"], "station": station, "dut_sn": dut_sn,
           # ble/dock 无 open_device（无设备句柄概念）——缺省 None 而非派发前 AttributeError（#77 复核 H1）
           "dev": None if mock else getattr(mod, "open_device", lambda d: None)(plan["device"])}
    rep = TestReport(plan, dut_sn, station)
    for step in plan["steps"]:
        stype = step["type"]
        h = handlers.get(stype)
        if h is None:
            rep.add(StepResult(step.get("name", stype), False, note=f"未知步骤类型 {stype}"))
            continue
        try:
            rep.add(h(ctx, step))
        except Exception as e:  # 设备拔出/驱动异常不应中断整站流程
            rep.add(StepResult(step.get("name", stype), False, note=f"异常: {e}"))
    return rep


def main(argv=None):
    ap = argparse.ArgumentParser("usbtest 产测框架")
    ap.add_argument("--plan", required=True, help="YAML 测试计划")
    ap.add_argument("--dut-sn", default="AUTO", help="DUT 序列号（产线扫码/AUTO 自动取）")
    ap.add_argument("--station", default=DEFAULT_STATION)
    ap.add_argument("--mock", action="store_true", help="无硬件模拟模式（CI/演示）")
    ap.add_argument("--report-dir", default="reports")
    a = ap.parse_args(argv)
    plan = load_plan(a.plan)
    rep = run_plan(plan, a.dut_sn, a.station, a.mock)
    rep.save_json(a.report_dir)
    print(f"\n===== 判定: {rep.verdict and 'PASS' or 'FAIL'} =====")
    return 0 if rep.verdict else 1


if __name__ == "__main__":
    sys.exit(main())
