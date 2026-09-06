// winusb_port.cpp — WinUSB 管道访问实现。未真机运行，按 MSDN 口径编写。
#include "usb/winusb_port.h"

#include <cstdio>
#include <cstring>

#pragma comment(lib, "winusb.lib")

void winusb_iface_closer::close(H h) noexcept {
    if (h) ::WinUsb_Free(h);
}

// ---------------------------------------------------------------------------
// 纯逻辑：路径 VID/PID 解析与数据管道选型
// ---------------------------------------------------------------------------
void WinUsbPort::parse_vid_pid(const std::wstring& path, unsigned* vid, unsigned* pid) {
    *vid = 0;
    *pid = 0;
    // 形如 \\?\usb#vid_1234&pid_5678#…：找 "vid_" 后 1~4 位十六进制，再跳 "&pid_" 同法
    auto parse_hex4 = [](const wchar_t* p, unsigned* out) -> const wchar_t* {
        unsigned v = 0;
        int n = 0;
        while (n < 4 && *p) {
            wchar_t c = *p;
            if (c >= L'0' && c <= L'9') v = v * 16 + (c - L'0');
            else if (c >= L'A' && c <= L'F') v = v * 16 + (c - L'A' + 10);
            else if (c >= L'a' && c <= L'f') v = v * 16 + (c - L'a' + 10);
            else break;
            ++p;
            ++n;
        }
        *out = n ? v : 0;
        return n ? p : nullptr;
    };
    const wchar_t* s = path.c_str();
    while (*s) {
        if ((s[0] == L'v' || s[0] == L'V') && (s[1] == L'i' || s[1] == L'I') &&
            (s[2] == L'd' || s[2] == L'D') && s[3] == L'_') {
            unsigned v = 0, p = 0;
            const wchar_t* q = parse_hex4(s + 4, &v);
            if (q && q[0] == L'&' && (q[1] == L'p' || q[1] == L'P') && (q[2] == L'i' || q[2] == L'I') &&
                (q[3] == L'd' || q[3] == L'D') && q[4] == L'_') {
                parse_hex4(q + 5, &p);
                *vid = v;
                *pid = p;
            }
            break;      // 首个 vid_ 即设备自身（复合设备子接口路径不再嵌套 vid_）
        }
        ++s;
    }
}

bool WinUsbPort::select_data_pipes(const std::vector<WinUsbPipeInfo>& pipes,
                                   unsigned char* in_pipe, bool* in_is_bulk,
                                   unsigned char* out_pipe, bool* out_is_bulk) {
    *in_pipe = 0;
    *out_pipe = 0;
    *in_is_bulk = false;
    *out_is_bulk = false;
    for (const auto& p : pipes) {
        if (p.id == 0) continue;      // 哨兵值（调用方 query_pipes 已滤控制/等时）
        if (p.is_in) {
            // 批量优先；同型取编号最小（遍历序即 QueryPipe 序，显式比较保持确定）
            bool better = *in_pipe == 0 ||
                          (p.is_bulk && !*in_is_bulk) ||
                          (p.is_bulk == *in_is_bulk && p.id < *in_pipe);
            if (better) {
                *in_pipe = p.id;
                *in_is_bulk = p.is_bulk;
            }
        } else {
            bool better = *out_pipe == 0 ||
                          (p.is_bulk && !*out_is_bulk) ||
                          (p.is_bulk == *out_is_bulk && p.id < *out_pipe);
            if (better) {
                *out_pipe = p.id;
                *out_is_bulk = p.is_bulk;
            }
        }
    }
    return *in_pipe != 0;
}

// ---------------------------------------------------------------------------
// WinUsbPort
// ---------------------------------------------------------------------------
bool WinUsbPort::open(const std::wstring& path, std::wstring* err) {
    close();
    m_caps = {};

    // WinUSB 设备须以读写+重叠标志打开（MSDN：WinUsb_Initialize 前置要求）
    m_handle.reset(::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OVERLAPPED, nullptr));
    if (!m_handle.valid()) {
        if (err) *err = wraii::win_err(L"CreateFileW(WinUSB)", ::GetLastError());
        return false;
    }

    WINUSB_INTERFACE_HANDLE iface = nullptr;
    if (!::WinUsb_Initialize(m_handle.get(), &iface) || iface == nullptr) {
        if (err) *err = wraii::win_err(L"WinUsb_Initialize", ::GetLastError());
        // 常见根因：设备未绑定 WinUSB 驱动（INF/复合固件缺 MS OS 描述符）
        m_handle.reset();
        return false;
    }
    m_iface.reset(iface);

    parse_vid_pid(path, &m_caps.vid, &m_caps.pid);

    if (!query_pipes(err)) {
        close();
        return false;
    }
    return true;
}

void WinUsbPort::close() noexcept {
    m_iface.reset();        // 先释放接口句柄，再关文件句柄（MSDN 生命周期顺序）
    m_handle.reset();
}

bool WinUsbPort::query_pipes(std::wstring* err) {
    // 接口 0 备用设置 0：USB_INTERFACE_DESCRIPTOR.bNumEndpoints 给管道数上界，
    // QueryPipe 逐个取 WINUSB_PIPE_INFORMATION（PipeId 0x8X=IN / 0x0X=OUT）
    USB_INTERFACE_DESCRIPTOR desc{};
    if (!::WinUsb_QueryInterfaceSettings(m_iface.get(), 0, &desc)) {
        if (err) *err = wraii::win_err(L"WinUsb_QueryInterfaceSettings", ::GetLastError());
        return false;
    }
    std::vector<WinUsbPipeInfo> pipes;
    for (UCHAR i = 0; i < desc.bNumEndpoints; ++i) {
        WINUSB_PIPE_INFORMATION pi{};
        if (!::WinUsb_QueryPipe(m_iface.get(), 0, i, &pi)) {
            if (err) *err = wraii::win_err(L"WinUsb_QueryPipe", ::GetLastError());
            return false;
        }
        m_caps.n_pipes += 1;
        if (pi.PipeType != UsbdPipeTypeBulk && pi.PipeType != UsbdPipeTypeInterrupt) continue;
        pipes.push_back(WinUsbPipeInfo{
            static_cast<unsigned char>(pi.PipeId),
            (pi.PipeId & 0x80) != 0,
            pi.PipeType == UsbdPipeTypeBulk,
            pi.MaximumPacketSize});
    }
    unsigned char in_id = 0, out_id = 0;
    bool in_bulk = false, out_bulk = false;
    select_data_pipes(pipes, &in_id, &in_bulk, &out_id, &out_bulk);
    m_caps.in_pipe = in_id;
    m_caps.in_is_bulk = in_bulk;
    m_caps.out_pipe = out_id;
    m_caps.out_is_bulk = out_bulk;
    for (const auto& p : pipes) {
        if (p.id == in_id) m_caps.in_max_packet = p.max_packet;
        if (p.id == out_id) m_caps.out_max_packet = p.max_packet;
    }
    // 无任何数据管道的接口（纯控制设备）不构成可会话通道
    if (in_id == 0 && out_id == 0) {
        if (err) *err = L"接口无批量/中断数据管道";
        return false;
    }
    return true;
}

bool WinUsbPort::read_pipe(std::vector<uint8_t>& buf, unsigned timeout_ms, bool* timed_out,
                           std::wstring* err) {
    if (timed_out) *timed_out = false;
    if (!is_open()) {
        if (err) *err = L"WinUSB 未打开";
        return false;
    }
    if (m_caps.in_pipe == 0) {
        if (err) *err = L"无数据 IN 管道";
        return false;
    }
    buf.assign(4096, 0);   // 读片缓冲：单传输超长时下层分片返回，会话层按帧累计

    OVERLAPPED ov{};
    ov.hEvent = ::CreateEventW(nullptr, TRUE /*手动复位*/, FALSE, nullptr);
    if (ov.hEvent == nullptr) {
        if (err) *err = wraii::win_err(L"CreateEventW", ::GetLastError());
        return false;
    }
    wraii::uhandle<wraii::handle_closer> evt(ov.hEvent);

    DWORD got = 0;
    if (!::WinUsb_ReadPipe(m_iface.get(), m_caps.in_pipe, buf.data(),
                           static_cast<ULONG>(buf.size()), nullptr, &ov)) {
        DWORD e = ::GetLastError();
        if (e != ERROR_IO_PENDING) {
            if (err) *err = wraii::win_err(L"WinUsb_ReadPipe", e);
            return false;
        }
        DWORD wait = ::WaitForSingleObject(ov.hEvent, timeout_ms);
        if (wait == WAIT_TIMEOUT) {
            // 超时：AbortPipe 取消本次传输并等待回收（口径同 HidPort::read_overlapped）。
            // 边界竞态防护：超时判定与中止生效之间传输可能已完成——GOR 成功即交付
            // 该帧（设备不会重发，按轮空丢弃即丢帧——对抗复核指出）
            ::WinUsb_AbortPipe(m_iface.get(), m_caps.in_pipe);
            DWORD got2 = 0;
            if (::GetOverlappedResult(m_handle.get(), &ov, &got2, TRUE) && got2 > 0) {
                buf.resize(got2);
                return true;
            }
            if (timed_out) *timed_out = true;
            return false;
        }
        if (wait != WAIT_OBJECT_0) {
            // WAIT_FAILED 等：先取码，再取消在途传输并回收（避免 RAII 关闭事件后留孤儿 IO）
            DWORD e2 = ::GetLastError();
            ::WinUsb_AbortPipe(m_iface.get(), m_caps.in_pipe);
            DWORD dummy = 0;
            ::GetOverlappedResult(m_handle.get(), &ov, &dummy, TRUE);
            if (err) *err = wraii::win_err(L"WaitForSingleObject", e2);
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
        // 未进 PENDING 即完成：仍经 GetOverlappedResult 取长度（以 MSDN 为准）
        if (!::GetOverlappedResult(m_handle.get(), &ov, &got, FALSE)) {
            if (err) *err = wraii::win_err(L"GetOverlappedResult", ::GetLastError());
            return false;
        }
    }

    if (got == 0) {
        if (err) *err = L"读到 0 字节";
        return false;
    }
    buf.resize(got);
    return true;
}

bool WinUsbPort::write_pipe(const uint8_t* data, size_t len, std::wstring* err,
                            unsigned timeout_ms) {
    if (!is_open()) {
        if (err) *err = L"WinUSB 未打开";
        return false;
    }
    if (m_caps.out_pipe == 0) {
        if (err) *err = L"无数据 OUT 管道（只读设备）";
        return false;
    }
    if (len == 0 || len > 0xFFFFFFFFull) {
        if (err) *err = L"写入长度须在 [1, 4G)";
        return false;
    }

    OVERLAPPED ov{};
    ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) {
        if (err) *err = wraii::win_err(L"CreateEventW", ::GetLastError());
        return false;
    }
    wraii::uhandle<wraii::handle_closer> evt(ov.hEvent);

    DWORD done = 0;
    if (!::WinUsb_WritePipe(m_iface.get(), m_caps.out_pipe, const_cast<uint8_t*>(data),
                            static_cast<ULONG>(len), nullptr, &ov)) {
        DWORD e = ::GetLastError();
        if (e != ERROR_IO_PENDING) {
            if (err) *err = wraii::win_err(L"WinUsb_WritePipe", e);
            return false;
        }
        // send 为同步语义但等待有界：固件不收 OUT（NAK 永续）时挂死的会是调用
        // 线程（会话台 send 即 UI 线程），超时 → AbortPipe 取消并回收（口径同 read_pipe）
        DWORD wait = ::WaitForSingleObject(ov.hEvent, timeout_ms);
        bool have_done = false;
        if (wait == WAIT_TIMEOUT) {
            ::WinUsb_AbortPipe(m_iface.get(), m_caps.out_pipe);
            DWORD done2 = 0;
            // 边界竞态：超时判定与中止生效之间传输可能已完成——GOR 成功且足量按成功，
            // 否则重发会造成设备收到重复命令（对抗复核指出的误报路径）
            if (::GetOverlappedResult(m_handle.get(), &ov, &done2, TRUE) && done2 == len) {
                done = done2;
                have_done = true;
            } else {
                if (err) *err = L"写超时：设备未接收 OUT 传输（固件未处理该管道？）";
                return false;
            }
        } else if (wait != WAIT_OBJECT_0) {
            DWORD e2 = ::GetLastError();   // 先取码再回收，防回收序列覆写（诊断口径）
            ::WinUsb_AbortPipe(m_iface.get(), m_caps.out_pipe);
            DWORD dummy = 0;
            ::GetOverlappedResult(m_handle.get(), &ov, &dummy, TRUE);
            if (err) *err = wraii::win_err(L"WaitForSingleObject", e2);
            return false;
        }
        if (!have_done && !::GetOverlappedResult(m_handle.get(), &ov, &done, FALSE)) {
            if (err) *err = wraii::win_err(L"GetOverlappedResult", ::GetLastError());
            return false;
        }
    } else if (!::GetOverlappedResult(m_handle.get(), &ov, &done, FALSE)) {
        if (err) *err = wraii::win_err(L"GetOverlappedResult", ::GetLastError());
        return false;
    }
    if (done != len) {
        if (err) *err = wraii::fmt_v(L"短写：%lu / %zu 字节", done, len);
        return false;
    }
    return true;
}
