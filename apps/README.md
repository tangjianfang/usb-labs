# apps · 上位机应用（原生实现）

> 工程方案权威文档：[方案-上位机软件工程实施.md](方案-上位机软件工程实施.md)
> 测试计划/报告与 `tools/usbtest/`（Python 参考实现）**字段级同构**——同一份 JSON 计划可在 Python 参考实现与原生应用间互换。

## 应用矩阵

| 应用 | 平台栈 | 目录 | 状态 |
|---|---|---|---|
| USB DevStudio (Windows) | Win32 + C++20 + VS2022（零第三方依赖）· MS0 起六工作台壳 | [win/USBTestStudio/](win/USBTestStudio/) | 源码完成，待真机联调 |
| USBTestStudio (macOS) | Swift 5.9 + AppKit + IOKit | [mac/USBTestStudio/](mac/USBTestStudio/) | 源码完成，待真机联调 |
| 参考实现（跨平台） | Python（tools/usbtest） | [../tools/usbtest/](../tools/usbtest/) | 已验证（mock 全绿） |

## 支持的测试步骤（三端一致）

| 类型 | Windows | macOS | 判定 |
|---|---|---|---|
| enumerate | CfgMgr32/SetupDi | IOKit 扫描 | 发现且接口数符合 |
| descriptor_check | HIDP_CAPS/WinUSB 描述符 | IOHIDDevice 属性 | 与白名单一致 |
| hid_polling_rate | 重叠 ReadFile + QPC 直方图 | IOHIDReport 时间戳 | ≥ min_hz |
| hid_output_write | HidD_SetOutputReport | setReport | 写成功 |
| serial_loopback | ReadFile/WriteFile（工装） | FileHandle | 字节一致 |
| pd_telemetry | 串口契约采集 | 同（概要） | 字段在限内 |
| msc_read_verify | IOCTL_SCSI_PASS_THROUGH_DIRECT | 概要 | 模式一致（只读默认） |

## 报告契约（MES 集成点）

```json
{ "plan": "...", "station": "STN-01", "dut_sn": "...", "verdict": "PASS",
  "started": "...", "steps": [{ "name": "...", "pass": true, "measured": {...}, "note": "" }] }
```

三端（Windows/macOS/Python）输出字段完全一致；退出码 0/1。

## 知识库回链

协议行为“为什么”→ [USBTree](https://github.com/tangjianfang/USBTree)：HID 枝干（回报率语义）、CDC 枝干（线路编码）、MSC 枝干（BOT/CSW）、Type-C/PD 枝干（协商时序）、[抓包实战](../docs/02-抓包分析实战.md)。
