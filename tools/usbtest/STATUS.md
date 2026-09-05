# usbtest 状态矩阵（诚实清单）

> 更新：2026-09-05（evolve 终局）。状态定义见文末。

## 一、处理器 × 验证状态

| 后端 | 处理器 | 代码 | mock | 真实硬件 | 依赖工装/前置 |
|---|---|---|---|---|---|
| mock | 全部 27 类 | ✅ | ✅ CI 常绿 | —（即本职） | 无 |
| hid | enumerate | ✅ | ✅ | ❌ 待真机 | 无 |
| hid | hid_polling_rate | ✅ | ✅ | ❌ | 移动机构或固件自测上报模式 |
| hid | hid_output_write | ✅ | ✅ | ❌ | 光敏/目检确认 LED |
| hid | hid_report_loopback | ✅ | ✅ | ❌ | 环回工装（固件自环测试模式更佳，见 Lab1 TODO） |
| cdc | serial_loopback | ✅ | ✅ | ❌ | TX-RX 短接治具 |
| cdc | line_coding | ✅ | ✅ | ❌ | 无（pyusb 控制传输） |
| cdc | dfu_verify | ✅ | ✅ | ❌ | dfu-util + 设备 DFU 分区 |
| msc | msc_inquiry / msc_capacity | ✅ | ✅ | ❌ | 无 |
| msc | msc_write_verify | ✅ | ✅ | ❌ | **DESTRUCTIVE**：仅空白盘/授权测试 |
| uvc | uvc_formats / uvc_capture_frames | ✅ | ✅ | ❌ | opencv-python |
| pd | pd_attach / pd_negotiate / measure_voltage | ✅ | ✅ | ❌ | **前置：Lab5 固件实现遥测输出契约**（见下） |
| ble | ble_scan_connect / gatt_discover / notify | ✅ | ✅ | ❌ | 屏蔽箱 + 适配器 |
| uac | uac_record_level | ⚠️ 仅 mock | ✅ | — | 需 OS 音频路由 + sounddevice 后端（未实现） |
| dock | dock_topology / hub_port_cycle | ✅ | ✅ | ❌ | Linux lsusb（Windows 建议接 UsbTreeView CLI） |

## 二、PD 固件↔测试工具契约（待闭合的口子）

`pd_test.py` 依赖 DUT 遥测串口输出 `key=value` 行（2 秒窗口采集）：

```
cc_state=Attached.SNK        # CC 状态（Attached/Wait/…）
contract_v=9                 # 当前合同电压
contract_i=2.25              # 当前合同电流
vbus_v=9.02                  # VBUS 实测（固件 ADC）
```

**Lab5 固件需新增 telemetry 模块**（在 PE_SNK Ready 状态与 VBUS 采样处向 CDC/调试口打印上述行）。契约已冻结：新增字段必须向后兼容（只加不改）。固件实现后，`pd_attach/pd_negotiate/measure_voltage` 三个处理器即可真机验证。

## 三、状态定义

- ✅：代码完整且经 mock 全链路（计划解析→执行→报告→退出码）
- ❌ 真实硬件：代码未经真机执行——**这是本框架当前与商业产测工具的主要差距**；上真机后把本表该行改为 ✅ 并注明设备/日期
- ⚠️ 仅 mock：真实后端未实现，只有确定性模拟

## 四、真机验证路线（建议顺序）

1. 手头有 RP2040/STM32 板：先跑 Lab1/HID 三个处理器（无工装也能测枚举+回报率+LED 目检）；
2. U 盘/读卡器：msc_inquiry/capacity（只读安全）→ write_verify（破坏性，最后）；
3. PD：先完成 Lab5 遥测契约固件，再跑三处理器；
4. 每过一项：更新本表该行为 ✅+设备名+日期，并 git 提交。
