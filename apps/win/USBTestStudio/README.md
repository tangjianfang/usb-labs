# USBTestStudio — USB 产线测试上位机（Windows）

USB-Labs 的 Windows 原生上位机：**测试计划(JSON) → 步骤执行（枚举/描述符/HID 回报率/串口环回/MSC 只读校验/PD 遥测）→ 判定 → JSON 报告 + MES 退出码**。

- Win32 API + C++20（VS2022 v143）、x64、Unicode、`/W4 /utf-8 /permissive-`
- **零第三方依赖**（JSON writer/parser 自研，见 `src/framework/`）
- 高性能：重叠 I/O（OVERLAPPED + 事件）+ QPC 计时；引擎在 `std::jthread`
- 稳定：全部 Win32 句柄 RAII（`wraii::unique_handle`）；UI 线程零阻塞 I/O；引擎→UI 经 `PostMessage(WM_APP+1..3)` + 堆载 payload；日志有界队列 + 丢弃计数
- 人性化：原生控件、PerMonitorV2 DPI、快捷键 F5 / Ctrl+R / Ctrl+S、等宽着色日志

> **重要声明：本工程未在真机上编译验证。** 代码按 MSDN 口径编写，所有需要复核的 API 细节见文末“不确定 API 清单”，源码内亦有行内注释标注（“以 MSDN 为准”）。

---

## 1. 打开与构建

### 方式 A：Visual Studio 2022（sln）
1. 双击 `USBTestStudio.sln`（v143 工具集，需勾选“MSVC v143 + Windows 10/11 SDK”）。
2. 选 `Release | x64` → 生成（Ctrl+Shift+B）。
3. 产物：`build\msbuild\x64\Release\USBTestStudio.exe`。

### 方式 B：CMake
```bat
cd C:\tjf\github\usb-labs\apps\win\USBTestStudio
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```
或在 VS2022 中“打开文件夹”直接加载 `CMakeLists.txt`。

需要重试链标准 C++20（`std::jthread`、`stop_token` 要求 VS2022 17.x）；未使用 `<format>`（数字格式化用 `swprintf`，规避实现差异）。

## 2. 运行

```
USBTestStudio.exe [--plan <路径>] [--dut-sn <SN>] [--station <工位>] [--auto] [--console]
```
- 计划默认查找 **exe 同目录 `plan.json`**；示例见 `plans/sample_plan.json`。
- `--auto`：产线模式，测试完成后自动退出并把**退出码**交给调用方。
- `--console`：**EP-4 工程师通信控制台**（独立窗口，产测模式旁路）——设备目录
  即时过滤（搜索框输入 名称/VID:PID/协议/路径 子串，空格分隔多词 AND，协议
  复选框二次过滤；目录含 USB/HID 接口、USB 大容量盘、COM 口三类来源），F5
  后台重扫，**双击设备行开会话**——四类通道：HID 报告/串口终端/WinUSB 批量
  管道（设备须绑定 WinUSB 驱动，open 失败即弹因）/MSC 只读 SCSI 直通（发送框
  即 CDB：IN 命令回收数据帧，CHECK CONDITION 回收 18 字节 SENSE 帧）。
- 退出码：`0` PASS，`1` FAIL，`2` 中止（用户关闭/停止），`3` 计划加载失败。
- 报告自动写盘 `exe\reports\report_<计划名>_<DUT_SN>_<时间戳>.json`；也可 Ctrl+S 另存。
- 快捷键：**F5** 扫描设备，**Ctrl+R** 运行计划，**Ctrl+S** 导出报告。

## 3. 测试计划 JSON（与 tools/usbtest 计划互通）

字段与 `tools/usbtest/*.py` 的 YAML 计划同名同义；顶层：

```jsonc
{
  "name": "usb_prod_sample",       // 报告 plan 字段
  "station": "STN-01",             // 工位号（可选，默认 STN-01）
  "dut_sn": "AUTO",                // DUT 序列号（可用 --dut-sn 覆盖）
  "device": {
    "vid": "0x1234",               // 数字或 "0x…" 字符串；-1/缺省 = 不限
    "pid": "0x5678",
    "port": "COM7",                // CDC 串口（serial_loopback 用）
    "telemetry_port": "COM8",      // PD 遥测串口（pd_* 用）
    "baud": 115200,
    "drive": -1                    // PhysicalDriveN 序号；-1 = 自动探测 USB 盘
  },
  "steps": [ { "type": "...", "name": "显示名", ... }, ... ]
}
```

### 支持的步骤类型（与 Python HANDLERS 同名）

| type | 专有字段 | 判定（口径同 Python 版） |
|---|---|---|
| `enumerate` | — | 匹配 VID/PID 的设备数 > 0，measured `{interfaces, total}` |
| `descriptor_check` | `usage_page`/`usage`（白名单，可选） | HID 打开成功且 HIDP_CAPS 与白名单一致 |
| `hid_polling_rate` | `seconds`、`limits.min_hz` | 均值 Hz ≥ min_hz（需触发设备上报，如移动鼠标） |
| `hid_output_write` | `report`（字节数组，[0]=Report ID）、`report_id` | HidD_SetOutputReport 成功 |
| `hid_report_loopback` | `pattern`、`report_id`、`timeout_ms` | 输入报告回显与图案一致（需工装回显固件） |
| `serial_loopback` | `repeat`（图案 = bytes(range(256)) × repeat） | 逐字节一致（需 TX-RX 短接工装或固件回显） |
| `msc_inquiry` | — | SCSI 直通 INQUIRY 状态 0，measured `{vendor, product, type}` |
| `msc_capacity` | `limits.min_gb` | 容量 ≥ 下限，measured `{gb, block}` |
| `msc_read_verify` | `lba`、`blocks`、`loops` | 同区段双读比对一致且非全 0x00/0xFF（**只读**，不写盘） |
| `pd_attach` | `window_ms` | 遥测 `cc_state` 含 "Attached" |
| `pd_negotiate` | `command`、`expect_v`、`tol_v`(默认0.5) | `contract_v` 在期望 ± 容差内 |
| `measure_voltage` | `expect_v`、`tol_v`(默认0.25) | `vbus_v` 在期望 ± 容差内 |
| `msc_write_verify` | — | **DESTRUCTIVE，本版不执行 → FAIL** |
| `line_coding` / `dfu_verify` | — | 需 WinUSB/外部工具链，本版未实现 → FAIL |
| 其他 | — | 未知类型 → FAIL（同 Python 版口径） |

### PD 遥测契约（与 `tools/usbtest/pd_test.py` 一致）

DUT 固件经 `telemetry_port` 以文本行输出 `key=value`（换行分隔），键：`cc_state`、`contract_v`、`contract_i`、`vbus_v`。收齐全部键即提前结束，否则读满窗口时间；可选 `command` 字段在采集前下发触发命令（附 `\n`）。每键取首个命中值（同 Python 口径）。

## 4. 报告 JSON（结构与 tools/usbtest 完全一致）

```json
{
  "plan": "usb_prod_sample",
  "station": "STN-01",
  "dut_sn": "AUTO",
  "verdict": "PASS",
  "started": "2026-09-06T12:34:56",
  "steps": [
    { "name": "回报率", "pass": true,
      "measured": { "hz": 999.2, "min_hz": 943.1, "max_hz": 1012.6, "samples": 2998 },
      "note": "" }
  ]
}
```

## 5. 真机联调步骤

1. **编译**：方式 A 或 B 构建 Release|x64。
2. **只做枚举冒烟**：把 `plans/sample_plan.json` 复制为 exe 旁 `plan.json`，先删掉除 `enumerate` 外的步骤，运行并按 F5 扫描，确认设备列表出现 `[USB]/[HID]` 条目且 VID/PID 正确。
3. **HID 回报率**：填入 DUT 的 `device.vid/pid`，保留 `hid_polling_rate`；运行时持续触发输入（移动鼠标/产测工装注入信号），观察日志中样本数与瞬时 Hz 直方图。
4. **串口环回**：接好环回工装（或固件回显固件），`device.port` 填 CDC 口；若设备枚举为 COM10 以上，程序已用 `\\.\COMx` 规范化路径。
5. **MSC 只读校验**：插入 DUT 的 SD/存储；`drive: -1` 自动探测 USB 总线盘；**接系统盘时务必填 `drive` 明确指定**（探测逻辑只挑 BusTypeUsb，但仍建议显式指定）。
6. **PD 遥测**：`device.telemetry_port` 填 DUT 遥测 CDC 口，固件按 `key=value` 行输出；先用串口终端人工核对行格式再进自动判定。
7. **产线接入**：MES 以 `--auto` 启动并读取退出码与 `reports\*.json`。

## 6. 架构与线程模型

```
UI（主线程）  MainWindow/LogView —— 只消费事件，零阻塞 I/O
                ▲ WM_APP+1 日志（有界队列+丢弃计数）/ +2 步骤 / +3 完成（堆载 payload）
引擎（jthread）TestEngine：计划解析 → 步骤分发 → 判定 → 报告写盘
                ▼ 调用
设备访问层     DeviceEnumerator(SetupDi/CfgMgr32) · HidPort(hid.dll+重叠I/O)
               SerialPort(\\.\COMx+DCB) · WinUsbPort(winusb.dll+批量/中断管道选型)
               MscScsi(IOCTL_SCSI_PASS_THROUGH_DIRECT)
```
扫描在独立的短生命周期线程执行（SetupDi 枚举 + PostMessage 回窗）。UI 侧在 `WM_APP+2/3` 中以 `unique_ptr` 接管并释放 payload。

## 7. 已知限制

1. **未在真机编译**——本工程按 MSDN 口径盲写；首次编译预计需要小幅修正（见下方不确定清单）。
2. `msc_write_verify`（DESTRUCTIVE 写读校验）未实现；`line_coding`（WinUSB 控制传输）与 `dfu_verify`（外部工具）未实现，执行到即 FAIL。
3. `READ_CAPACITY(10)` 上限 2TB；更大盘需 `READ_CAPACITY(16)`（service action 0x10），未实现。
4. `hid_report_loopback`/`serial_loopback` 依赖回显固件或工装短接，纯软件无法自测。
5. 报告 `started` 用本地时间（同 Python 版）；跨时区 MES 需自行约定时区。
6. 启动时后台扫描线程若恰逢窗口销毁，存在一次小分配的理论泄漏（PostMessage 成功但消息未被处理）；量级为单设备列表，工程上可接受。
7. ListView 列宽按 96dpi 基准写死，高 DPI 下未随 WM_DPICHANGED 重新按比例调整（仅窗口与字体缩放）。

## 8. 不确定 API 清单（首次真机编译/联调需按 MSDN 复核）

| # | API / 结构 | 用途 | 复核点 |
|---|---|---|---|
| 1 | `SetupDiGetDeviceInterfaceDetailW` 两段式调用 | 枚举设备路径 | `SP_DEVICE_INTERFACE_DETAIL_DATA_W.cbSize = sizeof(...)` 在 x64 的打包行为（必须与头文件打包一致；不得手填 8） |
| 2 | `CM_Get_DevNode_PropertyW` | HardwareIds/BusReportedDeviceDesc/InstanceId | 首次调用返回 `CR_BUFFER_SMALL` 并给出 size 的两段式模式；`DEVPROP_TYPE_STRING_LIST` 的多字符串布局 |
| 3 | `DEVPKEY_Device_BusReportedDeviceDesc` | 设备名 | 仅 Win8+ 设备树上报；取不到时回退 FriendlyName/产品字符串（代码已回退） |
| 4 | `HidD_SetNumInputBuffers` | 回报率测量防丢包 | 取值范围 [2,512]；对后续 ReadFile 的缓冲语义 |
| 5 | `HidD_SetOutputReport` 与 `FILE_FLAG_OVERLAPPED` 句柄组合 | HID 输出报告 | 同步 API 在重叠句柄上的行为（代码已加“临时同步句柄”回退） |
| 6 | `HidD_GetProductString` | 设备名 | 缓冲区须为 2 的倍数字节、宽字符返回、query-only 句柄（访问权限 0）是否足够 |
| 7 | `CancelIoEx` + `GetOverlappedResult(bWait=TRUE)` | 读超时路径 | 取消后回收操作的推荐序列；`ERROR_OPERATION_ABORTED` 竞态处理 |
| 8 | `SCSI_PASS_THROUGH_DIRECT` 缓冲布局 | SCSI 直通 | `Length`、`SenseInfoOffset`/`DataBuffer` 同缓冲、8 字节对齐、输入输出共用缓冲的方向与大小校验 |
| 9 | SENSE 固定/描述符格式解析 | 错误定位 | 0x70/0x71 与 0x72/0x73 格式 key/ASC/ASCQ 的字节位（SPC 口径） |
| 10 | `IOCTL_STORAGE_QUERY_PROPERTY` + `STORAGE_DEVICE_DESCRIPTOR.BusType` | USB 盘自动探测 | BusType 偏移随 Version 变化；`BusTypeUsb` 取值 |
| 11 | `COMMTIMEOUTS` 各字段组合 | 串口读兜底超时 | `ReadIntervalTimeout`/`ReadTotalTimeout*` 组合语义（本工程以自身 deadline 为主，内核值仅兜底） |
| 12 | `GetDpiForWindow` / `WM_DPICHANGED` | PerMonitorV2 | 最低系统 Win10 1607；`HIWORD(wParam)` 取 DPI、`lParam` 建议矩形用法 |
| 13 | RichEdit `MSFTEDIT_CLASS`("RICHEDIT50W") + `EM_SETCHARFORMAT` | 日志视图 | `SCF_SELECTION` 对“随后插入文本”的格式继承；`EM_EXLIMITTEXT` 上限；`EM_LINEINDEX` 裁剪序列 |
| 14 | `PostMessage` 跨线程堆指针 payload | 引擎→UI 封送 | WPARAM/LPARAM 在 64 位下的截断（代码按 `INT_PTR`/指针尺寸传递，需复核） |
| 15 | `WinUsb_Initialize` 前置与 `FILE_FLAG_OVERLAPPED` | WinUSB 批量管道（EP-4 S5） | 须以读写+重叠标志 CreateFile；未绑 WinUSB 驱动的设备返回失败码的区分 |
| 16 | `WinUsb_ReadPipe` + `WinUsb_AbortPipe` 超时回收 | WinUSB 读轮片 | 取消在途传输后 `GetOverlappedResult(bWait=TRUE)` 的回收序列与 `ERROR_OPERATION_ABORTED` 竞态（口径同 HidPort） |
| 17 | `WinUsb_Initialize` 接受 `GUID_DEVINTERFACE_USB_DEVICE` 接口路径 | EP-4 S5 目录 usb 行→通道 | 目录用 USB 设备节点接口路径（libusb 同口径）开 WinUSB；非 WinUSB 驱动设备的失败码区分需真机核对 |
| 18 | `CreateFileW(\\.\PhysicalDriveN, GENERIC_READ)` 无管理员权限行为 | EP-4 S5 MSC 目录扫描/会话 | 普通权限下打开可能失败（目录少一行/会话 open 报错）；扫描期 INQUIRY/READ_CAPACITY 对已挂载卷的副作用边界 |
| 19 | `WinUsb_WritePipe` 超时后 `WinUsb_AbortPipe` 取消 | EP-4 S5 写路径有界等待（evolve #71） | OUT 传输被取消时设备侧可能已收部分字节（帧不完整、上层按失败处理）；超时判定与中止生效间的完成竞态（代码已按 GOR 结果兜底，需真机证实）；AbortPipe 本身失败时 `GetOverlappedResult(bWait=TRUE)` 理论可无限等待（回收路径返值未设防，低概率）；3s 默认对慢设备（Flash 缓冲写）是否偏紧 |

## 9. 文件结构

```
USBTestStudio/
├── CMakeLists.txt / USBTestStudio.sln / USBTestStudio.vcxproj(.filters) / app.manifest
├── plans/sample_plan.json
└── src/
    ├── app/main.cpp              入口、命令行、消息循环
    ├── framework/win32_rai.*     RAII 句柄、错误消息、JSON writer、文件工具
    ├── framework/json_mini.*     测试计划 JSON 解析（自研轻量）
    ├── usb/device_enumerator.*   USB/HID 接口枚举（SetupDi + CfgMgr32）
    ├── usb/hid_port.*            HID caps / 重叠读 / 输出报告 / QPC 回报率直方图
    ├── usb/serial_port.*         DCB / 重叠读写 / 环回 / PD 遥测（key=value）
    ├── usb/msc_scsi.*            SCSI 直通 INQUIRY/READ_CAPACITY/READ10 + sense（只读）
    ├── usb/winusb_port.*         EP-4 S5 WinUSB 批量/中断管道（接口 0 + 管道选型 + 重叠读写；
    │                             VID/PID 路径解析与选型为纯逻辑，离线自测覆盖）
    ├── channel/channel.h         EP-4 会话台统一通道契约 IChannel（open/send/on_receive/stats）
    ├── channel/serial_channel.h  IChannel 串口实现（模板化 PortT，可注入假件自测）
    ├── channel/hid_channel.h     IChannel HID 实现（Report ID 透传，模板化同上）
    ├── channel/winusb_channel.h  EP-4 S5 IChannel WinUSB 实现（IN 传输=帧，模板化同上）
    ├── channel/msc_channel.h     EP-4 S5 IChannel MSC 实现（发送框=CDB，CDB 方向/长度计划表
    │                             为纯逻辑；CHECK CONDITION 以 SENSE 帧呈现；只读拒写，模板化同上）
    ├── discovery/device_catalog.h EP-4 S2 目录条目+即时过滤（多关键词 AND/kind 掩码，纯逻辑）
    ├── discovery/serial_enum.h   COM 口枚举（SERIALCOMM 注册表，数字序排序）
    ├── discovery/msc_enum.h      EP-4 S5 USB 大容量盘目录行（PhysicalDrive0..9 BusTypeUsb 过滤）
    ├── discovery/catalog_build.h EP-4 S2/S5 目录组装（DeviceInfo+MSC+COM 合流）+ 会话工厂（四通道→IChannel）
    ├── session/session_codec.h  EP-4 S3 收发编解码（发送框智能识别/双视图/时间戳行，纯逻辑）
    ├── session/session_core.h   EP-4 S3 会话核心（帧日志/发送历史/周期节拍/时间基准，纯逻辑）
    ├── session/session_view.h   EP-4 S3 显示视图模型（渲染游标：暂停/双视图切换的账面-显示解耦，纯逻辑）
    ├── parser/hid_parser.h      EP-4 S4 HID 报告解码（键码表/修饰键位图/鼠标位移/消费页用量/Report ID 剥离，纯逻辑）
    ├── parser/parser_select.h   EP-4 S4 解析器选型+帧解析调度（usage page/usage→键盘/鼠标/消费页/ASCII，纯逻辑）
    ├── engine/test_engine.*      计划加载、步骤分发、判定、JSON 报告、事件泵
    └── ui/log_view.* / main_window.* / console_window.* / session_pane.*   RichEdit 日志 / 产测主窗口 / EP-4 通信控制台（发现+会话标签台）/ EP-4 会话收发面板
```
