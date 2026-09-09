// session_pane.cpp — EP-4 S3 会话台后半·会话面板实现。纯 UI 接线层：
// 逻辑在 session_core/session_view（session_selftest 覆盖），本文件只做
// Win32 控件创建、消息分发与线程编组（读线程→PostMessage→UI 线程入账）。
// 未真机编译部分已按 MSDN 口径编写，随整片真机验收。
// 日志：ui.session（info=面板生命周期/状态跃迁，debug=逐操作，warn=可恢复失败，err=致命失败）。
#include "ui/session_pane.h"
#include "app/log.h"

#include <commctrl.h>

#include <cwchar>

#pragma comment(lib, "comctl32.lib")

namespace {

constexpr wchar_t kPaneClass[] = L"USBTestStudio_SessionPane";

auto slog = ustlog::logger("ui.session");

// WM_APP+2：读线程收到一帧（LPARAM: new std::vector<uint8_t>，UI 侧 delete；
// console_window 的 kMsgCatalogDone 用 WM_APP+1，本面板局部约定互不冲突）
constexpr UINT kMsgRx = WM_APP + 2;

constexpr UINT_PTR kSendSubclassId = 1;

// 接收区显示行数封顶（显示侧防膨胀；账面环形 cap 与此独立，均不影响计数）
constexpr LRESULT kMaxLines = 4000, kTrimSlack = 200;

HWND pane_control(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style,
                  int child_id) {
    return ::CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
                             parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(child_id)),
                             ::GetModuleHandleW(nullptr), nullptr);
}

} // namespace

bool SessionPane::register_class() {
    static const bool ok = [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &SessionPane::pane_proc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kPaneClass;
        return ::RegisterClassExW(&wc) != 0;
    }();
    return ok;
}

// ---------------------------------------------------------------------------
// 创建 / 销毁 / 消息分发
// ---------------------------------------------------------------------------
SessionPane* SessionPane::create(HWND parent, std::unique_ptr<IChannel> ch,
                                 std::wstring title) {
    if (!register_class() || !ch) return nullptr;
    SessionPane* p = new SessionPane();
    p->m_title = std::move(title);
    p->m_channel = std::move(ch);
    p->m_hwnd = ::CreateWindowExW(0, kPaneClass, L"", WS_CHILD, 0, 0, 0, 0, parent,
                                  nullptr, ::GetModuleHandleW(nullptr), p);
    if (!p->m_hwnd) {
        slog->error("创建会话面板窗口失败 GetLastError=0x{:08X}", ::GetLastError());
        delete p;
        return nullptr;
    }
    slog->info("会话面板已创建: {}", ustlog::w2u(p->m_title));
    return p;
}

SessionPane::~SessionPane() {
    slog->info("会话面板销毁: {}", ustlog::w2u(m_title));
    // 顺序：先断接收回调（读线程不再 Post）→ 销毁窗口（清理子控件/子类化）
    // → unique_ptr 析构通道（close 先 join 读线程再关句柄）
    if (m_channel) m_channel->set_receive_callback({});
    if (m_hwnd) ::DestroyWindow(m_hwnd);   // WM_NCDESTROY 置空 m_hwnd
    if (m_font) ::DeleteObject(m_font);
    if (m_mono) ::DeleteObject(m_mono);
}

LRESULT CALLBACK SessionPane::pane_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* w = reinterpret_cast<SessionPane*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w));
        w->m_hwnd = hwnd;   // WM_CREATE 之前回填句柄
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* self = reinterpret_cast<SessionPane*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return ::DefWindowProcW(hwnd, msg, wp, lp);
    return self->on_message(msg, wp, lp);
}

LRESULT SessionPane::on_message(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            on_create();
            return 0;

        case kMsgRx: {
            std::unique_ptr<std::vector<uint8_t>> payload(
                reinterpret_cast<std::vector<uint8_t>*>(lp));
            on_rx(payload.get());
            return 0;
        }

        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            if (id == IDC_BTN_SEND && code == BN_CLICKED) do_send();
            else if (id == IDC_SEND_EDIT && code == EN_CHANGE) {
                if (!m_setting_text && !m_in_history)   // 草稿只记"非游历态"的编辑
                    m_draft = fetch_text(m_send);
            } else if (id == IDC_CHK_PERIODIC && code == BN_CLICKED) {
                if (::SendMessageW(m_chkPeriodic, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                    m_core.periodic.set_interval(parse_interval());
                    m_core.periodic.arm(now_ms());
                    slog->debug("周期发送已启动，间隔 {}ms", m_core.periodic.interval());
                } else {
                    m_core.periodic.disarm();
                    slog->debug("周期发送已停止");
                }
                status_refresh();
            } else if (id == IDC_EDIT_INTERVAL && code == EN_CHANGE) {
                if (m_core.periodic.armed())   // 运行中间隔即改即生效（相位不动）
                    m_core.periodic.set_interval(parse_interval());
            } else if (id == IDC_CHK_PAUSE && code == BN_CLICKED) {
                const bool paused =
                    ::SendMessageW(m_chkPause, BM_GETCHECK, 0, 0) == BST_CHECKED;
                m_view.set_paused(paused);
                slog->debug("接收视图{}", paused ? "暂停（后台继续收）" : "恢复");
                if (!paused) render_poll();   // 恢复即补齐暂停期间仍在账内的帧
            } else if (id == IDC_BTN_CLEAR && code == BN_CLICKED) {
                ::SetWindowTextW(m_rx, L"");   // 清屏不清账：游标不动，旧帧不回放
            } else if (id == IDC_CHK_HEX && code == BN_CLICKED) {
                m_view.hex_view =
                    ::SendMessageW(m_chkHex, BM_GETCHECK, 0, 0) == BST_CHECKED;
                slog->debug("Hex 视图切换: {}", m_view.hex_view ? "开" : "关");
                render_rebuild();
            } else if (id == IDC_CHK_ABSTS && code == BN_CLICKED) {
                m_view.absolute_ts =
                    ::SendMessageW(m_chkAbsTs, BM_GETCHECK, 0, 0) == BST_CHECKED;
                slog->debug("绝对时间戳视图切换: {}", m_view.absolute_ts ? "开" : "关");
                render_rebuild();
            } else if (id == IDC_CHK_PARSED && code == BN_CLICKED) {
                m_view.parsed_view =
                    ::SendMessageW(m_chkParsed, BM_GETCHECK, 0, 0) == BST_CHECKED;
                slog->debug("解析视图切换: {}", m_view.parsed_view ? "开" : "关");
                render_rebuild();   // 原始|解析切换 = 按新口径看当前账面（§4.7）
            } else if (id == IDC_ENCODING && code == CBN_SELENDOK) {
                status_refresh();   // 模式名入状态行
            }
            return 0;
        }

        case WM_DESTROY:
            if (m_channel) m_channel->set_receive_callback({});   // belt & suspenders
            return 0;

        case WM_NCDESTROY:
            m_hwnd = nullptr;
            return 0;

        default:
            return ::DefWindowProcW(m_hwnd, msg, wp, lp);
    }
}

// ---------------------------------------------------------------------------
// 控件创建 / 字体 / 布局
// ---------------------------------------------------------------------------
void SessionPane::on_create() {
    m_dpi = ::GetDpiForWindow(m_hwnd);

    // 会话时间基准：t0 = 当前 tick，锚定当前 Unix 毫秒（绝对时刻 = t + 锚）
    const unsigned long long tick = ::GetTickCount64();
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    const unsigned long long unix_ms = u.QuadPart / 10000ULL - 11644473600000ULL;
    m_core.set_time_base(tick, unix_ms - tick);

    create_controls();
    make_fonts();
    apply_fonts();

    // S4 解析视图（§4.7 跟随设备协议）：选型来自通道描述（HID 顶层 usage
    // page/usage，HidChannel open 时透传）；HID 已收录选型默认开，无解析器
    // （未收录 usage 组合）置灰——原始视图始终可用
    m_view.parser = parser_select::for_channel(m_channel->desc());
    slog->debug("解析视图选型: {}", ustlog::w2u(std::wstring(m_view.parser.name())));
    if (m_view.parser.kind == parser_select::PaneParser::Kind::none) {
        ::EnableWindow(m_chkParsed, FALSE);
    } else if (m_view.parser.kind != parser_select::PaneParser::Kind::ascii) {
        ::SendMessageW(m_chkParsed, BM_SETCHECK, BST_CHECKED, 0);
        m_view.parsed_view = true;
    }

    // 接收编组（channel.h 线程约定）：读线程只 Post 载荷，窗口已亡则地删
    SessionPane* self = this;
    m_channel->set_receive_callback([self](const std::vector<uint8_t>& d) {
        HWND h = self->m_hwnd;
        auto* payload = new std::vector<uint8_t>(d);
        if (!h || !::PostMessageW(h, kMsgRx, 0, reinterpret_cast<LPARAM>(payload)))
            delete payload;
    });
}

void SessionPane::create_controls() {
    m_send = pane_control(m_hwnd, L"Edit", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                          IDC_SEND_EDIT);
    ::SendMessageW(m_send, EM_SETCUEBANNER, FALSE,
                   reinterpret_cast<LPARAM>(L"发送内容：01 02 / AT+… （Ctrl+↵ 发送，↑↓ 历史）"));
    ::SetWindowSubclass(m_send, send_edit_subclass, kSendSubclassId,
                        reinterpret_cast<DWORD_PTR>(this));

    m_encoding = pane_control(m_hwnd, L"ComboBox", L"",
                              CBS_DROPDOWNLIST | WS_TABSTOP | WS_BORDER, IDC_ENCODING);
    for (const wchar_t* m : {L"自动识别", L"Hex 锁定", L"ASCII 锁定"})
        ::SendMessageW(m_encoding, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(m));
    ::SendMessageW(m_encoding, CB_SETCURSEL, 0, 0);

    m_btnSend = pane_control(m_hwnd, L"Button", L"发送", BS_PUSHBUTTON | WS_TABSTOP,
                             IDC_BTN_SEND);
    m_chkPeriodic = pane_control(m_hwnd, L"Button", L"周期",
                                 BS_AUTOCHECKBOX | WS_TABSTOP, IDC_CHK_PERIODIC);
    m_interval = pane_control(m_hwnd, L"Edit", L"1000", WS_BORDER | ES_NUMBER | WS_TABSTOP,
                              IDC_EDIT_INTERVAL);
    m_labelMs = pane_control(m_hwnd, L"Static", L"ms", SS_CENTERIMAGE, 0);

    m_chkPause = pane_control(m_hwnd, L"Button", L"暂停",
                              BS_AUTOCHECKBOX | BS_PUSHLIKE | WS_TABSTOP, IDC_CHK_PAUSE);
    m_btnClear = pane_control(m_hwnd, L"Button", L"清屏",
                              BS_PUSHBUTTON | WS_TABSTOP, IDC_BTN_CLEAR);
    m_chkHex = pane_control(m_hwnd, L"Button", L"Hex 视图",
                            BS_AUTOCHECKBOX | WS_TABSTOP, IDC_CHK_HEX);
    ::SendMessageW(m_chkHex, BM_SETCHECK, BST_CHECKED, 0);   // 默认 Hex（§4.6 惯例）
    m_chkAbsTs = pane_control(m_hwnd, L"Button", L"绝对时间",
                              BS_AUTOCHECKBOX | WS_TABSTOP, IDC_CHK_ABSTS);
    m_chkParsed = pane_control(m_hwnd, L"Button", L"解析视图",
                               BS_AUTOCHECKBOX | WS_TABSTOP, IDC_CHK_PARSED);

    m_rx = pane_control(m_hwnd, L"Edit", L"",
                        WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY
                            | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                        IDC_RX);
    m_status = pane_control(m_hwnd, L"Static", L"", SS_ENDELLIPSIS, IDC_PANE_STATUS);
}

void SessionPane::make_fonts() {
    if (m_font) ::DeleteObject(m_font);
    if (m_mono) ::DeleteObject(m_mono);
    m_font = ::CreateFontW(-MulDiv(9, static_cast<int>(m_dpi), 72), 0, 0, 0, FW_NORMAL,
                           FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                           L"Microsoft YaHei UI");
    m_mono = ::CreateFontW(-MulDiv(9, static_cast<int>(m_dpi), 72), 0, 0, 0, FW_NORMAL,
                           FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN,
                           L"Consolas");
}

void SessionPane::apply_fonts() {
    if (!m_font) return;
    HWND ui[] = {m_send,   m_encoding, m_btnSend, m_chkPeriodic, m_interval,
                 m_labelMs, m_chkPause, m_btnClear, m_chkHex, m_chkAbsTs, m_chkParsed,
                 m_status};
    for (HWND h : ui)
        if (h) ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
    if (m_rx && m_mono)
        ::SendMessageW(m_rx, WM_SETFONT, reinterpret_cast<WPARAM>(m_mono), TRUE);
}

void SessionPane::layout() {
    RECT rc{};
    ::GetClientRect(m_hwnd, &rc);
    const int w = rc.right, h = rc.bottom;
    const int m = S(8), ctlH = S(26);

    // 行 1（发送区）：发送框吃余量，右侧编码/发送/周期/间隔固定宽
    const int y1 = m;
    const int encW = S(96), sendBtnW = S(78), perW = S(50), itvW = S(64), msW = S(22);
    int x = m;
    int sendW = w - 2 * m - encW - sendBtnW - perW - itvW - msW - 5 * S(4);
    if (sendW < S(80)) sendW = S(80);   // 极窄窗保护
    ::MoveWindow(m_send, x, y1 + S(3), sendW, ctlH - S(6), TRUE);
    x += sendW + S(4);
    ::MoveWindow(m_encoding, x, y1, encW, S(160), TRUE);   // 高度含下拉展开_extent
    x += encW + S(4);
    ::MoveWindow(m_btnSend, x, y1, sendBtnW, ctlH, TRUE);
    x += sendBtnW + S(4);
    ::MoveWindow(m_chkPeriodic, x, y1, perW, ctlH, TRUE);
    x += perW;
    ::MoveWindow(m_interval, x, y1 + S(3), itvW, ctlH - S(6), TRUE);
    x += itvW;
    ::MoveWindow(m_labelMs, x, y1, msW, ctlH, TRUE);

    // 行 2（接收工具条）：暂停/清屏 + Hex/时间戳/解析切换
    const int y2 = y1 + ctlH + m;
    const int pauseW = S(60), clearW = S(60), hexW = S(82), absW = S(92), parseW = S(82);
    x = m;
    ::MoveWindow(m_chkPause, x, y2, pauseW, ctlH, TRUE);
    x += pauseW + S(4);
    ::MoveWindow(m_btnClear, x, y2, clearW, ctlH, TRUE);
    x += clearW + S(12);
    ::MoveWindow(m_chkHex, x, y2, hexW, ctlH, TRUE);
    x += hexW + S(4);
    ::MoveWindow(m_chkAbsTs, x, y2, absW, ctlH, TRUE);
    x += absW + S(4);
    ::MoveWindow(m_chkParsed, x, y2, parseW, ctlH, TRUE);

    // 行 3（接收区吃余量）+ 行 4（面板状态）
    const int y3 = y2 + ctlH + m;
    const int statusH = S(20);
    ::MoveWindow(m_rx, m, y3, w - 2 * m, h - y3 - statusH - m, TRUE);
    ::MoveWindow(m_status, m, h - statusH, w - 2 * m, statusH, TRUE);
}

void SessionPane::show(bool visible) noexcept {
    if (!m_hwnd) return;
    ::ShowWindow(m_hwnd, visible ? SW_SHOW : SW_HIDE);
    if (visible && m_send) ::SetFocus(m_send);   // 切到本会话即可直接敲发送框
}

void SessionPane::place(int x, int y, int w, int h) noexcept {
    if (!m_hwnd) return;
    ::MoveWindow(m_hwnd, x, y, w, h, TRUE);
    layout();
}

void SessionPane::relayout(UINT dpi) noexcept {
    m_dpi = dpi;
    make_fonts();
    apply_fonts();
    layout();
}

// ---------------------------------------------------------------------------
// 动作：发送 / 历史回选 / 接收入账 / 渲染
// ---------------------------------------------------------------------------
void SessionPane::do_send(bool* transport_failed) {
    if (transport_failed) *transport_failed = false;
    if (!m_channel || !m_channel->is_open()) {
        if (transport_failed) *transport_failed = true;   // 传输层失败（周期据此停）
        slog->warn("发送被拒：通道未打开");
        m_note = L"通道未打开";
        status_refresh();
        return;
    }
    const std::wstring text = fetch_text(m_send);
    const int sel = static_cast<int>(::SendMessageW(m_encoding, CB_GETCURSEL, 0, 0));
    const auto mode = sel == 1   ? session_codec::SendEncoding::hex_lock
                      : sel == 2 ? session_codec::SendEncoding::ascii_lock
                                 : session_codec::SendEncoding::auto_detect;
    const auto r = session_codec::parse_send_text(text, mode);
    if (!r.error.empty()) {
        slog->debug("发送内容解析失败: {}", ustlog::w2u(r.error));
        m_note = r.error;    // 内容问题：不算传输层失败，周期不因此停
        status_refresh();
        return;
    }
    if (r.bytes.empty()) {
        slog->debug("发送被拒：无可发内容");
        m_note = L"无可发内容";
        status_refresh();
        return;
    }
    std::wstring err;
    if (m_channel->send(r.bytes.data(), r.bytes.size(), &err)) {
        m_core.record_tx(r.bytes.data(), r.bytes.size(), now_ms());
        m_core.history.push(text);   // push 即结束游历（草稿态恢复）
        m_in_history = false;
        m_draft = text;
        wchar_t n[48];
        ::swprintf(n, 48, L"已发 %zu 字节（%s）", r.bytes.size(),
                   r.hex_mode ? L"Hex" : L"ASCII");
        m_note = n;
        slog->debug("已发送 {} 字节（{}）", r.bytes.size(), r.hex_mode ? "Hex" : "ASCII");
        render_poll();
    } else {
        if (transport_failed) *transport_failed = true;
        slog->warn("发送失败（传输层）: {}", ustlog::w2u(err));
        m_note = L"发送失败：" + err;
    }
    status_refresh();
}

void SessionPane::recall(bool up) {
    if (m_core.history.size() == 0) return;   // 空历史不动文本
    if (up) {
        if (!m_in_history) m_draft = fetch_text(m_send);   // 离开草稿前存底
        const std::wstring item = m_core.history.up();
        if (item.empty()) return;
        m_in_history = true;
        set_send_text(item);
    } else {
        const std::wstring item = m_core.history.down();
        if (item.empty()) {                 // 到最新再下 → 回底交还草稿
            m_in_history = false;
            set_send_text(m_draft);
        } else {
            m_in_history = true;
            set_send_text(item);
        }
    }
}

void SessionPane::on_rx(std::vector<uint8_t>* payload) {
    if (!payload) return;
    m_core.record_rx(payload->data(), payload->size(), now_ms());
    render_poll();
    status_refresh();
}

void SessionPane::render_poll() {
    append_lines(m_view.poll(m_core));
}

void SessionPane::render_rebuild() {
    const auto lines = m_view.rebuild(m_core);
    std::wstring all;
    for (const auto& l : lines) {
        all += l;
        all += L"\r\n";
    }
    ::SetWindowTextW(m_rx, all.c_str());
    scroll_to_end();
    status_refresh();
}

void SessionPane::append_lines(const std::vector<std::wstring>& lines) {
    if (lines.empty()) return;
    std::wstring chunk;
    for (const auto& l : lines) {
        chunk += l;
        chunk += L"\r\n";
    }
    const LRESULT len = ::GetWindowTextLengthW(m_rx);
    ::SendMessageW(m_rx, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
    ::SendMessageW(m_rx, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(chunk.c_str()));
    scroll_to_end();
    trim_receive();
}

void SessionPane::trim_receive() {
    // 显示行数封顶 + 滞回剪头：只防 UI 膨胀，不动账面（§4.6 不丢原始归档）
    const LRESULT lines = ::SendMessageW(m_rx, EM_GETLINECOUNT, 0, 0);
    if (lines <= kMaxLines + kTrimSlack) return;
    const LRESULT pos =
        ::SendMessageW(m_rx, EM_LINEINDEX, static_cast<WPARAM>(lines - kMaxLines), 0);
    ::SendMessageW(m_rx, EM_SETSEL, 0, static_cast<LPARAM>(pos));
    ::SendMessageW(m_rx, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
}

void SessionPane::status_refresh() {
    if (!m_status) return;
    const unsigned long long tx = m_core.journal.tx_frames();
    const unsigned long long rx = m_core.journal.rx_frames();
    std::wstring s = L"TX " + std::to_wstring(tx) + L" · RX " + std::to_wstring(rx);
    const int sel = static_cast<int>(::SendMessageW(m_encoding, CB_GETCURSEL, 0, 0));
    s += sel == 1   ? L" · Hex 锁定"
         : sel == 2 ? L" · ASCII 锁定"
                    : L" · 自动识别";
    if (m_core.periodic.armed())
        s += L" · 周期 " + std::to_wstring(m_core.periodic.interval()) + L"ms";
    if (m_view.parsed_view && m_view.parser.kind != parser_select::PaneParser::Kind::none)
        s += L" · 解析: " + std::wstring(m_view.parser.name());
    if (m_view.paused()) s += L" · 已暂停（后台继续收）";
    if (!m_note.empty()) s += L" · " + m_note;
    ::SetWindowTextW(m_status, s.c_str());
}

void SessionPane::tick(unsigned long long now) {
    if (!m_core.periodic.armed() || !m_core.periodic.due(now)) return;
    bool transport_failed = false;
    do_send(&transport_failed);
    if (!transport_failed) return;
    // 传输层失败 → 周期自动停止：NAK 永续的设备会让每次发送都顶满写超时（3s），
    // 继续打节拍 = UI 近乎持续冻结（#71 遗留缺陷池；解析错误/空内容不触发）
    m_core.periodic.disarm();
    slog->info("周期发送因传输失败自动停止");
    ::SendMessageW(m_chkPeriodic, BM_SETCHECK, BST_UNCHECKED, 0);
    m_note = L"周期已自动停止·" + m_note;
    status_refresh();
}

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
std::wstring SessionPane::fetch_text(HWND edit) const {
    std::wstring text;
    const int len = ::GetWindowTextLengthW(edit);
    if (len > 0) {
        text.resize(static_cast<size_t>(len) + 1);
        ::GetWindowTextW(edit, text.data(), len + 1);
        text.resize(static_cast<size_t>(len));
    }
    return text;
}

void SessionPane::set_send_text(const std::wstring& s) {
    m_setting_text = true;
    ::SetWindowTextW(m_send, s.c_str());
    m_setting_text = false;
    // 光标落末尾，回选后可直接续编
    const LRESULT len = ::GetWindowTextLengthW(m_send);
    ::SendMessageW(m_send, EM_SETSEL, static_cast<WPARAM>(len),
                   static_cast<LPARAM>(len));
}

unsigned SessionPane::parse_interval() const {
    wchar_t buf[32];
    ::GetWindowTextW(m_interval, buf, 32);
    wchar_t* end = nullptr;
    const unsigned long v = ::wcstoul(buf, &end, 10);
    return v ? unsigned(v) : 1000;   // 钳制交给 PeriodicSender（100ms~60s）
}

unsigned long long SessionPane::now_ms() const noexcept { return ::GetTickCount64(); }

void SessionPane::scroll_to_end() noexcept {
    ::SendMessageW(m_rx, WM_VSCROLL, SB_BOTTOM, 0);
}

// 发送框子类化：↑↓ 历史回选 + Ctrl+↵ 发送（设计 §4.3/§4.5）
LRESULT CALLBACK SessionPane::send_edit_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                                 UINT_PTR /*id*/, DWORD_PTR ref) {
    auto* self = reinterpret_cast<SessionPane*>(ref);
    if (msg == WM_KEYDOWN) {
        if (wp == VK_UP) {
            self->recall(true);
            return 0;
        }
        if (wp == VK_DOWN) {
            self->recall(false);
            return 0;
        }
        if (wp == VK_RETURN && (::GetKeyState(VK_CONTROL) & 0x8000)) {
            self->do_send();
            return 0;
        }
    }
    return ::DefSubclassProc(h, msg, wp, lp);
}
