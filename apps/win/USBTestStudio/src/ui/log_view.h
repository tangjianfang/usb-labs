// log_view.h — RichEdit（RICHEDIT50W）日志视图：等宽字体、级别着色（CFM_COLOR）、
// 5000 行上限裁剪、深色底。未真机编译，按 MSDN 口径编写。
#pragma once

#include "framework/win32_rai.h"

#include <string>

class LogView {
public:
    // 创建 RichEdit50W 子窗口（Msftedit.dll 须已由进程 LoadLibrary）。
    bool create(HWND parent, HINSTANCE hinst, int child_id, UINT dpi);

    void append(wraii::LogLevel level, const std::wstring& text);   // 自动时间戳 + 着色
    void clear();
    void apply_dpi(UINT dpi);                                       // DPI 变化后重设默认字号

    HWND hwnd() const noexcept { return m_hwnd; }

    static constexpr int kMaxLines = 5000;    // 上限：超出裁到 kKeepLines
    static constexpr int kKeepLines = 4000;

private:
    void trim_if_needed();
    void send_default_charformat(UINT dpi);

    HWND m_hwnd = nullptr;
    UINT m_dpi = 96;
};
