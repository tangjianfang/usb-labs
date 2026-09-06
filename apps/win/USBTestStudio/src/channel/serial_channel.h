// serial_channel.h — IChannel 的串口会话实现（EP-4 S1）：
// 适配既有 SerialPort（CDC/串口，重叠 I/O），后台读线程持续 read_some，
// 每个非空读片计为一帧并回调；send 同步 write_all。COMMTIMEOUTS 的 20ms
// 字节间隔兜底使读片天然贴合"帧"。模板化 PortT 以便离线自测注入回显假件。
#pragma once

#include "channel/channel.h"
#include "usb/serial_port.h"

#include <atomic>
#include <thread>

template <typename PortT = SerialPort>
class SerialChannelT : public IChannel {
public:
    explicit SerialChannelT(std::wstring name, unsigned baud = 115200)
        : m_name(std::move(name)), m_baud(baud) {}
    ~SerialChannelT() override { close(); }

    bool open(std::wstring* err = nullptr) override {
        if (is_open()) close();
        if (!m_port.open(m_name, m_baud, err)) return false;
        m_desc.kind = L"serial";
        m_desc.path = PortT::device_path(m_name);
        m_desc.display = m_name + L" @" + std::to_wstring(m_baud) + L" 8N1";
        m_stop = false;
        m_reader = std::thread([this] { read_loop(); });
        return true;
    }
    void close() noexcept override {
        if (m_reader.joinable()) {
            m_stop = true;
            m_reader.join();            // 先停线程再关句柄，避免读线程操作已关句柄
        }
        m_port.close();
    }
    bool is_open() const noexcept override { return m_port.is_open(); }

    bool send(const uint8_t* data, size_t len, std::wstring* err = nullptr) override {
        if (!m_port.write_all(data, len, err)) return false;
        std::lock_guard<std::mutex> g(m_mtx);
        m_stats.tx_frames += 1;
        m_stats.tx_bytes += len;
        return true;
    }

    void set_receive_callback(ReceiveCallback cb) override {
        std::lock_guard<std::mutex> g(m_mtx);
        m_cb = std::move(cb);
    }
    void set_read_timeout(unsigned ms) noexcept override { m_read_ms = ms ? ms : 200; }
    const ChannelDesc& desc() const noexcept override { return m_desc; }

    // 底层端口直访：产测引擎的环回/遥测等专用流程仍走 Port 原生接口
    PortT& port() noexcept { return m_port; }
    ChannelStats stats() const noexcept override {
        std::lock_guard<std::mutex> g(m_mtx);
        return m_stats;
    }

private:
    void read_loop() noexcept {
        std::vector<uint8_t> buf(4096);
        while (!m_stop) {
            size_t got = 0;
            if (!m_port.read_some(buf.data(), buf.size(), m_read_ms, &got)) return; // 句柄失效即退出
            if (got == 0) continue;
            ReceiveCallback cb;
            {
                std::lock_guard<std::mutex> g(m_mtx);
                m_stats.rx_frames += 1;
                m_stats.rx_bytes += got;
                cb = m_cb;
            }
            if (cb) cb(std::vector<uint8_t>(buf.begin(), buf.begin() + got));
        }
    }

    std::wstring m_name;                // 具名端口（COM7），打开时由 PortT 转 \\.\ 路径
    unsigned m_baud = 115200;
    unsigned m_read_ms = 200;
    PortT m_port;
    std::atomic<bool> m_stop{false};
    std::thread m_reader;
    mutable std::mutex m_mtx;           // 保护 m_stats / m_cb（const stats() 亦取锁）
    ChannelStats m_stats;
    ChannelDesc m_desc;
    ReceiveCallback m_cb;
};

using SerialChannel = SerialChannelT<SerialPort>;
