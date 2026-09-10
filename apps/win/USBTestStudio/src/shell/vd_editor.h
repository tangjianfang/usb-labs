// vd_editor.h — W3 虚拟调试器面板（设计 §3.1 布局 / 计划 MS2-T5）
// 薄壳：[模板▾][▶启动][⏹][⟲重置] 工具条 + 事件流 ListBox + 状态检查器静态卡 +
// 注入按钮排（六快捷）。VM 逻辑=会话驱动与事件渲染；逐 Item 场景表编辑 MS3 完整化。
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
#include "vd_host.h"
#include "vd_templates.h"

#include <string>
#include <vector>

namespace usts::shell::vd {

class VdEditorPanel {
public:
    static HWND__* create_w3(void* host);
    static VdEditorPanel* instance();

    void start(size_t template_index);      // ▶：建会话+注入报告源
    void stop();                            // ⏹
    void do_reset();                        // ⟲
    void drive_enum();                      // 驱动一遍标准枚举（W5 虚拟后端同入口）
    void inject_quick(Inject kind, int count);

    HWND event_list() const { return m_events; }
    HWND hwnd() const { return m_hwnd; }
    bool started() const { return m_session && m_session->started(); }
    size_t event_count() const { return m_rows; }

private:
    void append_event(const VdMsg& m);
    HWND m_hwnd = nullptr;
    HWND m_events = nullptr;
    HWND m_state = nullptr;
    std::unique_ptr<VdSession> m_session;
    KeyboardReportSource m_report_src;
    size_t m_rows = 0;
};

// ---------------------------------------------------------------------------
// 实现（薄壳；模块日志 ui.editor）
// ---------------------------------------------------------------------------
#include <memory>

inline VdEditorPanel* VdEditorPanel::instance() {
    static VdEditorPanel p;
    return &p;
}

inline HWND__* VdEditorPanel::create_w3(void* host) {
    auto log = ustlog::logger("ui.editor");
    auto* self = instance();
    if (self->m_hwnd && IsWindow(self->m_hwnd)) return self->m_hwnd;
    HWND parent = static_cast<HWND>(host);
    const HINSTANCE inst =
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE));
    self->m_hwnd = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                   0, 0, 100, 100, parent, nullptr, inst, nullptr);
    if (!self->m_hwnd) return nullptr;
    self->m_events = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                     WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL,
                                     0, 32, 520, 300, self->m_hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(1)), inst,
                                     nullptr);
    self->m_state = CreateWindowExW(0, L"STATIC", L"状态: 未启动",
                                    WS_CHILD | WS_VISIBLE, 524, 32, 300, 300,
                                    self->m_hwnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(2)), inst,
                                    nullptr);
    static const struct { const wchar_t* label; Inject k; int n; } kInjects[] = {
        {L"STALL 下一次", Inject::StallNext, 1}, {L"NAK ×3", Inject::NakTimes, 3},
        {L"掉电复位", Inject::DropPower, 1}, {L"翻转错误", Inject::ToggleErr, 1},
        {L"babble", Inject::Babble, 1},
    };
    for (int i = 0; i < 5; ++i)
        CreateWindowExW(0, L"BUTTON", kInjects[i].label, WS_CHILD | WS_VISIBLE,
                        0 + i * 110, 2, 104, 26, self->m_hwnd,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(10 + i)), inst,
                        nullptr);
    log->info("W3 虚拟调试器面板已创建");
    return self->m_hwnd;
}

inline void VdEditorPanel::append_event(const VdMsg& m) {
    if (!m_events) return;
    const std::wstring line =
        std::to_wstring(m.seq) + L"  " + (m.summary.empty() ? L"-" : wraii::utf8_to_wide(m.summary));
    ListBox_AddString(m_events, line.c_str());
    ++m_rows;
}

inline void VdEditorPanel::start(size_t tpl_index) {
    auto log = ustlog::logger("ui.editor");
    const auto& tpls = builtin_templates();
    if (tpl_index >= tpls.size()) { log->warn("模板索引越界"); return; }
    stop();
    m_rows = 0;
    if (m_events) ListBox_ResetContent(m_events);
    m_report_src = KeyboardReportSource{&tpls[tpl_index].key_seq};
    auto* src = &m_report_src;
    m_session = std::make_unique<VdSession>(
        tpls[tpl_index].model, tpls[tpl_index].id,
        [this] (const VdMsg& m) {
            if (m.t == "vd_event") append_event(m);
        });
    m_session->dev().set_report_source([src] { return (*src)(); });
    m_session->start();
    if (m_state)
        SetWindowTextW(m_state, L"状态: Attached（未连接主机侧——MS2 进程内直连）");
    log->info("W3 启动: 模板={}", tpls[tpl_index].id);
}

inline void VdEditorPanel::stop() {
    if (m_session) m_session->stop();
    m_session.reset();
}

inline void VdEditorPanel::do_reset() {
    if (m_session) m_session->reset();
}

inline void VdEditorPanel::drive_enum() {
    if (!m_session || !m_session->started()) return;
    uint8_t s[8];
    s[0]=0x80; s[1]=0x06; s[2]=0; s[3]=1; s[4]=0; s[5]=0; s[6]=18; s[7]=0;
    m_session->ctrl(s);                       // GET_DESCRIPTOR device
    s[0]=0; s[1]=5; s[2]=5; s[6]=0;
    m_session->ctrl(s);                       // SET_ADDRESS 5
    s[0]=0x80; s[1]=6; s[2]=0; s[3]=2; s[6]=59; s[7]=0;
    m_session->ctrl(s);                       // GET_DESCRIPTOR config
    s[0]=0; s[1]=9; s[2]=1; s[6]=0;
    m_session->ctrl(s);                       // SET_CONFIGURATION
    if (m_state)
        SetWindowTextW(m_state, L"状态: Configured（枚举完成）");
}

inline void VdEditorPanel::inject_quick(Inject kind, int count) {
    if (m_session) m_session->inject(kind, count);
}

} // namespace usts::shell::vd
