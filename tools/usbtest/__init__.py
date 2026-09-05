"""USB-Labs 产测框架 (tools/usbtest)

商业产线测试站通用模式：
  YAML 测试计划 → 步骤执行（真实/模拟后端）→ 量化判定 → JSON 报告 + 控制台表格 → 退出码(MES 集成)

用法:
  python -m usbtest.cli --plan labs/lab1-hid-composite/host/autotest.yaml --dut-sn AUTO
"""
__version__ = "1.0.0"
