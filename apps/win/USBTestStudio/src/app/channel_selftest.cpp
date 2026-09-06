// channel_selftest.cpp — EP-4 S1 通道层离线自测（无真机即可跑）：
// 1) 显式实例化 SerialChannelT<SerialPort> / HidChannelT<HidPort> /
//    WinUsbChannelT<WinUsbPort>，保证产线形态整体编译；
// 2) MockEchoPort / MockHidPort / MockWinUsbPort（回显假件）注入模板，验证读线程/
//    回调/统计/关闭等会话契约——即设计 §3 "调试台与产测引擎共用同一套通道" 的行为面；
// 3) WinUsbPort 纯逻辑（路径 VID/PID 解析、数据管道选型）直接离线验证。
// 真机收发验收（S1/S5 验收口径）仍按切片表在真机上执行。
#include "channel/hid_channel.h"
#include "channel/msc_channel.h"
#include "channel/serial_channel.h"
#include "channel/winusb_channel.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>

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

    bool set_output_report(const uint8_t* data, size_t len, std::wstring* err,
                           unsigned timeout_ms = 3000) {
        if (!m_open) { if (err) *err = L"HID 未打开"; return false; }
        if (len == 0 || len > m_caps.output_report_len) {
            if (err) *err = L"输出报告长度超出 [1, OutputReportByteLength]";
            return false;
        }
        last_write_timeout_ms = timeout_ms;
        if (fail_write) { if (err) *err = L"模拟写失败（NAK 永续）"; return false; }
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
    bool fail_write = false;             // 模拟写路径失败（NAK 永续超时口径）
    unsigned last_write_timeout_ms = 0;  // 通道 send 透传的写超时（#71 有界写契约）
    std::wstring m_path;
    HidCapsInfo m_caps;                  // 测试预置（真件由 open 时的 fill_caps 填充）

private:
    mutable std::mutex m_mtx;
    std::vector<std::vector<uint8_t>> m_queue;
    std::atomic<bool> m_open{false};     // 读线程读 / 调用方写
};

// 回显假件（WinUSB 形态）：与 WinUsbPort 同名成员子集（open/close/is_open/caps/
// read_pipe/write_pipe）。write_pipe 把整个 OUT 传输作为一帧 IN 传输入队——模拟
// "收到什么批量 OUT 就回什么批量 IN" 的工装固件（IN 传输口径=整帧）。
class MockWinUsbPort {
public:
    bool open(const std::wstring& path, std::wstring* err) {
        if (fail_open) { if (err) *err = L"模拟打开失败"; return false; }
        m_path = path;
        WinUsbPort::parse_vid_pid(path, &m_caps.vid, &m_caps.pid);   // 复用真件纯逻辑
        m_open = true;
        std::lock_guard<std::mutex> g(m_mtx); m_queue.clear();
        return true;
    }
    void close() noexcept { m_open = false; }
    bool is_open() const noexcept { return m_open.load(); }
    const WinUsbCaps& caps() const noexcept { return m_caps; }

    bool read_pipe(std::vector<uint8_t>& buf, unsigned timeout_ms, bool* timed_out,
                   std::wstring* = nullptr) {
        *timed_out = false;
        if (!m_open || fail_read || m_caps.in_pipe == 0) return false;   // 非超时错误：读线程退出
        std::unique_lock<std::mutex> g(m_mtx);
        if (m_queue.empty()) {
            g.unlock();
            Sleep(5);                                   // 模拟阻塞轮片，避免测试期热转
            *timed_out = true;                          // 轮空：读线程继续
            return false;
        }
        buf = std::move(m_queue.front());
        m_queue.erase(m_queue.begin());
        (void)timeout_ms;
        return true;
    }

    bool write_pipe(const uint8_t* data, size_t len, std::wstring* err,
                    unsigned timeout_ms = 3000) {
        last_write_timeout_ms = timeout_ms;
        if (!m_open) { if (err) *err = L"WinUSB 未打开"; return false; }
        if (m_caps.out_pipe == 0) { if (err) *err = L"无数据 OUT 管道（只读设备）"; return false; }
        if (fail_write) { if (err) *err = L"模拟写失败（NAK 永续）"; return false; }
        if (!echo_output) return true;
        std::lock_guard<std::mutex> g(m_mtx);
        m_queue.emplace_back(data, data + len);
        return true;
    }

    bool fail_open = false;
    bool fail_read = false;              // 模拟设备拔出：读线程应退出
    bool echo_output = true;
    bool fail_write = false;             // 模拟写路径失败（NAK 永续超时口径）
    unsigned last_write_timeout_ms = 0;  // 通道 send 透传的写超时（#71 有界写契约）
    std::wstring m_path;
    WinUsbCaps m_caps;                   // 测试预置管道选型（真件由 open 时的 query_pipes 填充）

private:
    mutable std::mutex m_mtx;
    std::vector<std::vector<uint8_t>> m_queue;
    std::atomic<bool> m_open{false};     // 读线程读 / 调用方写
};

// 回显假件（MSC 形态）：与 MscScsi 同名成员子集（open_physical_drive/close/
// is_open/read_capacity/scsi_inquiry/pass_through）。IN 命令按确定性图案填充
// data[i]=i^cdb[0]；force_status 预设非 0 时填固定格式 SENSE 并按真件口径
// 返回 false（真件：DeviceIoControl 成功而 ScsiStatus≠0 → false + status 回传）。
class MockMscPort {
public:
    bool open_physical_drive(unsigned index, bool write_access, std::wstring* err) {
        if (fail_open) {
            if (err) *err = L"模拟打开失败";
            return false;
        }
        m_index = index;
        m_last_write_access = write_access;
        m_open = true;
        return true;
    }
    void close() noexcept { m_open = false; }
    bool is_open() const noexcept { return m_open; }

    bool read_capacity(unsigned long long* total, unsigned* blk, std::wstring* err,
                       unsigned timeout_s = 0) {
        if (!m_open) return false;
        last_probe_timeout_s = timeout_s;
        s_last_probe_timeout_s = timeout_s;
        if (m_index < s_cap_from) return false;   // 按盘号模拟容量探测失败/超时
        // 与 MscScsi 同形态委托内核（对抗复核 P1，evolve #75）：通道 open /
        // auto_detect 既有用例自此真实穿越 msc_capacity_probe_impl，内核回归在
        // 此一并变红；容量由 pass_through 的 0x25/0x9E 应答旋钮（cap10_*/cap16_*）承接
        return msc_capacity_probe_impl<MockMscPort>(*this, total, blk, err, timeout_s);
    }
    bool scsi_inquiry(std::string* v, std::string* p, std::string* r, unsigned char* t,
                      std::wstring*, unsigned timeout_s = 0) {
        if (!m_open) return false;
        last_probe_timeout_s = timeout_s;
        s_last_probe_timeout_s = timeout_s;
        if (v) *v = vendor;
        if (p) *p = product;
        if (r) *r = rev;
        if (t) *t = 0;
        return true;
    }
    bool bus_is_usb(bool* is_usb, std::wstring*) {
        *is_usb = m_open && m_index >= s_usb_from;   // 按盘号模拟 BusTypeUsb
        return true;
    }
    bool pass_through(const uint8_t* cdb, uint8_t cdb_len, void* data, uint32_t data_len,
                      unsigned char* sense, unsigned char* scsi_status, int data_dir,
                      unsigned timeout_s, std::wstring* err) {
        last_cdb.assign(cdb, cdb + cdb_len);
        last_dir = data_dir;
        last_data_len = data_len;
        last_timeout_s = timeout_s;
        if (!m_open) {
            if (err) *err = L"盘未打开";
            return false;
        }
        if (fail_ioctl) {                 // OS 层失败：status 保持调用方初值 0xFF
            if (err) *err = L"模拟 DeviceIoControl 失败";
            return false;
        }
        if (force_status != 0) {          // CHECK CONDITION：SENSE 即设备应答
            if (scsi_status) *scsi_status = force_status;
            if (sense) {
                memset(sense, 0, 32);
                sense[0] = 0x70;
                sense[2] = sense_key;
                sense[12] = sense_asc;
                sense[13] = 0x00;
            }
            if (err) *err = L"模拟 SCSI CHECK CONDITION";
            return false;
        }
        if (scsi_status) *scsi_status = 0;
        auto* out = static_cast<uint8_t*>(data);
        // 容量命令应答（msc_capacity_probe_impl 的直通路径，evolve #75）：
        // 0x25 回 8 字节（last LBA BE32+块长 BE32）；0x9E/SA=0x10 回 32 字节
        //（last LBA BE64+块长 BE32）——大端编码与 SBC-3 一致。
        if (cdb_len == 10 && cdb[0] == 0x25) {
            ++cap10_calls;
            if (out && data_len >= 8) {
                put_be32(out, static_cast<uint32_t>(cap10_last_lba));
                put_be32(out + 4, cap10_block);
            }
            return true;
        }
        if (cdb_len == 16 && cdb[0] == 0x9E && cdb[1] == 0x10) {
            ++cap16_calls;
            if (cap16_fail) {             // 模拟 >2TB 盘 RC16 失败（留痕断言用）
                if (err) *err = L"模拟 READ_CAPACITY(16) 失败";
                return false;
            }
            if (out && data_len >= 12) {
                for (unsigned i = 0; i < 8; ++i)
                    out[i] = uint8_t((cap16_last_lba >> (56 - i * 8)) & 0xFF);
                put_be32(out + 8, cap16_block);
            }
            return true;
        }
        if (data && data_len > 0 && data_dir == kMscDirIn) {
            for (uint32_t i = 0; i < data_len; ++i) out[i] = uint8_t(i) ^ cdb[0];
        }
        return true;
    }

    static void put_be32(uint8_t* p, uint32_t v) {
        p[0] = uint8_t(v >> 24);
        p[1] = uint8_t(v >> 16);
        p[2] = uint8_t(v >> 8);
        p[3] = uint8_t(v);
    }

    bool fail_open = false;
    bool fail_ioctl = false;              // 模拟 OS 层直通失败
    unsigned char force_status = 0;       // 非 0 → 模拟该 SCSI 状态（CHECK CONDITION）
    unsigned char sense_key = 0x05, sense_asc = 0x21;   // ILLEGAL REQUEST / LBA 越界
    std::string vendor = "MOCKLAB", product = "UDISK-300", rev = "1.0";
    unsigned m_index = 0;
    bool m_last_write_access = true;
    std::vector<uint8_t> last_cdb;
    int last_dir = -1;
    uint32_t last_data_len = 0;
    unsigned last_timeout_s = 0;
    unsigned last_probe_timeout_s = 0;   // open 探测（容量/INQUIRY）透传的超时秒数
    // 容量直通应答旋钮（pass_through 的 0x25/0x9E 分支，evolve #75）；
    // 默认 ≈8GB 正常盘（read_capacity 已收编为经内核委托，此为唯一容量旋钮）
    unsigned long long cap10_last_lba = 15667199;
    unsigned cap10_block = 512;
    unsigned long long cap16_last_lba = 0x123456789ull;   // ≈2.5TB 盘（哨兵回退用例）
    unsigned cap16_block = 512;
    bool cap16_fail = false;
    unsigned cap10_calls = 0, cap16_calls = 0;
    // auto_detect 假件旋钮（msc_auto_detect_impl 每轮默认构造新实例，按盘号的
    // 行为差异只能经静态配置表达；默认 0 = 全盘 USB/容量恒成功，不影响既有用例）
    static inline unsigned s_usb_from = 0;        // 盘号 ≥ 此值才算 BusTypeUsb
    static inline unsigned s_cap_from = 0;        // 盘号 ≥ 此值 READ_CAPACITY 才成功
    static inline unsigned s_last_probe_timeout_s = 0;  // 跨实例镜像（实例随轮析构）

private:
    std::atomic<bool> m_open{false};
};

// 强制编译产线实例的全部成员（不运行）
template class SerialChannelT<SerialPort>;
template class HidChannelT<HidPort>;
template class WinUsbChannelT<WinUsbPort>;
template class MscChannelT<MscScsi>;

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

        // 有界写契约（evolve #72）：通道 send 显式透传 kSendTimeoutMs（对抗复核
        // 4-1：不依赖端口层默认参，防两侧默认漂移）；写失败（NAK 永续口径）不
        // 计统计且恢复后续发——真件的超时+CancelIoEx+GOR 回收序列依赖 OS 行为
        // 无法离线模拟，离线钉住通道层契约
        check(ch.port().last_write_timeout_ms == 3000, "HID send 透传通道级 3s 写超时");
        // 回退路径二次尝试策略（evolve #73，纯逻辑）：瞬时失败可二次尝试（句柄
        // 组合兼容回退），慢失败（≥1s，传输到达过设备）不再重试——否则 UI 线程
        // send 的等待上界被二次不受控控制传输放大（#72 残余缺陷）
        check(hid_sync_retry_allowed(0) && hid_sync_retry_allowed(999),
              "HID 回退策略：瞬时失败(<1s)允许二次尝试");
        check(!hid_sync_retry_allowed(1000) && !hid_sync_retry_allowed(5000),
              "HID 回退策略：慢失败(≥1s)不再二次尝试");
        ch.port().fail_write = true;
        check(!ch.send(rep1, sizeof rep1, &err), "HID 写失败 send 返回 false");
        check(!err.empty(), "HID 写失败给出 err");
        check(ch.stats().tx_frames == 3, "HID 写失败不计统计");
        ch.port().fail_write = false;
        check(ch.send(rep1, sizeof rep1, &err), "HID 写恢复后续发成功");

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
    // ============ WinUsbPort 纯逻辑（EP-4 S5 前半） ============
    {   // 路径 VID/PID 解析：大小写不敏感、1~4 位十六进制、缺失置 0
        unsigned v = 9, p = 9;
        WinUsbPort::parse_vid_pid(
            L"\\\\?\\usb#vid_1234&pid_00ab#6&1f2e3d4&0&1#{a5dcbf10-6530-11d2-901f-00c0047959a1}",
            &v, &p);
        check(v == 0x1234 && p == 0x00AB, "parse_vid_pid 小写全4位");
        WinUsbPort::parse_vid_pid(L"\\\\?\\usb#VID_1A2&PID_B#x", &v, &p);
        check(v == 0x1A2 && p == 0xB, "parse_vid_pid 大写短位");
        WinUsbPort::parse_vid_pid(L"\\\\?\\hid#vid_1234#x", &v, &p);
        check(v == 0 && p == 0, "parse_vid_pid 无 pid 置 0");
    }
    {   // 数据管道选型：批量优先、中断回退、同型取编号最小、只出不进
        auto mk = [](unsigned char id, bool in, bool bulk) {
            WinUsbPipeInfo pi;
            pi.id = id;
            pi.is_in = in;
            pi.is_bulk = bulk;
            return pi;
        };
        unsigned char in_id = 0, out_id = 0;
        bool in_bulk = false, out_bulk = false;
        auto sel = [&](const std::vector<WinUsbPipeInfo>& ps) {
            return WinUsbPort::select_data_pipes(ps, &in_id, &in_bulk, &out_id, &out_bulk);
        };
        check(sel({mk(0x81, true, true), mk(0x01, false, true)}) && in_id == 0x81 &&
                  out_id == 0x01 && in_bulk && out_bulk,
              "选型：批量 IN+OUT");
        check(sel({mk(0x81, true, false), mk(0x02, false, false)}) && !in_bulk && !out_bulk &&
                  out_id == 0x02,
              "选型：中断回退");
        check(sel({mk(0x81, true, true), mk(0x01, false, false), mk(0x86, true, false)}) &&
                  in_id == 0x81 && in_bulk && out_id == 0x01 && !out_bulk,
              "选型：混合批量IN+中断OUT");
        check(sel({mk(0x84, true, true), mk(0x82, true, true)}) && in_id == 0x82 && out_id == 0,
              "选型：同型取编号最小 / 只出不进 OUT=0");
        check(sel({}) == false, "选型：空管道表 false");
        in_id = 1;   // 上一轮残留应被重置
        check(sel({mk(0x01, false, true)}) == false && in_id == 0 && out_id == 0x01,
              "选型：只进不出 IN=0");
    }

    // ============ WinUsbChannel（EP-4 S5 前半） ============
    using TestUsb = WinUsbChannelT<MockWinUsbPort>;
    const std::wstring kUsbPath =
        L"\\\\?\\usb#vid_1234&pid_00ab#6&1f2e3d4&0&1#{a5dcbf10-6530-11d2-901f-00c0047959a1}";

    {   // 契约：未开即关 + 未开即发
        TestUsb ch(kUsbPath);
        check(!ch.is_open(), "WinUSB 初始未打开");
        check(ch.desc().kind.empty(), "WinUSB 未打开无描述");
        std::wstring err;
        const uint8_t b[] = {0xAA};
        check(!ch.send(b, sizeof b, &err), "WinUSB 未打开 send 失败");
        ch.close();                                     // 未开即关不崩溃
    }
    {   // 打开失败 → err 非空、仍处于关闭
        TestUsb ch(kUsbPath);
        ch.port().fail_open = true;
        std::wstring err;
        check(!ch.open(&err), "WinUSB open 失败返回 false");
        check(!err.empty(), "WinUSB open 失败给出 err");
        check(!ch.is_open(), "WinUSB 失败后未打开");
    }
    {   // 回显会话：描述（VID:PID 自路径解析+管道型别）、整传输一帧、统计、清回调
        TestUsb ch(kUsbPath);
        ch.port().m_caps.in_pipe = 0x81;
        ch.port().m_caps.in_is_bulk = true;
        ch.port().m_caps.out_pipe = 0x01;
        ch.port().m_caps.out_is_bulk = true;
        std::wstring err;
        check(ch.open(&err), "WinUSB open 成功");
        check(ch.desc().kind == L"usb", "WinUSB kind=usb");
        check(ch.desc().display ==
                  L"WinUSB 1234:00AB IN 0x81(bulk) OUT 0x01(bulk)", "WinUSB display 文本");
        check(ch.desc().path == kUsbPath, "WinUSB desc.path");

        std::mutex cb_mtx;
        std::vector<std::vector<uint8_t>> frames;
        ch.set_receive_callback([&](const std::vector<uint8_t>& b) {
            std::lock_guard<std::mutex> g(cb_mtx);
            frames.push_back(b);
        });

        const uint8_t pkt1[] = {0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};   // 8 字节传输
        check(ch.send(pkt1, sizeof pkt1, &err), "WinUSB send-1 成功");
        check(wait_for([&] {
            std::lock_guard<std::mutex> g(cb_mtx);
            return frames.size() >= 1 &&
                   frames[0] == std::vector<uint8_t>(pkt1, pkt1 + 8);
        }), "WinUSB 回显帧=整传输一帧");
        auto st = ch.stats();
        check(st.tx_frames == 1 && st.tx_bytes == 8 && st.rx_frames == 1 && st.rx_bytes == 8,
              "WinUSB tx/rx 统计");

        ch.set_receive_callback(nullptr);               // 清回调后不再投递
        Sleep(150);
        size_t n_at_clear = frames.size();
        const uint8_t pkt2[] = {0x5A};
        check(ch.send(pkt2, sizeof pkt2, &err), "WinUSB 清回调后 send");
        check(wait_for([&] { return ch.stats().rx_bytes == 9; }), "WinUSB 清回调后仍有 rx 统计");
        Sleep(150);
        {
            std::lock_guard<std::mutex> g(cb_mtx);
            check(frames.size() == n_at_clear, "WinUSB 清回调后不再调用");
        }

        ch.close();
        check(!ch.is_open(), "WinUSB close 后未打开");
    }
    {   // 只读设备（无 OUT 管道）：open 成功，send 拒绝并给出 err，统计不动
        TestUsb ch(kUsbPath);
        ch.port().m_caps.in_pipe = 0x81;
        ch.port().m_caps.in_is_bulk = true;
        ch.port().m_caps.out_pipe = 0;
        std::wstring err;
        check(ch.open(&err), "只读设备 open 成功");
        const uint8_t b[] = {0x01};
        check(!ch.send(b, sizeof b, &err), "无 OUT 管道 send 拒绝");
        check(!err.empty(), "无 OUT 管道 send 给出 err");
        check(ch.stats().tx_frames == 0, "拒绝的 send 不计统计");
        ch.close();
    }
    {   // 只出不进设备（无 IN 管道）：open 成功且不起读线程，send 正常
        TestUsb ch(kUsbPath);
        ch.port().m_caps.in_pipe = 0;
        ch.port().m_caps.out_pipe = 0x01;
        ch.port().m_caps.out_is_bulk = true;
        std::wstring err;
        check(ch.open(&err), "只出不进 open 成功");
        const uint8_t b[] = {0x01, 0x02};
        check(ch.send(b, sizeof b, &err), "只出不进 send 成功");
        Sleep(120);                                     // 若误起读线程，read_pipe 立即报错退出
        check(ch.is_open(), "只出不进通道保持打开");

        // 有界写契约（evolve #72，#71 签名漂移补覆盖）：透传通道级 3s 写超时
        // （kSendTimeoutMs，不依赖端口默认参）；写失败不计统计、恢复后续发
        // （无读线程干扰，纯写路径）
        check(ch.port().last_write_timeout_ms == 3000, "WinUSB send 透传通道级 3s 写超时");
        ch.port().fail_write = true;
        check(!ch.send(b, sizeof b, &err), "WinUSB 写失败 send 返回 false");
        check(!err.empty(), "WinUSB 写失败给出 err");
        check(ch.stats().tx_frames == 1, "WinUSB 写失败不计统计");
        ch.port().fail_write = false;
        check(ch.send(b, sizeof b, &err), "WinUSB 写恢复后续发成功");

        ch.close();
    }
    {   // 拔出模拟：读线程退出；send 写路径独立仍成功；句柄未关仍报 is_open
        TestUsb ch(kUsbPath);
        ch.port().m_caps.in_pipe = 0x81;
        ch.port().m_caps.in_is_bulk = true;
        ch.port().m_caps.out_pipe = 0x01;
        ch.port().m_caps.out_is_bulk = true;
        std::wstring err;
        check(ch.open(&err), "拔出模拟 open");
        ch.port().fail_read = true;                     // 下一轮读即"设备拔出"
        Sleep(150);                                     // 等读线程退出
        const uint8_t b[] = {0x00, 0x01};
        check(ch.send(b, sizeof b, &err), "WinUSB 拔出后 send（写路径独立）仍成功");
        Sleep(150);
        check(ch.stats().rx_frames == 0 && ch.stats().rx_bytes == 0,
              "WinUSB 读线程退出后无 rx");
        check(ch.is_open(), "WinUSB 通道仍报 is_open（句柄未关）");
    }
    {   // 重开：close → open 复用同一通道对象
        TestUsb ch(kUsbPath);
        ch.port().m_caps.in_pipe = 0x81;
        ch.port().m_caps.out_pipe = 0x01;
        ch.port().m_caps.out_is_bulk = true;
        std::wstring err;
        check(ch.open(&err), "WinUSB 重开第一次 open");
        const uint8_t b[] = {0x01, 0x02};
        check(ch.send(b, sizeof b, &err), "WinUSB 重开第一次 send");
        check(wait_for([&] { return ch.stats().rx_frames >= 1; }), "WinUSB 重开第一次回读");
        ch.close();
        check(ch.open(&err), "WinUSB 重开第二次 open");
        check(ch.send(b, sizeof b, &err), "WinUSB 重开第二次 send");
        check(wait_for([&] { return ch.stats().rx_frames >= 2; }), "WinUSB 重开第二次回读");
    }
    {   // set_read_timeout(0) 回退默认值不崩溃 + 析构路径
        TestUsb ch(kUsbPath);
        ch.port().m_caps.in_pipe = 0x81;
        ch.port().m_caps.out_pipe = 0x01;
        ch.set_read_timeout(0);
        std::wstring err;
        check(ch.open(&err), "WinUSB 短超时 open");
        ch.set_read_timeout(0);                         // 打开态改超时
        Sleep(120);
    }   // 析构（读线程在跑）——验证 ~WinUsbChannelT 不挂死

    // ============ MscChannel（EP-4 S5 后半） ============
    using TestMsc = MscChannelT<MockMscPort>;

    {   // parse_drive_index：三种合法形态 + 三种非法形态
        unsigned idx = 99;
        check(TestMsc::parse_drive_index(L"\\\\.\\PhysicalDrive3", &idx) && idx == 3,
              "drive 解析 \\\\.\\PhysicalDrive3");
        check(TestMsc::parse_drive_index(L"PhysicalDrive12", &idx) && idx == 12,
              "drive 解析裸名 PhysicalDrive12");
        check(TestMsc::parse_drive_index(L"physicaldrive0", &idx) && idx == 0,
              "drive 解析大小写不敏感");
        check(TestMsc::parse_drive_index(L"7", &idx) && idx == 7, "drive 解析纯数字");
        check(!TestMsc::parse_drive_index(L"COM7", &idx), "drive 解析拒绝 COM 名");
        check(!TestMsc::parse_drive_index(L"\\\\.\\PhysicalDrive", &idx),
              "drive 解析拒绝无数字");
        check(!TestMsc::parse_drive_index(L"PhysicalDrive3x", &idx),
              "drive 解析拒绝尾随杂字符");
    }
    {   // msc_plan_cdb：方向/响应长度表（SPC/SBC 口径）
        const uint8_t inquiry6[6] = {0x12, 0, 0, 0, 0x24, 0};
        check(msc_plan_cdb(inquiry6, 6, 512).dir == kMscDirIn
                  && msc_plan_cdb(inquiry6, 6, 512).resp_len == 36,
              "CDB 计划 INQUIRY 长度取 cdb[4]");
        const uint8_t alloc0[6] = {0x12, 0, 0, 0, 0, 0};
        check(msc_plan_cdb(alloc0, 6, 512).resp_len == 0, "CDB 计划 INQUIRY 分配 0");
        const uint8_t rcap[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        check(msc_plan_cdb(rcap, 10, 512).dir == kMscDirIn
                  && msc_plan_cdb(rcap, 10, 512).resp_len == 8,
              "CDB 计划 READ_CAPACITY 恒 8");
        const uint8_t rcap16[16] = {0x9E, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0};
        check(msc_plan_cdb(rcap16, 16, 512).dir == kMscDirIn
                  && msc_plan_cdb(rcap16, 16, 512).resp_len == 32,
              "CDB 计划 READ_CAPACITY(16) 分配长度大端 [10..13]");
        const uint8_t sa_other[16] = {0x9E, 0x11, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0};
        check(msc_plan_cdb(sa_other, 16, 512).dir == kMscDirUnspec,
              "CDB 计划 0x9E 其余 service action 按无数据直通");
        uint8_t read10[10] = {0x28, 0, 0, 0, 0, 0, 0, 0x00, 0x04, 0};
        check(msc_plan_cdb(read10, 10, 512).dir == kMscDirIn
                  && msc_plan_cdb(read10, 10, 512).resp_len == 4 * 512,
              "CDB 计划 READ10 块数×块大小");
        check(msc_plan_cdb(read10, 10, 0).resp_len == 0, "CDB 计划块大小未知置 0");
        uint8_t read12[12] = {0xA8, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0};   // 块数大端 [6..9]=2
        check(msc_plan_cdb(read12, 12, 512).resp_len == 2 * 512,
              "CDB 计划 READ12 块数大端 [6..9]");
        read10[7] = 0xFF;
        read10[8] = 0xFF;
        check(msc_plan_cdb(read10, 10, 512).resp_len == (1u << 20),
              "CDB 计划巨型 READ 封顶 1MiB");
        const uint8_t read16[16] = {0x88, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 3, 0, 2, 0, 0};
        check(msc_plan_cdb(read16, 16, 512).dir == kMscDirIn
                  && msc_plan_cdb(read16, 16, 512).resp_len == 2 * 512,
              "CDB 计划 READ16 块数大端 [12..13]（group [10..11] 不计入，#75）");
        const uint8_t tur[6] = {0x00, 0, 0, 0, 0, 0};
        check(msc_plan_cdb(tur, 6, 512).dir == kMscDirUnspec
                  && msc_plan_cdb(tur, 6, 512).resp_len == 0,
              "CDB 计划 TEST_UNIT_READY 无数据");
        const uint8_t write10[10] = {0x2A, 0, 0, 0, 0, 0, 0, 0, 1, 0};
        check(msc_plan_cdb(write10, 10, 512).dir == kMscDirOut, "CDB 计划 WRITE10 为 OUT");
        const uint8_t msel6[6] = {0x15, 0, 0, 0, 0, 0};
        check(msc_plan_cdb(msel6, 6, 512).dir == kMscDirOut, "CDB 计划 MODE_SELECT(6) 为 OUT");
        const uint8_t unknown[10] = {0xEF, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        check(msc_plan_cdb(unknown, 10, 512).dir == kMscDirUnspec,
              "CDB 计划未收录操作码按无数据直通");
        check(msc_plan_cdb(inquiry6, 5, 512).dir == kMscDirInvalid, "CDB 计划拒绝 5 字节");
        check(msc_plan_cdb(inquiry6, 8, 512).dir == kMscDirInvalid, "CDB 计划拒绝 8 字节");
    }
    {   // 契约：未开即发 + 未开即关
        TestMsc ch(L"\\\\.\\PhysicalDrive3");
        check(!ch.is_open(), "MSC 初始未打开");
        check(ch.desc().kind.empty(), "MSC 未打开无描述");
        const uint8_t cdb[6] = {0x12, 0, 0, 0, 0x24, 0};
        std::wstring err;
        check(!ch.send(cdb, 6, &err), "MSC 未打开 send 失败");
        ch.close();                                    // 未开即关不崩溃
    }
    {   // 打开失败 → err 非空、仍处于关闭
        TestMsc ch(L"\\\\.\\PhysicalDrive3");
        ch.port().fail_open = true;
        std::wstring err;
        check(!ch.open(&err), "MSC open 失败返回 false");
        check(!err.empty(), "MSC open 失败给出 err");
        check(!ch.is_open(), "MSC 失败后未打开");
    }
    {   // 会话主径：描述（INQUIRY 身份+容量）、CDB→数据帧、无数据命令、统计
        TestMsc ch(L"\\\\.\\PhysicalDrive3");
        std::wstring err;
        check(ch.open(&err), "MSC open 成功");
        check(ch.desc().kind == L"msc", "MSC kind=msc");
        check(ch.desc().path == L"\\\\.\\PhysicalDrive3", "MSC desc.path");
        check(ch.desc().display.find(L"PhysicalDrive3") != std::wstring::npos
                  && ch.desc().display.find(L"MOCKLAB UDISK-300") != std::wstring::npos
                  && ch.desc().display.find(L"8.02GB") != std::wstring::npos
                  && ch.desc().display.find(L"512B") != std::wstring::npos,
              "MSC display 含盘号/身份/容量/块大小");
        check(ch.port().m_last_write_access == false, "MSC 只读打开（write_access=false）");

        std::vector<std::vector<uint8_t>> frames;
        ch.set_receive_callback([&](const std::vector<uint8_t>& b) { frames.push_back(b); });

        const uint8_t inquiry[6] = {0x12, 0, 0, 0, 0x24, 0};
        check(ch.send(inquiry, 6, &err), "MSC send INQUIRY 成功");
        check(frames.size() == 1 && frames[0].size() == 36, "MSC INQUIRY 数据帧 36 字节");
        check(frames[0][0] == 0x12 && frames[0][5] == (5 ^ 0x12), "MSC 数据帧确定性图案");
        check(ch.port().last_cdb.size() == 6 && ch.port().last_cdb[0] == 0x12,
              "MSC 直通收到原样 CDB");
        check(ch.port().last_dir == kMscDirIn, "MSC 直通方向 IN");
        check(ch.port().last_data_len == 36, "MSC 直通缓冲 36");
        ChannelStats st = ch.stats();
        check(st.tx_frames == 1 && st.tx_bytes == 6 && st.rx_frames == 1 && st.rx_bytes == 36,
              "MSC INQUIRY 统计");
        check(ch.port().last_timeout_s == 3, "MSC 默认 3s 有界超时（UI 线程 send 上界）");

        const uint8_t tur[6] = {0x00, 0, 0, 0, 0, 0};
        check(ch.send(tur, 6, &err), "MSC send TEST_UNIT_READY 成功");
        check(frames.size() == 1, "MSC 无数据命令不产帧");
        check(ch.stats().tx_frames == 2, "MSC TUR 计 tx");

        ch.port().force_status = 2;                    // CHECK CONDITION
        uint8_t read10[10] = {0x28, 0, 0, 0, 0, 0, 0, 0, 0x04, 0};
        err.clear();
        check(ch.send(read10, 10, &err), "MSC CHECK CONDITION 仍算命令完成");
        check(frames.size() == 2 && frames[1].size() == 18, "MSC SENSE 帧 18 字节");
        check(frames[1][0] == 0x70 && frames[1][2] == 0x05 && frames[1][12] == 0x21,
              "MSC SENSE 帧 key/ASC 原样");
        st = ch.stats();
        check(st.rx_frames == 2 && st.rx_bytes == 36 + 18, "MSC SENSE 计 rx");
        ch.port().force_status = 0;

        const uint8_t write10[10] = {0x2A, 0, 0, 0, 0, 0, 0, 0, 1, 0};
        err.clear();
        check(!ch.send(write10, 10, &err), "MSC WRITE10 拒收");
        check(err.find(L"只读") != std::wstring::npos, "MSC 拒收给出只读原因");
        check(ch.stats().tx_frames == 3, "MSC 拒收不计统计（前 3 条已发）");

        const uint8_t bad5[5] = {0x12, 0, 0, 0, 0x24};
        err.clear();
        check(!ch.send(bad5, 5, &err), "MSC 5 字节 CDB 拒收");
        check(!err.empty(), "MSC 非法 CDB 给出 err");

        ch.port().fail_ioctl = true;                   // OS 层失败 → send false
        err.clear();
        check(!ch.send(inquiry, 6, &err), "MSC OS 层失败 send false");
        check(!err.empty(), "MSC OS 层失败给出 err");
        check(frames.size() == 2, "MSC OS 层失败不产帧");
        ch.port().fail_ioctl = false;

        ch.set_read_timeout(2000);
        check(ch.send(inquiry, 6, &err) && ch.port().last_timeout_s == 2,
              "MSC set_read_timeout 2000ms→2s");
        ch.set_read_timeout(500);
        check(ch.send(inquiry, 6, &err) && ch.port().last_timeout_s == 1,
              "MSC set_read_timeout 500ms 向上取整 1s");
        ch.set_read_timeout(0);
        check(ch.send(inquiry, 6, &err) && ch.port().last_timeout_s == 3,
              "MSC set_read_timeout 0→通道默认 3s（不回落端口 10s，#72 残余埋雷）");
        ch.set_read_timeout(4294967295u);
        check(ch.send(inquiry, 6, &err) && ch.port().last_timeout_s == 4294968u,
              "MSC set_read_timeout 极大值无回绕（(2^32-1+999)/1000，不为 0）");

        ch.close();
        check(!ch.is_open(), "MSC close 后未打开");
        check(ch.open(&err), "MSC 重开成功");
        check(ch.port().last_probe_timeout_s == 3, "MSC open 探测（容量/INQUIRY）透传 3s 有界");
        ch.close();
    }
    {   // msc_auto_detect_impl：产测域孪生探测路径（evolve #74，收口 #73 残余 2b）——
        // 假件按盘号配置 BusType/容量成败，钉住选择行为与 3s 有界超时透传
        //（旧实现不传超时 → 端口默认 10s；断言 3≠假件默认 0，非巧合钉）
        check(msc_auto_detect_impl<MockMscPort>() == 0, "auto_detect 默认取首个 USB 盘 0");
        check(MockMscPort::s_last_probe_timeout_s == 3,
              "auto_detect 探测透传 3s 有界（不回落端口默认 10s）");
        MockMscPort::s_usb_from = 2;
        check(msc_auto_detect_impl<MockMscPort>() == 2, "auto_detect 跳过非 USB 盘 0/1");
        MockMscPort::s_cap_from = 5;
        check(msc_auto_detect_impl<MockMscPort>() == 5, "auto_detect 跳过容量探测失败盘");
        MockMscPort::s_usb_from = 10;
        check(msc_auto_detect_impl<MockMscPort>() == -1, "auto_detect 无 USB 盘返回 -1");
        MockMscPort::s_usb_from = 0;   // 还原全局默认（后续若有用例不受旋钮影响）
        MockMscPort::s_cap_from = 0;
    }
    {   // msc_capacity_probe_impl：RC10 → 哨兵 → RC16（SBC-3，#75 关闭 README 限制 #3）
        MockMscPort p;
        check(p.open_physical_drive(0, false, nullptr), "容量内核 假件开盘");
        unsigned long long total = 0;
        unsigned blk = 0;
        std::wstring err;
        check(msc_capacity_probe_impl(p, &total, &blk, &err, 3),
              "容量内核 ≤2TB 盘 RC10 即成功");
        check(total == 15667200 && blk == 512, "容量内核 RC10 扇区/块大小解析");
        check(p.cap10_calls == 1 && p.cap16_calls == 0, "容量内核 ≤2TB 不升 RC16");
        check(p.last_timeout_s == 3, "容量内核 RC10 超时透传（≠假件默认 0）");
        check(p.last_dir == kMscDirIn, "容量内核 RC10 方向 IN");

        p.cap10_last_lba = 0xFFFFFFFFull;            // >2TB 哨兵（SBC-3 口径）
        p.cap16_last_lba = 0x123456789ull;           // ≈2.5TB 盘
        check(msc_capacity_probe_impl(p, &total, &blk, &err, 3), "容量内核 哨兵盘升 RC16 成功");
        check(total == 0x12345678Aull && blk == 512, "容量内核 RC16 BE64 扇区解析");
        check(p.cap10_calls == 2 && p.cap16_calls == 1, "容量内核 哨兵盘恰一次 RC16");
        check(p.last_cdb.size() == 16 && p.last_cdb[0] == 0x9E && p.last_cdb[1] == 0x10,
              "容量内核 RC16 CDB 16 字节 0x9E/SA=0x10");
        check(p.last_cdb[13] == 32, "容量内核 RC16 分配长度 32 于大端 [10..13]");
        check(p.last_data_len == 32, "容量内核 RC16 直通缓冲 32");
        check(p.last_timeout_s == 3, "容量内核 RC16 超时同界透传（探测上界不漂移）");

        p.cap16_fail = true;                         // RC16 失败 → false + 留痕
        err.clear();
        check(!msc_capacity_probe_impl(p, &total, &blk, &err, 3), "容量内核 RC16 失败回 false");
        check(err.find(L"READ_CAPACITY(16)（>2TB）") != std::wstring::npos,
              "容量内核 RC16 失败留痕命令名（内核前缀，非假件文案）");

        p.cap16_fail = false;
        p.cap10_last_lba = 100;                      // RC10 块长 0（≤2TB 分支拒绝）
        p.cap10_block = 0;
        check(!msc_capacity_probe_impl(p, &total, &blk, &err, 3)
                  && err.find(L"块大小 0") != std::wstring::npos,
              "容量内核 RC10 块长 0 拒绝");
        p.cap10_last_lba = 0xFFFFFFFFull;            // RC16 块长 0（哨兵分支拒绝）
        p.cap10_block = 512;
        p.cap16_block = 0;
        check(!msc_capacity_probe_impl(p, &total, &blk, &err, 3)
                  && err.find(L"READ_CAPACITY(16) 返回块大小 0") != std::wstring::npos,
              "容量内核 RC16 块长 0 拒绝");
    }
    {   // msc_read_cdb：READ(10)/(16) 选路与大端编码（纯逻辑，#75）
        uint8_t cdb[16] = {};
        uint8_t n = 0;
        check(msc_read_cdb(0x12345678, 4, cdb, &n) == 0x28 && n == 10,
              "read CDB ≤32 位 LBA 用 READ10");
        check(cdb[2] == 0x12 && cdb[3] == 0x34 && cdb[4] == 0x56 && cdb[5] == 0x78,
              "read CDB READ10 LBA 大端 [2..5]");
        check(cdb[7] == 0 && cdb[8] == 4, "read CDB READ10 块数大端 [7..8]");
        check(msc_read_cdb(0xFFFFFFFFull, 1, cdb, &n) == 0x28 && n == 10,
              "read CDB 最高 LBA=0xFFFFFFFF 仍 READ10（边界含端）");
        check(msc_read_cdb(0x100000000ull, 1, cdb, &n) == 0x88 && n == 16,
              "read CDB LBA=0x100000000 升 READ16");
        check(cdb[5] == 1 && cdb[2] == 0 && cdb[6] == 0 && cdb[9] == 0,
              "read CDB READ16 LBA 大端 8 字节（2^32 落 [5]，低 32 位同 READ10 源）");
        check(cdb[12] == 0 && cdb[13] == 1, "read CDB READ16 块数大端 [12..13]");
        check(msc_read_cdb(0xFFFFFFFFull, 2, cdb, &n) == 0x88,
              "read CDB LBA+块数越 32 位升 READ16");
        check(msc_read_cdb(0x123456789ABCDEF0ull, 1, cdb, &n) == 0x88 && cdb[2] == 0x12
                  && cdb[9] == 0xF0,
              "read CDB READ16 64 位 LBA 首尾字节落 [2]/[9]");
    }
    {   // msc_read_blocks_impl：容量同源 + READ(10)/(16) 选路接线（对抗复核 P2，#75）
        MockMscPort p;
        p.open_physical_drive(0, false, nullptr);
        std::vector<uint8_t> out;
        std::wstring err;
        check(msc_read_blocks_impl(p, 100, 4, out, &err), "read 内核 低 LBA READ10 成功");
        check(p.last_cdb.size() == 10 && p.last_cdb[0] == 0x28, "read 内核 低 LBA 仍 READ10");
        check(out.size() == 4 * 512 && out[1] == (1 ^ 0x28), "read 内核 数据帧图案+长度");

        p.cap10_last_lba = 0xFFFFFFFFull;            // >2TB：容量升 RC16，读选 READ16
        p.cap16_last_lba = 0x123456789ull;
        check(msc_read_blocks_impl(p, 0x100000000ull, 2, out, &err),
              "read 内核 高 LBA READ16 成功");
        check(p.last_cdb.size() == 16 && p.last_cdb[0] == 0x88,
              "read 内核 高 LBA 发 16 字节 READ16");
        check(p.cap10_calls == 2 && p.cap16_calls == 1,
              "read 内核 容量 RC10+RC16 探测各恰一次");
        check(out.size() == 2 * 512, "read 内核 READ16 长度块数×块长");

        err.clear();
        check(!msc_read_blocks_impl(p, 0x12345678Aull, 1, out, &err), "read 内核 越容量拒绝");
        check(err.find(L"超出容量") != std::wstring::npos, "read 内核 越容量留痕");
        check(!msc_read_blocks_impl(p, 0, 0, out, &err), "read 内核 0 块拒绝");
        check(!msc_read_blocks_impl(p, 0, 1025, out, &err), "read 内核 1025 块拒绝");
    }
    {   // 通道 open 的 >2TB 容量行（假件同形态委托，内核真实穿越，对抗复核 P1）
        TestMsc ch(L"\\\\.\\PhysicalDrive4");
        ch.port().cap10_last_lba = 0xFFFFFFFFull;
        ch.port().cap16_last_lba = 0x123456789ull;   // ≈2502GB
        std::wstring err;
        check(ch.open(&err), ">2TB 盘 open 成功");
        check(ch.desc().display.find(L"2502.00GB") != std::wstring::npos
                  && ch.desc().display.find(L"512B") != std::wstring::npos,
              ">2TB 容量行显示 RC16 真值（0x12345678A 扇区×512B）");
        check(ch.port().cap16_calls >= 1, ">2TB open 探测实际发出 RC16");
    }

    printf("channel_selftest: %d/%d PASS\n", g_pass, g_pass);
    return 0;
}
