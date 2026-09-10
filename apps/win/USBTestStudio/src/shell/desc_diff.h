// desc_diff.h — 模型字段级 diff（设计 §2.2 diff▾ / 计划 MS1-T6）
//
// 输出：深度优先遍历两模型，凡标量/结构差异产出 FieldDiff{path, a, b}；
// 结构增删以 path 带下标 + 对侧空值表示。序稳定（同构遍历）。
#pragma once

#include "desc_model.h"

#include <string>
#include <vector>

namespace usts::shell::desc {

struct FieldDiff {
    std::string path;
    std::string a;   // 显示形态（hex 带前缀/十进制/原文）
    std::string b;
};

std::vector<FieldDiff> diff_models(const DescModel& x, const DescModel& y);
std::wstring diff_summary(const std::vector<FieldDiff>& d);   // "N 处差异"

// ---------------------------------------------------------------------------
// 实现（header-only；模块日志 desc.diff）
// ---------------------------------------------------------------------------
namespace detail {

inline std::string hx(uint32_t v) {
    char b[16];
    snprintf(b, sizeof(b), "%#x", v);
    return b;
}

inline void cmp(std::vector<FieldDiff>& out, const std::string& path,
                const std::string& a, const std::string& b) {
    if (a != b) out.push_back({path, a, b});
}

inline void diff_device(std::vector<FieldDiff>& o, const DeviceDesc& x,
                        const DeviceDesc& y) {
    cmp(o, "device.bcdUSB", hx(x.bcd_usb), hx(y.bcd_usb));
    cmp(o, "device.bDeviceClass", hx(x.dev_class), hx(y.dev_class));
    cmp(o, "device.bDeviceSubClass", hx(x.sub_class), hx(y.sub_class));
    cmp(o, "device.bDeviceProtocol", hx(x.protocol), hx(y.protocol));
    cmp(o, "device.bMaxPacketSize0", hx(x.max_packet0), hx(y.max_packet0));
    cmp(o, "device.idVendor", hx(x.vid), hx(y.vid));
    cmp(o, "device.idProduct", hx(x.pid), hx(y.pid));
    cmp(o, "device.bcdDevice", hx(x.bcd_device), hx(y.bcd_device));
    cmp(o, "device.iManufacturer", std::to_string(x.i_man), std::to_string(y.i_man));
    cmp(o, "device.iProduct", std::to_string(x.i_prod), std::to_string(y.i_prod));
    cmp(o, "device.iSerialNumber", std::to_string(x.i_serial), std::to_string(y.i_serial));
    cmp(o, "device.bNumConfigurations", std::to_string(x.num_configs),
        std::to_string(y.num_configs));
}

inline void diff_ep(std::vector<FieldDiff>& o, const std::string& base,
                    const EndpointDesc& x, const EndpointDesc& y) {
    cmp(o, base + ".bEndpointAddress", hx(x.address), hx(y.address));
    cmp(o, base + ".bmAttributes", hx(x.attributes), hx(y.attributes));
    cmp(o, base + ".wMaxPacketSize", std::to_string(x.max_packet),
        std::to_string(y.max_packet));
    cmp(o, base + ".bInterval", std::to_string(x.interval), std::to_string(y.interval));
}

inline void diff_if(std::vector<FieldDiff>& o, const std::string& base,
                    const InterfaceDesc& x, const InterfaceDesc& y) {
    cmp(o, base + ".bInterfaceNumber", std::to_string(x.number), std::to_string(y.number));
    cmp(o, base + ".bAlternateSetting", std::to_string(x.alt), std::to_string(y.alt));
    cmp(o, base + ".bInterfaceClass", hx(x.if_class), hx(y.if_class));
    cmp(o, base + ".bInterfaceSubClass", hx(x.sub_class), hx(y.sub_class));
    cmp(o, base + ".bInterfaceProtocol", hx(x.protocol), hx(y.protocol));
    cmp(o, base + ".iInterface", std::to_string(x.i_if), std::to_string(y.i_if));
    const std::string xr = x.hid ? x.hid->report_hex : "";
    const std::string yr = y.hid ? y.hid->report_hex : "";
    cmp(o, base + ".hid.report", xr.empty() ? "(无)" : xr.substr(0, 16) + "…",
        yr.empty() ? "(无)" : yr.substr(0, 16) + "…");
    const size_t xn = x.endpoints.size(), yn = y.endpoints.size();
    for (size_t i = 0; i < xn || i < yn; ++i) {
        const std::string eb = base + ".endpoints[" + std::to_string(i) + "]";
        if (i < xn && i < yn) diff_ep(o, eb, x.endpoints[i], y.endpoints[i]);
        else if (i < xn) o.push_back({eb, "存在", "(删除)"});
        else o.push_back({eb, "(新增)", "存在"});
    }
}

inline void diff_cfg(std::vector<FieldDiff>& o, const std::string& base,
                     const ConfigDesc& x, const ConfigDesc& y) {
    cmp(o, base + ".bConfigurationValue", std::to_string(x.value), std::to_string(y.value));
    cmp(o, base + ".iConfiguration", std::to_string(x.i_cfg), std::to_string(y.i_cfg));
    cmp(o, base + ".bmAttributes", hx(x.attributes), hx(y.attributes));
    cmp(o, base + ".bMaxPower", std::to_string(x.max_power), std::to_string(y.max_power));
    const size_t xn = x.interfaces.size(), yn = y.interfaces.size();
    for (size_t i = 0; i < xn || i < yn; ++i) {
        const std::string fb = base + ".interfaces[" + std::to_string(i) + "]";
        if (i < xn && i < yn) diff_if(o, fb, x.interfaces[i], y.interfaces[i]);
        else if (i < xn) o.push_back({fb, "存在", "(删除)"});
        else o.push_back({fb, "(新增)", "存在"});
    }
}

} // namespace detail

inline std::vector<FieldDiff> diff_models(const DescModel& x, const DescModel& y) {
    auto log = ustlog::logger("desc.diff");
    std::vector<FieldDiff> o;
    detail::diff_device(o, x.device, y.device);
    const size_t cn = (std::max)(x.configs.size(), y.configs.size());
    for (size_t i = 0; i < cn; ++i) {
        const std::string cb = "configs[" + std::to_string(i) + "]";
        if (i < x.configs.size() && i < y.configs.size())
            detail::diff_cfg(o, cb, x.configs[i], y.configs[i]);
        else if (i < x.configs.size()) o.push_back({cb, "存在", "(删除)"});
        else o.push_back({cb, "(新增)", "存在"});
    }
    detail::cmp(o, "strings.langids", wraii::wide_to_utf8(
         [&] { std::wstring s; for (auto& l : x.strings.langids) s += l + L","; return s; }()),
         wraii::wide_to_utf8(
         [&] { std::wstring s; for (auto& l : y.strings.langids) s += l + L","; return s; }()));
    log->debug("diff_models: {} 处差异", o.size());
    return o;
}

inline std::wstring diff_summary(const std::vector<FieldDiff>& d) {
    return L"" + std::to_wstring(d.size()) + L" 处差异";
}

} // namespace usts::shell::desc
