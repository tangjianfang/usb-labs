// trace_editor.h — W4 追踪台面板（计划 MS4-T9）
// 薄壳：过滤输入框+即时过滤列表+统计静态卡。捕获源=vd 事件灌入（MS4 口径）；
// USBPcap/pcapng 外联源归滚动（合并计划诚实账⑤）。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>

#include "../app/log.h"
#include "trace_dsl.h"
#include "trace_model.h"
#include "trace_stats.h"

#include <string>
#include <vector>

namespace usts::shell::trace {

class TracePanel {
public:
    static HWND__* create_w4(void* host);
    static TracePanel* instance();

    void load(const TraceModel& m);            // 灌入全量（过滤输入重算显示）
    void set_filter(const std::string& dsl);   // 即时过滤（语法错→状态行显示 err 位置）
    void refresh();

    size_t shown() const { return m_shown_rows; }
    bool filter_valid() const { return m_filter_ok; }
    HWND hwnd() const { return m_hwnd; }
    HWND list() const { return m_list; }

private:
    HWND m_hwnd = nullptr, m_edit = nullptr, m_list = nullptr, m_stats = nullptr,
         m_status = nullptr;
    TraceModel m_model;
    DslExpr m_filter;
    bool m_filter_ok = true;
    size_t m_shown_rows = 0;
};

// ---------------------------------------------------------------------------
// 实现（薄壳；模块日志 ui.editor）
// ---------------------------------------------------------------------------
inline TracePanel* TracePanel::instance() { static TracePanel p; return &p; }

inline HWND__* TracePanel::create_w4(void* host) {
    auto log = ustlog::logger("ui.editor");
    auto* self = instance();
    if (self->m_hwnd && IsWindow(self->m_hwnd)) return self->m_hwnd;
    HWND parent = static_cast<HWND>(host);
    const HINSTANCE inst =
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE));
    self->m_hwnd = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 100, 100,
                                   parent, nullptr, inst, nullptr);
    if (!self->m_hwnd) return nullptr;
    self->m_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 2, 500, 24,
                                   self->m_hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(1)), inst,
                                   nullptr);
    self->m_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                   WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL, 0, 30,
                                   700, 280, self->m_hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(2)), inst,
                                   nullptr);
    self->m_stats = CreateWindowExW(0, L"STATIC", L"统计: —", WS_CHILD | WS_VISIBLE, 704, 30,
                                    260, 200, self->m_hwnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(3)), inst,
                                    nullptr);
    self->m_status = CreateWindowExW(0, L"STATIC", L"过滤: （空=全部）", WS_CHILD | WS_VISIBLE,
                                     504, 4, 300, 20, self->m_hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(4)), inst,
                                     nullptr);
    log->info("W4 追踪台面板已创建");
    return self->m_hwnd;
}

inline void TracePanel::set_filter(const std::string& dsl) {
    auto log = ustlog::logger("trace.dsl");
    DslError err;
    if (dsl.empty()) {
        m_filter = DslExpr{};
        m_filter_ok = true;
        if (m_status) SetWindowTextW(m_status, L"过滤: （空=全部）");
    } else if (DslExpr::parse(dsl, m_filter, err)) {
        m_filter_ok = true;
        if (m_status) SetWindowTextW(m_status, L"过滤: ✓ 语法正确");
    } else {
        m_filter_ok = false;
        m_filter = DslExpr{};   // 语法错=不过滤（显示全量，状态行报错）
        if (m_status)
            SetWindowTextW(m_status, (L"过滤: ✗ 位置 " + std::to_wstring(err.pos) + L" " +
                                      wraii::utf8_to_wide(err.message))
                                         .c_str());
        log->debug("过滤语法错误: {} @{}", err.message, err.pos);
    }
    refresh();
}

inline void TracePanel::load(const TraceModel& m) {
    m_model = m;
    refresh();
}

inline void TracePanel::refresh() {
    auto log = ustlog::logger("ui.editor");
    if (!m_list) return;
    ListBox_ResetContent(m_list);
    m_shown_rows = 0;
    for (const auto& r : m_model.rows()) {
        if (!m_filter.matches(r)) continue;
        std::wstring line = std::to_wstring(r.ts_ms) + L"ms " +
                            wraii::utf8_to_wide(r.source) +
                            (r.host_to_dev ? L" OUT " : L" IN ") +
                            wraii::utf8_to_wide(r.summary);
        if (r.mark) line += L" ⟨" + std::to_wstring(r.mark) + L"⟩";
        ListBox_AddString(m_list, line.c_str());
        ++m_shown_rows;
    }
    const auto s = compute_stats(m_model);
    if (m_stats)
        SetWindowTextW(m_stats, (L"统计: " + std::to_wstring(s.total) + L" 行 / IN " +
                                 std::to_wstring(s.in_rows) + L" / OUT " +
                                 std::to_wstring(s.out_rows) + L"\nNAK " +
                                 std::to_wstring(s.nak_rows) + L"（" +
                                 std::to_wstring(int(s.nak_rate() * 100)) + L"%）/ 错误 " +
                                 std::to_wstring(s.error_rows) + L"\n数据 " +
                                 std::to_wstring(s.bytes) + L" 字节")
                                    .c_str());
    log->debug("追踪视图刷新: {}/{} 行", m_shown_rows, m_model.size());
}

} // namespace usts::shell::trace
