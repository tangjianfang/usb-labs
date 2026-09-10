// trace_stats.h — 统计与 diff（W4 / 计划 MS4-T8）
#pragma once

#include "trace_model.h"

#include <string>
#include <vector>

namespace usts::shell::trace {

struct TraceStats {
    size_t total = 0;
    size_t in_rows = 0, out_rows = 0;
    size_t nak_rows = 0;
    size_t error_rows = 0;      // mark==3
    size_t bytes = 0;           // 数据字节合计（hex/2）
    double nak_rate() const { return total ? double(nak_rows) / total : 0; }
};

inline TraceStats compute_stats(const TraceModel& m) {
    auto log = ustlog::logger("trace.stats");
    TraceStats s;
    for (const auto& r : m.rows()) {
        ++s.total;
        if (r.host_to_dev) ++s.out_rows; else ++s.in_rows;
        if (r.summary.find("NAK") != std::string::npos) ++s.nak_rows;
        if (r.mark == 3) ++s.error_rows;
        s.bytes += r.hex.size() / 2;
    }
    log->debug("stats: {} 行 NAK 率 {:.1}%", s.total, s.nak_rate() * 100);
    return s;
}

struct TraceDiffRow {
    size_t index = 0;           // 对齐序
    std::string kind;           // "same" "modified" "added" "removed"
    std::string a_summary, b_summary;
};

// 按序对齐（教学口径：逐行对位；长度差尾部记 added/removed）
inline std::vector<TraceDiffRow> diff_traces(const TraceModel& a, const TraceModel& b) {
    auto log = ustlog::logger("trace.stats");
    std::vector<TraceDiffRow> out;
    const size_t n = (std::max)(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        TraceDiffRow d;
        d.index = i;
        const bool has_a = i < a.size(), has_b = i < b.size();
        if (has_a && has_b) {
            const auto& ra = a.rows()[i];
            const auto& rb = b.rows()[i];
            d.kind = (ra.host_to_dev == rb.host_to_dev && ra.summary == rb.summary &&
                      ra.hex == rb.hex)
                         ? "same"
                         : "modified";
            d.a_summary = ra.summary;
            d.b_summary = rb.summary;
        } else if (has_a) {
            d.kind = "removed";
            d.a_summary = a.rows()[i].summary;
        } else {
            d.kind = "added";
            d.b_summary = b.rows()[i].summary;
        }
        out.push_back(std::move(d));
    }
    log->debug("diff: {} 对齐行", out.size());
    return out;
}

} // namespace usts::shell::trace
