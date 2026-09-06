// winusb_channel.h — IChannel 的 WinUSB 批量管道会话实现（EP-4 S5 前半）：
// 适配 WinUsbPort：open 填描述（VID:PID 自路径解析 + 选型管道号），后台读线程
// read_pipe 轮片，每个完成的 IN 传输计为一帧并回调；send 即 write_pipe
//（同步有界：默认 3s 超时后取消回收并报错——固件不收 OUT 时不挂死调用线程；
// 无 OUT 管道的只读设备 send 必然失败，由 err 呈现）。
// 读线程口径与 Serial/HID 同：超时=轮空继续；非超时错误（常见为设备拔出）
// =线程退出，通道仍报 is_open，由会话台感知无帧后提示。
// 目录接线（usb 行 → 本通道）随 S5 后半与 MSC 一并落——设备接口路径须由
// GUID 枚举产出，非 DeviceEnumerator 现有实例路径。模板化 PortT 以便离线
// 自测注入回显假件。
#pragma once

#include "channel/channel.h"
#include "usb/winusb_port.h"

#include <atomic>
#include <thread>

template <typename PortT = WinUsbPort>
class WinUsbChannelT : public IChannel {
public:
    explicit WinUsbChannelT(std::wstring path) : m_path(std::move(path)) {}
    ~WinUsbChannelT() override { close(); }

    bool open(std::wstring* err = nullptr) override {
        if (is_open()) close();
        if (!m_port.open(m_path, err)) return false;
        const WinUsbCaps& c = m_port.caps();
        m_desc.kind = L"usb";
        m_desc.path = m_path;
        // 描述含选型管道与型别：工程师排障时直接可见数据通路
        const wchar_t* in_kind = c.in_pipe ? (c.in_is_bulk ? L"bulk" : L"intr") : L"-";
        const wchar_t* out_kind = c.out_pipe ? (c.out_is_bulk ? L"bulk" : L"intr") : L"-";
        m_desc.display = wraii::fmt_v(L"WinUSB %04X:%04X IN 0x%02X(%s) OUT 0x%02X(%s)",
                                      c.vid, c.pid, c.in_pipe, in_kind, c.out_pipe, out_kind);
        m_stop = false;
        if (c.in_pipe)                       // 只出不进的设备无从驱动读线程
            m_reader = std::thread([this] { read_loop(); });
        return true;
    }
    void close() noexcept override {
        if (m_reader.joinable()) {
            m_stop = true;
            m_reader.join();            // 读线程至多阻塞一个轮片（m_read_ms）
        }
        m_port.close();
    }
    bool is_open() const noexcept override { return m_port.is_open(); }

    bool send(const uint8_t* data, size_t len, std::wstring* err = nullptr) override {
        if (!m_port.write_pipe(data, len, err)) return false;
        std::lock_guard<std::mutex> g(m_mtx);
        m_stats.tx_frames += 1;
        m_stats.tx_bytes += len;
        return true;
    }

    void set_receive_callback(ReceiveCallback cb) override {
        std::lock_guard<std::mutex> g(m_mtx);
        m_cb = std::move(cb);
    }
    void set_read_timeout(unsigned ms) noexcept override { m_read_ms = ms ? ms : 100; }
    const ChannelDesc& desc() const noexcept override { return m_desc; }

    // 底层端口直访：产测引擎的控制传输等专用流程仍走 Port 原生接口
    PortT& port() noexcept { return m_port; }
    ChannelStats stats() const noexcept override {
        std::lock_guard<std::mutex> g(m_mtx);
        return m_stats;
    }

private:
    void read_loop() noexcept {
        while (!m_stop) {
            std::vector<uint8_t> buf;
            bool timed_out = false;
            if (m_port.read_pipe(buf, m_read_ms, &timed_out)) {
                ReceiveCallback cb;
                {
                    std::lock_guard<std::mutex> g(m_mtx);
                    m_stats.rx_frames += 1;
                    m_stats.rx_bytes += buf.size();
                    cb = m_cb;
                }
                if (cb) cb(buf);
            } else if (timed_out) {
                continue;               // 轮空：设备本周期无 IN 传输
            } else {
                return;                 // 读错误（常见为拔出）→ 退出读线程
            }
        }
    }

    std::wstring m_path;                // WinUSB 设备接口路径（GUID 枚举产出，S5 后半接入）
    unsigned m_read_ms = 100;
    PortT m_port;
    std::atomic<bool> m_stop{false};
    std::thread m_reader;
    mutable std::mutex m_mtx;           // 保护 m_stats / m_cb（const stats() 亦取锁）
    ChannelStats m_stats;
    ChannelDesc m_desc;
    ReceiveCallback m_cb;
};

using WinUsbChannel = WinUsbChannelT<WinUsbPort>;
