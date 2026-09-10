// logview_model.h — 日志视图模型（设计 §0.5 性能红线 / 计划 MS0-T8）
//
// 契约：
//   1. 有界环形缓冲（默认 cap=100000 行，性能红线：10 万行 append < 2s）；
//   2. 视口语义：offset 从最新往前数（0=最新一行），返回按时间正序，越界自动裁剪；
//   3. clear() 保留 dropped 计数（"已丢弃 N 行"提示用）；
//   4. UI 只经 viewport() 取指针视图——虚拟化列表的数据源，禁止整表拷贝。
#pragma once

#include "../app/log.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace usts::shell {

struct LogRow {
    uint64_t seq;       // 全局单调序（append 返回值）
    int64_t ts_ms;      // 时间戳（相对/绝对由填充方决定）
    char level;         // 'T'/'D'/'I'/'W'/'E'
    uint32_t tid;       // 线程号
    std::string msg;    // 已格式化消息（UTF-8）
};

class LogViewModel {
public:
    explicit LogViewModel(size_t cap = 100000) : m_cap(cap) {}

    uint64_t append(LogRow r);
    size_t size() const { return m_rows.size(); }
    size_t cap() const { return m_cap; }
    uint64_t dropped() const { return m_dropped; }

    // 视口：从最新第 offset 行起，最多 count 行，按时间正序返回（指针指向内部，勿跨 append 持有）
    std::vector<const LogRow*> viewport(size_t offset_from_newest, size_t count) const;

    void clear();   // 清行不清 dropped

private:
    size_t m_cap;
    uint64_t m_next_seq = 0;   // 每实例独立单调序（测试隔离）
    uint64_t m_dropped = 0;
    std::deque<LogRow> m_rows;
};

// ---------------------------------------------------------------------------
// 实现（header-only；模块日志 shell.logview——注意：本模型自身不打逐行日志，
// 只在 cap 调整等低频操作打 debug，避免日志洪水自反馈）
// ---------------------------------------------------------------------------
inline uint64_t LogViewModel::append(LogRow r) {
    r.seq = m_next_seq++;
    if (m_rows.size() >= m_cap) {
        m_rows.pop_front();
        ++m_dropped;
    }
    m_rows.push_back(std::move(r));
    return m_rows.back().seq;
}

inline std::vector<const LogRow*> LogViewModel::viewport(size_t offset_from_newest,
                                                         size_t count) const {
    std::vector<const LogRow*> out;
    if (m_rows.empty() || count == 0) return out;
    // offset 从最新往前数；clamp 到 [0, size-1]
    if (offset_from_newest >= m_rows.size()) return out;
    const size_t newest_idx = m_rows.size() - 1 - offset_from_newest;   // 最新的那一行下标
    // 取 [newest_idx - count + 1, newest_idx] 区间（往前裁剪），再正序输出
    const size_t begin = (newest_idx + 1 > count) ? (newest_idx + 1 - count) : 0;
    for (size_t i = begin; i <= newest_idx; ++i) out.push_back(&m_rows[i]);
    return out;
}

inline void LogViewModel::clear() {
    auto log = ustlog::logger("shell.logview");
    log->debug("clear: {} 行（dropped={} 保留）", m_rows.size(), m_dropped);
    m_rows.clear();
}

} // namespace usts::shell
