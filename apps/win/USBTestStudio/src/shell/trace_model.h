// trace_model.h — 协议追踪时间线模型（W4 / 计划 MS4-T6）
#pragma once

#include "../app/log.h"
#include "vd_core.h"

#include <string>
#include <vector>

namespace usts::shell::trace {

struct TraceRow {
    uint32_t ts_ms = 0;
    std::string source;     // "会话" / "vd" / "USBPcap"（外联源归滚动）
    bool host_to_dev = true;
    std::string summary;
    std::string hex;        // 数据（十六进制，可空）
    int mark = 0;           // 0 普通 1 断点 2 注入 3 错误
};

class TraceModel {
public:
    void append(TraceRow r) { m_rows.push_back(std::move(r)); }
    size_t size() const { return m_rows.size(); }
    const std::vector<TraceRow>& rows() const { return m_rows; }
    void clear() { m_rows.clear(); }
    // 视口：offset 从最新往前数，最多 count 行正序返回
    std::vector<const TraceRow*> viewport(size_t offset_from_newest, size_t count) const;
    // 从虚拟设备事件流灌入（字段级转换：seq→ts 逐行、mark 直传）
    static TraceModel from_vd_events(const vd::VirtualDevice& dev,
                                     const std::string& source = "vd");
private:
    std::vector<TraceRow> m_rows;
};

inline std::vector<const TraceRow*> TraceModel::viewport(size_t off, size_t count) const {
    std::vector<const TraceRow*> out;
    if (m_rows.empty() || count == 0 || off >= m_rows.size()) return out;
    const size_t newest = m_rows.size() - 1 - off;
    const size_t begin = (newest + 1 > count) ? (newest + 1 - count) : 0;
    for (size_t i = begin; i <= newest; ++i) out.push_back(&m_rows[i]);
    return out;
}

inline TraceModel TraceModel::from_vd_events(const vd::VirtualDevice& dev,
                                             const std::string& source) {
    auto log = ustlog::logger("trace.model");
    TraceModel m;
    for (const auto& e : dev.events()) {
        TraceRow r;
        r.ts_ms = e.ts_ms;
        r.source = source;
        r.host_to_dev = e.host_to_dev;
        r.summary = e.summary;
        static const char* kHex = "0123456789ABCDEF";
        for (uint8_t b : e.data) {
            r.hex += kHex[b >> 4];
            r.hex += kHex[b & 0xF];
        }
        r.mark = e.mark;
        m.m_rows.push_back(std::move(r));
    }
    log->debug("from_vd_events: {} 行", m.size());
    return m;
}

} // namespace usts::shell::trace
