//
//  HIDTester.swift
//  USBTestStudio — HID 回报率测量 / 输出报告 / 报告环回（设备访问层）
//
//  技术口径（与方案一致，勿改）：
//    IOHIDManagerCreate
//      → IOHIDManagerSetDeviceMatching（按 VID/PID/Usage 过滤）
//      → IOHIDManagerCopyDevices 取设备集
//      → 每台 IOHIDDeviceSetInputMatching（nil = 不过滤输入报告）
//      → IOHIDDeviceRegisterInputReportCallback + IOHIDDeviceOpen
//      → 回调内 DispatchTime.now() 打时间戳 → 间隔直方图 → 回报率判定
//    输出路径：IOHIDDeviceSetReport（kIOHIDReportTypeOutput）。
//
//  线程口径：
//    测量在调用方线程（TestEngine 后台串行队列）进行；
//    把 manager 挂到“当前线程”的 CFRunLoop 上，用 CFRunLoopRunInMode 手动泵事件，
//    回调带锁写共享缓冲，统计与泵同线程，避免跨线程数据竞争。
//
//  不确定 API（以 Xcode 实际头文件为准）：
//    · IOHIDDeviceRegisterInputReportCallback 的 Swift 导入签名（sender 参数是否
//      已桥接为 IOHIDDevice），若不匹配按本文件回调处注释调整；
//    · IOHIDManagerCopyDevices 返回 CFSet，桥接为 Set<IOHIDDevice> 的写法；
//    · kIOHIDReportTypeOutput 常量的导入形式（匿名 C 枚举常量）；
//    · IOHIDDeviceSetReport 的 reportID=0 语义：0 表示设备不含 Report ID，
//      缓冲区即报告本体（与固件描述符约定一致时才有效，联调时以固件为准）。
//

import Foundation
import IOKit
import IOKit.hid

// MARK: - 测量结果

/// 一次回报率测量的统计结果（直方图 + 分位数）。
struct HIDReportStats {
    var deviceName = ""
    var vendorID: Int = 0
    var productID: Int = 0
    var maxInputReportSize = 0
    var sampleCount = 0            // 收到的输入报告数
    var durationMs = 0.0           // 实际采样时长
    var minMs = 0.0                // 报告间隔最小值
    var medianMs = 0.0             // 中位间隔
    var p95Ms = 0.0                // 95 分位间隔
    var maxMs = 0.0                // 报告间隔最大值（抖动上限，用于 max_interval_ms 判定）
    var medianHz = 0.0             // 1000 / medianMs，判定主指标
    var effectiveHz = 0.0          // (sampleCount-1) / durationSec，吞吐口径
    var histogram: [(label: String, count: Int)] = []   // 间隔直方图（0.25ms 桶）
}

// MARK: - 测试器

final class HIDTester {

    private let lock = NSLock()
    private var intervalsMs: [Double] = []
    private var reportCount = 0
    private var firstReportAt: DispatchTime?
    private var lastReportAt: DispatchTime?
    private var recentReports: [[UInt8]] = []      // 最近 32 条输入报告（环回比对用）
    private var maxSeenReportLength = 0

    // MARK: 回报率测量

    /// 打开匹配的 HID 设备并测量输入报告回报率。
    /// - Parameters:
    ///   - sampleMs: 采样时长（毫秒），来自计划步骤的 seconds 字段
    ///   - log: 进度日志回调（引擎队列线程）
    /// - Note: 设备固件必须持续上报（如鼠标持续移动 / sensor 流使能），否则 sampleCount=0。
    func measurePollingRate(vid: Int?, pid: Int?, usagePage: Int?, usage: Int?,
                            sampleMs: Int,
                            log: (String) -> Void) -> HIDReportStats {
        resetAccumulators()
        var stats = HIDReportStats()

        openMatchingDevices(vid: vid, pid: pid, usagePage: usagePage, usage: usage, log: log) {
            manager, devices in

            // 记录 caps（来自 HID 报告描述符解析结果）
            if let first = devices.first {
                stats.deviceName = self.deviceString(first, kIOHIDProductKey) ?? "?"
                stats.vendorID = self.deviceInt(first, kIOHIDVendorIDKey) ?? 0
                stats.productID = self.deviceInt(first, kIOHIDProductIDKey) ?? 0
                stats.maxInputReportSize = self.deviceInt(first, kIOHIDMaxInputReportSizeKey) ?? 0
            }

            let start = DispatchTime.now()
            let budgetNs: UInt64 = UInt64(max(200, sampleMs)) * 1_000_000

            // 手动泵当前线程 RunLoop，直到采样时长耗尽
            while DispatchTime.now().uptimeNanoseconds &- start.uptimeNanoseconds < budgetNs {
                CFRunLoopRunInMode(CFRunLoopMode.defaultMode, 0.02, false)
            }

            let elapsedMs = Double(DispatchTime.now().uptimeNanoseconds &- start.uptimeNanoseconds) / 1e6
            stats.durationMs = elapsedMs
        }

        // 统计（与采样同线程）
        lock.lock()
        let intervals = intervalsMs
        let count = reportCount
        lock.unlock()

        stats.sampleCount = count
        if !intervals.isEmpty {
            let sorted = intervals.sorted()
            stats.minMs = sorted.first ?? 0
            stats.maxMs = sorted.last ?? 0
            stats.medianMs = percentile(sorted, 0.5)
            stats.p95Ms = percentile(sorted, 0.95)
            stats.medianHz = stats.medianMs > 0 ? 1000.0 / stats.medianMs : 0
            stats.effectiveHz = elapsedSeconds(stats.durationMs) > 0
                ? Double(max(0, count - 1)) / elapsedSeconds(stats.durationMs)
                : 0
            stats.histogram = Self.histogram(intervals)
        }
        return stats
    }

    // MARK: 输出报告（对应计划步骤 hid_output_write）

    /// 向第一台匹配设备发送 Output Report。
    /// - Returns: (是否成功, IOReturn 码)
    @discardableResult
    func sendOutputReport(payload: [UInt8], vid: Int?, pid: Int?,
                          log: (String) -> Void) -> (ok: Bool, kr: IOReturn) {
        var sentKR: IOReturn = kIOReturnError
        openMatchingDevices(vid: vid, pid: pid, usagePage: nil, usage: nil, log: log) { _, devices in
            guard let dev = devices.first else { return }
            var buffer = payload
            // reportID = 0：设备无 Report ID 时的默认报告；带 ID 的设备需固件/计划约定
            let kr = buffer.withUnsafeMutableBufferPointer { ptr -> IOReturn in
                guard let base = ptr.baseAddress else { return kIOReturnError }
                return IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, 0, base, CFIndex(ptr.count))
            }
            sentKR = kr
            log(String(format: "setReport(%d 字节) → kr=0x%08x%@", buffer.count, kr,
                       kr == kIOReturnSuccess ? " (成功)" : " (失败)"))
        }
        return (sentKR == kIOReturnSuccess, sentKR)
    }

    // MARK: 报告环回（对应计划步骤 hid_report_loopback，需工装固件回显）

    /// 发送 pattern，并在 timeoutMs 内等待输入报告中回显同一 pattern。
    /// 比对规则：输入报告后缀或包含 pattern 即命中（容忍 Report ID 前缀占位）。
    func reportLoopback(pattern: [UInt8], vid: Int?, pid: Int?, timeoutMs: Int,
                        log: (String) -> Void) -> (ok: Bool, detail: String) {
        var result = (ok: false, detail: "未执行")
        openMatchingDevices(vid: vid, pid: pid, usagePage: nil, usage: nil, log: log) {
            manager, devices in
            guard let dev = devices.first else {
                result.detail = "无匹配 HID 设备"
                return
            }
            // 先清空历史报告，避免旧数据误判
            self.lock.lock(); self.recentReports.removeAll(); self.lock.unlock()

            var buffer = pattern
            let sendKR = buffer.withUnsafeMutableBufferPointer { ptr -> IOReturn in
                guard let base = ptr.baseAddress else { return kIOReturnError }
                return IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, 0, base, CFIndex(ptr.count))
            }
            guard sendKR == kIOReturnSuccess else {
                result.detail = String(format: "setReport 失败 kr=0x%08x", sendKR)
                return
            }

            let start = DispatchTime.now()
            let budgetNs: UInt64 = UInt64(max(50, timeoutMs)) * 1_000_000
            while DispatchTime.now().uptimeNanoseconds &- start.uptimeNanoseconds < budgetNs {
                if let echo = self.takeReport(containing: pattern) {
                    result = (true, "回显 \(echo.count) 字节，模式命中")
                    log("环回命中: " + hexPreview(echo))
                    return
                }
                CFRunLoopRunInMode(CFRunLoopMode.defaultMode, 0.01, false)
            }
            result.detail = "超时(\(timeoutMs)ms)未收到含模式 [\(pattern.map { String($0, radix: 16) }.joined(separator: " "))] 的输入报告"
        }
        return result
    }

    // MARK: 设备打开/关闭（内部通用）

    /// 打开匹配的 HID 设备集合，注册输入回调，body 执行完后统一清理。
    private func openMatchingDevices(vid: Int?, pid: Int?, usagePage: Int?, usage: Int?,
                                     log: (String) -> Void,
                                     _ body: (IOHIDManager, Set<IOHIDDevice>) -> Void) {
        var match: [String: Any] = [:]
        if let vid { match[kIOHIDVendorIDKey] = vid }
        if let pid { match[kIOHIDProductIDKey] = pid }
        if let usagePage { match[kIOHIDDeviceUsagePageKey] = usagePage }
        if let usage { match[kIOHIDDeviceUsageKey] = usage }

        guard let manager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone)) else {
            log("IOHIDManagerCreate 失败")
            return
        }
        // 空匹配字典 = 枚举全部 HID 设备
        IOHIDManagerSetDeviceMatching(manager, match.isEmpty ? nil : match as CFDictionary)

        // 挂当前线程 RunLoop（TestEngine 后台队列线程），由本类手动泵
        IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetCurrent(), CFRunLoopMode.defaultMode.rawValue)
        defer {
            IOHIDManagerUnscheduleFromRunLoop(manager, CFRunLoopGetCurrent(), CFRunLoopMode.defaultMode.rawValue)
            IOHIDManagerClose(manager, IOOptionBits(kIOHIDOptionsTypeNone))
        }

        let openKR = IOHIDManagerOpen(manager, IOOptionBits(kIOHIDOptionsTypeNone))
        guard openKR == kIOReturnSuccess else {
            log(String(format: "IOHIDManagerOpen 失败 kr=0x%08x", openKR))
            return
        }
        guard let devices = IOHIDManagerCopyDevices(manager) as? Set<IOHIDDevice>, !devices.isEmpty else {
            log("未匹配到 HID 设备（检查 VID/PID 或设备是否为 HID 类）")
            return
        }

        let context = Unmanaged.passUnretained(self).toOpaque()
        for device in devices {
            let name = deviceString(device, kIOHIDProductKey) ?? "?"
            let usageInfo = "\(deviceInt(device, kIOHIDUsagePageKey) ?? -1):\(deviceInt(device, kIOHIDPrimaryUsageKey) ?? -1)"
            log("HID 设备: \(name) VID=\(deviceInt(device, kIOHIDVendorIDKey) ?? 0) " +
                "PID=\(deviceInt(device, kIOHIDProductIDKey) ?? 0) " +
                "usagePage:usage=\(usageInfo) maxInput=\(deviceInt(device, kIOHIDMaxInputReportSizeKey) ?? 0)")

            // 逐设备不过滤输入报告（nil = 全部送回调）
            IOHIDDeviceSetInputMatching(device, nil)
            IOHIDDeviceRegisterInputReportCallback(device, { ctx, result, sender, type, reportID, report, length in
                // Swift 导入形参顺序以 SDK 为准：
                // (context, result, sender, type, reportID, report, reportLength)
                guard let ctx = ctx, result == kIOReturnSuccess, length > 0,
                      let report = report else { return }
                let tester = Unmanaged<HIDTester>.fromOpaque(ctx).takeUnretainedValue()
                var bytes = [UInt8](repeating: 0, count: length)
                for i in 0..<length { bytes[i] = report[i] }
                tester.ingestReport(now: DispatchTime.now(), bytes: bytes)
            }, context)

            let devKR = IOHIDDeviceOpen(device, IOOptionBits(kIOHIDOptionsTypeNone))
            if devKR != kIOReturnSuccess {
                log(String(format: "IOHIDDeviceOpen 失败 kr=0x%08x（设备可能被独占），跳过", devKR))
            }
        }

        body(manager, devices)
    }

    // MARK: 数据摄入与检索

    /// 输入报告回调入口：打时间戳、算间隔、存最近报告。加锁保护（回调即泵线程，串行安全）。
    fileprivate func ingestReport(now: DispatchTime, bytes: [UInt8]) {
        lock.lock()
        defer { lock.unlock() }
        reportCount += 1
        maxSeenReportLength = max(maxSeenReportLength, bytes.count)
        if let last = lastReportAt {
            let ms = Double(now.uptimeNanoseconds &- last.uptimeNanoseconds) / 1e6
            // 丢弃 >5s 的空洞（用户暂停操作产生的空档不该计入直方图）
            if ms > 0, ms < 5_000 { intervalsMs.append(ms) }
        } else {
            firstReportAt = now
        }
        lastReportAt = now
        recentReports.append(bytes)
        if recentReports.count > 32 { recentReports.removeFirst() }
    }

    /// 取走第一条包含 pattern 的最近报告（命中即从缓冲移除，防重复命中）。
    private func takeReport(containing pattern: [UInt8]) -> [UInt8]? {
        lock.lock()
        defer { lock.unlock() }
        guard let idx = recentReports.firstIndex(where: { $0.contains(subsequence: pattern) }) else { return nil }
        return recentReports.remove(at: idx)
    }

    private func resetAccumulators() {
        lock.lock()
        intervalsMs.removeAll()
        reportCount = 0
        firstReportAt = nil
        lastReportAt = nil
        recentReports.removeAll()
        maxSeenReportLength = 0
        lock.unlock()
    }

    // MARK: 工具

    private func deviceInt(_ device: IOHIDDevice, _ key: String) -> Int? {
        (IOHIDDeviceGetProperty(device, key as CFString) as? NSNumber)?.intValue
    }

    private func deviceString(_ device: IOHIDDevice, _ key: String) -> String? {
        IOHIDDeviceGetProperty(device, key as CFString) as? String
    }

    private func percentile(_ sorted: [Double], _ p: Double) -> Double {
        guard !sorted.isEmpty else { return 0 }
        let idx = Int((Double(sorted.count - 1) * p).rounded())
        return sorted[min(idx, sorted.count - 1)]
    }

    private func elapsedSeconds(_ ms: Double) -> Double { ms / 1000.0 }

    /// 间隔直方图：0.25ms 一桶，至 16ms，其余归入 "≥16"。
    private static func histogram(_ intervals: [Double]) -> [(label: String, count: Int)] {
        var buckets = [Int](repeating: 0, count: 64)
        var overflow = 0
        for v in intervals {
            let b = Int(v / 0.25)
            if b >= 64 { overflow += 1 } else { buckets[b] += 1 }
        }
        var result: [(label: String, count: Int)] = []
        for (i, c) in buckets.enumerated() where c > 0 {
            result.append((String(format: "%.2f–%.2fms", Double(i) * 0.25, Double(i + 1) * 0.25), c))
        }
        if overflow > 0 { result.append(("≥16ms", overflow)) }
        return result
    }
}

// MARK: - 小工具

private extension Array where Element == UInt8 {
    /// 子序列包含判定（环回模式命中用；模式很短，O(n*m) 可接受）。
    func contains(subsequence pattern: [UInt8]) -> Bool {
        guard !pattern.isEmpty, pattern.count <= count else { return false }
        for start in 0...(count - pattern.count) {
            if self[start..<(start + pattern.count)].elementsEqual(pattern) { return true }
        }
        return false
    }
}

func hexPreview(_ bytes: [UInt8], limit: Int = 16) -> String {
    bytes.prefix(limit).map { String(format: "%02x", $0) }.joined(separator: " ")
}
