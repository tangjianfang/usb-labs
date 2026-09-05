# usbtest · USB 产测框架（商业产线级）

> 一套 YAML 计划驱动的上位机自动测试框架：**枚举 → 描述符 → 功能 → 性能 → 判定 → 报告**。
> 面向产线测试站（工控机 + 治具），也可 `--mock` 模式在无硬件 CI 里跑流程。

## 架构

```
tests 计划(YAML)                    后端模块(usbtest/*_test.py)
┌──────────────┐    core.run_plan   ┌────────────────────────┐
│ device:      │ ─────────────────▶ │ hid_test  (hidapi/pyusb)│
│  vid/pid/... │                    │ cdc_test  (pyserial/usb)│
│ steps:       │                    │ msc_test  (BOT+SCSI)    │
│  - type: ... │                    │ uvc_test  (描述符+cv2)   │
│  - limits:   │                    │ pd_test   (遥测+SCPI)   │
└──────────────┘                    │ ble_test  (bleak)       │
                                    │ dock_test (拓扑/循环)    │
                                    │ mock_test (无硬件)       │
                                    └────────────────────────┘
          ↓ StepResult 列表
   JSON 报告 + 控制台表格 + 退出码（MES 集成点）
```

## 产线使用

```bash
pip install -r tools/usbtest/requirements.txt   # 或按需最小安装
python -m usbtest --plan labs/lab1-hid-composite/host/autotest.yaml \
                  --dut-sn $(扫码枪输入) --station STN-01
echo $?    # 0=PASS 1=FAIL → MES 联动
```

报告落在 `reports/`（JSON，含工位号/DUT SN/逐步测量值），按时间戳归档，可接 MES/数据库。

## 计划字段

```yaml
name: lab1-hid 复合设备产测
device: {backend: hid, vid: 0x2341, pid: 0x0001}   # backend: hid/cdc/msc/uvc/pd/ble/dock/mock
steps:
  - {type: enumerate, name: 枚举检测}
  - {type: hid_polling_rate, name: 回报率, seconds: 2, limits: {min_hz: 900}}
  - {type: hid_output_write, name: LED 点亮, report: [0x21, 0x00]}
report: {json: reports/}
```

- `type` 必须在各后端模块的 `HANDLERS` 中注册；
- `limits` 与实测值比较，超出即 FAIL（Step 级 fail 不会中断整站，全部跑完再判定）；
- **mock 模式**：`--mock` 用 `mock_test.py` 返回确定性通过——用于 CI 验证计划文件语法与报告链路。

## 各实验室产测计划

| 实验室 | 计划文件 | 覆盖 |
|---|---|---|
| Lab1 HID | [labs/lab1-.../host/autotest.yaml](../../labs/lab1-hid-composite/host/autotest.yaml) | 枚举/回报率/LED/环回 |
| Lab2 CDC+DFU | [labs/lab2-.../host/autotest.yaml](../../labs/lab2-cdc-dfu/host/autotest.yaml) | 环回/线路编码/DFU 校验 |
| Lab3 MSC | [labs/lab3-.../host/autotest.yaml](../../labs/lab3-msc/host/autotest.yaml) | INQUIRY/容量/写读校验 |
| Lab4 UVC+UAC | [labs/lab4-.../host/autotest.yaml](../../labs/lab4-uvc-uac/host/autotest.yaml) | 格式/帧采集 |
| Lab5 PD | [labs/lab5-.../host/autotest.yaml](../../labs/lab5-pd-charger/host/autotest.yaml) | attach/协商/VBUS |
| Lab6 BLE | [labs/lab6-.../host/autotest.yaml](../../labs/lab6-ble-hid-dongle/host/autotest.yaml) | 扫描/GATT/通知 |
| Lab7 Dock | [labs/lab7-.../host/autotest.yaml](../../labs/lab7-dock-altmode/host/autotest.yaml) | 拓扑/插拔循环 |

## 硬件工装要求（诚实清单）

| 步骤类型 | 需要的工装 |
|---|---|
| 串口环回 | TX-RX 短接治具 |
| HID 环回/通知 | 按键机构（电磁铁/舵机压键）或固件自环测试模式 |
| HID 回报率 | 移动机构或固件自动上报测试模式 |
| MSC 写读校验 | **破坏性**：仅产线空白盘/授权测试 |
| PD 测量 | 可编程源/电子负载（SCPI）可选；无仪器走固件遥测 |
| BLE | 屏蔽箱（防邻道干扰）+ 标准 BLE 适配器 |

## 产线集成（MES）

- 退出码 0/1；JSON 报告含工位号/DUT SN/时间戳/逐步测量值；
- 扫码枪输入 → `--dut-sn`；工位号写进计划或 `--station` 参数；
- 报告目录按日期归档，可配数据库上传（由 MES 侧消费 JSON）。
