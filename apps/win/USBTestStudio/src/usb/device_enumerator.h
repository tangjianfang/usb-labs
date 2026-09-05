// device_enumerator.h — USB / HID 设备接口枚举（SetupDi + CfgMgr32）。
// 未真机编译，按 MSDN 口径编写。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <vector>

struct DeviceInfo {
    std::wstring path;           // CreateFileW 可打开的设备接口路径
    std::wstring class_name;     // L"USB" 或 L"HID"
    unsigned vid = 0;            // 从 HardwareIds 解析（自身节点，失败回退父节点）
    unsigned pid = 0;
    std::wstring name;           // BusReportedDeviceDesc → FriendlyName → HID 产品字符串
    std::wstring instance_path;  // DEVPKEY_Device_InstanceId
    GUID interface_guid{};
};

class DeviceEnumerator {
public:
    // 扫描“USB 设备”与“HID 收集”两类设备接口。err 仅记录首个非致命错误（枚举继续）。
    static std::vector<DeviceInfo> scan(std::wstring* err = nullptr);

    // 单行摘要：L"HID VID_046D&PID_C52B Logitech 接收器"
    static std::wstring summary(const DeviceInfo& d);
};
