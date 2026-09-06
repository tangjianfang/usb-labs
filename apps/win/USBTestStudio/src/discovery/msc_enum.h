// msc_enum.h — EP-4 S5 后半·设备发现 MSC 侧：USB 大容量盘目录行。
// 扫描 PhysicalDrive0..9（与 MscScsi::auto_detect_usb_drive 同口径）：只读打开
// 成功且 BusTypeUsb 才入列——工程师在目录看到的 MSC 行即产测会选中的盘。
// 名称尽力而为取 INQUIRY vendor/product，失败回退 "USB 大容量磁盘 N"。
// INQUIRY/READ_CAPACITY 不读介质，扫描无副作用。已知边界：行内无 USB
// VID:PID（盘句柄侧只有 SCSI 身份；USBSTOR 节点交叉匹配留待真机样本）。
#pragma once

#include "discovery/device_catalog.h"
#include "usb/msc_scsi.h"

#include <string>
#include <vector>

namespace msc_enum {

// 盘号 → 目录条目（identity 为 INQUIRY 身份串，空则回退通用名）
inline ConsoleDevice make_device(unsigned index, const std::wstring& identity) {
    ConsoleDevice d;
    d.kind = DeviceKind::msc;
    d.path = MscScsi::device_path(index);
    d.name = identity.empty()
                 ? wraii::fmt_v(L"USB 大容量磁盘 %u", index)
                 : identity + wraii::fmt_v(L"（PhysicalDrive%u）", index);
    return d;
}

// 扫描探测超时（秒，evolve #73）：旧实现走端口默认 10s——扫描虽在后台线程不
// 冻结 UI，但病态盘 ×10 轮最坏可拖满分钟级且无取消；3s 与 MscChannel open
// 探测同界。有界性与失败可见性由 discovery_selftest 以假件钉住（对抗复核 2a/2c）
inline constexpr unsigned kScanTimeoutS = 3;

// 扫描内核（模板化 PortT 以便离线自测注入假件；产线形态即 MscScsi）
template <typename PortT = MscScsi>
std::vector<ConsoleDevice> scan_impl(std::wstring* err = nullptr) {
    std::vector<ConsoleDevice> out;
    for (unsigned i = 0; i < 10; ++i) {
        PortT probe;
        if (!probe.open_physical_drive(i, /*write_access=*/false, nullptr)) continue;
        bool usb = false;
        std::wstring e;
        if (!probe.bus_is_usb(&usb, &e)) {       // 属性查询失败：记录后跳过
            if (err && err->empty()) *err = e;
            continue;
        }
        if (!usb) continue;                      // 非 USB 盘（系统盘等）静默跳过
        unsigned long long sectors = 0;
        unsigned blk = 0;
        if (!probe.read_capacity(&sectors, &blk, &e, kScanTimeoutS)) {
            // 探测失败/超时可见（对抗复核 2c：休眠待起转的慢盘从目录消失时留线索，
            // 而非无声少一行）
            if (err && err->empty())
                *err = wraii::fmt_v(L"PhysicalDrive%u 探测失败/超时: %s", i, e.c_str());
            continue;                            // 无介质/不响应
        }
        std::string v8, p16, r4;
        probe.scsi_inquiry(&v8, &p16, &r4, nullptr, nullptr, kScanTimeoutS);   // 尽力而为
        std::wstring id;
        for (unsigned char c : v8)
            if (c) id.push_back(wchar_t(c));
        if (!p16.empty()) {
            if (!id.empty()) id += L' ';
            for (unsigned char c : p16)
                if (c) id.push_back(wchar_t(c));
        }
        out.push_back(make_device(i, id));
    }
    return out;
}

// 扫描本机 USB 大容量盘（无 U 盘机器上空表，同样绿）
inline std::vector<ConsoleDevice> scan(std::wstring* err = nullptr) {
    return scan_impl<MscScsi>(err);
}

} // namespace msc_enum
