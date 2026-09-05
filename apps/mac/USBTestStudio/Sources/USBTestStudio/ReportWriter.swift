//
//  ReportWriter.swift
//  USBTestStudio — JSON 报告（与 tools/usbtest 同构字段，可直接进 MES）
//
//  同构口径（tools/usbtest/core.py TestReport.save_json）：
//    {
//      "plan":     "<计划名>",
//      "station":  "STN-01",
//      "dut_sn":   "<DUT 序列号 / AUTO>",
//      "verdict":  "PASS" | "FAIL",
//      "started":  "ISO8601 秒级",
//      "steps": [ { "name", "pass", "measured": {...}, "note" } ]
//    }
//  文件名：report_<plan>_<dut_sn>_<yyyyMMdd_HHmmss>.json（与 Python 版一致）。
//  measured 为弱类型字典：Python 版直接放 dict，Swift 侧用 JSONValue 承载。
//

import Foundation

// MARK: - 弱类型值（对应 Python measured dict 的任意标量）

enum JSONValue: Codable {
    case string(String)
    case int(Int)
    case double(Double)
    case bool(Bool)

    // 构造助手（避免 Double/Int 字面量重载歧义）
    static func s(_ v: String) -> JSONValue { .string(v) }
    static func i(_ v: Int) -> JSONValue { .int(v) }
    static func b(_ v: Bool) -> JSONValue { .bool(v) }
    /// 四舍五入到 digits 位小数，报告不携带浮点尾噪
    static func d(_ v: Double, digits: Int = 3) -> JSONValue {
        let f = pow(10.0, Double(digits))
        return .double((v * f).rounded() / f)
    }

    // 展示用（引擎日志行）
    var text: String {
        switch self {
        case .string(let s): return s
        case .int(let i): return String(i)
        case .double(let d): return String(format: "%.3g", d)
        case .bool(let b): return b ? "true" : "false"
        }
    }
}

extension JSONValue {
    init(from decoder: Decoder) throws {
        let c = try decoder.singleValueContainer()
        if let v = try? c.decode(Bool.self) { self = .bool(v) }
        else if let v = try? c.decode(Int.self) { self = .int(v) }
        else if let v = try? c.decode(Double.self) { self = .double(v) }
        else if let v = try? c.decode(String.self) { self = .string(v) }
        else {
            throw DecodingError.dataCorruptedError(in: c, debugDescription: "JSONValue: 不支持的标量类型")
        }
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.singleValueContainer()
        switch self {
        case .string(let v): try c.encode(v)
        case .int(let v): try c.encode(v)
        case .double(let v): try c.encode(v)
        case .bool(let v): try c.encode(v)
        }
    }
}

// MARK: - 报告模型（字段与 tools/usbtest 完全一致）

struct StepResult: Codable {
    var name: String
    var pass: Bool
    var measured: [String: JSONValue]
    var note: String
}

struct TestReport: Codable {
    var plan: String        // 计划名
    var station: String     // 工位号
    var dutSn: String       // dut_sn
    var verdict: String     // "PASS" / "FAIL"
    var started: String     // ISO8601 秒级
    var steps: [StepResult]

    enum CodingKeys: String, CodingKey {
        case plan, station, verdict, started, steps
        case dutSn = "dut_sn"
    }
}

// MARK: - 写盘

enum ReportWriter {

    static let toolVersion = "1.0.0"
    static let toolName = "USBTestStudio"

    /// 默认报告目录：~/Desktop/USBTestStudio/reports
    static func defaultReportDirectory() -> String {
        let desktop = FileManager.default.urls(for: .desktopDirectory, in: .userDomainMask).first
        return desktop?.appendingPathComponent("USBTestStudio/reports").path
            ?? (NSTemporaryDirectory() + "USBTestStudio/reports")
    }

    /// 与 Python 版一致的秒级 ISO8601（2026-09-06T12:34:56）
    static func isoSeconds(_ date: Date) -> String {
        let f = ISO8601DateFormatter()
        f.formatOptions = [.withInternetDateTime]
        return f.string(from: date)
    }

    /// 编码报告（prettyPrinted + sortedKeys；非 ASCII 原样 UTF-8 输出，
    /// 等价 Python json.dumps(..., ensure_ascii=False, indent=2)）。
    static func encode(_ report: TestReport) throws -> Data {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        return try encoder.encode(report)
    }

    /// 落盘并返回文件 URL。目录不存在自动创建。
    static func write(_ report: TestReport, toDirectory directory: String) throws -> URL {
        let dirURL = URL(fileURLWithPath: directory, isDirectory: true)
        try FileManager.default.createDirectory(at: dirURL, withIntermediateDirectories: true)

        let ts = filenameTimestamp()
        let fileName = "report_\(sanitizeFilename(report.plan))_\(sanitizeFilename(report.dutSn))_\(ts).json"
        let fileURL = dirURL.appendingPathComponent(fileName)

        let data = try encode(report)
        try data.write(to: fileURL, options: .atomic)
        return fileURL
    }

    /// 文件名安全化（Windows 同名计划可互换，M4 里程碑口径）
    static func sanitizeFilename(_ raw: String) -> String {
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "-_."))
        let cleaned = raw.components(separatedBy: allowed).joined(separator: "_")
        let trimmed = cleaned.trimmingCharacters(in: CharacterSet(charactersIn: "._"))
        return trimmed.isEmpty ? "plan" : String(trimmed.prefix(48))
    }

    private static func filenameTimestamp() -> String {
        let f = DateFormatter()
        f.dateFormat = "yyyyMMdd_HHmmss"
        f.locale = Locale(identifier: "en_US_POSIX")
        return f.string(from: Date())
    }
}
