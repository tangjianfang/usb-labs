// hid_channel.h — IChannel 的 HID 会话实现（EP-4 S1 后半）：
// 适配既有 HidPort：open 按 caps 填描述（VID:PID + 产品串），后台读线程
// read_overlapped 轮片，每个输入报告计为一帧并回调——报告含 Report ID
// 前缀字节，原样透传给上层（解析面板 S4 负责展开）。send 即
// set_output_report（data[0]=Report ID，不足 OutputReportByteLength 由
// Port 内部补零；WriteFile 重叠有界主路默认 3s，设备 NAK 永续不挂死调用
// 线程；只读句柄设备 send 必然失败，由 err 呈现）。
// 读线程口径：超时=轮空继续；非超时错误（常见为设备拔出）=线程退出，
// 与 SerialChannelT 同——通道仍报 is_open，由会话台感知无帧后提示。
// 模板化 PortT 以便离线自测注入回显假件。
// 日志：channel.hid（info=开关通道，debug=逐报告收发/超时调整，warn=读线程退出，
// err=失败带 err 串）。
#pragma once

#include "channel/channel.h"
#include "usb/hid_port.h"
#include "app/log.h"

#include <atomic>
#include <thread>

template <typename PortT = HidPort>
class HidChannelT : public IChannel {
public:
    explicit HidChannelT(std::wstring path) : m_path(std::move(path)) {}
    ~HidChannelT() override { close(); }

    bool open(std::wstring* err = nullptr) override {
        if (is_open()) close();
        const ULONGLONG t0 = ::GetTickCount64();
        ustlog::logger("channel.hid")->info("打开 HID 通道 path={}", ustlog::w2u(m_path));
        if (!m_port.open(m_path, err)) {
            ustlog::logger("channel.hid")->error("打开 HID 通道失败（{} ms）：{}",
                                                 ::GetTickCount64() - t0,
                                                 err ? ustlog::w2u(*err) : std::string{});
            return false;
        }
        m_port.set_num_input_buffers(64);   // best-effort：调大环形缓冲防高速设备丢报告
        const HidCapsInfo& c = m_port.caps();
        m_desc.kind = L"hid";
        m_desc.path = m_path;
        m_desc.display = wraii::fmt_v(L"HID %04X:%04X", c.vid, c.pid);
        if (!c.product.empty()) m_desc.display += L" " + c.product;
        // 顶层 usage 能力透传给会话层：S4 解析面板按 usage page/usage 选型
        m_desc.hid_usage_page = c.usage_page;
        m_desc.hid_usage = c.usage;
        m_desc.hid_report_id = c.has_report_id;
        m_stop = false;
        m_reader = std::thread([this] { read_loop(); });
        ustlog::logger("channel.hid")->info(
            "HID 通道打开完成（{} ms）：{} usage_page={:#06x} usage={:#06x} report_id={}，读线程已启动",
            ::GetTickCount64() - t0, ustlog::w2u(m_desc.display), c.usage_page, c.usage,
            c.has_report_id);
        return true;
    }
    void close() noexcept override {
        const bool was_open = is_open();
        if (m_reader.joinable()) {
            m_stop = true;
            m_reader.join();            // 读线程至多阻塞一个轮片（m_read_ms）
        }
        m_port.close();
        if (was_open)
            ustlog::logger("channel.hid")->info("HID 通道已关闭 path={}", ustlog::w2u(m_path));
    }
    bool is_open() const noexcept override { return m_port.is_open(); }

    bool send(const uint8_t* data, size_t len, std::wstring* err = nullptr) override {
        if (!m_port.set_output_report(data, len, err, kSendTimeoutMs)) {
            ustlog::logger("channel.hid")->error("HID 发送 {} 字节失败（超时 {}ms）：{}", len,
                                                 kSendTimeoutMs,
                                                 err ? ustlog::w2u(*err) : std::string{});
            return false;
        }
        ustlog::logger("channel.hid")->log(spdlog::level::debug,
                                           "HID 发送输出报告 {} 字节（超时 {}ms）", len,
                                           kSendTimeoutMs);
        std::lock_guard<std::mutex> g(m_mtx);
        m_stats.tx_frames += 1;
        m_stats.tx_bytes += len;
        return true;
    }

    void set_receive_callback(ReceiveCallback cb) override {
        std::lock_guard<std::mutex> g(m_mtx);
        m_cb = std::move(cb);
    }
    void set_read_timeout(unsigned ms) noexcept override {
        m_read_ms = ms ? ms : 100;
        ustlog::logger("channel.hid")->log(spdlog::level::debug, "HID 读轮片超时={}ms",
                                           m_read_ms);
    }
    const ChannelDesc& desc() const noexcept override { return m_desc; }

    // 底层端口直访：产测引擎的回报率测量等专用流程仍走 Port 原生接口
    PortT& port() noexcept { return m_port; }
    ChannelStats stats() const noexcept override {
        std::lock_guard<std::mutex> g(m_mtx);
        return m_stats;
    }

private:
    void read_loop() noexcept {
        while (!m_stop) {
            std::vector<uint8_t> report;
            bool timed_out = false;
            if (m_port.read_overlapped(report, m_read_ms, &timed_out)) {
                ustlog::logger("channel.hid")->log(spdlog::level::debug,
                                                   "HID 收到输入报告 {} 字节", report.size());
                ReceiveCallback cb;
                {
                    std::lock_guard<std::mutex> g(m_mtx);
                    m_stats.rx_frames += 1;
                    m_stats.rx_bytes += report.size();
                    cb = m_cb;
                }
                if (cb) cb(report);
            } else if (timed_out) {
                continue;               // 轮空：设备本周期无输入报告
            } else {
                ustlog::logger("channel.hid")->warn(
                    "HID 读线程退出：read_overlapped 失败（常见为设备拔出）");
                return;                 // 读错误（常见为拔出）→ 退出读线程
            }
        }
    }

    std::wstring m_path;                // HID 设备接口路径（DeviceEnumerator 产出）
    unsigned m_read_ms = 100;
    PortT m_port;
    std::atomic<bool> m_stop{false};
    std::thread m_reader;
    mutable std::mutex m_mtx;           // 保护 m_stats / m_cb（const stats() 亦取锁）
    ChannelStats m_stats;
    ChannelDesc m_desc;
    ReceiveCallback m_cb;
};

using HidChannel = HidChannelT<HidPort>;
