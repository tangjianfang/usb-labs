// test_engine.h — 测试引擎（std::jthread）：加载 JSON 计划 → 顺序执行步骤 → 判定 →
// JSON 报告写盘（与 tools/usbtest 报告同构：plan/station/dut_sn/verdict/started/steps[]）。
// 引擎 → UI 通信：PostMessage(WM_APP+1..3) + 堆载 payload（UI 侧 delete）；
// 日志经有界队列 + 丢弃计数（MessagePumpEvents），防消息洪泛。
// 未真机编译，按 MSDN 口径编写。
#pragma once

#include "framework/json_mini.h"
#include "framework/win32_rai.h"
#include "usb/device_enumerator.h"
#include "usb/hid_port.h"
#include "usb/msc_scsi.h"
#include "usb/serial_port.h"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// 引擎 → UI 消息（UI 侧在 WM_* 中以 unique_ptr 接管 LPARAM payload 并 delete）
// ---------------------------------------------------------------------------
constexpr UINT kMsgEngineLog  = WM_APP + 1;   // LPARAM: LogEvent*
constexpr UINT kMsgEngineStep = WM_APP + 2;   // LPARAM: StepEvent*
constexpr UINT kMsgEngineDone = WM_APP + 3;   // LPARAM: DoneEvent*

enum class ExitCode : int { Pass = 0, Fail = 1, Aborted = 2, PlanError = 3 };

// ---------------------------------------------------------------------------
// 步骤结果（measured 键值与 tools/usbtest 报告的 measured 字段同构）
// ---------------------------------------------------------------------------
struct MeasValue {
    enum class Kind { Str, Int, Dbl };
    Kind kind = Kind::Str;
    std::wstring s;
    long long i = 0;
    double d = 0;
};
inline MeasValue mv_str(std::wstring v) { MeasValue m; m.kind = MeasValue::Kind::Str; m.s = std::move(v); return m; }
inline MeasValue mv_int(long long v)    { MeasValue m; m.kind = MeasValue::Kind::Int; m.i = v; return m; }
inline MeasValue mv_dbl(double v)       { MeasValue m; m.kind = MeasValue::Kind::Dbl; m.d = v; return m; }

struct StepResult {
    std::wstring name;
    bool pass = false;
    std::vector<std::pair<std::wstring, MeasValue>> measured;
    std::wstring note;
};

struct LogEvent {
    wraii::LogLevel level;
    std::wstring text;
};

struct StepEvent {
    int index = 0;
    int total = 0;
    StepResult result;
};

struct DoneEvent {
    bool all_pass = false;
    int exit_code = 0;
};

// ---------------------------------------------------------------------------
// 计划结构（字段与 tools/usbtest 的 YAML 计划互通，JSON 为本上位机载体）
// ---------------------------------------------------------------------------
struct DeviceConfig {                 // 计划 "device" 节点
    long long vid = -1;               // -1 = 未指定；JSON 中为数字或 "0x1234" 字符串
    long long pid = -1;
    std::wstring port;                // CDC 串口，如 L"COM7"
    std::wstring telemetry_port;      // PD 遥测串口
    unsigned baud = 115200;
    long long drive = -1;             // PhysicalDrive 序号，-1 自动探测 USB 盘
};

struct PlanStep {
    std::wstring type;                // 与 tools/usbtest HANDLERS 同名
    std::wstring name;
    long long timeout_ms = 0;         // 0 = 类型默认

    // hid_polling_rate：seconds / limits.min_hz
    double seconds = 2.0;
    long long min_hz = 0;
    // hid_output_write / hid_report_loopback
    std::vector<uint8_t> report;      // [report_id, ...]
    unsigned report_id = 0;
    long long hid_timeout_ms = 500;
    std::vector<uint8_t> pattern;     // 环回回显图案，缺省 {0x01, 0x55}
    // serial_loopback
    long long repeat = 4;
    // msc_inquiry / msc_capacity / msc_read_verify（lba/blocks/loops；limits.min_gb）
    long long lba = 0;
    long long blocks = 8;
    long long loops = 8;
    double min_gb = 0;
    // pd_attach / pd_negotiate / measure_voltage：command/expect_v/tol_v/window_ms（或 window_s）
    std::wstring command;
    double expect_v = 5.0;
    double tol_v = 0;                 // 0 = 类型默认（negotiate 0.5 / voltage 0.25）
    int window_ms = 2000;
    // descriptor_check 白名单（可选）
    long long expect_usage_page = -1;
    long long expect_usage = -1;
};

struct TestPlan {
    std::wstring name = L"plan";
    std::wstring station = L"STN-01";
    std::wstring dut_sn = L"AUTO";
    DeviceConfig device;
    std::vector<PlanStep> steps;
};

// ---------------------------------------------------------------------------
// 事件接口（引擎线程调用；实现方负责跨线程封送）
// ---------------------------------------------------------------------------
class IEngineEvents {
public:
    virtual ~IEngineEvents() = default;
    virtual void on_log(wraii::LogLevel level, const std::wstring& text) = 0;
    virtual void on_step(int index, int total, const StepResult& result) = 0;
    virtual void on_done(bool all_pass, int exit_code) = 0;
};

// PostMessage 封送实现：日志有界队列 + 丢弃计数；步骤/完成事件堆载 payload 直投。
class MessagePumpEvents final : public IEngineEvents {
public:
    explicit MessagePumpEvents(HWND hwnd) noexcept : m_hwnd(hwnd) {}

    void on_log(wraii::LogLevel level, const std::wstring& text) override;
    void on_step(int index, int total, const StepResult& result) override;
    void on_done(bool all_pass, int exit_code) override;

    // 仅 UI 线程调用：弹出全部排队日志；返回 true 表示队列仍有剩余（UI 应再次投递 kMsgEngineLog）。
    bool drain_logs(const std::function<void(wraii::LogLevel, const std::wstring&)>& emit,
                    unsigned long long* dropped_out);

private:
    bool post(UINT msg, WPARAM wp, LPARAM lp) noexcept;

    static constexpr size_t kLogQueueCap = 512;

    HWND m_hwnd;
    std::mutex m_mtx;
    std::deque<LogEvent> m_queue;                  // 有界队列（引擎写 / UI 读，互斥保护）
    unsigned long long m_dropped = 0;              // 丢弃计数
    std::atomic<bool> m_notify_pending{false};     // 已有 kMsgEngineLog 在途
};

// ---------------------------------------------------------------------------
// TestEngine
// ---------------------------------------------------------------------------
class TestEngine {
public:
    explicit TestEngine(IEngineEvents& events) noexcept : m_ev(events) {}
    ~TestEngine();                                  // stop_and_join

    bool load_plan(const std::wstring& json_path, std::wstring* err = nullptr);
    bool has_plan() const noexcept { return m_plan_loaded; }
    const TestPlan& plan() const noexcept { return m_plan; }

    void start();                                   // 启动 jthread（已运行则忽略）
    void request_stop() noexcept { m_thread.request_stop(); }
    void stop_and_join() noexcept;
    bool running() const noexcept { return m_thread.joinable(); }

    // 报告（结构 = tools/usbtest core.py TestReport.save_json）
    bool write_report_to(const std::wstring& path, std::wstring* err);
    std::wstring suggest_report_name() const;
    static std::wstring default_reports_dir();      // exe_dir\reports

private:
    using CancelFn = std::function<bool()>;

    void run(std::stop_token st);
    StepResult fail_step(std::wstring note);
    StepResult run_step(const PlanStep& st, const CancelFn& cancel, int index, int total);

    StepResult step_enumerate(const PlanStep& st);
    StepResult step_descriptor_check(const PlanStep& st);
    StepResult step_hid_polling_rate(const PlanStep& st, const CancelFn& cancel);
    StepResult step_hid_output_write(const PlanStep& st);
    StepResult step_hid_report_loopback(const PlanStep& st);
    StepResult step_serial_loopback(const PlanStep& st, const CancelFn& cancel);
    StepResult step_msc_inquiry(const PlanStep& st);
    StepResult step_msc_capacity(const PlanStep& st);
    StepResult step_msc_read_verify(const PlanStep& st, const CancelFn& cancel);
    StepResult step_pd_attach(const PlanStep& st, const CancelFn& cancel);
    StepResult step_pd_negotiate(const PlanStep& st, const CancelFn& cancel);
    StepResult step_measure_voltage(const PlanStep& st, const CancelFn& cancel);

    void ensure_scan();
    const DeviceInfo* find_device(const wchar_t* cls) const;
    std::wstring build_report_json(bool aborted) const;
    static bool parse_plan_value(const minijson::Value& root, TestPlan& plan, std::wstring* err);

    IEngineEvents& m_ev;
    mutable std::mutex m_mtx;
    TestPlan m_plan;
    bool m_plan_loaded = false;
    std::vector<StepResult> m_results;
    std::wstring m_started_iso;                     // 报告 started 字段
    std::vector<DeviceInfo> m_scan;                 // 步骤间共享的扫描缓存（仅引擎线程访问）
    std::jthread m_thread;
};
