// serial_port.h — CDC 串口访问：CreateFileW(\\.\COMx) + DCB + 重叠 ReadFile/WriteFile；
// serial_loopback_test（与 tools/usbtest/cdc_test.py 同图案口径）；
// pd_telemetry_collect（与 tools/usbtest/pd_test.py 同“key=value 行”遥测契约）。
// 未真机编译，按 MSDN 口径编写。
#pragma once

#include "framework/win32_rai.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

class SerialPort {
public:
    // L"COM7" → L"\\\\.\\COM7"（COM10 及以上必须用 \\.\ 前缀）
    static std::wstring device_path(const std::wstring& port);

    bool open(const std::wstring& port, unsigned baud, std::wstring* err = nullptr);
    void close() noexcept { m_handle.reset(); }
    bool is_open() const noexcept { return m_handle.valid(); }
    const std::wstring& port() const noexcept { return m_port; }

    // parity: NOPARITY/EVENPARITY/ODDPARITY...（winbase.h 常量，默认 8N1）
    bool configure_dcb(unsigned baud, unsigned data_bits = 8, unsigned stop_bits = ONESTOPBIT,
                       unsigned char parity = NOPARITY, std::wstring* err = nullptr);

    bool purge(std::wstring* err = nullptr);   // 清收发缓冲

    bool write_all(const uint8_t* data, size_t len, std::wstring* err = nullptr);

    // 尽力读：超时前读到的任意字节（0 字节且未出错也是正常返回）
    bool read_some(uint8_t* buf, size_t cap, unsigned timeout_ms, size_t* got,
                   std::wstring* err = nullptr);

    // 读满 len 字节或超时失败（总超时控制）
    bool read_exact(uint8_t* buf, size_t len, unsigned total_timeout_ms, std::wstring* err);

private:
    bool overlapped_xfer(bool write, void* buf, DWORD len, DWORD* done, unsigned timeout_ms,
                         std::wstring* err);

    wraii::uhandle<wraii::handle_closer> m_handle;
    std::wstring m_port;
};

// ---------------------------------------------------------------------------
// 串口环回（工装 TX-RX 短接或固件回显）
// ---------------------------------------------------------------------------
struct SerialLoopbackResult {
    bool ok = false;
    unsigned repeat = 0;              // 图案重复次数
    unsigned long long bytes_written = 0;
    unsigned long long bytes_read = 0;    // 实际回读字节数
    unsigned mismatch_at = 0xFFFFFFFF;    // 首个不匹配偏移
    double bytes_per_sec = 0;
    std::wstring detail;
};

// 图案与 tools/usbtest/cdc_test.py 一致：bytes(range(256)) * repeat
SerialLoopbackResult serial_loopback_test(SerialPort& port, unsigned repeat, unsigned total_timeout_ms,
                                          const std::function<bool()>& cancelled = nullptr);

// ---------------------------------------------------------------------------
// PD 遥测（与 tools/usbtest/pd_test.py 的 _telemetry 契约一致）：
// 固件经 CDC 输出形如 "cc_state=Attached.SRC" / "contract_v=5.06" 的 key=value 行；
// 收齐全部 keys 即提前结束，否则读满 window_ms。
// ---------------------------------------------------------------------------
struct PdTelemetryResult {
    bool ok = false;                                // 是否收齐全部 keys
    std::map<std::wstring, std::wstring> values;    // key → 原始字符串值
    unsigned lines_total = 0;                       // 收到的总行数
    std::vector<std::wstring> raw_lines;            // 调试用原始行（最多 64 行）
    std::wstring detail;
};

PdTelemetryResult pd_telemetry_collect(SerialPort& port, const std::vector<std::wstring>& keys,
                                       const std::wstring& command /*可空，发送后附 \n*/,
                                       int window_ms,
                                       const std::function<bool()>& cancelled = nullptr);
