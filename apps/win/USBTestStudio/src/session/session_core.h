// session_core.h — EP-4 S3 会话台·会话逻辑核心（设计 §4.4 周期发送 / §4.5 发送
// 历史 / §4.6 接收区）：三个正交组件 + 会话聚合。
//   PeriodicSender 周期节拍：间隔钳制 100ms~60s（§4.4），到期判定由调用方
//     传入单调毫秒时刻，UI 定时器/产测引擎共用同一语义；
//   TxHistory 发送历史：最近 50 条、连续重复折叠、上下键游标（草稿态语义 =
//     串口助手：到顶粘住、回底交还草稿）；
//   FrameJournal 帧日志：环形保留最近 cap 帧供显示/回看，总计数不封顶；
//     清屏只清显示不清账（§4.6"暂停滚动/清屏时后台继续收"的账面支撑）。
// SessionCore 聚合并持时间基准：t0 = 会话打开 tick（相对时间戳零点），
// anchor = 打开时的"Unix 毫秒 - tick"，绝对时刻 = (t + anchor) % 一天毫秒
// （tick 溢出回绕 49.7 天 ≫ 会话生命周期，由调用方重建会话规避）。
// 纯逻辑无 Win32 依赖：IChannel 回调字节经 record_rx 入账、发送经 record_tx，
// S3 后半的 UI 会话标签页与产测引擎共用同一套账。
// 日志：session.core（info=会话时间基准/周期发送启停，debug=逐操作·历史游标·
// 环形淘汰留痕；due 节拍与逐帧渲染装配不打日志）。
#pragma once

#include "channel/channel.h"
#include "parser/parser_select.h"
#include "session/session_codec.h"
#include "app/log.h"

#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace session_core {

// —— 周期发送节拍（§4.4：间隔 100ms~60s） ——
class PeriodicSender {
public:
    static constexpr unsigned kMinMs = 100, kMaxMs = 60000;

    void set_interval(unsigned ms) noexcept {
        m_ms = clamp(ms);
        ustlog::logger("session.core")->debug("周期发送间隔设置 {}→{}ms（钳制 {}~{}）", ms, m_ms,
                                              kMinMs, kMaxMs);
    }
    unsigned interval() const noexcept { return m_ms; }
    void arm(unsigned long long now_ms) noexcept {
        m_armed = true;
        m_last = now_ms;
        ustlog::logger("session.core")->info("周期发送启动 间隔={}ms", m_ms);
    }
    void disarm() noexcept {
        m_armed = false;
        ustlog::logger("session.core")->info("周期发送停止");
    }
    bool armed() const noexcept { return m_armed; }

    // 到期判定：到期则记下本次时刻并返回 true（调用方随即发送一帧）
    bool due(unsigned long long now_ms) noexcept {
        if (!m_armed || now_ms - m_last < m_ms) return false;
        m_last = now_ms;
        return true;
    }

private:
    static unsigned clamp(unsigned ms) noexcept {
        return ms < kMinMs ? kMinMs : (ms > kMaxMs ? kMaxMs : ms);
    }
    unsigned m_ms = kMinMs;
    unsigned long long m_last = 0;
    bool m_armed = false;
};

// —— 发送历史（§4.5：最近 50 条，上下键回选） ——
class TxHistory {
public:
    static constexpr size_t kCap = 50;

    size_t size() const noexcept { return m_items.size(); }

    void push(std::wstring_view text) {
        if (text.empty()) return;
        m_cursor = kDraft;                                  // 新发送即结束游历
        if (!m_items.empty() && m_items.front() == text) {  // 连续重复折叠
            ustlog::logger("session.core")->debug("发送历史折叠连续重复（保持 {} 条）",
                                                  m_items.size());
            return;
        }
        m_items.push_front(std::wstring(text));
        if (m_items.size() > kCap) {
            m_items.pop_back();
            ustlog::logger("session.core")->debug("发送历史入账 {}/{}（环形淘汰最旧）",
                                                  m_items.size(), kCap);
        } else {
            ustlog::logger("session.core")->debug("发送历史入账 {}/{}", m_items.size(), kCap);
        }
    }

    // 上键：草稿态 → 最新；否则向更旧一档；到顶粘住（继续上键仍返回最旧条）
    std::wstring up() {
        if (m_items.empty()) return {};
        if (m_cursor == kDraft) m_cursor = 0;
        else if (m_cursor + 1 < m_items.size()) ++m_cursor;
        ustlog::logger("session.core")->debug("历史游标↑ → 序 {}（共 {} 条）", m_cursor,
                                              m_items.size());
        return m_items[m_cursor];
    }
    // 下键：向更新一档；已在最新处再下键 → 回草稿态（返回空串，UI 恢复草稿）
    std::wstring down() {
        if (m_items.empty() || m_cursor == kDraft) return {};
        if (m_cursor == 0) {
            m_cursor = kDraft;
            ustlog::logger("session.core")->debug("历史游标↓ 回草稿态（交还草稿）");
            return {};
        }
        --m_cursor;
        ustlog::logger("session.core")->debug("历史游标↓ → 序 {}（共 {} 条）", m_cursor,
                                              m_items.size());
        return m_items[m_cursor];
    }

    void clear() noexcept {
        ustlog::logger("session.core")->debug("发送历史清空（原 {} 条）", m_items.size());
        m_items.clear();
        m_cursor = kDraft;
    }

private:
    static constexpr size_t kDraft = static_cast<size_t>(-1);
    std::deque<std::wstring> m_items;   // front = 最新
    size_t m_cursor = kDraft;
};

// —— 帧日志（§4.6：显示环形保留，计数不封顶；清屏不清账） ——
class FrameJournal {
public:
    explicit FrameJournal(size_t cap = 2000) noexcept : m_cap(cap ? cap : 1) {}

    void record(bool out, const uint8_t* data, size_t len, unsigned long long t_ms) {
        ChannelFrame f;
        f.out = out;
        f.t_ms = t_ms;
        f.bytes.assign(data, data + len);
        if (out) ++m_tx; else ++m_rx;
        m_frames.push_back(std::move(f));
        if (m_frames.size() > m_cap) {
            m_frames.pop_front();
            ustlog::logger("session.core")->debug("帧入账 {} {}B（存活 {}/{}，环形淘汰最旧）",
                                                  out ? "OUT" : "IN", len, m_frames.size(), m_cap);
        } else {
            ustlog::logger("session.core")->debug("帧入账 {} {}B（存活 {}/{}）", out ? "OUT" : "IN",
                                                  len, m_frames.size(), m_cap);
        }
    }

    const std::deque<ChannelFrame>& frames() const noexcept { return m_frames; }
    unsigned long long tx_frames() const noexcept { return m_tx; }
    unsigned long long rx_frames() const noexcept { return m_rx; }

    void clear() noexcept {   // 清屏：只清显示，计数保留
        ustlog::logger("session.core")->debug("帧日志清屏（存活 {}→0，计数保留 tx={} rx={}）",
                                              m_frames.size(), m_tx, m_rx);
        m_frames.clear();
    }

private:
    std::deque<ChannelFrame> m_frames;   // back = 最新
    size_t m_cap;
    unsigned long long m_tx = 0, m_rx = 0;
};

// —— 会话聚合（S3 后半每个会话标签页一枚；产测引擎亦可复用账面） ——
class SessionCore {
public:
    FrameJournal journal;
    TxHistory history;
    PeriodicSender periodic;

    // 打开会话时由 UI 传入：t0 = 当前 tick；anchor = 当前 Unix 毫秒 - 当前 tick
    void set_time_base(unsigned long long t0, unsigned long long anchor_epoch_ms) noexcept {
        ustlog::logger("session.core")->info("会话时间基准设定 t0={} anchor={}ms", t0,
                                             anchor_epoch_ms);
        m_t0 = t0;
        m_anchor = anchor_epoch_ms;
    }

    void record_tx(const uint8_t* d, size_t n, unsigned long long t) { journal.record(true, d, n, t); }
    void record_rx(const uint8_t* d, size_t n, unsigned long long t) { journal.record(false, d, n, t); }

    // 帧时间戳标签（t 与 t0 同一 tick 源，约定 t ≥ t0）
    std::wstring ts_absolute(unsigned long long t) const {
        return session_codec::format_time_of_day((t + m_anchor) % 86400000ULL);
    }
    std::wstring ts_relative(unsigned long long t) const {
        return t >= m_t0 ? session_codec::format_relative(t - m_t0)
                         : session_codec::format_relative(0);
    }

    // 接收区一行（视图/时间戳口径在此收口，UI 只拼接）。parser 非空且选型
    // 非 none 时正文走解析文本（S4 解析视图：设计 §2 "原始|解析"切换的解析侧），
    // 否则按 hex/ascii 原始视图——同一行装配，两视图只差正文。
    std::wstring frame_line(const ChannelFrame& f, bool hex_view, bool absolute_ts,
                            const parser_select::PaneParser* parser = nullptr) const {
        const std::wstring ts = absolute_ts ? ts_absolute(f.t_ms) : ts_relative(f.t_ms);
        if (parser != nullptr && parser->kind != parser_select::PaneParser::Kind::none)
            return session_codec::format_frame_body_line(ts, f.out,
                                                         parser_select::parse_frame(f, *parser));
        return session_codec::format_frame_line(f, hex_view, ts);
    }

private:
    unsigned long long m_t0 = 0;
    unsigned long long m_anchor = 0;
};

}  // namespace session_core
