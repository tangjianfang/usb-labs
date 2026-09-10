// command_palette.h — 命令面板（VM 纯逻辑 + 薄壳窗；设计 §8 / 计划 MS0-T11）
//
// PaletteVM 契约：
//   1. open()=空查询全量（前 8 行显示）；type/backspace/set_query 即时过滤+模糊排序；
//   2. move(±1) 循环移动；confirm() 返回选中命令（不 invoke——调用方决定）；
//   3. close() 后 visible=false；last_query 供重复打开恢复。
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
#include "command_registry.h"

#include <string>
#include <utility>
#include <vector>

namespace usts::shell {

struct PaletteVM {
    std::vector<std::pair<Command, int>> rows;
    int selected = 0;
    std::wstring last_query;
    bool visible = false;

    void open() {
        auto log = ustlog::logger("shell.cmd");
        visible = true;
        selected = 0;
        last_query.clear();
        rows = fuzzy_rank(L"", CommandRegistry::instance().all());
        if (rows.size() > 8) rows.resize(8);   // 空查询显示前 8（设计 §8）
        log->debug("palette open: {} 行", rows.size());
    }
    void close() {
        visible = false;
        ustlog::logger("shell.cmd")->debug("palette close（query={}）",
                                           wraii::wide_to_utf8(last_query));
    }
    void set_query(const std::wstring& q) {
        auto log = ustlog::logger("shell.cmd");
        last_query = q;
        rows = fuzzy_rank(q, CommandRegistry::instance().all());
        // 空查询=前 8（与 open 同口径，设计 §8）；有查询=上限 50 防长列表
        rows.resize((std::min)(rows.size(), q.empty() ? size_t{8} : size_t{50}));
        if (selected >= static_cast<int>(rows.size())) selected = 0;
        log->debug("palette query {}: {} 命中", wraii::wide_to_utf8(q), rows.size());
    }
    void type(wchar_t ch) { set_query(last_query + ch); }
    void backspace() {
        if (!last_query.empty()) last_query.pop_back();
        set_query(last_query);
    }
    bool move(int delta) {
        if (rows.empty()) return false;
        selected = (selected + delta + static_cast<int>(rows.size()))
                   % static_cast<int>(rows.size());
        return true;
    }
    const Command* confirm() const {
        if (visible && selected >= 0 &&
            selected < static_cast<int>(rows.size()))
            return &rows[selected].first;
        return nullptr;
    }
};

// 薄壳：宿主按键转发给 VM，双击/Enter=确认回调（T12 接主窗口 Ctrl+K）
class CommandPaletteWnd {
public:
    using ConfirmFn = void (*)(const std::string& cmd_id);
    static bool register_class(HINSTANCE inst);
    bool create(HINSTANCE inst, HWND owner);
    void show(PaletteVM& vm, ConfirmFn on_confirm);
    HWND hwnd() const { return m_hwnd; }

private:
    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    HWND m_hwnd = nullptr;
    HWND m_edit = nullptr;
    HWND m_list = nullptr;
    PaletteVM* m_vm = nullptr;
    ConfirmFn m_on_confirm = nullptr;
};

// ---------------------------------------------------------------------------
// CommandPaletteWnd 实现（owner 弹层：编辑框+列表，ESC 关闭）
// ---------------------------------------------------------------------------
inline bool CommandPaletteWnd::register_class(HINSTANCE inst) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &CommandPaletteWnd::wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    wc.lpszClassName = L"USTSCmdPalette";
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

inline bool CommandPaletteWnd::create(HINSTANCE inst, HWND owner) {
    auto log = ustlog::logger("ui.shell");
    m_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"USTSCmdPalette",
                             L"命令面板", WS_POPUPWINDOW | WS_CAPTION,
                             0, 0, 480, 320, owner, nullptr, inst, this);
    if (!m_hwnd) {
        log->error("palette CreateWindowExW 失败 GLE=0x{:08X}", GetLastError());
        return false;
    }
    m_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                             8, 8, 464, 24, m_hwnd, reinterpret_cast<HMENU>(1), inst, nullptr);
    m_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                             WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL |
                                 LBS_NOINTEGRALHEIGHT,
                             8, 40, 464, 240, m_hwnd, reinterpret_cast<HMENU>(2), inst, nullptr);
    log->debug("palette 窗口已创建");
    return m_edit && m_list;
}

inline void CommandPaletteWnd::show(PaletteVM& vm, ConfirmFn on_confirm) {
    m_vm = &vm;
    m_on_confirm = on_confirm;
    vm.open();
    SetWindowTextW(m_edit, L"");
    ListBox_ResetContent(m_list);
    for (const auto& [cmd, score] : vm.rows)
        ListBox_AddString(m_list, (cmd.title + L"    " +
                                   wraii::utf8_to_wide(cmd.shortcut)).c_str());
    if (!vm.rows.empty()) ListBox_SetCurSel(m_list, 0);
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    SetFocus(m_edit);
}

inline LRESULT CALLBACK CommandPaletteWnd::wnd_proc(HWND hwnd, UINT msg, WPARAM wp,
                                                    LPARAM lp) {
    auto* self = reinterpret_cast<CommandPaletteWnd*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<CommandPaletteWnd*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    auto log = ustlog::logger("ui.shell");
    switch (msg) {
    case WM_COMMAND:
        if (HIWORD(wp) == EN_UPDATE && LOWORD(wp) == 1 && self->m_vm) {
            wchar_t buf[128] = {};
            GetWindowTextW(self->m_edit, buf, 128);
            self->m_vm->set_query(buf);
            ListBox_ResetContent(self->m_list);
            for (const auto& [cmd, score] : self->m_vm->rows)
                ListBox_AddString(self->m_list,
                                  (cmd.title + L"    " +
                                   wraii::utf8_to_wide(cmd.shortcut)).c_str());
            if (!self->m_vm->rows.empty()) ListBox_SetCurSel(self->m_list, 0);
        } else if (HIWORD(wp) == LBN_DBLCLK && LOWORD(wp) == 2 && self->m_vm &&
                   self->m_on_confirm) {
            if (const Command* c = self->m_vm->confirm()) {
                log->debug("palette 确认: {}", c->id);
                self->m_vm->close();
                ShowWindow(hwnd, SW_HIDE);
                self->m_on_confirm(c->id);
            }
        }
        return 0;
    case WM_KEYDOWN:   // 编辑框加速键（↑↓ 选择 / ESC 关 / Enter 确认）
        if (!self->m_vm) break;
        if (wp == VK_DOWN) { self->m_vm->move(1); ListBox_SetCurSel(self->m_list, self->m_vm->selected); return 0; }
        if (wp == VK_UP)   { self->m_vm->move(-1); ListBox_SetCurSel(self->m_list, self->m_vm->selected); return 0; }
        if (wp == VK_ESCAPE) { self->m_vm->close(); ShowWindow(hwnd, SW_HIDE); return 0; }
        if (wp == VK_RETURN && self->m_on_confirm) {
            if (const Command* c = self->m_vm->confirm()) {
                log->debug("palette 确认: {}", c->id);
                self->m_vm->close();
                ShowWindow(hwnd, SW_HIDE);
                self->m_on_confirm(c->id);
            }
            return 0;
        }
        break;
    case WM_DESTROY:
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace usts::shell
