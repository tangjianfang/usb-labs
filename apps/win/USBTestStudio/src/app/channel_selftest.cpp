// channel_selftest.cpp — EP-4 S1 通道层离线自测（无真机即可跑）：
// 1) 显式实例化 SerialChannelT<SerialPort>，保证产线形态整体编译；
// 2) MockEchoPort（回显假件）注入模板，验证读线程/回调/统计/关闭等
//    会话契约——即设计 §3 "调试台与产测引擎共用同一套通道" 的行为面。
// 真机收发验收（S1 验收口径）仍按切片表在真机上执行。
#include "channel/serial_channel.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>

// 回显假件：与 SerialPort 同名成员子集（open/close/is_open/device_path/
// write_all/read_some）。write_all 入队，read_some 出队——模拟 TX-RX 短接。
class MockEchoPort {
public:
    static std::wstring device_path(const std::wstring& p) {
        if (p.rfind(L"\\\\.\\", 0) == 0) return p;
        return L"\\\\.\\" + p;
    }
    bool open(const std::wstring& name, unsigned baud, std::wstring* err) {
        if (fail_open) { if (err) *err = L"模拟打开失败"; return false; }
        m_name = name; m_baud = baud; m_open = true;
        std::lock_guard<std::mutex> g(m_mtx); m_queue.clear();
        return true;
    }
    void close() noexcept { m_open = false; }
    bool is_open() const noexcept { return m_open.load(); }

    bool write_all(const uint8_t* data, size_t len, std::wstring*) {
        if (!m_open) return false;
        std::lock_guard<std::mutex> g(m_mtx);
        m_queue.insert(m_queue.end(), data, data + len);
        return true;
    }
    bool read_some(uint8_t* buf, size_t cap, unsigned timeout_ms, size_t* got,
                   std::wstring* = nullptr) {
        *got = 0;
        if (!m_open) return false;
        std::unique_lock<std::mutex> g(m_mtx);
        if (m_queue.empty()) return true;              // 静默轮空（读线程再转）
        size_t n = m_queue.size() > cap ? cap : m_queue.size();
        memcpy(buf, m_queue.data(), n);
        m_queue.erase(m_queue.begin(), m_queue.begin() + n);
        *got = n;
        (void)timeout_ms;
        return true;
    }

    bool fail_open = false;
    std::wstring m_name;
    unsigned m_baud = 0;

private:
    mutable std::mutex m_mtx;
    std::vector<uint8_t> m_queue;
    std::atomic<bool> m_open{false};   // 读线程读 / 调用方写
};

// 强制编译产线实例的全部成员（不运行）
template class SerialChannelT<SerialPort>;

static int g_pass = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        printf("FAIL: %s (passed %d before failure)\n", what, g_pass);
        exit(1);
    }
    ++g_pass;
}
static bool wait_for(const std::function<bool()>& pred, unsigned timeout_ms = 2000) {
    for (unsigned t = 0; t < timeout_ms; t += 10) {
        if (pred()) return true;
        Sleep(10);
    }
    return pred();
}

int wmain() {
    // device_path 规则（真件静态逻辑）
    check(SerialPort::device_path(L"COM7") == L"\\\\.\\COM7", "device_path COM7");
    check(SerialPort::device_path(L"COM10") == L"\\\\.\\COM10", "device_path COM10");
    check(SerialPort::device_path(L"\\\\.\\COM3") == L"\\\\.\\COM3", "device_path 幂等");

    using TestChannel = SerialChannelT<MockEchoPort>;

    {   // 契约：未开即关
        TestChannel ch(L"COM7", 115200);
        check(!ch.is_open(), "初始未打开");
        check(ch.desc().kind.empty(), "未打开无描述");
        ch.close();                                     // 未开即关不崩溃
    }
    {   // 打开失败 → err 非空、仍处于关闭
        TestChannel ch(L"COM9");
        ch.port().fail_open = true;
        std::wstring err;
        check(!ch.open(&err), "open 失败返回 false");
        check(!err.empty(), "open 失败给出 err");
        check(!ch.is_open(), "失败后未打开");
    }
    {   // 回显收发：回调、内容、统计、清回调
        TestChannel ch(L"COM7", 115200);
        std::wstring err;
        check(ch.open(&err), "open 成功");
        check(ch.is_open(), "open 后已打开");
        check(ch.desc().kind == L"serial", "kind=serial");
        check(ch.desc().display == L"COM7 @115200 8N1", "display 文本");
        check(ch.desc().path == L"\\\\.\\COM7", "desc.path");

        std::mutex cb_mtx;
        std::vector<std::vector<uint8_t>> frames;
        ch.set_receive_callback([&](const std::vector<uint8_t>& b) {
            std::lock_guard<std::mutex> g(cb_mtx);
            frames.push_back(b);
        });

        const uint8_t msg1[] = {0x31, 0x32, 0x33, 0x0D, 0x0A};
        check(ch.send(msg1, sizeof msg1, &err), "send-1 成功");
        bool got1 = wait_for([&] {
            std::lock_guard<std::mutex> g(cb_mtx);
            return frames.size() >= 1 && frames[0] == std::vector<uint8_t>(msg1, msg1 + 5);
        });
        check(got1, "回显帧内容一致");
        auto st = ch.stats();
        check(st.tx_frames == 1 && st.tx_bytes == 5, "tx 统计");
        check(st.rx_bytes == 5 && st.rx_frames >= 1, "rx 统计");

        const uint8_t msg2[] = {0x00, 0x01, 0x02};
        check(ch.send(msg2, sizeof msg2, &err), "send-2 成功");
        check(wait_for([&] { return ch.stats().rx_bytes == 8; }), "第二帧回读累计");
        check(ch.stats().tx_bytes == 8, "tx 累计 8");

        ch.set_receive_callback(nullptr);               // 清回调后不再投递
        Sleep(150);
        size_t n_at_clear = frames.size();
        check(ch.send(msg1, sizeof msg1, &err), "send-3 成功");
        check(wait_for([&] { return ch.stats().rx_bytes == 13; }), "清回调后仍有 rx 统计");
        Sleep(150);
        check(frames.size() == n_at_clear, "清回调后不再调用");

        ch.close();
        check(!ch.is_open(), "close 后未打开");
    }
    {   // set_read_timeout(0) 回退默认值不崩溃 + 析构路径
        TestChannel ch(L"COM5", 9600);
        ch.set_read_timeout(0);
        ch.set_read_timeout(50);
        std::wstring err;
        check(ch.open(&err), "短超时 open");
        ch.set_read_timeout(0);                         // 打开态改超时
        Sleep(120);
    }   // 析构（读线程在跑）——验证 ~SerialChannelT 不挂死
    printf("channel_selftest: %d/%d PASS\n", g_pass, g_pass);
    return 0;
}
