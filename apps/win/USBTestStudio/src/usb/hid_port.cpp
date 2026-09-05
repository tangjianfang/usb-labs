// hid_port.cpp — HID 重叠 I/O 与回报率测量。未真机编译，按 MSDN 口径编写。
#include "usb/hid_port.h"

#include <cstdio>
#include <cstring>

#pragma comment(lib, "hid.lib")

// ---------------------------------------------------------------------------
// HidPort
// ---------------------------------------------------------------------------
bool HidPort::open(const std::wstring& path, std::wstring* err) {
    close();
    m_path = path;

    m_handle.reset(::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OVERLAPPED, nullptr));
    if (m_handle.valid()) {
        m_caps.write_capable = true;
    } else {
        // 只读回退（键盘/系统集合等常见拒绝写）
        m_handle.reset(::CreateFileW(path.c_str(), GENERIC_READ,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_OVERLAPPED, nullptr));
        if (!m_handle.valid()) {
            if (err) *err = wraii::win_err(L"CreateFileW(HID)", ::GetLastError());
            m_path.clear();
            return false;
        }
    }

    if (!fill_caps(err)) {
        close();
        return false;
    }

    wchar_t product[256] = {};
    if (::HidD_GetProductString(m_handle.get(), product, sizeof(product))) {
        product[255] = L'\0';
        m_caps.product = product;
    }
    return true;
}

bool HidPort::fill_caps(std::wstring* err) {
    PHIDP_PREPARSED_DATA preparsed = nullptr;
    if (!::HidD_GetPreparsedData(m_handle.get(), &preparsed) || preparsed == nullptr) {
        if (err) *err = wraii::win_err(L"HidD_GetPreparsedData", ::GetLastError());
        return false;
    }
    // RAII：任何退出路径都释放 PreparsedData
    struct PreparsedGuard {
        PHIDP_PREPARSED_DATA p;
        ~PreparsedGuard() { if (p) ::HidD_FreePreparsedData(p); }
    } guard{preparsed};

    HIDP_CAPS caps{};
    if (::HidP_GetCaps(preparsed, &caps) != HIDP_STATUS_SUCCESS) {
        if (err) *err = L"HidP_GetCaps 失败（HIDP_STATUS 非 SUCCESS）";
        return false;
    }

    HIDD_ATTRIBUTES attr{};
    attr.Size = sizeof(attr);
    if (::HidD_GetAttributes(m_handle.get(), &attr)) {
        m_caps.vid = attr.VendorID;
        m_caps.pid = attr.ProductID;
        m_caps.version = attr.VersionNumber;
    }

    m_caps.usage_page = caps.UsagePage;
    m_caps.usage = caps.Usage;
    m_caps.input_report_len = caps.InputReportByteLength;
    m_caps.output_report_len = caps.OutputReportByteLength;
    m_caps.feature_report_len = caps.FeatureReportByteLength;
    return true;
}

bool HidPort::set_num_input_buffers(ULONG count) {
    if (!m_handle.valid()) return false;
    // 以 MSDN 为准：取值范围 [2, 512]
    if (count < 2) count = 2;
    if (count > 512) count = 512;
    return ::HidD_SetNumInputBuffers(m_handle.get(), count) != FALSE;
}

bool HidPort::read_overlapped(std::vector<uint8_t>& report, unsigned timeout_ms, bool* timed_out,
                              std::wstring* err) {
    if (timed_out) *timed_out = false;
    if (!m_handle.valid()) {
        if (err) *err = L"HID 未打开";
        return false;
    }
    if (m_caps.input_report_len == 0) {
        if (err) *err = L"InputReportByteLength 为 0";
        return false;
    }

    report.assign(m_caps.input_report_len, 0);

    OVERLAPPED ov{};
    ov.hEvent = ::CreateEventW(nullptr, TRUE /*手动复位*/, FALSE, nullptr);
    if (ov.hEvent == nullptr) {
        if (err) *err = wraii::win_err(L"CreateEventW", ::GetLastError());
        return false;
    }
    wraii::uhandle<wraii::handle_closer> evt(ov.hEvent);

    DWORD got = 0;
    if (!::ReadFile(m_handle.get(), report.data(), static_cast<DWORD>(report.size()), nullptr,
                    &ov)) {
        DWORD e = ::GetLastError();
        if (e != ERROR_IO_PENDING) {
            // ERROR_OPERATION_ABORTED/ERROR_GEN_FAILURE：设备中途拔出等
            if (err) *err = wraii::win_err(L"ReadFile(HID)", e);
            return false;
        }
        DWORD wait = ::WaitForSingleObject(ov.hEvent, timeout_ms);
        if (wait == WAIT_TIMEOUT) {
            // 超时：取消本次 I/O 并等待操作回收（以 MSDN 为准）
            ::CancelIoEx(m_handle.get(), &ov);
            DWORD dummy = 0;
            ::GetOverlappedResult(m_handle.get(), &ov, &dummy, TRUE);
            if (timed_out) *timed_out = true;
            return false;
        }
        if (wait != WAIT_OBJECT_0) {
            if (err) *err = wraii::win_err(L"WaitForSingleObject", ::GetLastError());
            return false;
        }
        if (!::GetOverlappedResult(m_handle.get(), &ov, &got, FALSE)) {
            DWORD e2 = ::GetLastError();
            if (e2 == ERROR_OPERATION_ABORTED) {
                if (timed_out) *timed_out = true;   // 竞态下被取消，按超时口径处理
                return false;
            }
            if (err) *err = wraii::win_err(L"GetOverlappedResult", e2);
            return false;
        }
    } else {
        // 句柄未进 PENDING 即完成（不太可能，仍按完成处理）
        if (!::GetOverlappedResult(m_handle.get(), &ov, &got, FALSE)) {
            if (err) *err = wraii::win_err(L"GetOverlappedResult", ::GetLastError());
            return false;
        }
    }

    if (got == 0) {
        if (err) *err = L"读到 0 字节";
        return false;
    }
    report.resize(got);
    return true;
}

bool HidPort::set_output_report(const uint8_t* data, size_t len, std::wstring* err) {
    if (!m_handle.valid()) {
        if (err) *err = L"HID 未打开";
        return false;
    }
    if (m_caps.output_report_len == 0) {
        if (err) *err = L"设备无输出报告（OutputReportByteLength=0）";
        return false;
    }
    if (len == 0 || len > m_caps.output_report_len) {
        if (err)
            *err = wraii::fmt_v(L"输出报告长度 %zu 超出 [1, %hu]", len, m_caps.output_report_len);
        return false;
    }

    std::vector<uint8_t> buf(m_caps.output_report_len, 0);
    ::memcpy(buf.data(), data, len);

    // HidD_SetOutputReport 内部为同步控制传输。若在 FILE_FLAG_OVERLAPPED 句柄上调用失败，
    // 回退为“临时同步句柄”再试一次（组合行为以 MSDN 为准，见 README 不确定点）。
    if (::HidD_SetOutputReport(m_handle.get(), buf.data(),
                               static_cast<ULONG>(buf.size())) != FALSE)
        return true;

    DWORD sync_err = ::GetLastError();
    HANDLE h2 = ::CreateFileW(m_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                              nullptr);
    if (h2 == INVALID_HANDLE_VALUE) {
        if (err) *err = wraii::win_err(L"HidD_SetOutputReport", sync_err);
        return false;
    }
    bool ok = ::HidD_SetOutputReport(h2, buf.data(), static_cast<ULONG>(buf.size())) != FALSE;
    ::CloseHandle(h2);
    if (!ok && err) *err = wraii::win_err(L"HidD_SetOutputReport", ::GetLastError());
    return ok;
}

// ---------------------------------------------------------------------------
// 回报率测量（QPC 直方图）
// ---------------------------------------------------------------------------
HidPollResult hid_polling_rate_measure(const std::wstring& path, int seconds,
                                       const std::function<bool()>& cancelled) {
    HidPollResult r;
    std::function<bool()> never = [] { return false; };
    const std::function<bool()>& stop = cancelled ? cancelled : never;   // 两侧均为左值，可安全绑定引用

    HidPort port;
    std::wstring err;
    if (!port.open(path, &err)) {
        r.detail = L"打开 HID 失败: " + err;
        return r;
    }
    port.set_num_input_buffers(512);   // 尽量不丢报告（best-effort）

    // 排空排队中的历史报告（最多 512 次、每次 1ms 超时）
    for (int k = 0; k < 512; ++k) {
        std::vector<uint8_t> junk;
        bool to = false;
        if (!port.read_overlapped(junk, 1, &to) && to) break;
    }

    LARGE_INTEGER qpf{};
    if (!::QueryPerformanceFrequency(&qpf) || qpf.QuadPart == 0) {
        r.detail = L"QueryPerformanceFrequency 失败";
        return r;
    }

    constexpr unsigned kHistBuckets = 256;   // 256 × 100us = 25.6ms 量程
    r.hist.assign(kHistBuckets, 0);
    r.hist_bucket_us = 100;

    LARGE_INTEGER t0{};
    ::QueryPerformanceCounter(&t0);
    double total_s = 0;
    double last_s = -1;
    double min_hz = 1e18, max_hz = 0;
    unsigned samples = 0;
    unsigned consec_timeout = 0;
    const double limit_s = static_cast<double>(seconds);

    while (true) {
        if (stop()) break;
        LARGE_INTEGER now{};
        ::QueryPerformanceCounter(&now);
        if ((now.QuadPart - t0.QuadPart) / static_cast<double>(qpf.QuadPart) >= limit_s) break;

        std::vector<uint8_t> report;
        bool to = false;
        err.clear();
        if (port.read_overlapped(report, 200, &to, &err)) {
            ::QueryPerformanceCounter(&now);
            double t = (now.QuadPart - t0.QuadPart) / static_cast<double>(qpf.QuadPart);
            if (last_s >= 0) {
                double dt = t - last_s;
                if (dt > 0) {
                    total_s += dt;
                    double hz = 1.0 / dt;
                    if (hz < min_hz) min_hz = hz;
                    if (hz > max_hz) max_hz = hz;
                    size_t idx = static_cast<size_t>(dt * 1e6 / r.hist_bucket_us);
                    if (idx < kHistBuckets)
                        r.hist[idx] += 1;
                    else
                        r.hist_over += 1;
                }
            }
            last_s = t;
            ++samples;
            consec_timeout = 0;
        } else if (to) {
            if (++consec_timeout >= 5) {
                r.detail = L"连续 5 次读取超时：设备未上报（需移动鼠标/触发输入）";
                break;
            }
        } else {
            r.detail = L"读取失败: " + err;   // 常见为设备拔出
            break;
        }
    }

    LARGE_INTEGER t1{};
    ::QueryPerformanceCounter(&t1);
    r.total_s = (t1.QuadPart - t0.QuadPart) / static_cast<double>(qpf.QuadPart);
    r.samples = samples;
    r.avg_hz = total_s > 0 ? static_cast<double>(samples > 0 ? samples - 1 : 0) / total_s : 0;
    r.min_hz = samples >= 2 ? min_hz : 0;
    r.max_hz = samples >= 2 ? max_hz : 0;
    r.ok = samples >= 10 && r.avg_hz > 0;
    if (r.ok && r.detail.empty()) {
        r.detail = wraii::fmt_v(L"样本 %u，均值 %.1f Hz，瞬时 [%.1f, %.1f] Hz", samples, r.avg_hz,
                                r.min_hz, r.max_hz);
    }
    return r;
}
