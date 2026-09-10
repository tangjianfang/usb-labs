// w1_diag.h — W1 诊断三件套（计划 MS5-T10）：能力档案 / ghost 扫描 / WinUSB 向导 VM
#pragma once

#include "../app/log.h"
#include "../framework/json_mini.h"
#include "../framework/win32_rai.h"

#include <string>
#include <vector>

namespace usts::shell::w1 {

// ---------------------------------------------------------------------------
// ① 能力档案（设计 §1.2：设备全景清单 JSON——给应用团队的兼容性矩阵数据）
// ---------------------------------------------------------------------------
struct DeviceSummary {
    std::wstring vid = L"0x2341", pid = L"0x0002";
    std::wstring name = L"USB-Lab 键鼠";
    std::wstring speed = L"Full-Speed";
    std::vector<std::wstring> interfaces;   // "HID 键盘(boot)" / "HID 鼠标(boot)" …
    std::vector<std::wstring> endpoints;    // "EP1 IN 中断 8B/10ms"
    std::wstring driver = L"hidusb.sys";
};
std::string capability_json(const DeviceSummary& d);

// ---------------------------------------------------------------------------
// ② Ghost 设备扫描数据层（注入设备列表+在场标志；枚举源归 UI 域）
// ---------------------------------------------------------------------------
struct DeviceRow {
    std::wstring instance_id;
    bool present = true;      // 当前总线在场？
    std::wstring name;
};
std::vector<DeviceRow> scan_ghosts(const std::vector<DeviceRow>& devices);

// ---------------------------------------------------------------------------
// ③ WinUSB 绑定向导三步 VM（选设备→生成兼容 ID→绑定校验；实绑定动作归 UI 域）
// ---------------------------------------------------------------------------
struct WinUsbWizard {
    int step = 0;                       // 0 选设备 1 兼容 ID 2 绑定校验
    std::wstring vidpid;                // "USB\VID_2341&PID_0002"
    std::wstring compat_id;             // 生成的兼容 ID

    std::wstring validate() const {
        if (step == 0 && vidpid.empty()) return L"请选择设备";
        if (step == 1 && compat_id.empty()) return L"兼容 ID 生成失败（VID/PID 形状非法）";
        return L"";
    }
    bool next() {
        if (!validate().empty()) return false;
        if (step >= 2) return false;
        if (step == 0) compat_id = make_compat_id(vidpid);
        ++step;
        return true;
    }
    bool back() { return step > 0 && (--step, true); }

    // "USB\VID_2341&PID_0002" → "USB\VID_2341&PID_0002&REV_0100"（兼容 ID 生成口径：
    // 保 VID/PID 段；非 USB\ 前缀=空（校验失败））
    static std::wstring make_compat_id(const std::wstring& vidpid);
};

// ---------------------------------------------------------------------------
// 实现（header-only；模块日志 w1.diag）
// ---------------------------------------------------------------------------
inline std::string capability_json(const DeviceSummary& d) {
    auto log = ustlog::logger("w1.diag");
    wraii::json_writer w;
    w.begin_object();
    w.key(L"v").int_value(1);
    w.key(L"name").string_value(d.name);
    w.key(L"vid").string_value(d.vid);
    w.key(L"pid").string_value(d.pid);
    w.key(L"speed").string_value(d.speed);
    w.key(L"driver").string_value(d.driver);
    w.key(L"interfaces").begin_array();
    for (const auto& i : d.interfaces) w.string_value(i);
    w.end_array();
    w.key(L"endpoints").begin_array();
    for (const auto& e : d.endpoints) w.string_value(e);
    w.end_array();
    w.end_object();
    log->debug("capability_json: {} 接口 {} 端点", d.interfaces.size(), d.endpoints.size());
    return wraii::wide_to_utf8(w.result());
}

inline std::vector<DeviceRow> scan_ghosts(const std::vector<DeviceRow>& devices) {
    auto log = ustlog::logger("w1.diag");
    std::vector<DeviceRow> out;
    for (const auto& d : devices)
        if (!d.present) out.push_back(d);
    log->debug("scan_ghosts: {}/{} ghost", out.size(), devices.size());
    return out;
}

inline std::wstring WinUsbWizard::make_compat_id(const std::wstring& vidpid) {
    const std::wstring prefix = L"USB\\VID_";
    if (vidpid.size() < prefix.size() ||
        _wcsnicmp(vidpid.c_str(), prefix.c_str(), prefix.size()) != 0)
        return L"";
    const auto amp = vidpid.find(L'&');
    if (amp == std::wstring::npos || vidpid.find(L"PID_") == std::wstring::npos) return L"";
    return vidpid + L"&REV_0100";
}

} // namespace usts::shell::w1
