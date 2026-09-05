//
//  TestEngine.swift
//  USBTestStudio — 测试引擎（可独立于 UI 的核心层）
//
//  职责：JSON 测试计划解析 → 后台串行队列逐步执行 → 量化判定 → TestReport JSON 落盘。
//
//  口径（与 tools/usbtest/core.py、Windows 版一致）：
//    · 步骤级异常不中断整站：单步 FAIL 记录 note 后继续下一步；
//    · 未知步骤类型 → FAIL + note（与 Python 版同判据）；
//    · verdict = 所有步骤 pass 且非空；
//    · 报告写盘失败 → 降级写入 TEMP 目录并告警（方案 §5）。
//
//  并发口径：
//    引擎在专属串行 DispatchQueue 执行；Swift 5.9 严格并发（Sendable 严格检查）
//    非本工程强制要求；对外回调在引擎队列触发，由 UI 侧自行 DispatchQueue.main 转发。
//

import Foundation

// MARK: - 计划模型（字段与 tools/usbtest YAML 计划同构；本应用读 JSON，M4 双平台互通）

/// "0x1234"（字符串）或 1234（十进制整数）均可解析的 VID/PID 容错包装。
@propertyWrapper
struct HexOrInt: Codable {
    var wrappedValue: Int?

    init(wrappedValue: Int? = nil) { self.wrappedValue = wrappedValue }

    init(from decoder: Decoder) throws {
        let c = try decoder.singleValueContainer()
        if let i = try? c.decode(Int.self) {
            wrappedValue = i
        } else if let s = try? c.decode(String.self) {
            let t = s.trimmingCharacters(in: .whitespaces)
            if t.lowercased().hasPrefix("0x") {
                wrappedValue = Int(t.dropFirst(2), radix: 16)
            } else {
                wrappedValue = Int(t)
            }
        } else {
            wrappedValue = nil
        }
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.singleValueContainer()
        if let v = wrappedValue { try c.encode(v) } else { try c.encodeNil() }
    }
}

struct TestPlan: Codable {
    var name: String
    var station: String?          // 计划内可覆盖工位号（引擎默认 STN-01）
    var device: DeviceSpec
    var steps: [PlanStep]
}

struct DeviceSpec: Codable {
    @HexOrInt var vid: Int?
    @HexOrInt var pid: Int?
    var backend: String?          // hid / cdc / pd / …（引擎按步骤类型分发，仅参考）
    var namePrefix: String?       // name_prefix
    var port: String?             // macOS: "/dev/tty.usbmodemXXX"；缺省自动选择
    var baud: Int?
    var telemetryPort: String?    // telemetry_port
    var serialPrefix: String?     // serial_prefix

    enum CodingKeys: String, CodingKey {
        case vid, pid, backend, port, baud
        case namePrefix = "name_prefix"
        case telemetryPort = "telemetry_port"
        case serialPrefix = "serial_prefix"
    }
}

struct PlanStep: Codable {
    var type: String
    var name: String?
    var seconds: Double?          // hid_polling_rate 采样时长
    var timeoutMs: Int?           // timeout_ms
    var repeatCount: Int?         // repeat（serial_loopback 轮数）
    var limits: Limits?           // 阈值（min_hz / max_interval_ms）
    var report: [Int]?            // hid_output_write 载荷
    var pattern: [Int]?           // hid_report_loopback 模式
    var expectV: Double?          // expect_v
    var tolV: Double?             // tol_v
    var command: String?          // pd_negotiate 指令（如 "req 9"）
    var image: String?            // dfu_verify（macOS 未实现，保留字段）
    var alt: Int?

    struct Limits: Codable {
        var minHz: Double?          // min_hz
        var maxIntervalMs: Double?  // max_interval_ms（抖动上限）

        enum CodingKeys: String, CodingKey {
            case minHz = "min_hz"
            case maxIntervalMs = "max_interval_ms"
        }
    }

    enum CodingKeys: String, CodingKey {
        case type, name, seconds, limits, report, pattern, command, image, alt
        case timeoutMs = "timeout_ms"
        case repeatCount = "repeat"
        case expectV = "expect_v"
        case tolV = "tol_v"
    }

    var displayName: String { name?.isEmpty == false ? name! : type }
}

// MARK: - 引擎错误

enum TestStepError: Error, LocalizedError {
    case dutNotFound
    case serialPortNotFound
    case noTelemetry

    var errorDescription: String? {
        switch self {
        case .dutNotFound: return "未发现匹配计划的 DUT（VID/PID/名称前缀）"
        case .serialPortNotFound: return "未找到可用串口（/dev/tty.usbmodem*）"
        case .noTelemetry: return "遥测窗口内未解析到任何样本"
        }
    }
}

// MARK: - 委托（引擎队列触发，UI 自行转主线程）

protocol TestEngineDelegate: AnyObject {
    func engineDidLog(_ engine: TestEngine, message: String)
    func engineDidUpdateProgress(_ engine: TestEngine, completed: Int, total: Int, currentStep: String)
    func engineDidFinish(_ engine: TestEngine, report: TestReport, jsonPath: String?)
}

// MARK: - 引擎

final class TestEngine {

    weak var delegate: TestEngineDelegate?

    var station = "STN-01"
    var reportDirectory = ReportWriter.defaultReportDirectory()
    private(set) var isRunning = false
    private var cancelRequested = false
    private var dutSn = "AUTO"

    private let queue = DispatchQueue(label: "usblabs.usbteststudio.engine", qos: .userInitiated)

    // MARK: 计划加载

    /// 解析 JSON 计划（字段名与 tools/usbtest YAML 计划一致）。
    func loadPlan(from url: URL) throws -> TestPlan {
        let data = try Data(contentsOf: url)
        return try JSONDecoder().decode(TestPlan.self, from: data)
    }

    // MARK: 运行控制

    func run(plan: TestPlan) {
        guard !isRunning else { return }
        isRunning = true
        cancelRequested = false
        if let s = plan.station, !s.isEmpty { station = s }
        queue.async { [weak self] in
            guard let self else { return }
            defer { self.isRunning = false }
            self.executePlan(plan)
        }
    }

    /// 协作式取消：当前步骤执行完后，剩余步骤记 FAIL(note=取消)。
    func cancel() {
        guard isRunning else { return }
        cancelRequested = true
        emitLog("已请求停止，等待当前步骤结束…")
    }

    // MARK: 主流程（引擎队列）

    private func executePlan(_ plan: TestPlan) {
        let startedDate = Date()
        emitLog("========== 计划开始: \(plan.name)（工位 \(station)） ==========")

        // 1. 枚举
        emitLog("IOKit 扫描 USB 总线…")
        let devices = DeviceScanner.scan()
        emitLog("共 \(devices.count) 台 USB 设备")
        for d in devices {
            let sn = d.serialNumber.isEmpty ? "-" : d.serialNumber
            emitLog("  · \(d.name) [\(d.vidPidText)] \(d.speedLabel) sn=\(sn)")
        }

        // 2. 选定 DUT
        let dut = devices.first {
            DeviceScanner.matches($0, vid: plan.device.vid, pid: plan.device.pid,
                                  namePrefix: plan.device.namePrefix)
        }
        if let d = dut {
            if dutSn == "AUTO" { dutSn = d.serialNumber.isEmpty ? "AUTO" : d.serialNumber }
            emitLog("DUT: \(d.name) [\(d.vidPidText)] sn=\(dutSn) speed=\(d.speedLabel)")
        } else {
            emitLog("警告: 未发现符合计划的 DUT，相关步骤将 FAIL（不中断整站）")
        }

        // 3. 逐步执行（步骤级失败不中断整站）
        var results: [StepResult] = []
        let total = plan.steps.count
        for (idx, step) in plan.steps.enumerated() {
            delegate?.engineDidUpdateProgress(self, completed: idx, total: total,
                                              currentStep: step.displayName)
            if cancelRequested {
                results.append(StepResult(name: step.displayName, pass: false,
                                          measured: [:], note: "用户取消，未执行"))
                continue
            }
            emitLog("── 步骤 \(idx + 1)/\(total) [\(step.type)] \(step.displayName)")
            let outcome: StepOutcome
            do {
                outcome = try dispatch(step: step, plan: plan, devices: devices, dut: dut)
            } catch {
                // 设备拔出/驱动异常等：单步 FAIL 继续（与 Python 版口径一致）
                outcome = StepOutcome(pass: false, measured: [:],
                                      note: "异常: \(error.localizedDescription)")
            }
            let m = outcome.measured.map { "\($0.key)=\($0.value.text)" }
                .sorted().joined(separator: " ")
            emitLog("\(outcome.pass ? "PASS" : "FAIL") | \(step.displayName) | \(m) \(outcome.note)".trimmingCharacters(in: .whitespaces))
            results.append(StepResult(name: step.displayName, pass: outcome.pass,
                                      measured: outcome.measured, note: outcome.note))
        }
        delegate?.engineDidUpdateProgress(self, completed: total, total: total, currentStep: "完成")

        // 4. 判定 + 落盘
        let verdict = (!results.isEmpty && results.allSatisfy(\.pass)) ? "PASS" : "FAIL"
        let report = TestReport(plan: plan.name, station: station, dutSn: dutSn,
                                verdict: verdict,
                                started: ReportWriter.isoSeconds(startedDate),
                                steps: results)

        var jsonPath: String?
        do {
            jsonPath = try ReportWriter.write(report, toDirectory: reportDirectory).path
            emitLog("报告已写入: \(jsonPath!)")
        } catch {
            // 降级 TEMP（方案 §5：报告写盘失败 → 降级输出并告警）
            do {
                let fallback = NSTemporaryDirectory() + "USBTestStudio"
                jsonPath = try ReportWriter.write(report, toDirectory: fallback).path
                emitLog("警告: 主目录写盘失败(\(error.localizedDescription))，报告降级写入: \(jsonPath!)")
            } catch {
                emitLog("警告: 报告写盘彻底失败: \(error.localizedDescription)")
            }
        }

        emitLog("========== 判定: \(verdict) ==========")
        delegate?.engineDidFinish(self, report: report, jsonPath: jsonPath)
    }

    // MARK: 步骤分发

    private struct StepOutcome {
        var pass: Bool
        var measured: [String: JSONValue]
        var note: String
    }

    private func dispatch(step: PlanStep, plan: TestPlan,
                          devices: [USBDeviceInfo], dut: USBDeviceInfo?) throws -> StepOutcome {
        switch step.type {
        case "enumerate":          return runEnumerate(step, plan: plan, devices: devices, dut: dut)
        case "descriptor_check":   return runDescriptorCheck(step, plan: plan, dut: dut)
        case "hid_polling_rate":   return runHIDPollingRate(step, plan: plan)
        case "hid_output_write":   return runHIDOutputWrite(step, plan: plan)
        case "hid_report_loopback":return runHIDReportLoopback(step, plan: plan)
        case "serial_loopback":    return runSerialLoopback(step, plan: plan)
        case "line_coding":        return runLineCoding(step, plan: plan)
        case "pd_attach",
             "pd_negotiate",
             "measure_voltage":    return runPDTelemetry(step, plan: plan)
        case "msc_read_verify":
            return StepOutcome(pass: false, measured: [:],
                               note: "macOS 无 MSC SCSI 直通，未实现（概要占位，请用 Windows 版执行）")
        case "dfu_verify":
            return StepOutcome(pass: false, measured: [:],
                               note: "macOS 版未实现 DFU 校验（概要占位，请用 Windows 版执行）")
        default:
            return StepOutcome(pass: false, measured: [:], note: "未知步骤类型 \(step.type)")
        }
    }

    // MARK: 各步骤实现

    /// enumerate：扫描到 ≥1 台，且计划给出 vid/pid/name_prefix 时必须命中。
    private func runEnumerate(_ step: PlanStep, plan: TestPlan,
                              devices: [USBDeviceInfo], dut: USBDeviceInfo?) -> StepOutcome {
        var m: [String: JSONValue] = ["devices": .i(devices.count)]
        let hasFilter = plan.device.vid != nil || plan.device.pid != nil
                      || (plan.device.namePrefix?.isEmpty == false)
        if hasFilter {
            m["matched"] = .i(dut == nil ? 0 : 1)
            guard let d = dut else {
                return StepOutcome(pass: false, measured: m,
                                   note: "未发现匹配 VID/PID/名称前缀的设备")
            }
            m["vid"] = .i(Int(d.vid))
            m["pid"] = .i(Int(d.pid))
            return StepOutcome(pass: true, measured: m, note: d.name)
        }
        guard !devices.isEmpty else {
            return StepOutcome(pass: false, measured: m, note: "总线无 USB 设备")
        }
        return StepOutcome(pass: true, measured: m, note: devices.first?.name ?? "")
    }

    /// descriptor_check：用 IORegistry 镜像的设备描述符字段与计划 device 白名单比对。
    /// （IORegistryEntryCreateCFProperties 已含 bcdUSB/bcdDevice/bDeviceClass 等，
    ///  与描述符一致；全量描述符逐字段校验需控制传输直访，macOS 侧为概要实现。）
    private func runDescriptorCheck(_ step: PlanStep, plan: TestPlan, dut: USBDeviceInfo?) -> StepOutcome {
        var m: [String: JSONValue] = [:]
        guard let d = dut else {
            return StepOutcome(pass: false, measured: m, note: TestStepError.dutNotFound.localizedDescription)
        }
        m["bcd_usb"] = .s(d.bcdUSB)
        m["bcd_device"] = .s(d.bcdDevice)
        m["class"] = .i(Int(d.deviceClass))
        m["subclass"] = .i(Int(d.deviceSubClass))
        m["protocol"] = .i(Int(d.deviceProtocol))
        m["max_packet_0"] = .i(d.maxPacketSize0)
        m["speed"] = .s(d.speedLabel)

        var notes: [String] = []
        if let v = plan.device.vid, Int(d.vid) != v { notes.append("vid 不符(期望 \(v) 实际 \(d.vid))") }
        if let p = plan.device.pid, Int(d.pid) != p { notes.append("pid 不符(期望 \(p) 实际 \(d.pid))") }
        if let prefix = plan.device.namePrefix, !prefix.isEmpty, !d.name.hasPrefix(prefix) {
            notes.append("名称前缀不符(期望 \(prefix) 实际 \(d.name))")
        }
        let ok = notes.isEmpty
        return StepOutcome(pass: ok, measured: m, note: ok ? "描述符与白名单一致" : notes.joined(separator: "; "))
    }

    /// hid_polling_rate：DispatchTime 间隔直方图，主判定 medianHz ≥ limits.min_hz。
    private func runHIDPollingRate(_ step: PlanStep, plan: TestPlan) -> StepOutcome {
        let sampleMs = Int((step.seconds ?? 2.0) * 1000)
        let tester = HIDTester()
        let stats = tester.measurePollingRate(vid: plan.device.vid, pid: plan.device.pid,
                                              usagePage: nil, usage: nil,
                                              sampleMs: sampleMs, log: emitLog)
        var m: [String: JSONValue] = [
            "hz": .d(stats.medianHz, digits: 1),
            "hz_eff": .d(stats.effectiveHz, digits: 1),
            "min_ms": .d(stats.minMs),
            "p95_ms": .d(stats.p95Ms),
            "max_ms": .d(stats.maxMs),
            "samples": .i(stats.sampleCount)
        ]
        for bucket in stats.histogram.prefix(8) {
            m["hist[\(bucket.label)]"] = .i(bucket.count)
        }

        guard stats.sampleCount >= 2 else {
            return StepOutcome(pass: false, measured: m,
                               note: "无足够输入报告：固件需持续上报（移动轴/使能流），或 VID/PID 不匹配")
        }
        let minHz = step.limits?.minHz ?? 0
        var ok = stats.medianHz >= minHz
        var note = "中位 \(String(format: "%.1f", stats.medianHz))Hz ≥ 下限 \(String(format: "%.0f", minHz))Hz"
        if let maxInterval = step.limits?.maxIntervalMs {
            let within = stats.maxMs <= maxInterval
            ok = ok && within
            note += within ? "" : "; 最大间隔 \(String(format: "%.2f", stats.maxMs))ms 超限 \(maxInterval)ms"
        }
        return StepOutcome(pass: ok, measured: m, note: note)
    }

    /// hid_output_write：setReport 发送计划载荷，写成功即过（点亮类确认项由产线目检）。
    private func runHIDOutputWrite(_ step: PlanStep, plan: TestPlan) -> StepOutcome {
        let payload = (step.report ?? [0x21, 0x00]).map { UInt8(clamping: $0) }
        let tester = HIDTester()
        let (ok, kr) = tester.sendOutputReport(payload: payload, vid: plan.device.vid,
                                               pid: plan.device.pid, log: emitLog)
        return StepOutcome(pass: ok,
                           measured: ["written": .i(ok ? 1 : 0), "kr": .s(String(format: "0x%08x", kr))],
                           note: ok ? "输出报告已发送" : "setReport 失败")
    }

    /// hid_report_loopback：setReport 发模式，等输入报告回显（需工装固件配合）。
    private func runHIDReportLoopback(_ step: PlanStep, plan: TestPlan) -> StepOutcome {
        let pattern = (step.pattern ?? [0x55, 0xAA]).map { UInt8(clamping: $0) }
        let tester = HIDTester()
        let r = tester.reportLoopback(pattern: pattern, vid: plan.device.vid, pid: plan.device.pid,
                                      timeoutMs: step.timeoutMs ?? 500, log: emitLog)
        return StepOutcome(pass: r.ok,
                           measured: ["pattern": .s(hexPreview(pattern))],
                           note: r.detail)
    }

    /// serial_loopback：TX-RX 工装环回，字节级比对，默认 4 轮。
    private func runSerialLoopback(_ step: PlanStep, plan: TestPlan) throws -> StepOutcome {
        let path = try resolveSerialPort(explicit: plan.device.port)
        emitLog("串口: \(path) baud=\(plan.device.baud ?? 115200)")
        let session = try SerialSession(path: path, baud: plan.device.baud ?? 115200)
        defer { session.close() }

        let result = session.loopback(rounds: step.repeatCount ?? 4,
                                      payloadTemplate: "USB-Labs PING",
                                      timeoutMs: step.timeoutMs ?? 500,
                                      log: emitLog)
        return StepOutcome(pass: result.pass, measured: [
            "rounds": .i(result.rounds),
            "passed": .i(result.roundsPassed),
            "tx": .i(result.bytesTx),
            "rx": .i(result.bytesRx),
            "ms": .d(result.elapsedMs, digits: 1)
        ], note: result.pass ? "字节级一致" : result.firstError)
    }

    /// line_coding（概要）：macOS 无 CDC SET_LINE_CODING 直通，只回读本地 termios 速率。
    private func runLineCoding(_ step: PlanStep, plan: TestPlan) throws -> StepOutcome {
        let path = try resolveSerialPort(explicit: plan.device.port)
        let session = try SerialSession(path: path, baud: plan.device.baud ?? 115200)
        defer { session.close() }
        let current = session.currentBaud
        let expect = plan.device.baud ?? 115200
        return StepOutcome(pass: current == expect, measured: ["baud": .i(current)],
                           note: "macOS 概要实现：仅校验本机 termios 速率，未触达设备线路编码寄存器")
    }

    /// pd_attach / pd_negotiate / measure_voltage：经 CDC 串口读固件遥测（概要实现）。
    private func runPDTelemetry(_ step: PlanStep, plan: TestPlan) -> StepOutcome {
        let path: String
        do {
            path = try resolveSerialPort(explicit: plan.device.telemetryPort ?? plan.device.port)
        } catch {
            return StepOutcome(pass: false, measured: [:], note: error.localizedDescription)
        }
        emitLog("遥测串口: \(path)")
        guard let session = try? SerialSession(path: path, baud: plan.device.baud ?? 115200) else {
            return StepOutcome(pass: false, measured: [:], note: "遥测串口打开失败: \(path)")
        }
        defer { session.close() }

        // negotiate 可携带指令（如 "req 9"）；attach/measure 只读
        var command: String?
        if step.type == "pd_negotiate" {
            command = step.command ?? "req \(Int(step.expectV ?? 5))"
        }
        let samples = session.collectTelemetry(command: command,
                                               durationMs: Int((step.seconds ?? 1.0) * 1000) + 700,
                                               log: emitLog)

        var m: [String: JSONValue] = ["samples": .i(samples.count)]
        guard let last = samples.last else {
            return StepOutcome(pass: false, measured: m,
                               note: TestStepError.noTelemetry.localizedDescription
                                   + "（解析规则以固件协议为准，见 README）")
        }
        m["vbus_v"] = .d(last.voltageV)
        if let i = last.currentA { m["i_a"] = .d(i) }

        switch step.type {
        case "pd_attach":
            // CC attach 判据：窗口内出现有效 VBUS/遥测输出
            return StepOutcome(pass: last.voltageV > 0.2, measured: m,
                               note: "概要实现：以遥测电压 >0.2V 判定 attach")
        default:
            let expect = step.expectV ?? 5.0
            let tol = step.tolV ?? 0.5
            let ok = abs(last.voltageV - expect) <= tol
            return StepOutcome(pass: ok, measured: m,
                               note: String(format: "期望 %.2fV ± %.2fV，实测 %.3fV", expect, tol, last.voltageV)
                                   + "（概要实现，解析以固件协议为准）")
        }
    }

    // MARK: 工具

    /// 计划端口解析："/dev/..." 直用；"tty.usbmodemXXX" 补前缀；空/缺省自动选第一个。
    private func resolveSerialPort(explicit: String?) throws -> String {
        if let p = explicit, !p.isEmpty {
            return p.hasPrefix("/dev/") ? p : "/dev/" + p
        }
        let ports = SerialPortScanner.listPorts()
        guard let first = ports.first else { throw TestStepError.serialPortNotFound }
        emitLog("自动选择串口 \(first.path)（候选 \(ports.count) 个）")
        return first.path
    }

    private func emitLog(_ message: String) {
        delegate?.engineDidLog(self, message: message)
    }
}
