// device_catalog.h — EP-4 工程师通信控制台·S2 设备发现（设计 §2 目录表格 +
// §4.1 即时过滤）：控制台目录条目 ConsoleDevice（名称/VID:PID/协议/路径）与
// 过滤核心——搜索框子串匹配（不区分 ASCII 大小写，多关键词空格分隔 AND），
// 协议复选框 = kind 位掩码二次过滤。纯逻辑无 Win32 依赖：S2 后半的 UI 表格
// 与产测引擎复用同一套。匹配域 = 名称+VID:PID+路径+协议标签（含英文同义词，
// 如串口可被 cdc/serial 命中）。"输 2341 三秒定位目标" 的收敛即此过滤器。
#pragma once

#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

// 设备协议种类（兼作复选框位掩码；WinUSB/MSC 细分在 S5 通道落地时展开）
enum class DeviceKind : unsigned {
    hid    = 1u << 0,   // HID 收集 → HidChannel 报告会话
    serial = 1u << 1,   // CDC/串口 → SerialChannel 终端会话
    usb    = 1u << 2,   // 其他 USB 设备接口（S5 前占位）
};
constexpr unsigned operator|(DeviceKind a, DeviceKind b) noexcept {
    return unsigned(a) | unsigned(b);
}
constexpr unsigned kind_bits(DeviceKind k) noexcept { return unsigned(k); }

struct ConsoleDevice {
    DeviceKind kind = DeviceKind::usb;
    std::wstring name;           // 显示名称（BusReportedDeviceDesc/产品串/COM7）
    unsigned vid = 0, pid = 0;   // 0 = 无（如物理串口），此时 vidpid_text() 为空
    std::wstring path;           // 打开用路径（HID 接口路径 / COM 名）

    std::wstring vidpid_text() const {   // "1234:0002"（%04X，与 HidChannel desc 同口径）
        if (!vid) return {};
        wchar_t buf[16];
        swprintf(buf, 16, L"%04X:%04X", vid, pid);
        return buf;
    }
    std::wstring kind_text() const {     // 目录"协议"列
        switch (kind) {
            case DeviceKind::hid:    return L"HID";
            case DeviceKind::serial: return L"串口";
            default:                 return L"USB";
        }
    }
    std::wstring kind_synonym() const {  // 可搜索英文同义词（小写）
        switch (kind) {
            case DeviceKind::hid:    return L"hid";
            case DeviceKind::serial: return L"serial cdc";
            default:                 return L"usb winusb";
        }
    }
};

// —— 匹配实现（ASCII 折叠小写；中文等非 ASCII 字符原样参与子串匹配） ——
inline bool dc_is_space(wchar_t c) noexcept {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}
inline std::wstring dc_lower(std::wstring_view s) {
    std::wstring out(s);
    for (auto& c : out)
        if (c >= L'A' && c <= L'Z') c = wchar_t(c + (L'a' - L'A'));
    return out;
}
inline std::wstring dc_search_blob(const ConsoleDevice& d) {
    std::wstring blob = dc_lower(d.name);
    blob += L' ';
    blob += dc_lower(d.vidpid_text());
    blob += L' ';
    blob += dc_lower(d.path);
    blob += L' ';
    blob += dc_lower(d.kind_text());
    blob += L' ';
    blob += dc_lower(d.kind_synonym());
    return blob;
}

// query 命中判定：按空白切分关键词，全部（AND）为可搜索文本子串才算命中；
// 空 query 视为全选。kind_mask = 0 视为协议全选。
inline bool device_matches(const ConsoleDevice& d, std::wstring_view query,
                           unsigned kind_mask = 0) {
    if (kind_mask != 0 && (unsigned(d.kind) & kind_mask) == 0) return false;
    const std::wstring blob = dc_search_blob(d);
    const std::wstring q = dc_lower(query);
    size_t i = 0;
    while (i < q.size()) {
        while (i < q.size() && dc_is_space(q[i])) ++i;
        size_t j = i;
        while (j < q.size() && !dc_is_space(q[j])) ++j;
        if (j > i && blob.find(q.substr(i, j - i)) == std::wstring::npos) return false;
        i = j;
    }
    return true;
}

// 即时过滤：返回命中子集（保持原顺序——输入过程中表格行不跳动）
inline std::vector<ConsoleDevice> filter_devices(const std::vector<ConsoleDevice>& devices,
                                                 std::wstring_view query,
                                                 unsigned kind_mask = 0) {
    std::vector<ConsoleDevice> out;
    for (const auto& d : devices)
        if (device_matches(d, query, kind_mask)) out.push_back(d);
    return out;
}
