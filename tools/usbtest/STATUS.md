# usbtest 状态矩阵（诚实清单）

> 更新：2026-09-05（evolve 终局）；2026-09-07（evolve #77：六真实后端漏导入 StepResult 收口 + 复核硬化三处——run_plan 无 open_device 后端派发前崩溃/UUID 全形整串匹配/notify 无 HOGP 干净失败，离线自测 tests/test_backends.py 26 例 CI 常绿）；2026-09-10（T9/T10：uac_record_level 真实后端落地（T9）——sounddevice 可选依赖，无库优雅失败不裸抛 ImportError，Windows 开发机经 core.run_plan 全链打通至真实录音电平判定；dock 后端 Windows 路径（T10）——UsbTreeView.exe CLI 优先/pnputil /enum-devices 回退（zh-CN GBK 输出解码已钉），可注入纯函数留测试缝，离线自测 tests/test_backends.py 35 例 CI 常绿）。状态定义见文末。

## 一、处理器 × 验证状态

| 后端 | 处理器 | 代码 | mock | 真实硬件 | 依赖工装/前置 |
|---|---|---|---|---|---|
| mock | 全部 27 类 | ✅ | ✅ CI 常绿 | —（即本职） | 无 |
| hid | enumerate | ✅ | ✅ | ✅ 真机（2026-09-10：Razer 1532:00B2，9 集合） | 无 |
| hid | hid_polling_rate | ✅ | ✅ | ◐ 真机链路通（静置 3s 实测 0Hz——需动鼠标/工装触发上报，工具行为如实） | 移动机构或固件自测上报模式 |
| hid | hid_output_write | ✅ | ✅ | ◐ 真机（Razer 拒通用输出报告 written=-1——消费鼠非产测 DUT；对自研固件 LED 有效） | 光敏/目检确认 LED |
| hid | hid_report_loopback | ✅ | ✅ | ❌ | 环回工装（固件自环测试模式更佳，见 Lab1 TODO） |
| cdc | serial_loopback | ✅ | ✅ | ❌ | TX-RX 短接治具 |
| cdc | line_coding | ✅ | ✅ | ❌ | 无（pyusb 控制传输） |
| cdc | dfu_verify | ✅ | ✅ | ❌ | dfu-util + 设备 DFU 分区 |
| msc | msc_inquiry / msc_capacity | ✅ | ✅ | ❌ | 无 |
| msc | msc_write_verify | ✅ | ✅ | ❌ | **DESTRUCTIVE**：仅空白盘/授权测试 |
| uvc | uvc_formats / uvc_capture_frames | ✅ | ✅ | ✅ 真机（集成摄像头 13d3:56D5：14 UVC 接口；640x480×5 帧实拍出图） | opencv-python + libusb（libusb-package 提供 DLL，PATH 挂载） |
| pd | pd_attach / pd_negotiate / measure_voltage | ✅ | ✅ | ❌ | **前置：Lab5 固件实现遥测输出契约**（见下） |
| ble | ble_scan_connect / gatt_discover / notify | ✅ | ✅ | ❌ | 屏蔽箱 + 适配器 |
| uac | uac_record_level | ✅ 代码完整（sounddevice 可选依赖，未装时优雅失败） | ✅ | ◐ 主机侧✅（2026-09-10 板载 Realtek 麦 -58.2 dBFS 实录）；USB UAC DUT 待工装 | OS 音频路由（DUT 麦克风→工控机默认输入设备）+ pip install sounddevice |
| dock | dock_topology / hub_port_cycle | ✅ | ✅ | ◐ 主机侧✅（2026-09-10 实跑 dock_topology PASS，7 HS 端口）；dock DUT 待接入 | Linux: lsusb -t；Windows: UsbTreeView.exe CLI（/c /f 文本导出，参数以 UsbTreeView 官方文档为准）优先 → pnputil /enum-devices（Win10 2004+ 内置，zh-CN GBK 输出已兼容）回退。pnputil 枚举路径已在 zh-CN Windows 开发机实测（主机侧枚举非 dock DUT 联调） |

## 二、PD 固件↔测试工具契约（待闭合的口子）

`pd_test.py` 依赖 DUT 遥测串口输出 `key=value` 行（2 秒窗口采集）：

```
cc_state=Attached.SNK        # CC 状态（Attached/Wait/…）
contract_v=9                 # 当前合同电压
contract_i=2.25              # 当前合同电流
vbus_v=9.02                  # VBUS 实测（固件 ADC）
```

**固件侧已实现（2026-09-10，ROADMAP T4）**：`labs/lab5-pd-charger/firmware/src/telemetry.c` 实现 `pd_telemetry_emit`（四行 key=value，MSVC 宿主自检实证输出格式）；`pe_sink.c` 每 500ms 节流 + 状态沿即时 emit；VBUS 经 `pd_telemetry_set_vbus_sampler()` 注入板级 ADC（未注入按 0.0，判定以 contract_v 为准）。契约冻结不变：新增字段只加不改。剩余口子 = 真机烧录 Lab5 固件后的三处理器真机验证（T8 工装域）。

## 三、状态定义

- ✅：代码完整且经 mock 全链路（计划解析→执行→报告→退出码）
- ❌ 真实硬件：代码未经真机执行——**这是本框架当前与商业产测工具的主要差距**；上真机后把本表该行改为 ✅ 并注明设备/日期
- ⚠️ 仅 mock：真实后端未实现，只有确定性模拟

## 四、真机验证路线（建议顺序）

1. 手头有 RP2040/STM32 板：先跑 Lab1/HID 三个处理器（无工装也能测枚举+回报率+LED 目检）；
2. U 盘/读卡器：msc_inquiry/capacity（只读安全）→ write_verify（破坏性，最后）；
3. PD：先完成 Lab5 遥测契约固件，再跑三处理器；
4. 每过一项：更新本表该行为 ✅+设备名+日期，并 git 提交。

## 五、原生上位机（apps/）构建状态

| 平台 | 编译 | 真机联调 |
|---|---|---|
| Windows USBTestStudio（Win32/C++20） | ✅ MSVC 14.51 (VS 18) Release 编译链接通过 + 冒烟运行（2026-09-05） | 待真机（插 DUT 后按 README 联调步骤） |
| macOS USBTestStudio（Swift/AppKit） | ⏸ 挂起（本机无 macOS/Xcode，源码就绪） | ⏸ |
