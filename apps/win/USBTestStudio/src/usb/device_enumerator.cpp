// device_enumerator.cpp — SetupDiGetClassDevsW/SetupDiEnumDeviceInterfaces/
// SetupDiGetDeviceInterfaceDetailW 两段式枚举 + CM_Get_DevNode_PropertyW 取
// HardwareIds / BusReportedDeviceDesc / InstanceId。
// 未真机编译，按 MSDN 口径编写（不确定点见文件尾与 README）。
#include "usb/device_enumerator.h"

#include "framework/win32_rai.h"

#include <initguid.h>   // DEVPKEY_* 常量在本 TU 实例化（必须先于 devpropdef/cfgmgr32）
#include <cfgmgr32.h>
#include <devpkey.h>
#include <hidsdi.h>
#include <initguid.h>   // 必须在 devpkey.h 之前：实例化 DEVPKEY_*（仅本编译单元）
#include <setupapi.h>

#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "hid.lib")

namespace {

// GUID_DEVINTERFACE_USB_DEVICE（usbiodef.h 同值；此处自行定义避免 initguid 全局实例化冲突）
// {A5DCBF10-6530-11D2-901F-00C04FB951ED}
constexpr GUID kUsbDeviceGuid = {0xA5DCBF10, 0x6530, 0x11D2,
                                 {0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED}};

// 读取设备节点字符串属性（DEVPROP_TYPE_STRING 或 STRING_LIST）。
// 两段式：先取所需字节数（CR_BUFFER_SMALL），再取数据（以 MSDN 为准）。
std::wstring devprop_string(DEVINST dn, const DEVPROPKEY& key, bool* ok = nullptr) {
    if (ok) *ok = false;
    if (dn == 0) return {};
    DEVPROPTYPE type = DEVPROP_TYPE_EMPTY;
    ULONG size = 0;
    CONFIGRET cr = ::CM_Get_DevNode_PropertyW(dn, &key, &type, nullptr, &size, 0);
    if (cr != CR_BUFFER_SMALL || size == 0) return {};
    std::wstring buf(size / sizeof(wchar_t) + 1, L'\0');
    cr = ::CM_Get_DevNode_PropertyW(dn, &key, &type, reinterpret_cast<PBYTE>(buf.data()), &size, 0);
    if (cr != CR_SUCCESS) return {};
    if (type != DEVPROP_TYPE_STRING && type != DEVPROP_TYPE_STRING_LIST) return {};
    if (ok) *ok = true;
    // 去掉尾部终止 NUL（STRING_LIST 的内部分隔 NUL 保留，供 split 使用）
    size_t chars = size / sizeof(wchar_t);
    while (chars > 0 && buf[chars - 1] == L'\0') --chars;
    buf.resize(chars);
    return buf;
}

// DEVPROP_TYPE_STRING_LIST：多字符串以 \0 分隔、\0\0 结束
std::vector<std::wstring> split_string_list(const std::wstring& list) {
    std::vector<std::wstring> out;
    size_t start = 0;
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i] == L'\0') {
            if (i > start) out.push_back(list.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < list.size()) out.push_back(list.substr(start));
    return out;
}

bool parse_hex4(const std::wstring& s, size_t pos, unsigned* v) {
    if (pos + 4 > s.size()) return false;
    unsigned val = 0;
    for (int k = 0; k < 4; ++k) {
        wchar_t c = s[pos + static_cast<size_t>(k)];
        unsigned d;
        if (c >= L'0' && c <= L'9')      d = static_cast<unsigned>(c - L'0');
        else if (c >= L'a' && c <= L'f') d = static_cast<unsigned>(c - L'a') + 10u;
        else if (c >= L'A' && c <= L'F') d = static_cast<unsigned>(c - L'A') + 10u;
        else return false;
        val = val * 16u + d;
    }
    *v = val;
    return true;
}

// HardwareIds 形如 USB\VID_046D&PID_C52B 或 HID\VID_046D&PID_C52B&MI_00&COL01
bool parse_vid_pid_token(const std::wstring& hwid, unsigned* vid, unsigned* pid) {
    size_t v = hwid.find(L"VID_");
    size_t p = hwid.find(L"PID_");
    if (v == std::wstring::npos || p == std::wstring::npos) return false;
    unsigned vv = 0, pp = 0;
    if (!parse_hex4(hwid, v + 4, &vv) || !parse_hex4(hwid, p + 4, &pp)) return false;
    *vid = vv;
    *pid = pp;
    return true;
}

bool parse_vid_pid_from_list(const std::wstring& list, unsigned* vid, unsigned* pid) {
    for (const auto& token : split_string_list(list)) {
        unsigned v = 0, p = 0;
        if (parse_vid_pid_token(token, &v, &p)) {
            *vid = v;
            *pid = p;
            return true;
        }
    }
    return false;
}

// 以“仅查询”方式打开 HID 取产品字符串（best-effort，失败返回空）
std::wstring hid_product_string(const std::wstring& path) {
    wraii::uhandle<wraii::handle_closer> h(::CreateFileW(
        path.c_str(), 0 /*查询权限*/, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, 0, nullptr));
    if (!h.valid()) {
        h.reset(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, 0, nullptr));
        if (!h.valid()) return {};
    }
    wchar_t buf[256] = {};
    // HidD_GetProductString 缓冲区需为 2 的倍数字节（以 MSDN 为准）
    if (!::HidD_GetProductString(h.get(), buf, sizeof(buf))) return {};
    buf[255] = L'\0';
    return buf;
}

void scan_interface_class(const GUID& guid, const wchar_t* cls, std::vector<DeviceInfo>& out,
                          std::wstring* err) {
    wraii::uhandle<wraii::hdevinfo_closer> set(::SetupDiGetClassDevsW(
        &guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
    if (!set.valid()) {
        if (err && err->empty()) *err = wraii::win_err(L"SetupDiGetClassDevsW", ::GetLastError());
        return;
    }

    SP_DEVICE_INTERFACE_DATA ifData{};
    ifData.cbSize = sizeof(ifData);   // 以 MSDN 为准：cbSize 必须显式初始化

    for (DWORD i = 0;; ++i) {
        if (!::SetupDiEnumDeviceInterfaces(set.get(), nullptr, &guid, i, &ifData)) {
            DWORD e = ::GetLastError();
            if (e != ERROR_NO_MORE_ITEMS && err && err->empty())
                *err = wraii::win_err(L"SetupDiEnumDeviceInterfaces", e);
            break;
        }

        // 第一段：取所需缓冲区大小（预期 ERROR_INSUFFICIENT_BUFFER）
        DWORD needed = 0;
        if (!::SetupDiGetDeviceInterfaceDetailW(set.get(), &ifData, nullptr, 0, &needed, nullptr) &&
            ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            if (err && err->empty()) *err = wraii::win_err(L"SetupDiGetDeviceInterfaceDetailW", ::GetLastError());
            continue;
        }
        if (needed == 0) continue;

        std::vector<uint8_t> buf(needed);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
        // x64 上 SP_DEVICE_INTERFACE_DETAIL_DATA_W 按头文件打包 8 字节对齐，
        // 取 sizeof 即可与 MSDN 口径一致（cbSize = sizeof 结构含一个 WCHAR）。
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        SP_DEVINFO_DATA devInfo{};
        devInfo.cbSize = sizeof(devInfo);
        if (!::SetupDiGetDeviceInterfaceDetailW(set.get(), &ifData, detail, needed, nullptr,
                                                &devInfo)) {
            if (err && err->empty()) *err = wraii::win_err(L"SetupDiGetDeviceInterfaceDetailW", ::GetLastError());
            continue;
        }

        DeviceInfo d;
        d.path = detail->DevicePath;
        d.class_name = cls;
        d.interface_guid = guid;

        DEVINST dn = devInfo.DevInst;

        // HardwareIds → VID/PID（自身节点；HID 收集节点若解析失败，回退父 USB 设备节点）
        unsigned vid = 0, pid = 0;
        std::wstring hwids = devprop_string(dn, DEVPKEY_Device_HardwareIds);
        if (!parse_vid_pid_from_list(hwids, &vid, &pid)) {
            DEVINST parent = 0;
            if (::CM_Get_Parent(&parent, dn, 0) == CR_SUCCESS) {
                std::wstring phw = devprop_string(parent, DEVPKEY_Device_HardwareIds);
                parse_vid_pid_from_list(phw, &vid, &pid);
                std::wstring brd = devprop_string(parent, DEVPKEY_Device_BusReportedDeviceDesc);
                if (!brd.empty()) d.name = brd;   // 总线上报的描述串（Win8+，取不到则回退）
            }
        }
        d.vid = vid;
        d.pid = pid;

        if (d.name.empty()) d.name = devprop_string(dn, DEVPKEY_Device_FriendlyName);
        if (d.name.empty() && d.class_name == L"HID") d.name = hid_product_string(d.path);
        d.instance_path = devprop_string(dn, DEVPKEY_Device_InstanceId);

        out.push_back(std::move(d));
    }
}

} // namespace

std::vector<DeviceInfo> DeviceEnumerator::scan(std::wstring* err) {
    std::vector<DeviceInfo> out;
    GUID hidGuid = {};
    ::HidD_GetHidGuid(&hidGuid);
    scan_interface_class(kUsbDeviceGuid, L"USB", out, err);
    scan_interface_class(hidGuid, L"HID", out, err);
    return out;
}

std::wstring DeviceEnumerator::summary(const DeviceInfo& d) {
    return wraii::fmt_v(L"[%s] VID_%04X&PID_%04X %s", d.class_name.c_str(), d.vid, d.pid,
                        d.name.empty() ? d.instance_path.c_str() : d.name.c_str());
}
