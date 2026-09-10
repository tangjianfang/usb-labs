// vd_host.h — ustsvd 协议层（设计 §3.4 / 附录 D / 计划 MS2-T4）
// MS2 形态：进程内直连（GUI↔vd 同进程），消息=附录 D 信封 JSON-lines（编码/解码
// 齐备，命名管道传输层在 DLL 化时接同一编解码——接口不变，注记）。
#pragma once

#include "vd_core.h"

#include <string>
#include <vector>

namespace usts::shell::vd {

// 附录 D 消息（t 字段）
struct VdMsg {
    std::string t;                 // vd_start/vd_stop/vd_event/vd_break/vd_resume/vd_step/vd_inject/vd_client
    uint64_t seq = 0;
    uint32_t ts_ms = 0;
    std::string summary;           // vd_event
    std::vector<uint8_t> data;     // vd_event
    std::string state;             // vd_start/vd_client
    std::string kind_detail;       // vd_break/vd_inject 附带
    int mark = 0;
    std::string to_json_line() const;
    static bool from_json_line(const std::string& line, VdMsg& out, std::string& err);
};

// GUI 侧会话：持虚拟设备，把驱动动作翻译为 vd_* 消息（回调消费）
class VdSession {
public:
    using Sink = std::function<void(const VdMsg&)>;
    VdSession(desc::DescModel model, std::string tpl_id, Sink sink);
    void start();
    void stop();
    // 主机动作（消息进出对称：动作 → 设备 → 事件消息经 sink 回流）
    std::vector<uint8_t> ctrl(const uint8_t setup[8]);
    std::vector<uint8_t> poll(uint8_t ep);
    void inject(Inject kind, int count);
    void reset();
    VirtualDevice& dev() { return m_dev; }
    bool started() const { return m_started; }
private:
    void flush_events(int mark_override = 0);
    VirtualDevice m_dev;
    std::string m_tpl_id;
    Sink m_sink;
    bool m_started = false;
    uint64_t m_seen_events = 0;
};

// ---------------------------------------------------------------------------
// 实现（header-only；模块日志 vd.host）
// ---------------------------------------------------------------------------
inline std::string VdMsg::to_json_line() const {
    wraii::json_writer w(false);
    w.begin_object();
    w.key(L"v").int_value(1);
    w.key(L"t").string_value(wraii::utf8_to_wide(t));
    if (seq) w.key(L"seq").int_value(static_cast<long long>(seq));
    if (ts_ms) w.key(L"ts").int_value(ts_ms);
    if (!summary.empty()) w.key(L"summary").string_value(wraii::utf8_to_wide(summary));
    if (!state.empty()) w.key(L"state").string_value(wraii::utf8_to_wide(state));
    if (!kind_detail.empty())
        w.key(L"detail").string_value(wraii::utf8_to_wide(kind_detail));
    if (mark) w.key(L"mark").int_value(mark);
    w.end_object();
    return wraii::wide_to_utf8(w.result());
}

inline bool VdMsg::from_json_line(const std::string& line, VdMsg& out, std::string& err) {
    const std::wstring wide = wraii::utf8_to_wide(line);
    minijson::Value v;
    std::wstring werr;
    if (!minijson::parse(wide, v, &werr) || !v.is_object()) {
        err = "vd 消息解析失败";
        return false;
    }
    out.t = wraii::wide_to_utf8(minijson::obj_str(v, L"t", L""));
    if (out.t.empty()) { err = "vd 消息缺 t"; return false; }
    out.seq = static_cast<uint64_t>(minijson::obj_int(v, L"seq", 0));
    out.ts_ms = static_cast<uint32_t>(minijson::obj_int(v, L"ts", 0));
    out.summary = wraii::wide_to_utf8(minijson::obj_str(v, L"summary", L""));
    out.state = wraii::wide_to_utf8(minijson::obj_str(v, L"state", L""));
    out.kind_detail = wraii::wide_to_utf8(minijson::obj_str(v, L"detail", L""));
    out.mark = static_cast<int>(minijson::obj_int(v, L"mark", 0));
    return true;
}

inline VdSession::VdSession(desc::DescModel model, std::string tpl_id, Sink sink)
    : m_dev(std::move(model)), m_tpl_id(std::move(tpl_id)), m_sink(std::move(sink)) {}

inline void VdSession::start() {
    auto log = ustlog::logger("vd.host");
    m_started = true;
    m_seen_events = 0;
    VdMsg m;
    m.t = "vd_start";
    m.state = m_tpl_id;
    if (m_sink) m_sink(m);
    log->info("会话启动: 模板={}", m_tpl_id);
}

inline void VdSession::stop() {
    auto log = ustlog::logger("vd.host");
    VdMsg m;
    m.t = "vd_stop";
    if (m_sink) m_sink(m);
    m_started = false;
    log->info("会话停止");
}

inline void VdSession::flush_events(int mark_override) {
    const auto& evs = m_dev.events();
    for (; m_seen_events < evs.size(); ++m_seen_events) {
        VdMsg m;
        m.t = "vd_event";
        m.seq = evs[m_seen_events].seq;
        m.ts_ms = evs[m_seen_events].ts_ms;
        m.summary = evs[m_seen_events].summary;
        m.data = evs[m_seen_events].data;
        m.mark = mark_override ? mark_override : evs[m_seen_events].mark;
        if (m_sink) m_sink(m);
    }
}

inline std::vector<uint8_t> VdSession::ctrl(const uint8_t setup[8]) {
    const auto r = m_dev.host_ctrl(setup);
    flush_events();
    if (r.stall) {
        VdMsg m;
        m.t = "vd_break";   // STALL 视作软断点信号（附录 D mark 语义）
        m.kind_detail = "stall";
        m.summary = "控制传输 STALL";
        if (m_sink) m_sink(m);
    }
    return r.data;
}

inline std::vector<uint8_t> VdSession::poll(uint8_t ep) {
    auto r = m_dev.poll_in_report(ep);
    flush_events();
    return r;
}

inline void VdSession::inject(Inject kind, int count) {
    VdMsg m;
    m.t = "vd_inject";
    m.kind_detail = std::to_string(static_cast<int>(kind));
    m.seq = static_cast<uint64_t>(count);
    if (m_sink) m_sink(m);
    m_dev.inject(kind, count);
    flush_events(2);
}

inline void VdSession::reset() {
    m_dev.reset();
    flush_events();
}

} // namespace usts::shell::vd
