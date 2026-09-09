//
//  Log.swift
//  USBTestStudio — 文件日志（UTSLog · T7 首切片）
//
//  ⚠ 本切片未编译验证（需 macOS），按 Swift 5.9 / AppKit 口径编写；
//    编译若报签名不匹配，按注释备选写法调整（工程惯例见 README §10）。
//
//  规范对齐（apps/win/USBTestStudio/src/app/log.h，Windows spdlog 门面）：
//    · 行格式：[yyyy-MM-dd HH:MM:SS.mmm][LEVEL][component] message（UTF-8）。
//      Windows 版模式串为 [%Y-%m-%d %H:%M:%S.%e][%l][%n][T%t] %v——模块段后还有线程段，
//      本切片按 T7 首切片口径省略线程段。
//    · 级别语义（与 C++ 注释逐条对齐）：trace=逐包/逐字节 · debug=逐操作（参数+结果+耗时）·
//      info=生命周期（启动/打开/关闭/扫描）· warn=可恢复异常（重试/回退/超时留痕）·
//      err=失败（必带错误码十六进制）。
//    · sink：追加写 ~/Library/Logs/USBTestStudio/<component>.log；简单大小检查，
//      超 5MB 轮转 <component>.log.1（保留一代；Windows 版为 5MB×3）。
//      另镜像写 stderr（对应 Windows OutputDebugString 靶：`swift run` 终端 / Console.app 可见）。
//    · 级别开关：环境变量 USBTS_LOG_LEVEL=trace|debug|info|warn|err（兼容 warning/error 别名，
//      与 C++ parse_level 同口径），缺省 info；warn 及以上即 fsync（对应 C++ flush_on(warn)，
//      崩溃前最后几条不丢）。
//    · 线程安全：全部写路径收敛到串行 DispatchQueue（对应 C++ sink 内部互斥）；
//      setup 幂等（首次调用生效，等价 std::call_once）。
//    · 日志不可拖死业务：任何落盘失败静默降级为仅 stderr，绝不向调用方抛错。
//

import Foundation

// MARK: - 级别

/// 日志级别（rawValue 序 = spdlog trace(0)…err(4)；过滤按数值比较）。
enum UTSLogLevel: Int, Comparable {

    case trace = 0, debug = 1, info = 2, warn = 3, err = 4

    static func < (lhs: UTSLogLevel, rhs: UTSLogLevel) -> Bool { lhs.rawValue < rhs.rawValue }

    /// 行内级别字面量（小写，与 spdlog %l 渲染口径一致）。
    var label: String {
        switch self {
        case .trace: return "trace"
        case .debug: return "debug"
        case .info:  return "info"
        case .warn:  return "warn"
        case .err:   return "err"
        }
    }

    /// 环境变量解析（与 C++ parse_level 同口径：兼容 warning/error 别名）；
    /// 未设置/无法识别返回 nil，由调用方落默认 info。
    static func parse(_ raw: String?) -> UTSLogLevel? {
        switch raw?.trimmingCharacters(in: .whitespaces).lowercased() {
        case "trace":           return .trace
        case "debug":           return .debug
        case "info":            return .info
        case "warn", "warning": return .warn
        case "err", "error":    return .err
        default:                return nil
        }
    }
}

// MARK: - 日志门面

enum UTSLog {

    /// 轮转阈值 5MB（与 Windows rotating_file_sink 5MB 同值；本切片仅保留 .1 一代）。
    static let maxFileBytes: UInt64 = 5 * 1024 * 1024

    private static let queue = DispatchQueue(label: "usblabs.usbteststudio.log")

    /// 时间戳格式化器：仅在 queue 内使用（DateFormatter 非线程安全，以队列收玫）。
    private static let timestampFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd HH:mm:ss.SSS"
        f.locale = Locale(identifier: "en_US_POSIX")
        return f
    }()

    private static var component = "app"        // 默认 [component] 段与落盘文件名
    private static var level: UTSLogLevel = .info
    private static var fileURL: URL?
    private static var handle: FileHandle?
    private static var currentBytes: UInt64 = 0
    private static var didSetup = false
    // 注：component/level 仅在 setup（main.swift，app.run 之前）写入一次，此后多线程只读，
    // 与工程「队列纪律」口径一致（未启用 Sendable 严格检查，见 README §2）。

    // MARK: 初始化

    /// 进程级初始化（幂等，首次生效）。`component` 决定落盘文件名与默认 [component] 段
    /// （对应 C++ 根 logger "app"；各模块可用 log(_:component:) 另标模块名，共用同一文件）。
    static func setup(component: String = "app") {
        queue.sync {
            guard !didSetup else { return }
            didSetup = true
            self.component = component.isEmpty ? "app" : component
            level = UTSLogLevel.parse(ProcessInfo.processInfo.environment["USBTS_LOG_LEVEL"]) ?? .info

            // ~/Library/Logs/USBTestStudio/（对应 Windows %LOCALAPPDATA%\USBTestStudio\logs）
            let dir = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask).first?
                .appendingPathComponent("Logs", isDirectory: true)
                .appendingPathComponent("USBTestStudio", isDirectory: true)
            guard let dir else { return }   // 目录解析失败：降级仅 stderr
            try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            openFileLocked(at: dir.appendingPathComponent("\(self.component).log"))
        }
    }

    // MARK: 级别入口（调用方任意线程；入队前过滤，低级别零格式化开销）

    /// trace：逐包/逐字节（如输入报告逐条转储）。
    static func trace(_ message: String, component name: String? = nil) { write(.trace, message, component: name) }
    /// debug：逐操作（参数+结果+耗时）。
    static func debug(_ message: String, component name: String? = nil) { write(.debug, message, component: name) }
    /// info：生命周期（启动/打开/关闭/扫描）。
    static func info(_ message: String, component name: String? = nil) { write(.info, message, component: name) }
    /// warn：可恢复异常（重试/回退/超时留痕），写后立即 fsync。
    static func warn(_ message: String, component name: String? = nil) { write(.warn, message, component: name) }
    /// err：失败（必带错误码十六进制），写后立即 fsync。
    static func err(_ message: String, component name: String? = nil) { write(.err, message, component: name) }

    // MARK: 写入

    private static func write(_ lvl: UTSLogLevel, _ message: String, component name: String?) {
        guard lvl >= level else { return }
        queue.async {
            let line = "[\(timestampFormatter.string(from: Date()))][\(lvl.label)][\(name ?? component)] \(message)\n"
            let data = Data(line.utf8)

            // stderr 靶（对应 Windows OutputDebugString：终端 `swift run` / Console.app 可见）
            FileHandle.standardError.write(data)

            if handle == nil, let url = fileURL { openFileLocked(at: url) }   // 自愈：逐条重试打开
            guard let h = handle else { return }    // 文件不可写（权限/占用）：已降级仅 stderr

            if currentBytes + UInt64(data.count) > maxFileBytes { rotateLocked() }
            guard let h2 = handle else { return }
            h2.write(data)                          // 追加写（打开即 seekToEnd）
            currentBytes += UInt64(data.count)
            if lvl >= .warn { h2.synchronizeFile() }    // 对齐 C++ flush_on(warn)
        }
    }

    // MARK: 文件管理（仅日志串行队列内调用，_Locked 后缀表意）

    private static func openFileLocked(at url: URL) {
        fileURL = url
        if !FileManager.default.fileExists(atPath: url.path) {
            FileManager.default.createFile(atPath: url.path, contents: nil)
        }
        guard let h = try? FileHandle(forWritingTo: url) else { return }   // 打不开：降级仅 stderr
        h.seekToEndOfFile()     // 追加写（不截断历史）
        handle = h
        currentBytes = UInt64(((try? url.resourceValues(forKeys: [.fileSizeKey]))?.fileSize) ?? 0)
    }

    /// app.log → app.log.1（覆盖上一代），随后重开空文件继续追加。
    private static func rotateLocked() {
        guard let url = fileURL else { return }
        handle?.closeFile()
        handle = nil
        let rotated = URL(fileURLWithPath: url.path + ".1")
        try? FileManager.default.removeItem(at: rotated)
        try? FileManager.default.moveItem(at: url, to: rotated)
        openFileLocked(at: url)
    }
}
