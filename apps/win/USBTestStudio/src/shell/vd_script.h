// vd_script.h — 场景脚本 .ustssim（设计 §3.2 场景脚本 / 计划 MS2-T3）
// 事件表（触发/动作/断言/延时/循环）+ 执行器：step(dev) 推进一步。
#pragma once

#include "vd_core.h"

#include <string>
#include <vector>

namespace usts::shell::vd {

struct ScriptEvent {
    enum class Kind { Wait, Inject, Assert, Delay, Loop } kind = Kind::Wait;
    // Wait: cond ∈ {"configured", "addressed"}；Assert: expr ∈ {"report", "state:Configured"}
    std::string cond;
    Inject inject = Inject::None;
    int count = 1;          // Inject 次数 / Delay 毫秒 / Loop 剩余轮数
};

struct VdScript {
    std::vector<ScriptEvent> events;
    std::string to_json() const;
    static bool from_json(const std::string& text, VdScript& out, std::string& err);
};

struct ScriptResult {
    bool done = false;          // 全部事件执行完
    bool failed = false;        // 断言失败
    std::string note;
};

class ScriptRunner {
public:
    explicit ScriptRunner(VdScript s) : m_script(std::move(s)) {}
    // 单步：推进当前事件；Wait=轮询设备状态（满足即过）；Loop=跳回标记
    ScriptResult step(VirtualDevice& dev);
    size_t pc() const { return m_pc; }
private:
    VdScript m_script;
    size_t m_pc = 0;
    size_t m_loop_back = static_cast<size_t>(-1);
    int m_delay_left = 0;
};

// ---------------------------------------------------------------------------
// 实现（header-only；模块日志 vd.script）
// ---------------------------------------------------------------------------
namespace detail {

inline const wchar_t* event_kind_key(ScriptEvent::Kind k) {
    switch (k) {
    case ScriptEvent::Kind::Inject: return L"inject";
    case ScriptEvent::Kind::Assert: return L"assert";
    case ScriptEvent::Kind::Delay: return L"delay";
    case ScriptEvent::Kind::Loop: return L"loop";
    default: return L"wait";
    }
}

} // namespace detail

inline std::string VdScript::to_json() const {
    wraii::json_writer w;
    w.begin_object();
    w.key(L"v").int_value(1);
    w.key(L"events").begin_array();
    for (const auto& e : events) {
        w.begin_object();
        w.key(L"kind").string_value(detail::event_kind_key(e.kind));
        if (!e.cond.empty()) w.key(L"cond").string_value(wraii::utf8_to_wide(e.cond));
        if (e.kind == ScriptEvent::Kind::Inject)
            w.key(L"inject").int_value(static_cast<int>(e.inject));
        if (e.count != 1) w.key(L"count").int_value(e.count);
        w.end_object();
    }
    w.end_array();
    w.end_object();
    return wraii::wide_to_utf8(w.result());
}

inline bool VdScript::from_json(const std::string& text, VdScript& out,
                                std::string& err) {
    const std::wstring wide = wraii::utf8_to_wide(text);
    minijson::Value v;
    std::wstring werr;
    if (!minijson::parse(wide, v, &werr) || !v.is_object()) {
        err = "ustssim 解析失败";
        return false;
    }
    out.events.clear();
    const auto* arr = v.find(L"events");
    if (!arr || !arr->is_array()) { err = "ustssim 缺 events"; return false; }
    for (const auto& e : arr->array) {
        ScriptEvent ev;
        const auto k = minijson::obj_str(e, L"kind", L"wait");
        if (k == L"inject") ev.kind = ScriptEvent::Kind::Inject;
        else if (k == L"assert") ev.kind = ScriptEvent::Kind::Assert;
        else if (k == L"delay") ev.kind = ScriptEvent::Kind::Delay;
        else if (k == L"loop") ev.kind = ScriptEvent::Kind::Loop;
        else ev.kind = ScriptEvent::Kind::Wait;
        ev.cond = wraii::wide_to_utf8(minijson::obj_str(e, L"cond", L""));
        ev.inject = static_cast<Inject>(minijson::obj_int(e, L"inject", 0));
        ev.count = static_cast<int>(minijson::obj_int(e, L"count", 1));
        out.events.push_back(ev);
    }
    return true;
}

inline ScriptResult ScriptRunner::step(VirtualDevice& dev) {
    auto log = ustlog::logger("vd.script");
    ScriptResult r;
    if (m_pc >= m_script.events.size()) { r.done = true; return r; }
    const auto& e = m_script.events[m_pc];
    auto advance = [&] { ++m_pc; };
    switch (e.kind) {
    case ScriptEvent::Kind::Wait:
        if (e.cond == "configured") {
            if (dev.state() == DevState::Configured) advance();
            else r.note = "等待 Configured";
        } else if (e.cond == "addressed") {
            if (dev.state() == DevState::Addressed || dev.state() == DevState::Configured)
                advance();
            else r.note = "等待 Addressed";
        } else {
            advance();   // 未知条件=直过（宽松）
        }
        break;
    case ScriptEvent::Kind::Inject:
        dev.inject(e.inject, e.count);
        advance();
        break;
    case ScriptEvent::Kind::Assert:
        if (e.cond == "state:Configured") {
            if (dev.state() == DevState::Configured) advance();
            else { r.failed = true; r.note = "断言失败: state != Configured"; }
        } else {
            advance();
        }
        break;
    case ScriptEvent::Kind::Delay:
        if (m_delay_left == 0) m_delay_left = e.count;
        --m_delay_left;
        r.note = "延时剩余 " + std::to_string(m_delay_left);
        if (m_delay_left <= 0) { m_delay_left = 0; advance(); }
        break;
    case ScriptEvent::Kind::Loop:
        if (e.count > 0) {
            const_cast<ScriptEvent&>(e).count -= 1;   // 轮数内减（数据语义=剩余轮）
            m_loop_back = m_pc;
            advance();
        } else {
            advance();
        }
        break;
    }
    log->debug("script step: pc={} note={}", m_pc, r.note);
    if (m_pc >= m_script.events.size()) r.done = true;
    return r;
}

} // namespace usts::shell::vd
