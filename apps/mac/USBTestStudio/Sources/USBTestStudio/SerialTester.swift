//
//  SerialTester.swift
//  USBTestStudio — CDC 串口环回 / PD 遥测采集（设备访问层）
//
//  技术口径（与方案一致，勿改）：
//    · 端口枚举：FileManager 列 /dev，过滤 "tty.usbmodem*"（CDC-ACM）/ "tty.usbserial*"；
//    · 读写：open(2) 取 fd → FileHandle(fileDescriptor:closeOnDealloc:false) 读写；
//    · 超时：termios 置 VMIN=0 / VTIME=n（单位 100ms），单次 read 最多阻塞 n×100ms，
//      应用层再用 DispatchTime 总预算兜底；
//    · PD 遥测：macOS 无 MSC/供应商直通，遥测统一走 CDC 串口（固件控制台），
//      本文件解析器为“概要”实现，行格式以固件协议为准（见 parseTelemetryLine）。
//
//  环回口径：serial_loopback 需 TX-RX 短接工装；每轮负载唯一（含轮次号+随机段），
//  防止残留旧字节造成假通过；字节级一致性判定（与 Windows 版 / cdc_test.py 同判据）。
//
//  不确定 API（以 Xcode 实际头文件为准）：
//    · VMIN / VTIME 常量（macOS termios.h 定义为 16/17）；c_cc 在 Swift 中是定长元组，
//      用 withUnsafeMutableBytes 写入；若常量未导入，直接用字面量 16/17；
//    · FileHandle.readData(ofLength:) 在 fd 超时返回 0 字节时会得到空 Data
//      （与 EOF 不可区分），因此必须配合 VTIME + 应用层预算，不可裸用 availableData。
//

import Foundation
import Darwin

// MARK: - 串口枚举

struct SerialPortInfo {
    let path: String      // "/dev/tty.usbmodemXXXX"
    let bsdName: String   // "tty.usbmodemXXXX"
}

enum SerialPortScanner {
    /// 枚举 USB CDC / USB-Serial 串口（仅 tty 口；cu 口留给用户手动指定场景）。
    static func listPorts() -> [SerialPortInfo] {
        let entries = (try? FileManager.default.contentsOfDirectory(atPath: "/dev")) ?? []
        return entries
            .filter { $0.hasPrefix("tty.usbmodem") || $0.hasPrefix("tty.usbserial") }
            .sorted()
            .map { SerialPortInfo(path: "/dev/" + $0, bsdName: $0) }
    }
}

// MARK: - 环回结果 / 遥测样本

struct SerialLoopbackResult {
    var rounds = 0
    var roundsPassed = 0
    var bytesTx = 0
    var bytesRx = 0
    var firstError = ""
    var elapsedMs = 0.0

    var pass: Bool { rounds > 0 && roundsPassed == rounds }
}

struct TelemetrySample {
    let tMs: Double        // 相对采集起点的毫秒
    let voltageV: Double   // VBUS 电压（V）
    let currentA: Double?  // 电流（A，固件未输出则为 nil）
}

// MARK: - 串口会话

enum SerialError: Error, LocalizedError {
    case openFailed(path: String, code: Int32)
    case configureFailed(what: String, code: Int32)
    case writeFailed(code: Int32)
    case portNotFound

    var errorDescription: String? {
        switch self {
        case .openFailed(let p, let c):
            return "open(\(p)) 失败 errno=\(c)：\(String(cString: strerror(c)))"
        case .configureFailed(let what, let c):
            return "termios 配置失败(\(what)) errno=\(c)"
        case .writeFailed(let c):
            return "write 失败 errno=\(c)：\(String(cString: strerror(c)))"
        case .portNotFound:
            return "未找到 /dev/tty.usbmodem* / tty.usbserial* 串口"
        }
    }
}

/// 一条打开并配置为 raw 模式的串口连接。用完必须 close()（deinit 兜底）。
final class SerialSession {

    private var fd: Int32          // close() 后置 -1，防 deinit 重复关闭
    private let path: String
    private var lineBuffer = Data()

    /// 打开并配置 raw 模式 + 指定波特率。
    init(path: String, baud: Int = 115200) throws {
        self.path = path
        // O_NOCTTY：不把该口当控制终端；保持阻塞模式（VTIME 才生效）
        let fd = Darwin.open(path, O_RDWR | O_NOCTTY)
        guard fd >= 0 else { throw SerialError.openFailed(path: path, code: errno) }
        self.fd = fd

        var tio = termios()
        guard tcgetattr(fd, &tio) == 0 else {
            close()
            throw SerialError.configureFailed(what: "tcgetattr", code: errno)
        }

        // raw 模式：无回显/无行缓冲/无特殊字符处理，8N1
        cfmakeraw(&tio)
        tio.c_cflag |= tcflag_t(CLOCAL | CREAD)     // 忽略载波 + 使能接收
        tio.c_cflag &= ~tcflag_t(CRTSCTS)           // 关硬件流控（工装多数不支持）
        tio.c_cflag &= ~tcflag_t(CSIZE | PARENB)    // 8 数据位、无校验
        tio.c_cflag |= tcflag_t(CS8)

        let speed = Self.baudConstant(baud)
        cfsetispeed(&tio, speed)
        cfsetospeed(&tio, speed)

        // VMIN=0 / VTIME=2：每次 read 最多阻塞 200ms，配合应用层总预算
        withUnsafeMutableBytes(of: &tio.c_cc) { cc in
            cc[Int(VMIN)] = UInt8(0)     // macOS termios.h: VMIN = 16
            cc[Int(VTIME)] = UInt8(2)    // macOS termios.h: VTIME = 17，单位 100ms
        }

        guard tcsetattr(fd, TCSANOW, &tio) == 0 else {
            close()
            throw SerialError.configureFailed(what: "tcsetattr", code: errno)
        }
    }

    deinit { close() }

    // MARK: 基础读写

    func write(_ data: Data) throws {
        let n = data.withUnsafeBytes { raw -> Int in
            guard let base = raw.baseAddress else { return -1 }
            return Darwin.write(fd, base, raw.count)
        }
        guard n == data.count else { throw SerialError.writeFailed(code: errno) }
    }

    /// 在 budgetMs 总预算内读取至少 minBytes 字节（每次 read 阻塞 ≤ VTIME×100ms）。
    /// 返回实际读到的数据（可能不足 minBytes，表示预算耗尽）。
    func read(minBytes: Int, budgetMs: Int) -> Data {
        var out = Data()
        let start = DispatchTime.now()
        while out.count < minBytes {
            let chunk = handleRead(ofLength: max(1, minBytes - out.count))
            if !chunk.isEmpty { out.append(chunk) }
            let elapsed = Double(DispatchTime.now().uptimeNanoseconds &- start.uptimeNanoseconds) / 1e6
            if elapsed >= Double(budgetMs) { break }
        }
        return out
    }

    /// 预算期内读取并丢弃所有字节（清残留，防环回假通过）。
    func drain(budgetMs: Int = 200) {
        let start = DispatchTime.now()
        while true {
            let chunk = handleRead(ofLength: 256)
            if chunk.isEmpty { break }   // 一次 VTIME 超时无数据即认为排空
            let elapsed = Double(DispatchTime.now().uptimeNanoseconds &- start.uptimeNanoseconds) / 1e6
            if elapsed >= Double(budgetMs) { break }
        }
        lineBuffer.removeAll()
    }

    /// 逐行读取（\n 分隔），预算耗尽返回 nil。
    func readLine(budgetMs: Int) -> String? {
        let start = DispatchTime.now()
        while true {
            if let nl = lineBuffer.firstIndex(of: 0x0A) {
                let lineData = lineBuffer[lineBuffer.startIndex..<nl]
                lineBuffer.removeSubrange(lineBuffer.startIndex...nl)
                return String(data: Data(lineData), encoding: .utf8)
                    ?? String(decoding: Data(lineData), as: UTF8.self)
            }
            let chunk = handleRead(ofLength: 256)
            if !chunk.isEmpty { lineBuffer.append(chunk) }
            let elapsed = Double(DispatchTime.now().uptimeNanoseconds &- start.uptimeNanoseconds) / 1e6
            if elapsed >= Double(budgetMs) { return nil }
        }
    }

    /// 本地 termios 回读的波特率（line_coding 步骤“概要”校验用；
    /// macOS 无 CDC SET_LINE_CODING 直通，无法读取设备侧真实线路编码）。
    var currentBaud: Int {
        var tio = termios()
        guard tcgetattr(fd, &tio) == 0 else { return 0 }
        let speed = UInt32(cfgetospeed(&tio))
        return Self.baudValue(speed) ?? Int(speed)
    }

    func close() {
        guard fd >= 0 else { return }
        Darwin.close(fd)
        fd = -1
    }

    /// FileHandle 包装读：fd 已配 VTIME，readData 阻塞 ≤200ms；超时返回空 Data。
    /// （closeOnDealloc=false：fd 生命周期归本类管理。）
    private func handleRead(ofLength n: Int) -> Data {
        let handle = FileHandle(fileDescriptor: fd, closeOnDealloc: false)
        let chunk = handle.readData(ofLength: n)
        return chunk.isEmpty ? Data() : chunk
    }

    // MARK: 环回测试

    /// TX-RX 工装环回：每轮发送唯一负载并回读比对，字节级一致才算过。
    func loopback(rounds: Int, payloadTemplate: String, timeoutMs: Int,
                  log: (String) -> Void) -> SerialLoopbackResult {
        var result = SerialLoopbackResult()
        result.rounds = rounds
        drain(budgetMs: 200)   // 清残留
        let start = DispatchTime.now()

        for round in 1...max(1, rounds) {
            // 轮次号 + 随机段：确保每轮负载唯一
            let random = String(format: "%08X", UInt32.random(in: UInt32.min...UInt32.max))
            let text = "\(payloadTemplate)#\(round)|\(random)\n"
            let data = Data(text.utf8)
            do {
                try write(data)
            } catch {
                result.firstError = "第\(round)轮发送失败: \(error.localizedDescription)"
                break
            }
            result.bytesTx += data.count

            let echo = read(minBytes: data.count, budgetMs: timeoutMs)
            result.bytesRx += echo.count
            if echo == data {
                result.roundsPassed += 1
                log("环回第 \(round)/\(rounds) 轮 OK (\(data.count) 字节)")
            } else {
                result.firstError = "第\(round)轮不一致: tx=\"\(text.trimmingCharacters(in: .newlines).prefix(48))\" " +
                    "rx=\"\(String(decoding: echo.prefix(48), as: UTF8.self))\""
                log("环回第 \(round)/\(rounds) 轮 MISMATCH")
                break
            }
        }
        result.elapsedMs = Double(DispatchTime.now().uptimeNanoseconds &- start.uptimeNanoseconds) / 1e6
        return result
    }

    // MARK: PD 遥测采集（概要）

    /// 可选发送 command 后，在 durationMs 内收集遥测行并解析为样本。
    /// 行格式以固件协议为准，解析规则见 parseTelemetryLine（“概要”实现）。
    func collectTelemetry(command: String?, durationMs: Int,
                          log: (String) -> Void) -> [TelemetrySample] {
        drain(budgetMs: 200)
        if let command, !command.isEmpty {
            do { try write(Data((command + "\n").utf8)) }
            catch { log("遥测指令发送失败: \(error.localizedDescription)") }
        }

        var samples: [TelemetrySample] = []
        let start = DispatchTime.now()
        let budgetNs: UInt64 = UInt64(max(200, durationMs)) * 1_000_000
        while DispatchTime.now().uptimeNanoseconds &- start.uptimeNanoseconds < budgetNs {
            if let line = readLine(budgetMs: 150) {
                let trimmed = line.trimmingCharacters(in: .whitespaces)
                guard !trimmed.isEmpty else { continue }
                if let sample = Self.parseTelemetryLine(trimmed) {
                    let t = Double(DispatchTime.now().uptimeNanoseconds &- start.uptimeNanoseconds) / 1e6
                    samples.append(TelemetrySample(tMs: t, voltageV: sample.voltageV, currentA: sample.currentA))
                    log(String(format: "遥测[t=%.0fms] V=%.3fV%@",
                               t, sample.voltageV,
                               sample.currentA.map { String(format: " I=%.3fA", $0) } ?? ""))
                }
            }
        }
        return samples
    }

    /// 遥测行解析（概要）：依次尝试
    ///   1) JSON 行：{"v":9.01,"i":0.35} / {"vbus_v":9.01} / {"v_mv":9000,"i_ma":350}
    ///   2) key=value 行：v=9.01 i=0.35 / VBUS=9000mV / v: 9.01
    /// 启发式：数值 ≥ 1000 视为 mV/mA。产线使用前请按固件实际协议收紧。
    static func parseTelemetryLine(_ line: String) -> (voltageV: Double, currentA: Double?)? {
        // 1) JSON 行
        if let data = line.data(using: .utf8),
           let obj = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] {
            let v = obj["v"] as? Double ?? obj["vbus_v"] as? Double
                ?? (obj["v_mv"] as? Double).map { $0 / 1000.0 }
            guard let v else { return nil }
            let i = obj["i"] as? Double ?? obj["i_a"] as? Double
                ?? (obj["i_ma"] as? Double).map { $0 / 1000.0 }
            return (v, i)
        }

        // 2) key=value / key: value 行（忽略大小写的键，v/V/VBUS 取电压，i/I/CUR 取电流）
        var voltage: Double?
        var current: Double?
        let pairs = line.split(whereSeparator: { $0 == " " || $0 == "," || $0 == "\t" })
        for pair in pairs {
            let parts = pair.split(whereSeparator: { $0 == "=" || $0 == ":" }, maxSplits: 1)
            guard parts.count == 2,
                  let value = Double(parts[1].trimmingCharacters(in: CharacterSet(charactersIn: "mAVma "))) else { continue }
            let key = parts[0].lowercased()
            if key == "v" || key == "vbus" || key == "vbus_v" || key == "v_mv" {
                let v = key == "v_mv" || value >= 1000 ? value / 1000.0 : value
                if voltage == nil { voltage = v }
            } else if key == "i" || key == "cur" || key == "i_a" || key == "i_ma" {
                let a = key == "i_ma" || value >= 1000 ? value / 1000.0 : value
                if current == nil { current = a }
            }
        }
        guard let v = voltage else { return nil }
        return (v, current)
    }

    // MARK: 波特率映射

    /// macOS termios 只认离散波特率常量（无 BOTHER 自定义机制）。
    private static func baudConstant(_ baud: Int) -> speed_t {
        switch baud {
        case 9600:   return B9600
        case 19200:  return B19200
        case 38400:  return B38400
        case 57600:  return B57600
        case 230400: return B230400
        case 460800: return B460800
        case 921600: return B921600
        default:     return B115200   // 计划缺省 / 非标准值回落 115200
        }
    }

    private static func baudValue(_ speed: speed_t) -> Int? {
        switch speed {
        case B9600: return 9600
        case B19200: return 19200
        case B38400: return 38400
        case B57600: return 57600
        case B115200: return 115200
        case B230400: return 230400
        case B460800: return 460800
        case B921600: return 921600
        default: return nil
        }
    }
}
