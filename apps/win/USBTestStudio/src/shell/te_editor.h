// te_editor.h — W5 执行视图 + W6 报告中心面板（计划 MS3-T4）
// 薄壳：w5.runner=步骤表(ListBox)+verdict 横幅(Static)；w6.report_list=reports\*.json
// 扫描列表+详情文本。VM 逻辑=执行结果渲染/报告行格式。
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
#include "pipeline.h"
#include "te_model.h"
#include "te_runner.h"

#include <memory>
#include <string>
#include <vector>

namespace usts::shell::te {

class RunnerPanel {
public:
    static HWND__* create_w5(void* host);
    static RunnerPanel* instance();
    void run_mock(const PlanModel& plan);          // ▶(mock)：跑计划渲染步骤表
    void show_result(const RunResult& r);
    HWND list() const { return m_list; }
    HWND verdict() const { return m_verdict; }
    HWND hwnd() const { return m_hwnd; }
private:
    HWND m_hwnd = nullptr, m_list = nullptr, m_verdict = nullptr;
};

class ReportPanel {
public:
    static HWND__* create_w6(void* host);
    static ReportPanel* instance();
    void scan(const std::wstring& dir);            // reports 目录（json 含 verdict 视为报告）
    size_t count() const { return m_rows; }
    HWND list() const { return m_list; }
    HWND hwnd() const { return m_hwnd; }
private:
    HWND m_hwnd = nullptr, m_list = nullptr;
    size_t m_rows = 0;
};

// ---------------------------------------------------------------------------
// 实现（薄壳；模块日志 ui.editor）
// ---------------------------------------------------------------------------
inline RunnerPanel* RunnerPanel::instance() { static RunnerPanel p; return &p; }

inline HWND__* RunnerPanel::create_w5(void* host) {
    auto log = ustlog::logger("ui.editor");
    auto* self = instance();
    if (self->m_hwnd && IsWindow(self->m_hwnd)) return self->m_hwnd;
    HWND parent = static_cast<HWND>(host);
    const HINSTANCE inst =
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE));
    self->m_hwnd = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 100, 100,
                                   parent, nullptr, inst, nullptr);
    if (!self->m_hwnd) return nullptr;
    self->m_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                   WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL, 0, 32,
                                   700, 300, self->m_hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(1)), inst,
                                   nullptr);
    self->m_verdict = CreateWindowExW(0, L"STATIC", L"verdict: —", WS_CHILD | WS_VISIBLE,
                                      0, 0, 700, 28, self->m_hwnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(2)), inst,
                                      nullptr);
    log->info("W5 执行视图面板已创建");
    return self->m_hwnd;
}

inline void RunnerPanel::show_result(const RunResult& r) {
    auto log = ustlog::logger("ui.editor");
    ListBox_ResetContent(m_list);
    for (const auto& s : r.steps) {
        std::string line = (s.pass ? "PASS | " : "FAIL | ") + s.name + " [" + s.type + "]";
        if (!s.note.empty()) line += " — " + s.note;
        ListBox_AddString(m_list, wraii::utf8_to_wide(line).c_str());
    }
    const std::wstring v = r.verdict()
                               ? L"verdict: PASS（" + std::to_wstring(r.passed()) + L"/" +
                                     std::to_wstring(r.steps.size()) + L"）"
                               : L"verdict: FAIL（" + std::to_wstring(r.passed()) + L"/" +
                                     std::to_wstring(r.steps.size()) + L"）";
    SetWindowTextW(m_verdict, v.c_str());
    log->debug("执行视图渲染: {} 行 verdict={}", r.steps.size(), r.verdict());
}

inline void RunnerPanel::run_mock(const PlanModel& plan) {
    show_result(run_plan(plan, nullptr, Backend::Mock));
}

inline ReportPanel* ReportPanel::instance() { static ReportPanel p; return &p; }

inline HWND__* ReportPanel::create_w6(void* host) {
    auto log = ustlog::logger("ui.editor");
    auto* self = instance();
    if (self->m_hwnd && IsWindow(self->m_hwnd)) return self->m_hwnd;
    HWND parent = static_cast<HWND>(host);
    const HINSTANCE inst =
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE));
    self->m_hwnd = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 100, 100,
                                   parent, nullptr, inst, nullptr);
    if (!self->m_hwnd) return nullptr;
    self->m_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                   WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL, 0, 32,
                                   700, 300, self->m_hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(1)), inst,
                                   nullptr);
    log->info("W6 报告中心面板已创建");
    return self->m_hwnd;
}

inline void ReportPanel::scan(const std::wstring& dir) {
    auto log = ustlog::logger("ui.editor");
    m_rows = 0;
    ListBox_ResetContent(m_list);
    WIN32_FIND_DATAW fd;
    const HANDLE h = ::FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring text, err;
        if (!wraii::read_text_file_utf8(dir + L"\\" + fd.cFileName, text, &err)) continue;
        const std::wstring verdict = text.find(L"\"PASS\"") != std::wstring::npos
                                         ? L"PASS"
                                         : L"FAIL";
        ListBox_AddString(m_list, (std::wstring(fd.cFileName) + L"  " + verdict).c_str());
        ++m_rows;
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    log->debug("报告扫描: {} 份（{}）", m_rows, wraii::wide_to_utf8(dir));
}

} // namespace usts::shell::te
