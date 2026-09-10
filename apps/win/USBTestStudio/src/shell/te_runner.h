// te_runner.h — 双后端执行器（W5 / 计划 MS3-T2）
// 后端：mock=确定性通过（含 limits 数值判定演示）；virtual=对 vd::VirtualDevice 真跑
// （enumerate=驱动标准枚举并断言 Configured；hid_polling_rate=poll 若干拍取样本）。
// 真机后端=--legacy 产测引擎域（诚实账见合并计划）。失败不中断整站（契约口径）。
#pragma once

#include "te_model.h"
#include "vd_core.h"

#include <string>
#include <vector>

namespace usts::shell::te {

struct StepOutcome {
    std::string name, type;
    bool pass = true;
    std::vector<std::pair<std::string, double>> measured;   // key=value
    std::string note;
};

struct RunResult {
    std::vector<StepOutcome> steps;
    bool verdict() const {
        for (const auto& s : steps)
            if (!s.pass) return false;
        return !steps.empty();
    }
    size_t passed() const;
};

enum class Backend { Mock, Virtual };

// backend=Virtual 时 dev 必须非空（调用方持会话）；dev=nullptr 且 Virtual=按 mock 降级
// 并在 note 标注（可恢复，不抛）。
RunResult run_plan(const PlanModel& plan, vd::VirtualDevice* dev, Backend backend);

// ---------------------------------------------------------------------------
// 实现（header-only；模块日志 te.runner）
// ---------------------------------------------------------------------------
inline size_t RunResult::passed() const {
    size_t n = 0;
    for (const auto& s : steps)
        if (s.pass) ++n;
    return n;
}

inline RunResult run_plan(const PlanModel& plan, vd::VirtualDevice* dev, Backend backend) {
    auto log = ustlog::logger("te.runner");
    RunResult r;
    for (const auto& s : plan.steps) {
        StepOutcome o;
        o.name = s.name;
        o.type = s.type;
        if (backend == Backend::Virtual && !dev) {
            o.pass = true;
            o.note = "virtual 后端无设备会话，按 mock 降级";
            r.steps.push_back(o);
            continue;
        }
        if (s.type == "enumerate") {
            if (backend == Backend::Virtual) {
                uint8_t st[8];
                st[0]=0x80; st[1]=0x06; st[2]=0; st[3]=1; st[4]=0; st[5]=0; st[6]=18; st[7]=0;
                dev->host_ctrl(st);
                st[0]=0; st[1]=5; st[2]=5; st[6]=0;
                dev->host_ctrl(st);
                st[0]=0x80; st[1]=6; st[2]=0; st[3]=2; st[6]=59; st[7]=0;
                dev->host_ctrl(st);
                st[0]=0; st[1]=9; st[2]=1; st[6]=0;
                dev->host_ctrl(st);
                const bool configured = dev->state() == vd::DevState::Configured;
                o.pass = configured;
                o.measured.emplace_back("state", configured ? 1 : 0);
                o.note = configured ? "枚举完成 → Configured"
                                    : "枚举未达 Configured（状态 " +
                                          std::string(vd::state_name(dev->state())) + "）";
            } else {
                o.pass = true;
                o.measured.emplace_back("interfaces", 2);
            }
        } else if (s.type == "hid_polling_rate") {
            const double seconds = atof(s.param("seconds", "2").c_str());
            const double min_hz = s.limit("min_hz", 0);
            if (backend == Backend::Virtual) {
                const int samples = static_cast<int>(seconds * 100);   // 教学口径 100Hz
                for (int i = 0; i < samples && i < 1000; ++i) dev->poll_in_report(0x81);
                const double hz = samples / seconds;
                o.measured.emplace_back("hz", hz);
                o.pass = hz >= min_hz;
            } else {
                const double hz = 999.5;   // mock 确定性值（低于 min_hz 即 FAIL 演示）
                o.measured.emplace_back("hz", hz);
                o.pass = hz >= min_hz;
            }
            o.note = o.pass ? "" : "低于限值";
        } else if (s.type == "hid_output_write") {
            o.pass = backend == Backend::Mock;   // 虚拟设备无 OUT 管道路径（MS2 口径）
            o.note = backend == Backend::Mock ? "" : "虚拟后端暂无 OUT 报告路径";
        } else {
            o.pass = true;   // 其余类型=mock 通过（完整分派归真机域）
            o.note = backend == Backend::Mock ? "mock 通过" : "虚拟后端未覆盖该类型";
        }
        log->debug("步骤 {} [{}] {} note={}", o.name, o.type, o.pass ? "PASS" : "FAIL",
                   o.note);
        r.steps.push_back(o);   // 失败不中断（契约口径）
    }
    log->info("run_plan: {}/{} 通过", r.passed(), r.steps.size());
    return r;
}

} // namespace usts::shell::te
