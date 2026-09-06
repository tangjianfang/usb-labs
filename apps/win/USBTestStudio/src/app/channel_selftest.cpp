// channel_selftest.cpp — EP-4 S1 通道层离线自测（无真机即可跑）：
// 1) 显式实例化 SerialChannelT<SerialPort> / HidChannelT<HidPort>，保证产线形态整体编译；
// 2) MockEchoPort / MockHidPort（回显假件）注入模板，验证读线程/回调/统计/关闭等
//    会话契约——即设计 §3 "调试台与产测引擎共用同一套通道" 的行为面。
// 真机收发验收（S1 验收口径）仍按切片表在真机上执行。
#include "channel/hid_channel.h"
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

// 回显假件（HID 形态）：与 HidPort 同名成员子集（open/close/is_open/caps/
// set_num_input_buffers/read_overlapped/set_output_report）。set_output_report
// 把输出报告按 OutputReportByteLength 补零后作为输入报告入队——模拟
// "输出报告→输入报告回显"工装固件（同 hid_report_loopback 的口径）。
class MockHidPort {
public:
    bool open(const std::wstring& path, std::wstring* err) {
        if (fail_open) { if (err) *err = L"模拟打开失败"; return false; }
        m_path = path;
        m_open = true;
        std::lock_guard<std::mutex> g(m_mtx);
        m_queue.clear();
        return true;
    }
    void close() noexcept { m_open = false; }
    bool is_open() const noexcept { return m_open.load(); }
    const HidCapsInfo& caps() const noexcept { return m_caps; }
    bool set_num_input_buffers(unsigned long) { return true; }

    bool read_overlapped(std::vector<uint8_t>& report, unsigned timeout_ms, bool* timed_out,
                         std::wstring* = nullptr) {
        *timed_out = false;
        if (!m_open || fail_read) return false;         // 非超时错误：读线程应退出
        std::unique_lock<std::mutex> g(m_mtx);
        if (m_queue.empty()) {
            g.unlock();
            Sleep(5);                                   // 模拟阻塞轮片，避免测试期热转
            *timed_out = true;                          // 轮空：读线程继续
            return false;
        }
        report = std::move(m_queue.front());
        m_queue.erase(m_queue.begin());
        (void)timeout_ms;
        return true;
    }

    bool set_output_report(const uint8_t* data, size_t len, std::wstring* err) {
        if (!m_open) { if (err) *err = L"HID 未打开"; return false; }
        if (len == 0 || len > m_caps.output_report_len) {
            if (err) *err = L"输出报告长度超出 [1, OutputReportByteLength]";
            return false;
        }
        if (!echo_output) return true;
        std::lock_guard<std::mutex> g(m_mtx);
        std::vector<uint8_t> rep(m_caps.output_report_len, 0);   // 同 HidPort：定长补零
        memcpy(rep.data(), data, len);
        m_queue.push_back(std::move(rep));
        return true;
    }

    bool fail_open = false;
    bool fail_read = false;              // 模拟设备拔出：读线程应退出
    bool echo_output = true;
    std::wstring m_path;
    HidCapsInfo m_caps;                  // 测试预置（真件由 open 时的 fill_caps 填充）

private:
    mutable std::mutex m_mtx;
    std::vector<std::vector<uint8_t>> m_queue;
    std::atomic<bool> m_open{false};     // 读线程读 / 调用方写
};

// 强制编译产线实例的全部成员（不运行）
template class SerialChannelT<SerialPort>;
template class HidChannelT<HidPort>;

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

    // ============ HidChannel（EP-4 S1 后半） ============
    using TestHid = HidChannelT<MockHidPort>;
    const std::wstring kHidPath =
        L"\\\\?\\hid#vid_1234&pid_0002#8&2c9d0e1f&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";

    {   // 契约：未开即关 + 未开即发
        TestHid ch(kHidPath);
        check(!ch.is_open(), "HID 初始未打开");
        check(ch.desc().kind.empty(), "HID 未打开无描述");
        std::wstring err;
        const uint8_t r[] = {0x00, 0x01};
        check(!ch.send(r, sizeof r, &err), "HID 未打开 send 失败");
        ch.close();                                     // 未开即关不崩溃
    }
    {   // 打开失败 → err 非空、仍处于关闭
        TestHid ch(kHidPath);
        ch.port().fail_open = true;
        std::wstring err;
        check(!ch.open(&err), "HID open 失败返回 false");
        check(!err.empty(), "HID open 失败给出 err");
        check(!ch.is_open(), "HID 失败后未打开");
    }
    {   // 回显会话：描述/定长报告/Report ID/统计/清回调
        TestHid ch(kHidPath);
        ch.port().m_caps.vid = 0x1234;
        ch.port().m_caps.pid = 0x0002;
        ch.port().m_caps.product = L"USB-Labs Keyboard";
        ch.port().m_caps.input_report_len = 8;
        ch.port().m_caps.output_report_len = 8;
        std::wstring err;
        check(ch.open(&err), "HID open 成功");
        check(ch.desc().kind == L"hid", "HID kind=hid");
        check(ch.desc().display == L"HID 1234:0002 USB-Labs Keyboard", "HID display 文本");
        check(ch.desc().path == kHidPath, "HID desc.path");

        std::mutex cb_mtx;
        std::vector<std::vector<uint8_t>> frames;
        ch.set_receive_callback([&](const std::vector<uint8_t>& b) {
            std::lock_guard<std::mutex> g(cb_mtx);
            frames.push_back(b);
        });

        const uint8_t rep1[] = {0x00, 0x01, 0x02};       // 无 Report ID 设备：[0]=0
        check(ch.send(rep1, sizeof rep1, &err), "HID send-1 成功");
        check(wait_for([&] {
            std::lock_guard<std::mutex> g(cb_mtx);
            return frames.size() >= 1;
        }), "HID 回显帧到达");
        {
            std::lock_guard<std::mutex> g(cb_mtx);
            std::vector<uint8_t> expect(rep1, rep1 + 3);
            expect.resize(8, 0);                         // 输出报告定长补零
            check(frames[0] == expect, "HID 回显帧=定长报告(补零)");
        }
        auto st = ch.stats();
        check(st.tx_frames == 1 && st.tx_bytes == 3, "HID tx 统计");
        check(st.rx_frames == 1 && st.rx_bytes == 8, "HID rx 统计");

        const uint8_t rep2[] = {0x05, 0xAA};             // 带 Report ID 的报告
        check(ch.send(rep2, sizeof rep2, &err), "HID send-2 成功");
        check(wait_for([&] {
            std::lock_guard<std::mutex> g(cb_mtx);
            return frames.size() >= 2 && frames[1][0] == 0x05 && frames[1][1] == 0xAA;
        }), "HID Report ID 前缀保留");

        const uint8_t long_rep[9] = {};                  // 超过 OutputReportByteLength
        check(!ch.send(long_rep, sizeof long_rep, &err), "HID 超长 send 拒绝");
        check(!err.empty(), "HID 超长 send 给出 err");
        check(ch.stats().tx_frames == 2, "HID 失败 send 不计统计");
        check(!ch.send(rep1, 0, &err), "HID 空 send 拒绝");

        ch.set_receive_callback(nullptr);                // 清回调后不再投递
        Sleep(150);
        size_t n_at_clear = frames.size();
        check(ch.send(rep1, sizeof rep1, &err), "HID 清回调后 send");
        check(wait_for([&] { return ch.stats().rx_bytes == 24; }), "HID 清回调后仍有 rx 统计");
        Sleep(150);
        {
            std::lock_guard<std::mutex> g(cb_mtx);
            check(frames.size() == n_at_clear, "HID 清回调后不再调用");
        }

        ch.close();
        check(!ch.is_open(), "HID close 后未打开");
    }
    {   // 拔出模拟：读线程退出；send 写路径独立仍成功；句柄未关仍报 is_open
        TestHid ch(kHidPath);
        ch.port().m_caps.input_report_len = 8;
        ch.port().m_caps.output_report_len = 8;
        std::wstring err;
        check(ch.open(&err), "拔出模拟 open");
        ch.port().fail_read = true;                      // 下一轮读即"设备拔出"
        Sleep(150);                                      // 等读线程退出
        const uint8_t r[] = {0x00, 0x01};
        check(ch.send(r, sizeof r, &err), "拔出后 send（写路径独立）仍成功");
        Sleep(150);
        check(ch.stats().rx_frames == 0 && ch.stats().rx_bytes == 0, "读线程退出后无 rx");
        check(ch.is_open(), "通道仍报 is_open（句柄未关）");
    }
    {   // 重开：close → open 复用同一通道对象
        TestHid ch(kHidPath);
        ch.port().m_caps.input_report_len = 8;
        ch.port().m_caps.output_report_len = 8;
        std::wstring err;
        check(ch.open(&err), "重开第一次 open");
        const uint8_t r[] = {0x01, 0x02};
        check(ch.send(r, sizeof r, &err), "重开第一次 send");
        check(wait_for([&] { return ch.stats().rx_frames >= 1; }), "重开第一次回读");
        ch.close();
        check(ch.open(&err), "重开第二次 open");
        check(ch.send(r, sizeof r, &err), "重开第二次 send");
        check(wait_for([&] { return ch.stats().rx_frames >= 2; }), "重开第二次回读");
    }
    {   // set_read_timeout(0) 回退默认值不崩溃 + 析构路径
        TestHid ch(kHidPath);
        ch.port().m_caps.input_report_len = 4;
        ch.port().m_caps.output_report_len = 4;
        ch.set_read_timeout(0);
        std::wstring err;
        check(ch.open(&err), "HID 短超时 open");
        ch.set_read_timeout(0);                          // 打开态改超时
        Sleep(120);
    }   // 析构（读线程在跑）——验证 ~HidChannelT 不挂死
    printf("channel_selftest: %d/%d PASS\n", g_pass, g_pass);
    return 0;
}
