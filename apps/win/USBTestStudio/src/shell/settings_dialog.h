// settings_dialog.h — 设置对话框（薄壳；设计 §8/E.4 / 计划 MS0-T11）
// MS0 落核心三组：产线(工位号)/日志(级别)/通用——改=立即经 SettingsStore 保存并 log.info；
// 完整左树六组表单随 MS3 产线台细化（设计预留）。
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
#include "settings.h"

namespace usts::shell {

namespace detail {

// 极简输入框（标题/提示/预填文本 → OK=落 buf 返回 true / Cancel 或 ESC=false）
struct InputBoxState {
    wchar_t buf[128];
    bool ok = false;
};
inline INT_PTR CALLBACK input_box_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<InputBoxState*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(dlg, GWLP_USERDATA, static_cast<LONG_PTR>(lp));
        st = reinterpret_cast<InputBoxState*>(lp);
        SetDlgItemTextW(dlg, 200, st->buf);
        SetFocus(GetDlgItem(dlg, 200));
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            GetDlgItemTextW(dlg, 200, st->buf, 128);
            st->ok = true;
            EndDialog(dlg, IDOK);
        } else if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
        }
        return TRUE;
    default:
        return FALSE;
    }
}
inline bool input_box(HWND owner, const std::wstring& title, const std::wstring& prompt,
                      wchar_t* buf, size_t cap) {
    auto log = ustlog::logger("ui.shell");
    const std::wstring full_title = title + L" — " + prompt;
    log->debug("input_box: {}", wraii::wide_to_utf8(full_title));

    // 窗口化输入框（弹出+编辑+两钮+本地模态泵）
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"USTSInputBox";
    RegisterClassExW(&wc);
    HWND box = CreateWindowExW(WS_EX_TOPMOST, L"USTSInputBox",
                               full_title.c_str(),
                               WS_POPUPWINDOW | WS_CAPTION | WS_MINIMIZEBOX,
                               CW_USEDEFAULT, CW_USEDEFAULT, 420, 150, owner, nullptr,
                               wc.hInstance, nullptr);
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", buf,
                                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                12, 12, 390, 24, box, reinterpret_cast<HMENU>(200),
                                wc.hInstance, nullptr);
    CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                    220, 52, 80, 26, box, reinterpret_cast<HMENU>(IDOK),
                    wc.hInstance, nullptr);
    CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE,
                    310, 52, 80, 26, box, reinterpret_cast<HMENU>(IDCANCEL),
                    wc.hInstance, nullptr);
    InputBoxState st;
    lstrcpynW(st.buf, buf, 128);
    ShowWindow(box, SW_SHOW);
    SetFocus(edit);
    SendMessageW(edit, EM_SETSEL, 0, -1);
    bool ok = false;
    MSG msg;
    for (;;) {
        const int r = GetMessageW(&msg, nullptr, 0, 0);
        if (r <= 0) break;
        if (msg.hwnd == edit && msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_RETURN) {
                GetWindowTextW(edit, st.buf, 128); ok = st.ok = true; break;
            }
            if (msg.wParam == VK_ESCAPE) { st.ok = false; break; }
        }
        if (msg.hwnd == box && msg.message == WM_COMMAND) {
            if (LOWORD(msg.wParam) == IDOK) { GetWindowTextW(edit, st.buf, 128); ok = true; break; }
            if (LOWORD(msg.wParam) == IDCANCEL) break;
        }
        if (!IsWindow(box)) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DestroyWindow(box);
    if (ok) lstrcpynW(buf, st.buf, static_cast<int>(cap));
    log->debug("input_box: {}", ok ? "OK" : "Cancel");
    return ok;
}

} // namespace detail

class SettingsDialog {
public:
    // 顺序收集：工位号 → 日志级别；确认=保存；取消任意一步=不改动
    static bool run(HWND owner, Settings& s) {
        auto log = ustlog::logger("ui.shell");
        wchar_t station[128] = {};
        lstrcpynW(station, s.station.c_str(), 128);
        if (!detail::input_box(owner, L"设置", L"工位号", station, 128)) return false;
        wchar_t level[128] = {};
        lstrcpynW(level, s.log_level.c_str(), 128);
        if (!detail::input_box(owner, L"设置", L"日志级别 trace/debug/info/warn/err",
                               level, 128))
            return false;
        s.station = station;
        s.log_level = level;
        std::string err;
        if (!SettingsStore::save(s, err)) {
            MessageBoxW(owner, (L"保存失败: " + wraii::utf8_to_wide(err)).c_str(),
                        L"设置", MB_ICONERROR);
            return false;
        }
        log->info("设置已保存：station={} log_level={}", wraii::wide_to_utf8(s.station),
                  wraii::wide_to_utf8(s.log_level));
        return true;
    }
};

} // namespace usts::shell
