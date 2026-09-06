// session_view.h — EP-4 S3 会话台后半·显示视图模型（设计 §4.6 暂停滚动 /
// 双视图切换、§4.7 原始|解析切换的账面-显示解耦）：RenderCursor 以"绝对帧序号"记账——journal 的
// tx+rx 计数是只增总账，deque 存活帧首元素绝对序 = 总账 - 存活数。
//   poll()    未暂停时返回自游标以来仍在账内的帧的显示行并推进游标；暂停时
//             游标停走（返回空），恢复后一次 poll 补齐——环形淘汰导致暂停
//             期间最老帧滑出账面的不再可见，计数仍在（"不丢原始归档"的显示
//             侧口径；归档落盘挂后续切片）；
//   rebuild() 视图口径切换（Hex↔ASCII / 相对↔绝对时间戳）后全量重渲染账内
//             存活帧并把游标推到最新——切换即"按新口径看当前账面"。
// 显示行由 SessionCore::frame_line 统一生成（视图旗标持于此处）。清屏是纯
// UI 动作（清空编辑框、游标不动），后续 poll 只出新帧——不在此建模。
// 纯逻辑无 Win32 依赖：S3 接收区与产测报告共用同一视图口径。
#pragma once

#include "session/session_core.h"

#include <string>
#include <vector>

namespace session_view {

class RenderCursor {
public:
    bool hex_view = true;        // §4.6 Hex↔ASCII 双视图（默认 Hex，串口助手惯例）
    bool absolute_ts = false;    // 相对（默认，自会话基准）↔ 绝对时刻
    bool parsed_view = false;    // §4.7 原始|解析切换（false=原始；解析正文走 parser）
    parser_select::PaneParser parser;   // 解析器选型（面板按通道描述填，选型跟随协议）

    void set_paused(bool paused) noexcept { m_paused = paused; }
    bool paused() const noexcept { return m_paused; }
    unsigned long long rendered() const noexcept { return m_cursor; }   // 已渲染帧数

    // 渲染增量：未暂停时返回 [游标, 总账) ∩ 存活帧 的显示行并推进游标。
    std::vector<std::wstring> poll(const session_core::SessionCore& s) {
        if (m_paused) return {};
        const auto& j = s.journal;
        const unsigned long long total = j.tx_frames() + j.rx_frames();
        const auto& alive = j.frames();
        const unsigned long long first = total - alive.size();   // 存活首帧绝对序
        std::vector<std::wstring> out;
        for (unsigned long long a = (m_cursor > first ? m_cursor : first); a < total; ++a)
            out.push_back(s.frame_line(alive[static_cast<size_t>(a - first)], hex_view,
                                       absolute_ts, active_parser()));
        m_cursor = total;
        return out;
    }

    // 全量重渲染（口径切换后）：返回账内存活帧全部显示行，游标推到最新；
    // 暂停中亦然——重渲染本身就是"把当前账面按新口径看全"。
    std::vector<std::wstring> rebuild(const session_core::SessionCore& s) {
        const auto& j = s.journal;
        std::vector<std::wstring> out;
        out.reserve(j.frames().size());
        for (const auto& f : j.frames())
            out.push_back(s.frame_line(f, hex_view, absolute_ts, active_parser()));
        m_cursor = j.tx_frames() + j.rx_frames();
        return out;
    }

private:
    // 解析视图生效且选型可用时才把 parser 交给行装配（否则按原始视图）
    const parser_select::PaneParser* active_parser() const noexcept {
        return parsed_view && parser.kind != parser_select::PaneParser::Kind::none ? &parser
                                                                                   : nullptr;
    }

    unsigned long long m_cursor = 0;   // 显示游标：≤此绝对序的帧不再出增量（已渲染或已被环形淘汰跳过）
    bool m_paused = false;
};

}  // namespace session_view
