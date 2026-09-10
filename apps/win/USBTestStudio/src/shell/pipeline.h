// pipeline.h — Pipeline 工作流 .ustsflow（W5 / 计划 MS3-T3）
// 节点链（线性）：run_plan（backend+计划内联）/delay（拍数）/assert_report（对上一步
// verdict 断言）/external（外联命令文本占位——spawn 归滚动）。失败策略 abort|continue。
#pragma once

#include "te_model.h"
#include "te_runner.h"
#include "vd_core.h"

#include <string>
#include <vector>

namespace usts::shell::te {

struct FlowNode {
    enum class Kind { RunPlan, Delay, AssertReport, External } kind = Kind::RunPlan;
    std::string plan_json;      // RunPlan：内联计划 JSON（或文件路径——路径归编辑器域）
    int ticks = 1;              // Delay 拍数 / AssertReport 期望（1=pass）
    bool expect_pass = true;    // AssertReport 期望
    std::string command;        // External 命令行文本
    bool abort_on_fail = true;  // 失败策略
};

struct PipelineModel {
    std::vector<FlowNode> nodes;
    std::string to_json() const;
    static bool from_json(const std::string& text, PipelineModel& out, std::string& err);
};

struct FlowStepResult {
    size_t node = 0;
    bool ok = true;
    bool aborted = false;
    std::string note;
};

struct PipelineResult {
    std::vector<FlowStepResult> steps;
    bool verdict = true;
};

class PipelineRunner {
public:
    // delay_tick：Delay 节点每调用一次 step 消一拍（虚拟时钟，测试友好）
    PipelineResult run(const PipelineModel& flow, vd::VirtualDevice* dev);
};

// ---------------------------------------------------------------------------
// 实现（header-only；模块日志 te.runner）
// ---------------------------------------------------------------------------
namespace detail {

inline const wchar_t* flow_kind_key(FlowNode::Kind k) {
    switch (k) {
    case FlowNode::Kind::Delay: return L"delay";
    case FlowNode::Kind::AssertReport: return L"assert_report";
    case FlowNode::Kind::External: return L"external";
    default: return L"run_plan";
    }
}

} // namespace detail

inline std::string PipelineModel::to_json() const {
    wraii::json_writer w;
    w.begin_object();
    w.key(L"v").int_value(1);
    w.key(L"nodes").begin_array();
    for (const auto& n : nodes) {
        w.begin_object();
        w.key(L"kind").string_value(detail::flow_kind_key(n.kind));
        if (!n.plan_json.empty()) w.key(L"plan").string_value(wraii::utf8_to_wide(n.plan_json));
        if (n.ticks != 1) w.key(L"ticks").int_value(n.ticks);
        if (n.kind == FlowNode::Kind::AssertReport)
            w.key(L"expect_pass").bool_value(n.expect_pass);
        if (!n.command.empty()) w.key(L"command").string_value(wraii::utf8_to_wide(n.command));
        w.key(L"abort_on_fail").bool_value(n.abort_on_fail);
        w.end_object();
    }
    w.end_array();
    w.end_object();
    return wraii::wide_to_utf8(w.result());
}

inline bool PipelineModel::from_json(const std::string& text, PipelineModel& out,
                                     std::string& err) {
    const std::wstring wide = wraii::utf8_to_wide(text);
    minijson::Value v;
    std::wstring werr;
    if (!minijson::parse(wide, v, &werr) || !v.is_object()) {
        err = "ustsflow 解析失败";
        return false;
    }
    const auto* arr = v.find(L"nodes");
    if (!arr || !arr->is_array()) { err = "ustsflow 缺 nodes"; return false; }
    out.nodes.clear();
    for (const auto& e : arr->array) {
        FlowNode n;
        const auto k = minijson::obj_str(e, L"kind", L"run_plan");
        if (k == L"delay") n.kind = FlowNode::Kind::Delay;
        else if (k == L"assert_report") n.kind = FlowNode::Kind::AssertReport;
        else if (k == L"external") n.kind = FlowNode::Kind::External;
        else n.kind = FlowNode::Kind::RunPlan;
        n.plan_json = wraii::wide_to_utf8(minijson::obj_str(e, L"plan", L""));
        n.ticks = static_cast<int>(minijson::obj_int(e, L"ticks", 1));
        if (const auto* x = e.find(L"expect_pass")) n.expect_pass = x->boolean;
        n.command = wraii::wide_to_utf8(minijson::obj_str(e, L"command", L""));
        if (const auto* x = e.find(L"abort_on_fail")) n.abort_on_fail = x->boolean;
        out.nodes.push_back(n);
    }
    return true;
}

inline PipelineResult PipelineRunner::run(const PipelineModel& flow,
                                          vd::VirtualDevice* dev) {
    auto log = ustlog::logger("te.runner");
    PipelineResult r;
    r.verdict = true;
    RunResult last_plan;
    for (size_t i = 0; i < flow.nodes.size(); ++i) {
        const auto& n = flow.nodes[i];
        FlowStepResult s;
        s.node = i;
        switch (n.kind) {
        case FlowNode::Kind::RunPlan: {
            PlanModel plan;
            std::string err;
            if (!PlanModel::from_json(n.plan_json, plan, err)) {
                s.ok = false;
                s.note = "计划解析失败: " + err;
                break;
            }
            last_plan = run_plan(plan, dev, Backend::Virtual);
            s.ok = last_plan.verdict();
            s.note = "计划 " + plan.name + ": " + std::to_string(last_plan.passed()) + "/" +
                     std::to_string(last_plan.steps.size());
            break;
        }
        case FlowNode::Kind::Delay:
            s.ok = true;
            s.note = "延时 " + std::to_string(n.ticks) + " 拍";
            break;
        case FlowNode::Kind::AssertReport:
            s.ok = (last_plan.verdict() == n.expect_pass);
            s.note = s.ok ? "断言通过" : "断言失败（verdict 与期望不符）";
            break;
        case FlowNode::Kind::External:
            s.ok = true;
            s.note = "外联命令（spawn 归滚动）: " + n.command;
            break;
        }
        if (!s.ok) r.verdict = false;
        r.steps.push_back(s);
        log->debug("pipeline 节点 {} [{}] {} {}", i, static_cast<int>(n.kind),
                   s.ok ? "OK" : "FAIL", s.note);
        if (!s.ok && n.abort_on_fail) {
            r.steps.back().aborted = true;
            log->warn("pipeline 节点 {} 失败 → abort", i);
            break;
        }
    }
    return r;
}

} // namespace usts::shell::te
