//
//  DeviceScanner.swift
//  USBTestStudio — IOKit USB 设备枚举（设备访问层，不含任何 UI 类型）
//
//  技术口径（与方案一致，勿改）：
//    IOServiceGetMatchingServices(kIOMasterPortDefault, IOServiceMatching("IOUSBHostDevice"))
//      → IOIteratorNext 逐台遍历
//      → IORegistryEntryCreateCFProperties 读取 idVendor / idProduct / USB Product Name / Speed
//        以及镜像设备描述符的 bcdUSB / bcdDevice / bDeviceClass / bDeviceSubClass /
//        bDeviceProtocol / bMaxPacketSize0（供 descriptor_check 步骤比对）。
//
//  注意事项（以 Xcode 实际头文件为准）：
//    · kIOMasterPortDefault 自 macOS 12 起由 kIOMainPortDefault 取代（同一 mach_port 值的
//      宏别名）。若 Xcode 给出弃用告警，可直接替换为 kIOMainPortDefault。
//    · IORegistry 的 "Speed" 属性在现代 macOS（IOUSBHostFamily）下单位为 bit/s
//      （如 480000000 = 480 Mbps）。以 IORegistryExplorer 对真机实测为准。
//    · IORegistryEntryGetPath 的缓冲类型 io_name_t 是 C 定长数组（char[128]），
//      Swift 侧用 [CChar] 传入；若签名不匹配，改用 withUnsafeMutableBytes 绑定。
//
//  T7 首切片（本切片未编译验证，需 macOS，按 Swift 5.9 口径编写）新增：
//    filtered(_:by:) 发现过滤纯函数（EP-4 设计 §二 设备发现区 / §四.1 即时过滤）。
//    说明：本文件 scan() 是真实 IOKit 枚举实现（非 TODO/桩），过滤层直接作用于真实
//    枚举结果；纯函数无 UI 类型、无副作用，对任意 [USBDeviceInfo]（含桩数据）同样可跑。
//

import Foundation
import IOKit
import IOKit.usb

// MARK: - 设备信息模型

/// 一台 USB 设备的枚举快照（IORegistry 属性 ≈ 设备描述符镜像）。
struct USBDeviceInfo {
    let name: String             // "USB Product Name"（iProduct 字符串描述符）
    let vendorName: String       // "USB Vendor Name"
    let vid: UInt16              // idVendor
    let pid: UInt16              // idProduct
    let serialNumber: String     // "USB Serial Number"（iSerial）
    let speedMbps: Double        // 协商速率（Mbps）
    let speedLabel: String       // Low/Full/High/SuperSpeed…
    let locationID: UInt32       // 位置 ID（物理插拔口）
    let path: String             // kIOServicePlane 注册表路径
    let bcdUSB: String           // 描述符 bcdUSB（如 "0200"）
    let bcdDevice: String        // 描述符 bcdDevice
    let deviceClass: UInt8       // bDeviceClass
    let deviceSubClass: UInt8    // bDeviceSubClass
    let deviceProtocol: UInt8    // bDeviceProtocol
    let maxPacketSize0: Int      // bMaxPacketSize0

    /// "1234:0002" 形态，与抓包/报表口径一致
    var vidPidText: String { String(format: "%04X:%04X", vid, pid) }
}

// MARK: - 扫描器

enum DeviceScanner {

    /// 扫描当前挂在总线上的全部 USB 设备。
    /// 任何一步失败都不抛出（产线上枚举失败返回空列表，由引擎步骤判定 FAIL）。
    static func scan() -> [USBDeviceInfo] {
        var results: [USBDeviceInfo] = []

        // IOServiceMatching 返回的字典由 IOServiceGetMatchingServices 消费，无需手动释放。
        let matching = IOServiceMatching("IOUSBHostDevice")

        var iterator: io_iterator_t = 0
        let kr = IOServiceGetMatchingServices(kIOMasterPortDefault, matching, &iterator)
        guard kr == KERN_SUCCESS else {
            // kr 非 0：匹配服务失败（权限/内核态异常，极罕见）
            return results
        }
        defer { IOObjectRelease(iterator) }

        while true {
            let service = IOIteratorNext(iterator)
            if service == 0 { break }
            defer { IOObjectRelease(service) }

            var propsRef: Unmanaged<CFMutableDictionary>?
            guard IORegistryEntryCreateCFProperties(service, &propsRef, kCFAllocatorDefault, 0) == KERN_SUCCESS,
                  let cfDict = propsRef?.takeRetainedValue() else {
                continue
            }
            let dict = (cfDict as NSDictionary) as? [String: Any] ?? [:]

            let vid = num(dict, "idVendor") ?? 0
            let pid = num(dict, "idProduct") ?? 0
            let speedRaw = num(dict, "Speed") ?? 0
            let speedMbps = Double(speedRaw) / 1_000_000.0   // bit/s → Mbps（见文件头注释）

            results.append(USBDeviceInfo(
                name: str(dict, "USB Product Name") ?? str(dict, "USB Vendor Name") ?? "USB Device",
                vendorName: str(dict, "USB Vendor Name") ?? "",
                vid: UInt16(truncatingIfNeeded: vid),
                pid: UInt16(truncatingIfNeeded: pid),
                serialNumber: str(dict, "USB Serial Number") ?? "",
                speedMbps: speedMbps,
                speedLabel: speedLabel(forMbps: speedMbps),
                locationID: UInt32(truncatingIfNeeded: num(dict, "locationID") ?? 0),
                path: registryPath(of: service),
                bcdUSB: bcdText(num(dict, "bcdUSB")),
                bcdDevice: bcdText(num(dict, "bcdDevice")),
                deviceClass: UInt8(truncatingIfNeeded: num(dict, "bDeviceClass") ?? 0),
                deviceSubClass: UInt8(truncatingIfNeeded: num(dict, "bDeviceSubClass") ?? 0),
                deviceProtocol: UInt8(truncatingIfNeeded: num(dict, "bDeviceProtocol") ?? 0),
                maxPacketSize0: num(dict, "bMaxPacketSize0") ?? 0
            ))
        }
        // 稳定排序，保证 UI 列表顺序可复现
        return results.sorted { "\($0.vidPidText)\($0.path)" < "\($1.vidPidText)\($1.path)" }
    }

    /// 速率 → 可读标签（阈值按 USB 2.0/3.x 规范速率，实测值可能略低，取最近档位）。
    static func speedLabel(forMbps mbps: Double) -> String {
        switch mbps {
        case ..<2.0:     return "Low Speed"
        case ..<100.0:   return "Full Speed"
        case ..<1000.0:  return "High Speed"
        case ..<6000.0:  return "SuperSpeed"
        case ..<11000.0: return "SuperSpeed+ 10G"
        default:         return "SuperSpeed+ 20G"
        }
    }

    /// 计划的 device 匹配：vid/pid/name_prefix 全部满足才算命中（字段缺省即不约束）。
    static func matches(_ info: USBDeviceInfo, vid: Int?, pid: Int?, namePrefix: String?) -> Bool {
        if let v = vid, Int(info.vid) != v { return false }
        if let p = pid, Int(info.pid) != p { return false }
        if let prefix = namePrefix, !prefix.isEmpty, !info.name.hasPrefix(prefix) { return false }
        return true
    }

    // MARK: 发现过滤（T7 首切片，EP-4 设计 §二/§四.1）

    /// 即时过滤纯函数：大小写不敏感子串匹配 VID:PID（"1234:0002"，另含 "12340002"
    /// 紧凑十六进制形，方便只输 VID 段）/ 产品名 / IORegistry 路径；多关键词空格分隔 =
    /// AND（与 EP-4 设计 §四.1 口径一致，如 "1234 key"）。query 为空/纯空白 → 原样返回。
    /// 无副作用，可对任意 [USBDeviceInfo]（含桩数据）独立验证。
    static func filtered(_ devices: [USBDeviceInfo], by query: String) -> [USBDeviceInfo] {
        let tokens = query.split(whereSeparator: { $0.isWhitespace }).map { $0.lowercased() }
        guard !tokens.isEmpty else { return devices }
        return devices.filter { d in
            let haystacks = [
                d.name.lowercased(),
                d.vidPidText.lowercased(),
                String(format: "%04x%04x", d.vid, d.pid),   // 紧凑形："12340002"
                d.path.lowercased()
            ]
            return tokens.allSatisfy { token in haystacks.contains { $0.contains(token) } }
        }
    }

    // MARK: 私有工具

    /// IORegistry 数值统一按 NSNumber 取（注册表数值以 CFNumber 落地，直接 as? Int 常失败）。
    private static func num(_ dict: [String: Any], _ key: String) -> Int? {
        if let n = dict[key] as? NSNumber { return n.intValue }
        if let i = dict[key] as? Int { return i }
        return nil
    }

    private static func str(_ dict: [String: Any], _ key: String) -> String? {
        dict[key] as? String
    }

    /// bcd 字段注册表里是数值（0x0200 → "0200"）。
    private static func bcdText(_ raw: Int?) -> String {
        guard let raw else { return "-" }
        return String(format: "%04x", raw)
    }

    /// kIOServicePlane 注册表路径，用于日志/报告定位具体物理口。
    private static func registryPath(of service: io_registry_entry_t) -> String {
        var buffer = [CChar](repeating: 0, count: 512)
        // io_name_t 是 char[128]；此处给足 512 字节余量。
        // 若 Xcode 报指针类型不匹配，改为 withUnsafeMutableBytes + bindMemory(to: CChar.self)。
        let kr = IORegistryEntryGetPath(service, kIOServicePlane, &buffer)
        guard kr == KERN_SUCCESS else { return "" }
        return String(cString: buffer)
    }
}
