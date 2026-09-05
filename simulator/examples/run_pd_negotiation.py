import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from usbsim import pd as PD

src = PD.Source()                                  # 5V/3A + 9V/3A + 20V/2.25A
snk = PD.Sink(want_v=9, want_i=2000)               # 受电端想要 9V/2A
log = []

ok = PD.negotiate(src, snk, log)
print("协商结果:", "PASS" if ok else "FAIL", "| 合同 RDO =", hex(snk.rdo))
for e in log:
    print(f"  {e['dir']:<8} {e['hex'].hex() if isinstance(e['hex'], bytes) else e['hex']:<24} {e['desc']}")
print("\n说明: 序列为基于 PD 3.2 规范重构的教学样本（消息层），非真实 BMC 波形采集。")
