# USBTestStudio（macOS 版上位机）

USB-Labs 产线测试上位机的 macOS 原生实现：**JSON 测试计划 → 后台引擎逐步执行 → 量化判定 → JSON 报告**。
Swift 5.9 + AppKit + IOKit，**零第三方依赖**。与 Windows 版（`apps/` 方案文档）同构，报告字段与
`tools/usbtest` 完全一致（`plan / station / dut_sn / verdict / started / steps[]`），可直接进 MES。

## 1. 构建与运行

要求：macOS 13+（Ventura）、Xcode 15+（或 Command Line Tools）。

```bash
# 方式 A：命令行
cd apps/mac/USBTestStudio
swift build            # 产物在 .build/debug/USBTestStudio
swift run              # 直接启动 GUI

# 方式 B：Xcode
# 双击 Package.swift（或在目录内执行 `open Package.swift`），Xcode 自动解析 SPM 工程，
# 选择 My Mac 目标后 Cmd+R 运行。
```

> 本工程为 Swift Package executableTarget，无 .xcodeproj；首次 `swift build` 会生成
> Xcode 可识别的包结构。注意：**只能在 macOS 上构建**（依赖 AppKit/IOKit/Darwin）。

## 2. 工程结构

```
Sources/USBTestStudio/
├── main.swift          入口：裸 NSApplication + AppDelegate 装配 + UTSLog 初始化
├── Log.swift           日志层：UTSLog 文件日志（T7 首切片，对齐 Windows spdlog 规范）
├── AppDelegate.swift   UI 层：窗口/发现过滤/表格/日志/进度/快捷键/菜单（引擎回调经 main 队列渲染）
├── DeviceScanner.swift 设备访问层：IOKit 枚举 IOUSBHostDevice → [USBDeviceInfo] + 发现过滤纯函数
├── HIDTester.swift     设备访问层：IOHIDManager 回报率直方图 / setReport / 报告环回
├── SerialTester.swift  设备访问层：/dev/tty.usbmodem* 枚举 + FileHandle 环回 + PD 遥测
├── TestEngine.swift    引擎层：TestPlan(Codable) 解析 → 步骤执行/判定 → TestReport
└── ReportWriter.swift  报告层：与 tools/usbtest 同构字段 JSON 落盘
```

分层规则与线程口径：设备访问层不含 UI 类型；引擎在专属串行 `DispatchQueue` 执行；
UI 线程禁止阻塞 I/O；所有引擎委托回调由 UI 侧 `DispatchQueue.main` 转发。
（Swift 5.9 严格并发/Sendable 严格检查未启用，回调链路以队列纪律保证。）

## 3. 快捷键（对应 Windows 版 Ctrl 系）

| 快捷键 | 功能 |
|---|---|
| F5 | 扫描设备 |
| ⌘R | 运行测试（Windows: Ctrl+R） |
| ⌘S | 导出报告 |
| ⌘O | 打开测试计划 |
| ⌘. | 停止（协作式：当前步骤结束后生效） |
| ⌘C / ⌘A | 拷贝 / 全选日志 |

## 4. 测试计划（JSON）

字段与 `tools/usbtest` 的 YAML 计划**同构**（本应用读 JSON，双平台计划互通，M4 里程碑口径）。
VID/PID 既接受十进制整数，也接受 `"0x1234"` 字符串。YAML 计划可一次性转换：

```bash
python -c "import yaml,json,sys;json.dump(yaml.safe_load(open(sys.argv[1],encoding='utf-8')),open(sys.argv[2],'w',encoding='utf-8'),ensure_ascii=False,indent=2)" lab1.yaml lab1.json
```

示例（对应 `labs/lab1-hid-composite/host/autotest.yaml`）：

```json
{
  "name": "lab1-hid 复合设备产测",
  "station": "STN-01",
  "device": { "vid": "0x1234", "pid": "0x0002", "name_prefix": "USB-Labs", "baud": 115200 },
  "steps": [
    { "type": "enumerate",          "name": "枚举检测" },
    { "type": "descriptor_check",   "name": "描述符合规" },
    { "type": "hid_polling_rate",   "name": "回报率测量", "seconds": 2,
      "limits": { "min_hz": 900 } },
    { "type": "hid_output_write",   "name": "LED 点亮", "report": [33, 0] },
    { "type": "hid_report_loopback","name": "工装环回", "pattern": [85, 170], "timeout_ms": 500 }
  ]
}
```

串口/PD 示例（对应 lab2 / lab5）：

```json
{
  "name": "lab2-cdc-dfu 产测",
  "device": { "vid": "0xCAFE", "pid": "0x4010", "port": "auto", "baud": 115200 },
  "steps": [
    { "type": "enumerate",      "name": "枚举检测" },
    { "type": "line_coding",    "name": "线路编码往返" },
    { "type": "serial_loopback","name": "串口环回", "repeat": 4, "timeout_ms": 500 }
  ]
}
```

```json
{
  "name": "lab5-pd 产测",
  "device": { "telemetry_port": "auto", "baud": 115200 },
  "steps": [
    { "type": "pd_attach",     "name": "CC attach" },
    { "type": "pd_negotiate",  "name": "5V 合同", "expect_v": 5, "tol_v": 0.5 },
    { "type": "pd_negotiate",  "name": "9V 合同", "expect_v": 9, "tol_v": 0.5, "command": "req 9" },
    { "type": "measure_voltage","name": "VBUS 实测", "expect_v": 9, "tol_v": 0.25 }
  ]
}
```

`port` / `telemetry_port` 填 `auto`（默认）时自动选择第一个 `/dev/tty.usbmodem*`；
也可显式写 `/dev/tty.usbmodem2104369` 或省略 `/dev/` 前缀。

## 5. 步骤类型与判定（macOS 支持面）

| 步骤类型 | macOS 实现 | 判定 |
|---|---|---|
| enumerate | IOKit 扫描（IOServiceMatching("IOUSBHostDevice")） | 计划含 vid/pid/name_prefix 时必须命中 |
| descriptor_check | IORegistry 镜像的 bcdUSB/bcdDevice/class/… 白名单比对 | 全部字段一致 |
| hid_polling_rate | IOHIDReportCallback + DispatchTime 间隔直方图 | medianHz ≥ limits.min_hz（可选 max_interval_ms） |
| hid_output_write | IOHIDDeviceSetReport | kr == KERN_SUCCESS(0) |
| hid_report_loopback | setReport 模式 → 等输入报告回显 | 超时前命中 pattern |
| serial_loopback | FileHandle TX→RX 工装环回，字节级比对 | 全部轮次一致（repeat 默认 4） |
| line_coding | **概要**：仅回读本机 termios 速率（无 CDC 控制传输直通） | 本机速率 == device.baud |
| pd_attach / pd_negotiate / measure_voltage | **概要**：CDC 串口遥测行解析（README §4 末尾格式） | 电压样本存在且 \|v−expect_v\| ≤ tol_v |
| msc_read_verify / dfu_verify | 未实现（macOS 无 MSC/DFU 直通），占位 FAIL 并注明 | — |

稳定性口径：**单步异常/失败只置 FAIL + note，不中断整站**（与 `tools/usbtest` Python 版一致）；
报告写盘失败自动降级到 `$TMPDIR/USBTestStudio` 并在日志告警。

## 6. 报告

落盘目录：`~/Desktop/USBTestStudio/reports/`（UI 可 ⌘S 另存）。
文件名与字段与 Python 版一致：

```json
{
  "plan": "lab1-hid 复合设备产测",
  "station": "STN-01",
  "dut_sn": "LAB1-0002",
  "verdict": "PASS",
  "started": "2026-09-06T10:30:00+08:00",
  "steps": [
    { "name": "枚举检测", "pass": true,
      "measured": { "devices": 5, "matched": 1, "pid": 2, "vid": 4660 }, "note": "USB-Labs Composite" }
  ]
}
```

## 7. 签名与权限说明（按 Apple 公开文档口径）

* **本地开发（`swift build` / Xcode 直接运行）**：产物为 ad-hoc/未签名二进制。
  IOKit 枚举（`IOUSBHostDevice` 注册表读取）与 HID 打开（`IOHIDManagerOpen`）**通常无需任何
  特殊授权或 entitlement**；串口 `/dev/tty.usbmodem*` 常规为可读写权限，直接 `open(2)` 即可。
  若遇到打不开 HID 设备（kr=0xe00002c5 之类独占码），多由其他进程（浏览器/系统服务）占用，
  关闭占用方重试。
* **Developer ID 分发**：对外分发建议 `codesign --sign "Developer ID Application" --deep` 并公证
  （notarize）。HID/USB 直访不触发 TCC 弹窗；若应用内新增输入监听（全局事件tap）才涉及
  输入监控权限，本工程未使用。
* **App Store / 沙盒分发**：沙盒下访问 USB 需要 `com.apple.security.device.usb` entitlement，
  且沙盒内**无法直接 open(2) `/dev/tty.*`**（串口步骤会失败）。产线场景建议以 Developer ID
  非沙盒形式分发，或按 Apple 官方文档申请沙盒例外；以最新公开文档为准。
* 以上为公开文档口径的工程性总结，实际以对应 macOS/Xcode 版本的官方文档与真机验证为准。

## 8. 真机联调步骤

1. **枚举**：插上 DUT，F5 扫描；表格应出现 DUT 行（名称/VID:PID/速度/序列号/注册表路径）。
   设备多时可在表格上方搜索框输关键词即时过滤（VID:PID / 产品名 / 路径，空格分隔 = AND，
   不区分大小写），状态栏显示 `显示 N/M 台`。
2. **回报率**：打开 lab1 JSON → ⌘R。hid_polling_rate 期间**持续移动滚轮/轴**（固件上报流必须
   活跃），观察 `hz` 与直方图；`min_hz: 900` 需固件端点轮询 ≥1kHz 且主机侧稳定。
3. **LED/环回**：hid_output_write 观察板上 LED；hid_report_loopback 需工装固件把输出报告回显到
   IN 端点。
4. **串口环回**：插上 TX-RX 短接工装（或临时把 DUT 的 CDC 口 RX/TX 短接），`port: "auto"`，
   ⌘R 观察 `tx/rx/passed`。
5. **PD 遥测**：telemetry 固件按 `{"v":9.01,"i":0.35}`、`vbus_v=9.01` 或 `v=9000mV` 之一输出行；
   若协议不同，改 `SerialTester.parseTelemetryLine`（文件内已注明扩展点）。
6. **报告**：`~/Desktop/USBTestStudio/reports/` 查看与 MES 互通格式；⌘S 可另存。

## 9. 已知限制

* **MSC / DFU 未实现**：macOS 无 SCSI 直通与 DFU 通用直访（概要占位 FAIL），此类步骤请用
  Windows 版执行；两平台报告字段一致，可汇入同一 MES。
* **descriptor_check 为注册表镜像口径**：逐字段全量校验需控制传输直访，当前比对
  idVendor/idProduct/名称前缀 + bcd/class 等注册表镜像字段（与描述符一致）。
* **pd_telemetry 为概要实现**：解析器按三种常见行格式启发式识别（JSON / key=value / 毫伏单位），
  量产前必须按固件实际协议收紧。
* **hid_report_loopback 依赖固件**：普通鼠标/键盘不会回显输出报告，需工装固件支持。
* **蓝牙 HID 不经过 IOUSBHostDevice**：BT 连接的外设不在本枚举口径内。
* **IORegistry `Speed` 单位**：现代 macOS 下为 bit/s，代码按 Mbps 换算并就近分档；
  个别机型/系统版本字段语义若有出入，以 IORegistryExplorer 实测为准。
* **termios 波特率**：macOS 只支持离散标准速率常量（无自定义波特率机制），非标准值回落 115200。
* **Swift 严格并发**：未启用 Sendable 严格检查，跨线程边界以队列纪律 + NSLock 保证。

## 10. 代码内标注约定

所有无法离线确认签名细节的 API 均在源码注释中标注「以 Xcode 实际头文件为准」，主要包括：
`IORegistryEntryGetPath`（io_name_t 缓冲）、`IOHIDManagerCopyDevices` 桥接、
`IOHIDDeviceRegisterInputReportCallback` 回调形参、`kIOHIDReportTypeOutput` 导入形式、
`IOHIDDeviceSetReport` 的 reportID=0 语义、`VMIN/VTIME` 常量导入、`kIOMasterPortDefault`
弃用别名（macOS 12+ 为 `kIOMainPortDefault`）。编译若报签名不匹配，按注释给出的备选写法调整。

## 11. T7 首切片：文件日志 + 设备发现过滤

对齐 Windows 版 EP-4 S2「设备发现」的工程师体验核心
（`apps/win/USBTestStudio/src/app/log.h` 日志规范、`apps/设计-工程师通信控制台.md`
§二 设备发现区 / §四.1 即时过滤）。

| 能力 | 状态 |
|---|---|
| 文件日志（UTSLog） | ✓ 首切片 |
| 设备发现过滤（搜索框即时过滤） | ✓ 首切片 |
| EP-4 三区布局 / 多会话标签 / 收发台（S3 会话台） | 未移植 |

**文件日志**（`Sources/Log.swift`，`enum UTSLog`）：

* 落盘 `~/Library/Logs/USBTestStudio/app.log`（追加写；简单大小检查，超 5MB 轮转 `app.log.1`，保留一代）；
* 行格式 `[yyyy-MM-dd HH:MM:SS.mmm][LEVEL][component] message`，与 Windows spdlog 规范一致
  （Windows 版模块段后另有线程段 `[T%t]`，本切片按 T7 口径省略）；
* 级别语义同 C++：trace=逐包 · debug=逐操作 · info=生命周期 · warn=可恢复 · err=失败；
* 级别开关：环境变量 `USBTS_LOG_LEVEL=trace|debug|info|warn|err`（兼容 warning/error），缺省 info；
  warn 及以上写后立即 fsync（对应 C++ `flush_on(warn)`）；
* 线程安全：写路径收敛到串行 DispatchQueue；另镜像写 stderr（`swift run` 终端 / Console.app 可见，
  对应 Windows OutputDebugString 靶）；日志失败静默降级，不拖死业务。

**设备发现过滤**（`AppDelegate` 过滤框 + `DeviceScanner.filtered(_:by:)` 纯函数）：

* 设备表上方 `NSSearchField`：逐键即时过滤（`controlTextDidChange`，EN_CHANGE 等价）+ 500ms 防抖；
  回车 / ✕ 清空绕过防抖立即生效；
* 大小写不敏感子串匹配 VID:PID（含 `12340002` 紧凑十六进制形）/ 产品名 / IORegistry 路径；
  多关键词空格分隔 = AND（设计 §四.1）；
* 状态栏显示 `显示 N/M 台`；关键词变化 debug 留痕、命中数 info（component: `discovery`）；
* 过滤层为数据层纯函数，作用于真实 IOKit 枚举结果（`scan()` 非 TODO/桩），对桩数据同样可跑。

> ⚠ 本切片代码**未编译验证**（开发机无 macOS 工具链），按 Swift 5.9 / AppKit 口径编写；
> 涉及文件（main / AppDelegate / DeviceScanner / Log.swift）头部注释有同样标注，
> `swift build` 若报签名不匹配，按 README §10 与各文件注释的备选写法调整。
