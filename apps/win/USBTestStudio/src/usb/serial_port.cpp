// serial_port.cpp — 串口重叠 I/O、环回与 PD 遥测采集。
// 日志：channel.serial（info=开关口，debug=读写细节，err=失败带 GetLastError）。
#include "usb/serial_port.h"
#include "app/log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
auto slog = ustlog::logger("channel.serial");
} // namespace

// ---------------------------------------------------------------------------
// SerialPort
// ---------------------------------------------------------------------------
std::wstring SerialPort::device_path(const std::wstring& port) {
    if (port.rfind(L"\\\\.\\", 0) == 0) return port;   // 已带 \\.\ 前缀
    return L"\\\\.\\" + port;
}

bool SerialPort::open(const std::wstring& port, unsigned baud, std::wstring* err) {
    close();
    m_port = port;
    const ULONGLONG t0 = ::GetTickCount64();
    slog->info("打开串口 {} @{} 8N1", ustlog::w2u(device_path(port)), baud);
    m_handle.reset(::CreateFileW(device_path(port).c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    if (!m_handle.valid()) {
        const std::wstring e = wraii::win_err(L"CreateFileW(COM)", ::GetLastError());
        if (err) *err = e;
        slog->error("打开串口 {} 失败（{} ms）：{}", ustlog::w2u(port), ::GetTickCount64() - t0,
                  ustlog::w2u(e));
        m_port.clear();
        return false;
    }
    if (!configure_dcb(baud, 8, ONESTOPBIT, NOPARITY, err)) {
        slog->error("串口 {} DCB 配置失败", ustlog::w2u(port));
        close();
        return false;
    }
    purge();
    slog->info("串口 {} 打开完成（{} ms）", ustlog::w2u(port), ::GetTickCount64() - t0);
    return true;
}

bool SerialPort::configure_dcb(unsigned baud, unsigned data_bits, unsigned stop_bits,
                               unsigned char parity, std::wstring* err) {
    if (!m_handle.valid()) {
        if (err) *err = L"串口未打开";
        return false;
    }
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!::GetCommState(m_handle.get(), &dcb)) {
        if (err) *err = wraii::win_err(L"GetCommState", ::GetLastError());
        return false;
    }
    dcb.BaudRate = baud;
    dcb.ByteSize = static_cast<BYTE>(data_bits);
    dcb.StopBits = static_cast<BYTE>(stop_bits);
    dcb.Parity = parity;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;    // 产测常见：拉起 DTR/RTS
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    if (!::SetCommState(m_handle.get(), &dcb)) {
        if (err) *err = wraii::win_err(L"SetCommState", ::GetLastError());
        slog->error("SetCommState 失败 baud={} data={} stop={} parity={}: {}", baud, data_bits,
                  stop_bits, parity, err ? ustlog::w2u(*err) : std::string{});
        return false;
    }

    // 超时仅作内核侧兜底；read_exact/read_some 用自身 deadline 控制（以 MSDN 为准）
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout = 20;                 // 20ms 字节间隔即认为帧结束
    to.ReadTotalTimeoutMultiplier = 1;           // 每请求字节 ~1ms
    to.ReadTotalTimeoutConstant = 100;           // 基础 100ms
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant = 2000;
    if (!::SetCommTimeouts(m_handle.get(), &to)) {
        if (err) *err = wraii::win_err(L"SetCommTimeouts", ::GetLastError());
        return false;
    }
    return true;
}

bool SerialPort::purge(std::wstring* err) {
    if (!m_handle.valid()) return false;
    if (!::PurgeComm(m_handle.get(), PURGE_TXABORT | PURGE_RXABORT | PURGE_TXCLEAR | PURGE_RXCLEAR)) {
        if (err) *err = wraii::win_err(L"PurgeComm", ::GetLastError());
        return false;
    }
    return true;
}

bool SerialPort::overlapped_xfer(bool write, void* buf, DWORD len, DWORD* done,
                                 unsigned timeout_ms, std::wstring* err) {
    *done = 0;
    OVERLAPPED ov{};
    ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) {
        if (err) *err = wraii::win_err(L"CreateEventW", ::GetLastError());
        return false;
    }
    wraii::uhandle<wraii::handle_closer> evt(ov.hEvent);

    const ULONGLONG t0 = ::GetTickCount64();
    BOOL ok = FALSE;
    if (write) ok = ::WriteFile(m_handle.get(), buf, len, nullptr, &ov);
    else       ok = ::ReadFile(m_handle.get(), buf, len, nullptr, &ov);
    if (!ok) {
        DWORD e = ::GetLastError();
        if (e != ERROR_IO_PENDING) {
            if (err) *err = wraii::win_err(write ? L"WriteFile(COM)" : L"ReadFile(COM)", e);
            slog->error("{} {}B 失败 GetLastError=0x{:08X}：{}", write ? "WriteFile" : "ReadFile",
                      len, e, err ? ustlog::w2u(*err) : std::string{});
            return false;
        }
        DWORD wait = ::WaitForSingleObject(ov.hEvent, timeout_ms);
        if (wait != WAIT_OBJECT_0) {
            ::CancelIoEx(m_handle.get(), &ov);
            DWORD dummy = 0;
            ::GetOverlappedResult(m_handle.get(), &ov, &dummy, TRUE);
            if (err) *err = write ? L"写超时" : L"读超时";
            slog->warn("{} {}B 超时 {}ms（CancelIoEx 已投递）", write ? "WriteFile" : "ReadFile",
                       len, timeout_ms);
            return false;
        }
        if (!::GetOverlappedResult(m_handle.get(), &ov, done, FALSE)) {
            if (err) *err = wraii::win_err(L"GetOverlappedResult", ::GetLastError());
            slog->error("GetOverlappedResult 失败 GetLastError=0x{:08X}", ::GetLastError());
            return false;
        }
    } else {
        if (!::GetOverlappedResult(m_handle.get(), &ov, done, FALSE)) {
            if (err) *err = wraii::win_err(L"GetOverlappedResult", ::GetLastError());
            slog->error("GetOverlappedResult(同步完成) 失败 GetLastError=0x{:08X}", ::GetLastError());
            return false;
        }
    }
    slog->log(spdlog::level::debug, "{} {}B→{}B（{} ms）", write ? "TX" : "RX", len, *done,
              ::GetTickCount64() - t0);
    return true;
}

bool SerialPort::write_all(const uint8_t* data, size_t len, std::wstring* err) {
    if (!m_handle.valid()) {
        if (err) *err = L"串口未打开";
        return false;
    }
    size_t done = 0;
    while (done < len) {
        DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(len - done, 4096));
        DWORD wrote = 0;
        if (!overlapped_xfer(true, const_cast<uint8_t*>(data + done), chunk, &wrote, 5000, err))
            return false;
        if (wrote == 0) {
            if (err) *err = L"写入 0 字节";
            return false;
        }
        done += wrote;
    }
    return true;
}

bool SerialPort::read_some(uint8_t* buf, size_t cap, unsigned timeout_ms, size_t* got,
                           std::wstring* err) {
    *got = 0;
    if (!m_handle.valid()) {
        if (err) *err = L"串口未打开";
        return false;
    }
    if (cap == 0) return true;
    DWORD done = 0;
    if (!overlapped_xfer(false, buf, static_cast<DWORD>(cap), &done, timeout_ms, err))
        return false;
    *got = done;
    return true;
}

bool SerialPort::read_exact(uint8_t* buf, size_t len, unsigned total_timeout_ms, std::wstring* err) {
    size_t done = 0;
    ULONGLONG t0 = ::GetTickCount64();
    while (done < len) {
        ULONGLONG elapsed = ::GetTickCount64() - t0;
        if (elapsed >= total_timeout_ms) {
            if (err) *err = wraii::fmt_v(L"读超时：期望 %zu 字节，实收 %zu", len, done);
            return false;
        }
        unsigned left = static_cast<unsigned>(total_timeout_ms - elapsed);
        size_t got = 0;
        if (!read_some(buf + done, len - done, std::min<unsigned>(left, 500), &got, err))
            return false;
        if (got == 0) continue;   // 继续等 deadline
        done += got;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 环回（图案 = bytes(range(256)) * repeat，与 cdc_test.py 一致）
// ---------------------------------------------------------------------------
SerialLoopbackResult serial_loopback_test(SerialPort& port, unsigned repeat,
                                          unsigned total_timeout_ms,
                                          const std::function<bool()>& cancelled) {
    SerialLoopbackResult r;
    r.repeat = repeat == 0 ? 1 : repeat;
    std::vector<uint8_t> pattern;
    pattern.reserve(static_cast<size_t>(r.repeat) * 256);
    for (unsigned k = 0; k < r.repeat; ++k)
        for (int b = 0; b < 256; ++b)
            pattern.push_back(static_cast<uint8_t>(b));

    std::function<bool()> never = [] { return false; };
    const std::function<bool()>& stop = cancelled ? cancelled : never;

    port.purge();
    ULONGLONG t0 = ::GetTickCount64();

    std::wstring err;
    if (!port.write_all(pattern.data(), pattern.size(), &err)) {
        r.detail = L"写入失败: " + err;
        return r;
    }
    r.bytes_written = pattern.size();

    std::vector<uint8_t> back(pattern.size(), 0);
    size_t got_total = 0;
    while (got_total < back.size()) {
        if (stop()) {
            r.detail = L"被中止";
            return r;
        }
        ULONGLONG elapsed = ::GetTickCount64() - t0;
        if (elapsed >= total_timeout_ms) break;
        size_t got = 0;
        unsigned slice = std::min<unsigned>(static_cast<unsigned>(total_timeout_ms - elapsed), 500);
        if (!port.read_some(back.data() + got_total, back.size() - got_total, slice, &got, &err))
            break;
        got_total += got;
        if (got == 0 && got_total == 0 && !err.empty()) break;
    }
    r.bytes_read = got_total;

    ULONGLONG dt = ::GetTickCount64() - t0;
    r.bytes_per_sec = dt > 0 ? static_cast<double>(got_total) * 1000.0 / static_cast<double>(dt) : 0;

    // 逐字节比对：报告首个不匹配偏移（Python 版口径为整体相等 + “回读不匹配@NB”）
    r.ok = got_total == pattern.size();
    if (r.ok) {
        for (size_t i = 0; i < pattern.size(); ++i) {
            if (back[i] != pattern[i]) {
                r.ok = false;
                r.mismatch_at = static_cast<unsigned>(i);
                r.detail = wraii::fmt_v(L"数据不匹配 @%zu（发 %02X 收 %02X）", i, pattern[i], back[i]);
                break;
            }
        }
    } else {
        r.detail = wraii::fmt_v(L"回读不匹配@%zu B", got_total);
    }
    if (r.ok) r.detail = wraii::fmt_v(L"%zu 字节环回一致，%.0f B/s", pattern.size(), r.bytes_per_sec);
    slog->log(r.ok ? spdlog::level::info : spdlog::level::err,
              "串口环回：{}（写 {}B 回读 {}B，{:.0f} B/s）", ustlog::w2u(r.detail),
              r.bytes_written, r.bytes_read, r.bytes_per_sec);
    return r;
}

// ---------------------------------------------------------------------------
// PD 遥测（key=value 行）
// ---------------------------------------------------------------------------
PdTelemetryResult pd_telemetry_collect(SerialPort& port, const std::vector<std::wstring>& keys,
                                       const std::wstring& command, int window_ms,
                                       const std::function<bool()>& cancelled) {
    PdTelemetryResult r;
    std::function<bool()> never = [] { return false; };
    const std::function<bool()>& stop = cancelled ? cancelled : never;

    port.purge();

    if (!command.empty()) {
        std::wstring cmd = command + L"\n";
        std::string bytes = wraii::wide_to_utf8(cmd);
        std::wstring err;
        if (!port.write_all(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), &err)) {
            r.detail = L"触发命令发送失败: " + err;
            return r;
        }
    }

    std::vector<uint8_t> chunk(1024, 0);
    std::string line;                  // ASCII/UTF-8 行累积
    ULONGLONG t0 = ::GetTickCount64();
    auto collect_line = [&](const std::string& l) {
        // 剥离 \r、截断到首个 NUL
        size_t n = l.find('\0');
        std::string s = n == std::string::npos ? l : l.substr(0, n);
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
        if (s.empty()) return;
        ++r.lines_total;
        std::wstring w = wraii::utf8_to_wide(s);
        if (r.raw_lines.size() < 64) r.raw_lines.push_back(w);
        size_t eq = w.find(L'=');
        if (eq == std::wstring::npos || eq == 0) return;
        std::wstring k = w.substr(0, eq);
        std::wstring v = w.substr(eq + 1);
        for (auto& want : keys) {
            if (k == want && r.values.find(k) == r.values.end()) {
                r.values[k] = v;   // 与 Python 版一致：每 key 取首个命中
                break;
            }
        }
    };

    while (true) {
        bool all = true;
        for (const auto& k : keys)
            if (r.values.find(k) == r.values.end()) { all = false; break; }
        if (all) break;            // 收齐提前结束（同 Python len(out) < len(keys) 口径）

        if (stop()) { r.detail = L"被中止"; break; }
        ULONGLONG elapsed = ::GetTickCount64() - t0;
        if (elapsed >= static_cast<ULONGLONG>(window_ms)) break;

        size_t got = 0;
        unsigned slice = std::min<unsigned>(static_cast<unsigned>(window_ms - static_cast<int>(elapsed)), 200);
        if (!port.read_some(chunk.data(), chunk.size(), slice, &got)) break;
        if (got == 0) continue;
        for (size_t i = 0; i < got; ++i) {
            char c = static_cast<char>(chunk[i]);
            if (c == '\n') {
                collect_line(line);
                line.clear();
            } else {
                line.push_back(c);
                if (line.size() >= 512) {   // 异常长行保护
                    collect_line(line);
                    line.clear();
                }
            }
        }
    }
    if (!line.empty()) collect_line(line);

    bool all = !keys.empty();
    for (const auto& k : keys)
        if (r.values.find(k) == r.values.end()) { all = false; break; }
    r.ok = all;
    if (!all && r.detail.empty())
        r.detail = wraii::fmt_v(L"窗口 %d ms 内收到 %u 行，未收齐 %zu 个键", window_ms,
                                r.lines_total, keys.size());
    if (r.ok)
        r.detail = wraii::fmt_v(L"%u 行内收齐 %zu 键", r.lines_total, keys.size());
    slog->log(r.ok ? spdlog::level::info : spdlog::level::warn,
              "PD 遥测采集：{}（keys={}/{}，原始行 {}）", ustlog::w2u(r.detail),
              r.values.size(), keys.size(), r.lines_total);
    for (const auto& [k, v] : r.values)
        slog->debug("PD 遥测 {}={}", ustlog::w2u(k), ustlog::w2u(v));
    return r;
}
