// test_engine.cpp — 引擎实现：计划解析、步骤分发、判定、报告。未真机编译，按 MSDN 口径编写。
#include "engine/test_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

// ---------------------------------------------------------------------------
// MessagePumpEvents：有界队列 + 丢弃计数；步骤/完成事件堆载 payload
// ---------------------------------------------------------------------------
void MessagePumpEvents::on_log(wraii::LogLevel level, const std::wstring& text) {
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_queue.size() >= kLogQueueCap) {
            ++m_dropped;                       // 有界队列满：丢弃并计数，防内存膨胀
            return;
        }
        m_queue.push_back(LogEvent{level, text});
    }
    bool expected = false;
    if (m_notify_pending.compare_exchange_strong(expected, true))
        post(kMsgEngineLog, 0, 0);
}

void MessagePumpEvents::on_step(int index, int total, const StepResult& result) {
    auto* p = new StepEvent{index, total, result};
    if (!post(kMsgEngineStep, static_cast<WPARAM>(static_cast<INT_PTR>(index)),
              reinterpret_cast<LPARAM>(p)))
        delete p;                              // 窗口已销毁：就地释放，防泄漏
}

void MessagePumpEvents::on_done(bool all_pass, int exit_code) {
    auto* p = new DoneEvent{all_pass, exit_code};
    if (!post(kMsgEngineDone, 0, reinterpret_cast<LPARAM>(p)))
        delete p;
}

bool MessagePumpEvents::post(UINT msg, WPARAM wp, LPARAM lp) noexcept {
    return ::PostMessageW(m_hwnd, msg, wp, lp) != 0;
}

bool MessagePumpEvents::drain_logs(
    const std::function<void(wraii::LogLevel, const std::wstring&)>& emit,
    unsigned long long* dropped_out) {
    bool more = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        while (!m_queue.empty()) {
            emit(m_queue.front().level, m_queue.front().text);
            m_queue.pop_front();
        }
        if (dropped_out) *dropped_out = m_dropped;
        m_dropped = 0;
        m_notify_pending.store(false, std::memory_order_release);
        more = !m_queue.empty();               // 清除在途标志后又有新日志 → 让 UI 再拉一次
    }
    return more;
}

// ---------------------------------------------------------------------------
// TestEngine 生命周期
// ---------------------------------------------------------------------------
TestEngine::~TestEngine() { stop_and_join(); }

void TestEngine::stop_and_join() noexcept {
    if (m_thread.joinable()) {
        m_thread.request_stop();
        m_thread.join();
    }
}

void TestEngine::start() {
    if (m_thread.joinable()) return;           // 已在运行
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_results.clear();
    }
    m_scan.clear();
    m_started_iso = wraii::now_timestamp_iso();
    m_thread = std::jthread([this](std::stop_token st) { run(st); });
}

// ---------------------------------------------------------------------------
// 计划加载（自研轻量解析，仅支持本方案字段）
// ---------------------------------------------------------------------------
namespace {

std::vector<uint8_t> to_bytes(const std::vector<long long>& nums) {
    std::vector<uint8_t> out;
    out.reserve(nums.size());
    for (long long v : nums) out.push_back(static_cast<uint8_t>(v & 0xFF));
    return out;
}

} // namespace

bool TestEngine::parse_plan_value(const minijson::Value& root, TestPlan& plan, std::wstring* err) {
    if (!root.is_object()) {
        if (err) *err = L"计划根节点必须是对象";
        return false;
    }
    const minijson::Value* steps = root.find(L"steps");
    if (!steps || !steps->is_array()) {
        if (err) *err = L"计划缺少 steps 数组";
        return false;
    }

    plan.name = minijson::obj_str(root, L"name", L"plan");
    plan.station = minijson::obj_str(root, L"station", L"STN-01");
    plan.dut_sn = minijson::obj_str(root, L"dut_sn", L"AUTO");

    if (const minijson::Value* dev = root.find(L"device"); dev && dev->is_object()) {
        plan.device.vid = minijson::obj_vidpid(*dev, L"vid", -1);
        plan.device.pid = minijson::obj_vidpid(*dev, L"pid", -1);
        plan.device.port = minijson::obj_str(*dev, L"port", L"");
        plan.device.telemetry_port = minijson::obj_str(*dev, L"telemetry_port", L"");
        plan.device.baud = static_cast<unsigned>(minijson::obj_int(*dev, L"baud", 115200));
        plan.device.drive = minijson::obj_int(*dev, L"drive", -1);
    }

    for (const auto& sv : steps->array) {
        if (!sv.is_object()) {
            if (err) *err = L"steps 内存在非对象元素";
            return false;
        }
        PlanStep s;
        s.type = minijson::obj_str(sv, L"type", L"");
        if (s.type.empty()) {
            if (err) *err = L"步骤缺少 type 字段";
            return false;
        }
        s.name = minijson::obj_str(sv, L"name", s.type);
        s.timeout_ms = minijson::obj_int(sv, L"timeout_ms", 0);
        s.seconds = minijson::obj_double(sv, L"seconds", 2.0);
        s.report_id = static_cast<unsigned>(minijson::obj_int(sv, L"report_id", 0));
        // Python hid_report_loopback 用 step["timeout_ms"]（缺省 500）作读回显超时；
        // 本引擎同字段兼任步骤级看门狗，故 hid_timeout_ms 取其值，未设则 500。
        s.timeout_ms = minijson::obj_int(sv, L"timeout_ms", 0);
        s.hid_timeout_ms = s.timeout_ms > 0 ? s.timeout_ms : 500;
        s.repeat = minijson::obj_int(sv, L"repeat", 4);
        s.lba = minijson::obj_int(sv, L"lba", 0);
        s.blocks = minijson::obj_int(sv, L"blocks", 8);
        s.loops = minijson::obj_int(sv, L"loops", 8);
        s.command = minijson::obj_str(sv, L"command", L"");
        s.expect_v = minijson::obj_double(sv, L"expect_v", 5.0);
        s.tol_v = minijson::obj_double(sv, L"tol_v", 0.0);
        s.window_ms = static_cast<int>(minijson::obj_int(sv, L"window_ms",
                          static_cast<long long>(minijson::obj_double(sv, L"window", 2.0) * 1000.0)));
        s.expect_usage_page = minijson::obj_vidpid(sv, L"usage_page", -1);
        s.expect_usage = minijson::obj_vidpid(sv, L"usage", -1);

        // limits 嵌套（与 YAML 计划互通）：limits.min_hz / limits.min_gb
        if (const minijson::Value* lim = sv.find(L"limits"); lim && lim->is_object()) {
            s.min_hz = minijson::obj_int(*lim, L"min_hz", 0);
            s.min_gb = minijson::obj_double(*lim, L"min_gb", 0.0);
        } else {
            s.min_hz = minijson::obj_int(sv, L"min_hz", 0);
            s.min_gb = minijson::obj_double(sv, L"min_gb", 0.0);
        }
        // report / pattern：JSON 数组 → 字节数组
        if (const minijson::Value* arr = sv.find(L"report"); arr && arr->is_array()) {
            std::vector<long long> nums;
            for (const auto& e : arr->array) nums.push_back(e.as_int(0));
            s.report = to_bytes(nums);
        }
        if (const minijson::Value* arr = sv.find(L"pattern"); arr && arr->is_array()) {
            std::vector<long long> nums;
            for (const auto& e : arr->array) nums.push_back(e.as_int(0));
            s.pattern = to_bytes(nums);
        }
        plan.steps.push_back(std::move(s));
    }
    return true;
}

bool TestEngine::load_plan(const std::wstring& json_path, std::wstring* err) {
    std::wstring text;
    if (!wraii::read_text_file_utf8(json_path, text, err)) return false;

    minijson::Value root;
    std::wstring perr;
    size_t ppos = 0;
    if (!minijson::parse(text, root, &perr, &ppos)) {
        if (err) *err = wraii::fmt_v(L"JSON 解析失败 @字符 %zu: %s", ppos, perr.c_str());
        return false;
    }

    TestPlan plan;
    if (!parse_plan_value(root, plan, err)) return false;

    std::lock_guard<std::mutex> lk(m_mtx);
    m_plan = std::move(plan);
    m_plan_loaded = true;
    return true;
}

// ---------------------------------------------------------------------------
// 引擎主循环
// ---------------------------------------------------------------------------
void TestEngine::run(std::stop_token st) {
    TestPlan plan;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        plan = m_plan;
    }
    const int total = static_cast<int>(plan.steps.size());

    m_ev.on_log(wraii::LogLevel::Info,
                wraii::fmt_v(L"=== 测试开始 计划[%s] 工位[%s] DUT[%s]，共 %d 步 ===",
                             plan.name.c_str(), plan.station.c_str(), plan.dut_sn.c_str(), total));

    long long passed = 0, failed = 0;
    bool aborted = false;

    for (int i = 0; i < total; ++i) {
        if (st.stop_requested()) { aborted = true; break; }
        const PlanStep& stp = plan.steps[static_cast<size_t>(i)];
        m_ev.on_log(wraii::LogLevel::Info,
                    wraii::fmt_v(L"▶ 步骤 %d/%d %s (%s)", i + 1, total, stp.name.c_str(),
                                 stp.type.c_str()));

        // 步骤级超时 + 用户停止，统一以 CancelFn 注入各模块
        ULONGLONG t0 = ::GetTickCount64();
        long long timeout = stp.timeout_ms > 0 ? stp.timeout_ms : 60000;
        CancelFn cancel = [st, t0, timeout]() {
            return st.stop_requested() ||
                   (::GetTickCount64() - t0) > static_cast<ULONGLONG>(timeout);
        };

        StepResult r = run_step(stp, cancel, i, total);
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_results.push_back(r);
        }
        m_ev.on_log(r.pass ? wraii::LogLevel::Info : wraii::LogLevel::Error,
                    wraii::fmt_v(L"%s | %s %s", r.pass ? L"PASS" : L"FAIL", r.name.c_str(),
                                 r.note.c_str()));
        m_ev.on_step(i, total, r);
        if (r.pass) ++passed; else ++failed;
    }
    if (!aborted && st.stop_requested()) aborted = true;

    const bool verdict = !aborted && failed == 0 && (passed + failed) > 0;

    // 自动写盘 reports\（失败降级：UI 可 Ctrl+S 手动导出）
    std::wstring dir = default_reports_dir();
    ::CreateDirectoryW(dir.c_str(), nullptr);   // 已存在时静默失败，可忽略
    std::wstring path = dir + L"\\" + suggest_report_name();
    std::wstring werr;
    if (write_report_to(path, &werr))
        m_ev.on_log(wraii::LogLevel::Info, L"报告已写盘: " + path);
    else
        m_ev.on_log(wraii::LogLevel::Warn, L"报告写盘失败: " + werr + L"（可用 Ctrl+S 导出）");

    const int code = aborted ? static_cast<int>(ExitCode::Aborted)
                             : (verdict ? static_cast<int>(ExitCode::Pass)
                                        : static_cast<int>(ExitCode::Fail));
    m_ev.on_log(wraii::LogLevel::Info,
                wraii::fmt_v(L"=== 测试结束 判定 %s 退出码 %d ===",
                             aborted ? L"ABORTED" : (verdict ? L"PASS" : L"FAIL"), code));
    m_ev.on_done(verdict, code);
}

// ---------------------------------------------------------------------------
// 步骤分发（步骤级异常不中断整站，与 Python 版口径一致）
// ---------------------------------------------------------------------------
StepResult TestEngine::run_step(const PlanStep& st, const CancelFn& cancel, int index, int total) {
    StepResult r;
    r.name = st.name.empty() ? st.type : st.name;
    const std::wstring step_name = r.name;
    try {
        if (st.type == L"enumerate")              r = step_enumerate(st);
        else if (st.type == L"descriptor_check")  r = step_descriptor_check(st);
        else if (st.type == L"hid_polling_rate")  r = step_hid_polling_rate(st, cancel);
        else if (st.type == L"hid_output_write")  r = step_hid_output_write(st);
        else if (st.type == L"hid_report_loopback") r = step_hid_report_loopback(st);
        else if (st.type == L"serial_loopback")   r = step_serial_loopback(st, cancel);
        else if (st.type == L"msc_inquiry")       r = step_msc_inquiry(st);
        else if (st.type == L"msc_capacity")      r = step_msc_capacity(st);
        else if (st.type == L"msc_read_verify")   r = step_msc_read_verify(st, cancel);
        else if (st.type == L"pd_attach")         r = step_pd_attach(st, cancel);
        else if (st.type == L"pd_negotiate")      r = step_pd_negotiate(st, cancel);
        else if (st.type == L"measure_voltage")   r = step_measure_voltage(st, cancel);
        else if (st.type == L"msc_write_verify")
            r = fail_step(L"DESTRUCTIVE 写校验步骤，Windows 上位机默认不执行（见 README 已知限制）");
        else if (st.type == L"line_coding" || st.type == L"dfu_verify")
            r = fail_step(L"该步骤类型需要 WinUSB/外部工具链，本版未实现（见 README 已知限制）");
        else
            r = fail_step(wraii::fmt_v(L"未知步骤类型 %s", st.type.c_str()));
    } catch (const std::exception& e) {
        r = fail_step(wraii::fmt_v(L"异常: %hs", e.what()));
    } catch (...) {
        r = fail_step(L"异常: 未知");
    }
    r.name = step_name;   // 各 step_* 不填显示名，统一由此回填
    (void)index;
    (void)total;
    return r;
}

StepResult TestEngine::fail_step(std::wstring note) {
    StepResult r;
    r.pass = false;
    r.note = std::move(note);
    return r;
}

// ---------------------------------------------------------------------------
// 公共辅助
// ---------------------------------------------------------------------------
void TestEngine::ensure_scan() {
    if (m_scan.empty()) {
        std::wstring err;
        m_scan = DeviceEnumerator::scan(&err);
        if (!err.empty()) m_ev.on_log(wraii::LogLevel::Warn, L"枚举告警: " + err);
    }
}

const DeviceInfo* TestEngine::find_device(const wchar_t* cls) const {
    for (const auto& d : m_scan) {
        if (cls && d.class_name != cls) continue;
        if (m_plan.device.vid >= 0 && static_cast<long long>(d.vid) != m_plan.device.vid) continue;
        if (m_plan.device.pid >= 0 && static_cast<long long>(d.pid) != m_plan.device.pid) continue;
        return &d;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// 步骤实现（判定口径与 tools/usbtest 各 *_test.py 一致）
// ---------------------------------------------------------------------------
StepResult TestEngine::step_enumerate(const PlanStep&) {
    StepResult r;
    m_scan.clear();
    std::wstring err;
    m_scan = DeviceEnumerator::scan(&err);
    if (!err.empty()) m_ev.on_log(wraii::LogLevel::Warn, L"枚举告警: " + err);

    long long matched = 0;
    auto matches = [this](const DeviceInfo& d) {
        if (m_plan.device.vid >= 0 && static_cast<long long>(d.vid) != m_plan.device.vid)
            return false;
        if (m_plan.device.pid >= 0 && static_cast<long long>(d.pid) != m_plan.device.pid)
            return false;
        return true;
    };
    for (const auto& d : m_scan) {
        m_ev.on_log(wraii::LogLevel::Debug, L"  " + DeviceEnumerator::summary(d));
        if (matches(d)) ++matched;
    }
    r.measured.push_back({L"interfaces", mv_int(matched)});
    r.measured.push_back({L"total", mv_int(static_cast<long long>(m_scan.size()))});
    r.pass = matched > 0;
    if (!r.pass)
        r.note = L"未发现匹配 VID/PID 的设备（检查计划 device.vid/pid 或先点扫描）";
    return r;
}

StepResult TestEngine::step_descriptor_check(const PlanStep& st) {
    StepResult r;
    ensure_scan();
    const DeviceInfo* d = find_device(L"HID");
    if (!d) return fail_step(L"未发现匹配的 HID 设备");

    HidPort port;
    std::wstring err;
    if (!port.open(d->path, &err)) return fail_step(L"打开 HID 失败: " + err);
    const HidCapsInfo& c = port.caps();
    r.measured.push_back({L"usage_page", mv_int(c.usage_page)});
    r.measured.push_back({L"usage", mv_int(c.usage)});
    r.measured.push_back({L"input_len", mv_int(c.input_report_len)});
    r.measured.push_back({L"output_len", mv_int(c.output_report_len)});
    r.measured.push_back({L"vid", mv_int(c.vid)});
    r.measured.push_back({L"pid", mv_int(c.pid)});
    r.pass = true;
    if (st.expect_usage_page >= 0 && static_cast<long long>(c.usage_page) != st.expect_usage_page) {
        r.pass = false;
        r.note = wraii::fmt_v(L"UsagePage 0x%04X ≠ 白名单 0x%04llX", c.usage_page,
                              st.expect_usage_page);
    } else if (st.expect_usage >= 0 && static_cast<long long>(c.usage) != st.expect_usage) {
        r.pass = false;
        r.note = wraii::fmt_v(L"Usage 0x%04X ≠ 白名单 0x%04llX", c.usage, st.expect_usage);
    } else {
        r.note = wraii::fmt_v(L"UsagePage 0x%04X Usage 0x%04X 输入报告 %hu B", c.usage_page,
                              c.usage, c.input_report_len);
    }
    return r;
}

StepResult TestEngine::step_hid_polling_rate(const PlanStep& st, const CancelFn& cancel) {
    StepResult r;
    ensure_scan();
    const DeviceInfo* d = find_device(L"HID");
    if (!d) return fail_step(L"未发现匹配的 HID 设备");

    HidPollResult res = hid_polling_rate_measure(d->path, std::max(1, static_cast<int>(st.seconds)),
                                                 cancel);
    r.measured.push_back({L"hz", mv_dbl(std::round(res.avg_hz * 10.0) / 10.0)});
    r.measured.push_back({L"min_hz", mv_dbl(std::round(res.min_hz * 10.0) / 10.0)});
    r.measured.push_back({L"max_hz", mv_dbl(std::round(res.max_hz * 10.0) / 10.0)});
    r.measured.push_back({L"samples", mv_int(res.samples)});
    r.pass = res.ok && res.avg_hz >= static_cast<double>(st.min_hz);
    if (!res.ok)
        r.note = res.detail;
    else if (!r.pass)
        r.note = wraii::fmt_v(L"低于下限 %lld", st.min_hz);
    return r;
}

StepResult TestEngine::step_hid_output_write(const PlanStep& st) {
    StepResult r;
    ensure_scan();
    const DeviceInfo* d = find_device(L"HID");
    if (!d) return fail_step(L"未发现匹配的 HID 设备");

    HidPort port;
    std::wstring err;
    if (!port.open(d->path, &err)) return fail_step(L"打开 HID 失败: " + err);

    std::vector<uint8_t> buf = st.report.empty() ? std::vector<uint8_t>{0x00} : st.report;
    if (st.report_id != 0 && !buf.empty()) buf[0] = static_cast<uint8_t>(st.report_id);
    if (!port.set_output_report(buf.data(), buf.size(), &err)) return fail_step(err);
    r.measured.push_back({L"written", mv_int(static_cast<long long>(buf.size()))});
    r.pass = true;
    return r;
}

StepResult TestEngine::step_hid_report_loopback(const PlanStep& st) {
    StepResult r;
    ensure_scan();
    const DeviceInfo* d = find_device(L"HID");
    if (!d) return fail_step(L"未发现匹配的 HID 设备");

    HidPort port;
    std::wstring err;
    if (!port.open(d->path, &err)) return fail_step(L"打开 HID 失败: " + err);

    std::vector<uint8_t> pat = st.pattern.empty() ? std::vector<uint8_t>{0x01, 0x55} : st.pattern;
    std::vector<uint8_t> out;
    out.push_back(st.report_id != 0 ? static_cast<uint8_t>(st.report_id) : 0x01);
    out.insert(out.end(), pat.begin(), pat.end());
    if (!port.set_output_report(out.data(), out.size(), &err)) return fail_step(err);

    std::vector<uint8_t> echo;
    bool timed_out = false;
    if (!port.read_overlapped(echo, static_cast<unsigned>(st.hid_timeout_ms), &timed_out, &err)) {
        if (timed_out) return fail_step(L"等待回显超时");
        return fail_step(err);
    }
    std::wstring hex;
    wchar_t tmp[8];
    for (uint8_t b : echo) {
        ::swprintf(tmp, 8, L"%02X", b);
        hex += tmp;
    }
    r.measured.push_back({L"echo", mv_str(hex)});
    r.pass = echo.size() >= pat.size() + 1 &&
             ::memcmp(echo.data() + 1, pat.data(), pat.size()) == 0;
    if (!r.pass) r.note = L"回显与图案不一致";
    return r;
}

StepResult TestEngine::step_serial_loopback(const PlanStep& st, const CancelFn& cancel) {
    StepResult r;
    if (m_plan.device.port.empty()) return fail_step(L"计划 device.port 未指定串口");
    unsigned repeat = st.repeat > 0 ? static_cast<unsigned>(st.repeat) : 4;

    SerialPort sp;
    std::wstring err;
    if (!sp.open(m_plan.device.port, m_plan.device.baud, &err))
        return fail_step(L"打开串口失败: " + err);

    SerialLoopbackResult res = serial_loopback_test(sp, repeat, 5000, cancel);
    r.measured.push_back({L"bytes", mv_int(static_cast<long long>(res.bytes_read))});
    r.pass = res.ok;
    r.note = res.detail;
    return r;
}

StepResult TestEngine::step_msc_inquiry(const PlanStep&) {
    StepResult r;
    int drive = m_plan.device.drive >= 0 ? static_cast<int>(m_plan.device.drive)
                                         : MscScsi::auto_detect_usb_drive();
    if (drive < 0) return fail_step(L"未发现 USB 大容量存储盘（PhysicalDrive 自动探测失败）");

    MscScsi msc;
    std::wstring err;
    if (!msc.open_physical_drive(static_cast<unsigned>(drive), false, &err))
        return fail_step(err);
    std::string vendor, product, rev;
    unsigned char type = 0xFF;
    if (!msc.scsi_inquiry(&vendor, &product, &rev, &type, &err, kMscProbeTimeoutS))
        return fail_step(err);
    r.measured.push_back({L"vendor", mv_str(wraii::ascii_to_wide(vendor))});
    r.measured.push_back({L"product", mv_str(wraii::ascii_to_wide(product))});
    r.measured.push_back({L"type", mv_int(type)});
    r.pass = true;
    r.note = wraii::fmt_v(L"PhysicalDrive%d 类型 %u", drive, type);
    return r;
}

StepResult TestEngine::step_msc_capacity(const PlanStep& st) {
    StepResult r;
    int drive = m_plan.device.drive >= 0 ? static_cast<int>(m_plan.device.drive)
                                         : MscScsi::auto_detect_usb_drive();
    if (drive < 0) return fail_step(L"未发现 USB 大容量存储盘");

    MscScsi msc;
    std::wstring err;
    if (!msc.open_physical_drive(static_cast<unsigned>(drive), false, &err))
        return fail_step(err);
    unsigned long long sectors = 0;
    unsigned blk = 0;
    if (!msc.read_capacity(&sectors, &blk, &err, kMscProbeTimeoutS)) return fail_step(err);
    double gb = static_cast<double>(sectors) * static_cast<double>(blk) / 1e9;
    r.measured.push_back({L"gb", mv_dbl(std::round(gb * 100.0) / 100.0)});
    r.measured.push_back({L"block", mv_int(blk)});
    r.pass = gb >= st.min_gb;
    if (!r.pass) r.note = wraii::fmt_v(L"低于下限 %.0fGB", st.min_gb);
    return r;
}

StepResult TestEngine::step_msc_read_verify(const PlanStep& st, const CancelFn& cancel) {
    StepResult r;
    int drive = m_plan.device.drive >= 0 ? static_cast<int>(m_plan.device.drive)
                                         : MscScsi::auto_detect_usb_drive();
    if (drive < 0) return fail_step(L"未发现 USB 大容量存储盘");

    MscScsi msc;
    std::wstring err;
    if (!msc.open_physical_drive(static_cast<unsigned>(drive), false, &err))
        return fail_step(err);

    int last_pct = -1;
    auto progress = [&](int pct) {
        if (pct != last_pct) {
            last_pct = pct;
            m_ev.on_log(wraii::LogLevel::Debug, wraii::fmt_v(L"  只读校验进度 %d%%", pct));
        }
    };
    MscReadVerifyResult res =
        msc_read_verify(msc, static_cast<unsigned long long>(st.lba),
                        static_cast<unsigned>(st.blocks), static_cast<unsigned>(st.loops),
                        progress, cancel);
    r.measured.push_back({L"lba", mv_int(static_cast<long long>(res.lba_start))});
    r.measured.push_back({L"bytes",
                          mv_int(static_cast<long long>(res.total_sectors) * res.block_size)});
    r.measured.push_back({L"mbps", mv_dbl(std::round(res.mbps * 100.0) / 100.0)});
    r.pass = res.ok;
    r.note = res.detail;
    return r;
}

namespace {

double parse_v(const std::wstring& s) {
    // key=value 行的数值解析（非法/缺失按 0 处理，与 Python float(t.get(...) or 0) 口径一致）
    if (s.empty()) return 0.0;
    return ::_wtof(s.c_str());
}

} // namespace

StepResult TestEngine::step_pd_attach(const PlanStep& st, const CancelFn& cancel) {
    StepResult r;
    if (m_plan.device.telemetry_port.empty())
        return fail_step(L"计划 device.telemetry_port 未指定遥测串口");
    SerialPort sp;
    std::wstring err;
    if (!sp.open(m_plan.device.telemetry_port, m_plan.device.baud, &err))
        return fail_step(L"打开遥测串口失败: " + err);

    PdTelemetryResult res =
        pd_telemetry_collect(sp, {L"cc_state"}, L"", st.window_ms, cancel);
    std::wstring cc = res.values.count(L"cc_state") ? res.values[L"cc_state"] : L"";
    r.measured.push_back({L"cc_state", mv_str(cc)});
    r.pass = cc.find(L"Attached") != std::wstring::npos;
    if (!r.pass) r.note = L"CC 未 Attached";
    return r;
}

StepResult TestEngine::step_pd_negotiate(const PlanStep& st, const CancelFn& cancel) {
    StepResult r;
    if (m_plan.device.telemetry_port.empty())
        return fail_step(L"计划 device.telemetry_port 未指定遥测串口");
    SerialPort sp;
    std::wstring err;
    if (!sp.open(m_plan.device.telemetry_port, m_plan.device.baud, &err))
        return fail_step(L"打开遥测串口失败: " + err);

    PdTelemetryResult res =
        pd_telemetry_collect(sp, {L"contract_v", L"contract_i"}, st.command, st.window_ms, cancel);
    double v = parse_v(res.values.count(L"contract_v") ? res.values[L"contract_v"] : L"");
    double tol = st.tol_v > 0 ? st.tol_v : 0.5;
    r.measured.push_back({L"contract_v", mv_dbl(v)});
    r.pass = ::fabs(v - st.expect_v) <= tol;
    if (!r.pass) r.note = wraii::fmt_v(L"期望 %g±%gV", st.expect_v, tol);
    return r;
}

StepResult TestEngine::step_measure_voltage(const PlanStep& st, const CancelFn& cancel) {
    StepResult r;
    if (m_plan.device.telemetry_port.empty())
        return fail_step(L"计划 device.telemetry_port 未指定遥测串口");
    SerialPort sp;
    std::wstring err;
    if (!sp.open(m_plan.device.telemetry_port, m_plan.device.baud, &err))
        return fail_step(L"打开遥测串口失败: " + err);

    PdTelemetryResult res = pd_telemetry_collect(sp, {L"vbus_v"}, L"", st.window_ms, cancel);
    double v = parse_v(res.values.count(L"vbus_v") ? res.values[L"vbus_v"] : L"");
    double tol = st.tol_v > 0 ? st.tol_v : 0.25;
    r.measured.push_back({L"vbus_v", mv_dbl(v)});
    r.pass = ::fabs(v - st.expect_v) <= tol;
    if (!r.pass) r.note = wraii::fmt_v(L"期望 %g±%gV", st.expect_v, tol);
    return r;
}

// ---------------------------------------------------------------------------
// 报告（结构 = tools/usbtest core.py TestReport.save_json，可直接进 MES）
// ---------------------------------------------------------------------------
std::wstring TestEngine::build_report_json(bool aborted) const {
    TestPlan plan;
    std::vector<StepResult> results;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        plan = m_plan;
        results = m_results;
    }
    long long failed = 0;
    for (const auto& s : results)
        if (!s.pass) ++failed;
    const bool pass = !aborted && failed == 0 && !results.empty();

    wraii::json_writer w(true);
    w.begin_object();
    w.key(L"plan").string_value(plan.name);
    w.key(L"station").string_value(plan.station);
    w.key(L"dut_sn").string_value(plan.dut_sn);
    w.key(L"verdict").string_value(pass ? L"PASS" : L"FAIL");
    w.key(L"started").string_value(m_started_iso);
    w.key(L"steps").begin_array();
    for (const auto& s : results) {
        w.begin_object();
        w.key(L"name").string_value(s.name);
        w.key(L"pass").bool_value(s.pass);
        w.key(L"measured").begin_object();
        for (const auto& kv : s.measured) {
            w.key(kv.first);
            switch (kv.second.kind) {
                case MeasValue::Kind::Int: w.int_value(kv.second.i); break;
                case MeasValue::Kind::Dbl: w.double_value(kv.second.d); break;
                default:                   w.string_value(kv.second.s); break;
            }
        }
        w.end_object();
        w.key(L"note").string_value(s.note);
        w.end_object();
    }
    w.end_array();
    w.end_object();
    return w.result();
}

bool TestEngine::write_report_to(const std::wstring& path, std::wstring* err) {
    if (m_started_iso.empty()) m_started_iso = wraii::now_timestamp_iso();
    std::wstring json = build_report_json(false);
    std::string utf8 = wraii::wide_to_utf8(json);
    return wraii::write_file_bytes(path, utf8.data(), utf8.size(), err);
}

std::wstring TestEngine::suggest_report_name() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return L"report_" + wraii::sanitize_filename(m_plan.name) + L"_" +
           wraii::sanitize_filename(m_plan.dut_sn) + L"_" + wraii::now_stamp_compact() + L".json";
}

std::wstring TestEngine::default_reports_dir() {
    return wraii::exe_dir() + L"\\reports";
}
