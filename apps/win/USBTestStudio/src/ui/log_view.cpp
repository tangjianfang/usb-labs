// log_view.cpp — RichEdit 日志实现。未真机编译，按 MSDN 口径编写。
#include "ui/log_view.h"

#include <richedit.h>

#include <cstdio>
#include <cwchar>

namespace {

// richedit.h 的 MSFTEDIT_CLASS 为窄字符串宏，为避免宏形态差异自行定义宽字符类名
constexpr wchar_t kRichEditClass[] = L"RICHEDIT50W";

int control_id(int child_id) { return child_id; }

} // namespace

bool LogView::create(HWND parent, HINSTANCE hinst, int child_id, UINT dpi) {
    m_dpi = dpi;
    m_hwnd = ::CreateWindowExW(0, kRichEditClass, nullptr,
                               WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE |
                                   ES_AUTOVSCROLL | ES_READONLY,
                               0, 0, 0, 0, parent,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id(child_id))),
                               hinst, nullptr);
    if (!m_hwnd) return false;

    // 深色背景 + 大文本上限（EM_EXLIMITTEXT 默认 32KB，必须调大）
    ::SendMessageW(m_hwnd, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(RGB(0x1B, 0x1B, 0x1F)));
    ::SendMessageW(m_hwnd, EM_EXLIMITTEXT, 0, static_cast<LPARAM>(8 * 1024 * 1024));
    send_default_charformat(dpi);
    return true;
}

void LogView::send_default_charformat(UINT dpi) {
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR | CFM_BOLD;
    cf.dwEffects = 0;
    cf.yHeight = MulDiv(9, 20 * static_cast<int>(dpi), 72);   // 9pt，twips = 1/20 pt
    cf.crTextColor = RGB(0xD4, 0xD4, 0xD4);
    cf.bPitchAndFamily = FIXED_PITCH | FF_MODERN;
    ::wcscpy_s(cf.szFaceName, LF_FACESIZE, L"Consolas");
    ::SendMessageW(m_hwnd, EM_SETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&cf));
}

void LogView::apply_dpi(UINT dpi) {
    m_dpi = dpi;
    send_default_charformat(dpi);
}

void LogView::append(wraii::LogLevel level, const std::wstring& text) {
    if (!m_hwnd) return;

    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    wchar_t ts[48];
    ::swprintf(ts, 48, L"[%02u:%02u:%02u.%03u %s] ", st.wHour, st.wMinute, st.wSecond,
               st.wMilliseconds, wraii::log_level_name(level));
    std::wstring line = std::wstring(ts) + text + L"\r\n";

    // 追加：选区移到文末 → 先设该选区字符色（新插入文本继承选区格式）→ 替换
    int len = static_cast<int>(::GetWindowTextLengthW(m_hwnd));
    ::SendMessageW(m_hwnd, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));

    COLORREF color;
    switch (level) {
        case wraii::LogLevel::Debug: color = RGB(0x6A, 0x99, 0x55); break;
        case wraii::LogLevel::Warn:  color = RGB(0xE5, 0xC0, 0x7B); break;
        case wraii::LogLevel::Error: color = RGB(0xF4, 0x6A, 0x6A); break;
        default:                     color = RGB(0xD4, 0xD4, 0xD4); break;
    }
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    cf.crTextColor = color;
    ::SendMessageW(m_hwnd, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));

    ::SendMessageW(m_hwnd, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
    ::SendMessageW(m_hwnd, EM_SCROLLCARET, 0, 0);

    trim_if_needed();
}

void LogView::trim_if_needed() {
    int lines = static_cast<int>(::SendMessageW(m_hwnd, EM_GETLINECOUNT, 0, 0));
    if (lines <= kMaxLines) return;

    int cut_at_line = lines - kKeepLines;
    LONG_PTR idx = ::SendMessageW(m_hwnd, EM_LINEINDEX,
                                  static_cast<WPARAM>(cut_at_line), 0);
    if (idx <= 0) return;
    ::SendMessageW(m_hwnd, EM_SETSEL, static_cast<WPARAM>(0), static_cast<LPARAM>(idx));
    ::SendMessageW(m_hwnd, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
}

void LogView::clear() {
    if (!m_hwnd) return;
    ::SendMessageW(m_hwnd, EM_SETSEL, 0, 0);
    ::SendMessageW(m_hwnd, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
}
