// serial_enum.h — EP-4 S2 设备发现·串口侧：COM 口枚举。
// 注册表 HKLM\HARDWARE\DEVICEMAP\SERIALCOMM 的每个值即一个串口（值名 =
// \Device\VCP0 形式的内核对象名，值数据 = "COM7"）——设备管理器同口径，
// 无 USB 设备也能离线运行。COM 名按数字排序（COM2 < COM10）。
// 日志：discovery（info=扫描开始/结束+计数，debug=逐端口枚举，err=注册表失败带 rc）。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "discovery/device_catalog.h"
#include "app/log.h"

namespace serial_enum {

// "COM7" → 7；非 "COM+纯数字" 形态返回 0
inline unsigned com_number(const std::wstring& name) {
    auto eq = [](wchar_t c, wchar_t up) { return c == up || c == up + (L'a' - L'A'); };
    if (name.size() < 4 || !eq(name[0], L'C') || !eq(name[1], L'O') || !eq(name[2], L'M'))
        return 0;
    unsigned n = 0;
    for (size_t i = 3; i < name.size(); ++i) {
        if (name[i] < L'0' || name[i] > L'9') return 0;
        n = n * 10 + unsigned(name[i] - L'0');
        if (n > 4096) return 0;              // 防御病态长数字
    }
    return n;
}

// COM 口排序：号码数字序，号码相同（或均非法）回退字典序
inline bool com_name_less(const std::wstring& a, const std::wstring& b) {
    unsigned na = com_number(a), nb = com_number(b);
    if (na != nb) return na < nb;
    return a < b;
}

// 枚举本机 COM 名（已按 com_name_less 排序）。SERIALCOMM 键不存在 → 空表
// （本机无串口，不算错误）；其他打开失败写入 err。
inline std::vector<std::wstring> scan(std::wstring* err = nullptr) {
    std::vector<std::wstring> out;
    const ULONGLONG t0 = ::GetTickCount64();
    ustlog::logger("discovery")->info("开始扫描 COM 口（注册表 SERIALCOMM）");
    HKEY h = nullptr;
    LSTATUS rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM",
                               0, KEY_READ, &h);
    if (rc != ERROR_SUCCESS) {
        if (rc == ERROR_FILE_NOT_FOUND)
            ustlog::logger("discovery")->info("串口扫描：SERIALCOMM 键不存在，本机无串口（0 个）");
        else
            ustlog::logger("discovery")->error(
                "串口扫描失败：RegOpenKeyExW(SERIALCOMM) rc=0x{:08X}", static_cast<unsigned long>(rc));
        if (rc != ERROR_FILE_NOT_FOUND && err)
            *err = L"RegOpenKeyExW(SERIALCOMM) 失败: " + std::to_wstring(rc);
        return out;
    }
    for (DWORD i = 0;; ++i) {
        wchar_t vname[256];
        DWORD vname_len = 256;
        BYTE data[512];
        DWORD data_len = sizeof data;
        DWORD type = 0;
        rc = RegEnumValueW(h, i, vname, &vname_len, nullptr, &type, data, &data_len);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc != ERROR_SUCCESS || type != REG_SZ || data_len < sizeof(wchar_t)) continue;
        const wchar_t* s = reinterpret_cast<const wchar_t*>(data);
        size_t n = data_len / sizeof(wchar_t);
        if (n > 0 && s[n - 1] == L'\0') --n; // 值数据带/不带终止符两种写法都兼容
        std::wstring com(s, n);
        if (com_number(com) != 0) {
            ustlog::logger("discovery")->log(spdlog::level::debug, "枚举到串口：{} = {}",
                                             ustlog::w2u(vname), ustlog::w2u(com));
            out.push_back(std::move(com));
        }
    }
    RegCloseKey(h);
    std::sort(out.begin(), out.end(), com_name_less);
    ustlog::logger("discovery")->info("串口扫描完成：{} 个 COM 口（{} ms）", out.size(),
                                      ::GetTickCount64() - t0);
    return out;
}

// COM 名 → 目录条目（无 VID:PID；path 存 COM 名，SerialChannel 打开时自行转 \\.\ ）
inline ConsoleDevice make_device(std::wstring com_name) {
    ConsoleDevice d;
    d.kind = DeviceKind::serial;
    d.name = com_name;                        // 名称即端口名（USB CDC 的友好名由 S2 后半枚举侧补充）
    d.path = std::move(com_name);
    return d;
}

} // namespace serial_enum
