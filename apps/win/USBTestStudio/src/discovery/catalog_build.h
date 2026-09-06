// catalog_build.h — EP-4 S2 设备发现后半·目录组装与会话工厂：
// DeviceEnumerator 的 DeviceInfo（USB 设备节点 / HID 收集）与 serial_enum 的
// COM 口合流为控制台目录（ConsoleDevice）；make_channel 把目录条目落成
// IChannel 会话——HID→HidChannel 报告会话、串口→SerialChannel 终端会话、
// 其余 USB 接口待 S5（WinUSB/MSC）。"双击开会话"到打开通道为止，
// 收发台（发送框/接收区/历史/周期）是 S3 切片。
#pragma once

#include "channel/hid_channel.h"
#include "channel/serial_channel.h"
#include "discovery/device_catalog.h"
#include "discovery/serial_enum.h"
#include "usb/device_enumerator.h"

#include <memory>

namespace catalog_build {

// DeviceInfo → 目录条目：class_name=="HID" 的收集 → hid（path=设备接口路径，
// HidChannel 直接 CreateFile）；其余（USB 设备节点）→ usb。名称沿用枚举器已
// 解析的 BusReported/友好名/HID 产品串；为空时回退 VID:PID 文本，再回退
// 协议名——目录不出现空名行。
inline ConsoleDevice from_device_info(const DeviceInfo& d) {
    ConsoleDevice c;
    c.kind = d.class_name == L"HID" ? DeviceKind::hid : DeviceKind::usb;
    c.vid = d.vid;
    c.pid = d.pid;
    c.path = d.path;
    c.name = d.name;
    if (c.name.empty()) c.name = c.vidpid_text();
    if (c.name.empty()) c.name = c.kind_text() + L" 设备";
    return c;
}

// 目录组装：USB/HID 接口枚举 + COM 口（serial_enum::scan 已排序）原序合流。
// USB 节点与其 HID 收集各占一行（同一物理设备两行）——工程师既要选父节点
// 也要选具体报告接口，设计 §2 表格即两行并存口径。
inline std::vector<ConsoleDevice> build_catalog(const std::vector<DeviceInfo>& infos,
                                                const std::vector<std::wstring>& coms) {
    std::vector<ConsoleDevice> out;
    out.reserve(infos.size() + coms.size());
    for (const auto& d : infos) out.push_back(from_device_info(d));
    for (const auto& c : coms) out.push_back(serial_enum::make_device(c));
    return out;
}

// 目录条目 → 会话通道：hid/serial 即刻可开；usb 返回 nullptr（S5 提供
// WinUSB/MSC 通道），调用方据此提示而非报错。
inline std::unique_ptr<IChannel> make_channel(const ConsoleDevice& d) {
    switch (d.kind) {
        case DeviceKind::hid:    return std::make_unique<HidChannel>(d.path);
        case DeviceKind::serial: return std::make_unique<SerialChannel>(d.path);
        default:                 return nullptr;
    }
}

} // namespace catalog_build
