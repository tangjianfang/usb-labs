// console_window.cpp — EP-4 通信控制台窗口实现（上区 S2 设备发现 + 下区 S3 会话
// 标签台）。未真机编译，按 MSDN 口径编写。
// 日志：ui.console（info=窗口/扫描/会话生命周期，debug=过滤与选中逐操作，warn=可恢复失败，err=致命失败）。
#include "ui/console_window.h"
#include "app/log.h"

#include "discovery/catalog_build.h"
#include "discovery/msc_enum.h"

#include <commctrl.h>

#include <cwchar>
#include <thread>

#pragma comment(lib, "comctl32.lib")

namespace {

constexpr wchar_t kClassName[] = L"USBTestStudio_ConsoleWnd";
constexpr wchar_t kWindowTitle[] = L"USBTestStudio — 工程师通信控制台";

// WM_APP+1：后台扫描完成（LPARAM: new std::vector<ConsoleDevice>，UI 侧 delete）
constexpr UINT kMsgCatalogDone = WM_APP + 1;

// 目录列（96dpi 基准宽；layout 按窗口 DPI 重新应用）
const wchar_t* const kColTitles[] = {L"#", L"名称", L"VID:PID", L"协议", L"端口路径"};
constexpr int kColWidths[] = {40, 210, 90, 52, 460};

auto slog = ustlog::logger("ui.console");

HWND create_control(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style,
                    int child_id) {
    return ::CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(child_id)),
                             ::GetModuleHandleW(nullptr), nullptr);
}

} // namespace

// ---------------------------------------------------------------------------
// 注册 / 创建 / 消息循环
// ---------------------------------------------------------------------------
bool ConsoleWindow::register_class(HINSTANCE hinst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &ConsoleWindow::wnd_proc;
    wc.hInstance = hinst;
    wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = ::LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kClassName;
    if (::RegisterClassExW(&wc) == 0) {
        slog->error("注册控制台窗口类失败 GetLastError=0x{:08X}", ::GetLastError());
        return false;
    }
    return true;
}

ConsoleWindow::~ConsoleWindow() {
    slog->info("控制台窗口销毁");
    // m_panes 逐个析构：面板先断接收回调再销毁窗口，通道 close 前先 join 读线程
    if (m_accel) ::DestroyAcceleratorTable(m_accel);
    if (m_font) ::DeleteObject(m_font);
}

ConsoleWindow* ConsoleWindow::create(HINSTANCE hinst, int nCmdShow) {
    ConsoleWindow* w = new ConsoleWindow();
    w->m_hwnd = ::CreateWindowExW(0, kClassName, kWindowTitle, WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                  nullptr, nullptr, hinst, w);
    if (!w->m_hwnd) {
        slog->error("创建控制台窗口失败 GetLastError=0x{:08X}", ::GetLastError());
        delete w;
        return nullptr;
    }
    slog->info("控制台窗口已创建");
    w->m_dpi = ::GetDpiForWindow(w->m_hwnd);
    int cw = w->S(1000), ch = w->S(560);
    RECT work{};
    if (::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        int x = work.left + ((work.right - work.left) - cw) / 2;
        int y = work.top + ((work.bottom - work.top) - ch) / 2;
        ::SetWindowPos(w->m_hwnd, nullptr, x, y, cw, ch, SWP_NOZORDER);
    }
    ::ShowWindow(w->m_hwnd, nCmdShow);
    ::UpdateWindow(w->m_hwnd);
    return w;
}

LRESULT CALLBACK ConsoleWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* w = reinterpret_cast<ConsoleWindow*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w));
        w->m_hwnd = hwnd;   // WM_CREATE 之前回填句柄
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* self = reinterpret_cast<ConsoleWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return ::DefWindowProcW(hwnd, msg, wp, lp);
    return self->on_message(msg, wp, lp);
}

LRESULT ConsoleWindow::on_message(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            on_create();
            return 0;

        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) layout();
            return 0;

        case WM_DPICHANGED: {
            m_dpi = HIWORD(wp);
            const RECT* sug = reinterpret_cast<const RECT*>(lp);
            ::SetWindowPos(m_hwnd, nullptr, sug->left, sug->top, sug->right - sug->left,
                           sug->bottom - sug->top, SWP_NOZORDER | SWP_NOACTIVATE);
            make_fonts();
            apply_fonts();
            layout();
            for (auto& p : m_panes) p->relayout(m_dpi);   // 子窗口不收 WM_DPICHANGED
            return 0;
        }

        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            if (id == IDC_REFRESH) do_scan();
            else if (id == IDC_SEARCH && code == EN_CHANGE) refill();   // 即时过滤：输入即收敛
            else if ((id == IDC_CHK_HID || id == IDC_CHK_SERIAL || id == IDC_CHK_USB
                      || id == IDC_CHK_MSC)
                     && code == BN_CLICKED)
                refill();
            return 0;
        }

        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<NMHDR*>(lp);
            if (nm->idFrom == IDC_LIST && nm->code == NM_DBLCLK) on_activate_item();
            else if (nm->idFrom == IDC_TABS && nm->code == TCN_SELCHANGE) on_tab_switch();
            else if (nm->idFrom == IDC_TABS && nm->code == NM_RCLICK) on_tab_rclick();
            return 0;
        }

        case WM_TIMER: {
            if (wp == kTickTimer) {   // 各会话共享一个 100ms 节拍（周期发送 §4.4）
                const unsigned long long now = ::GetTickCount64();
                for (auto& p : m_panes) p->tick(now);
            }
            return 0;
        }

        case kMsgCatalogDone: {
            std::unique_ptr<std::vector<ConsoleDevice>> ev(
                reinterpret_cast<std::vector<ConsoleDevice>*>(lp));
            handle_scan_done(ev.get());
            return 0;
        }

        case WM_DESTROY:
            ::KillTimer(m_hwnd, kTickTimer);
            ::PostQuitMessage(0);
            return 0;

        default:
            return ::DefWindowProcW(m_hwnd, msg, wp, lp);
    }
}

int ConsoleWindow::run() {
    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!m_accel || ::TranslateAcceleratorW(m_hwnd, m_accel, &msg) == 0) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}

// ---------------------------------------------------------------------------
// 控件创建 / 字体 / 布局
// ---------------------------------------------------------------------------
void ConsoleWindow::on_create() {
    m_dpi = ::GetDpiForWindow(m_hwnd);
    create_controls();
    make_fonts();
    apply_fonts();

    ACCEL acc[1] = {{FVIRTKEY, VK_F5, IDC_REFRESH}};
    m_accel = ::CreateAcceleratorTableW(acc, 1);

    ::SetTimer(m_hwnd, kTickTimer, 100, nullptr);   // 周期发送节拍（§4.4 下限 100ms）

    do_scan();   // 启动即扫一次
}

void ConsoleWindow::create_controls() {
    m_search = create_control(m_hwnd, L"Edit", L"",
                              WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, IDC_SEARCH);
    ::SendMessageW(m_search, EM_SETCUEBANNER, FALSE,
                   reinterpret_cast<LPARAM>(L"搜索 名称 / VID:PID / 协议 / 路径（空格分隔多词）"));
    m_chkHid = create_control(m_hwnd, L"Button", L"HID", BS_AUTOCHECKBOX | WS_TABSTOP,
                              IDC_CHK_HID);
    m_chkSerial = create_control(m_hwnd, L"Button", L"串口", BS_AUTOCHECKBOX | WS_TABSTOP,
                                 IDC_CHK_SERIAL);
    m_chkUsb = create_control(m_hwnd, L"Button", L"USB", BS_AUTOCHECKBOX | WS_TABSTOP,
                              IDC_CHK_USB);
    m_chkMsc = create_control(m_hwnd, L"Button", L"MSC", BS_AUTOCHECKBOX | WS_TABSTOP,
                              IDC_CHK_MSC);
    for (HWND h : {m_chkHid, m_chkSerial, m_chkUsb, m_chkMsc})
        ::SendMessageW(h, BM_SETCHECK, BST_CHECKED, 0);   // 默认协议全选
    m_btnRefresh = create_control(m_hwnd, L"Button", L"刷新 (F5)",
                                  BS_PUSHBUTTON | WS_TABSTOP, IDC_REFRESH);
    m_list = create_control(m_hwnd, L"SysListView32", L"",
                            LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER
                                | LVS_NOSORTHEADER,
                            IDC_LIST);
    m_tabs = create_control(m_hwnd, L"SysTabControl32", L"", WS_TABSTOP, IDC_TABS);
    m_status = create_control(m_hwnd, L"msctls_statusbar32", L"", SBARS_SIZEGRIP, IDC_STATUS);

    ListView_SetExtendedListViewStyleEx(m_list, LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT,
                                        LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT);
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    for (int i = 0; i < 5; ++i) {
        col.iSubItem = i;
        col.pszText = const_cast<LPWSTR>(kColTitles[i]);
        col.cx = kColWidths[i];
        ::SendMessageW(m_list, LVM_INSERTCOLUMNW, static_cast<WPARAM>(i),
                       reinterpret_cast<LPARAM>(&col));
    }
}

void ConsoleWindow::make_fonts() {
    if (m_font) ::DeleteObject(m_font);
    m_font = ::CreateFontW(-MulDiv(9, static_cast<int>(m_dpi), 72), 0, 0, 0, FW_NORMAL, FALSE,
                           FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

void ConsoleWindow::apply_fonts() {
    if (!m_font) return;
    HWND ctrls[] = {m_search, m_chkHid, m_chkSerial, m_chkUsb, m_chkMsc, m_btnRefresh,
                    m_list, m_status};
    for (HWND h : ctrls)
        if (h) ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
}

void ConsoleWindow::layout() {
    RECT rc{};
    ::GetClientRect(m_hwnd, &rc);
    const int w = rc.right, h = rc.bottom;
    const int m = S(8), ctlH = S(26);
    const int rowY = m;

    int x = m;
    const int searchW = (w - 2 * m) * 42 / 100;
    ::MoveWindow(m_search, x, rowY + S(3), searchW, ctlH - S(6), TRUE);
    x += searchW + m;
    const int chkW = S(52);
    for (HWND chk : {m_chkHid, m_chkSerial, m_chkUsb, m_chkMsc}) {
        ::MoveWindow(chk, x, rowY, chkW, ctlH, TRUE);
        x += chkW;
    }
    const int btnW = S(110);
    ::MoveWindow(m_btnRefresh, x, rowY, btnW, ctlH, TRUE);

    ::SendMessageW(m_status, WM_SIZE, 0, 0);   // 状态栏自行贴底
    RECT sr{};
    ::GetWindowRect(m_status, &sr);
    const int statusH = sr.bottom - sr.top;

    // 中区：目录表格（S2）；下区：会话标签 + 面板（S3）。无会话时下区留白。
    const int row2Y = rowY + ctlH + m;
    const int below = h - row2Y - statusH - m;
    const int listH = below * 45 / 100 < S(150) ? S(150) : below * 45 / 100;
    ::MoveWindow(m_list, m, row2Y, w - 2 * m, listH, TRUE);

    const int tabsY = row2Y + listH + m;
    const int tabsH = below - listH - m;
    ::MoveWindow(m_tabs, m, tabsY, w - 2 * m, tabsH, TRUE);

    // 面板放进标签页显示区（TCM_ADJUSTRECT wParam=FALSE：窗口矩形 → 显示区）
    RECT dr{};
    ::GetClientRect(m_tabs, &dr);
    ::SendMessageW(m_tabs, TCM_ADJUSTRECT, FALSE, reinterpret_cast<LPARAM>(&dr));
    const int px = m + static_cast<int>(dr.left), py = tabsY + static_cast<int>(dr.top);
    const int pw = static_cast<int>(dr.right - dr.left);
    const int ph = static_cast<int>(dr.bottom - dr.top);
    for (auto& p : m_panes) p->place(px, py, pw, ph);

    // 列宽按窗口 DPI 重应用（创建时的基准宽是 96dpi 口径）
    for (int i = 0; i < 5; ++i)
        ::SendMessageW(m_list, LVM_SETCOLUMNWIDTH, static_cast<WPARAM>(i),
                       static_cast<LPARAM>(S(kColWidths[i])));

    int parts[2] = {w * 2 / 3, -1};
    ::SendMessageW(m_status, SB_SETPARTS, 2, reinterpret_cast<LPARAM>(parts));
    status_refresh();
}

// ---------------------------------------------------------------------------
// 扫描 / 过滤 / 会话
// ---------------------------------------------------------------------------
void ConsoleWindow::do_scan() {
    slog->info("触发设备扫描");
    if (m_status) {
        const wchar_t* s = L"扫描中…";
        ::SendMessageW(m_status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(s));
    }
    HWND hwnd = m_hwnd;
    std::jthread([hwnd]() {
        // 只读枚举（SetupDi + SERIALCOMM 注册表 + PhysicalDrive 属性查询/INQUIRY）
        // 在后台线程完成，目录经堆载 payload 投递回 UI 线程；窗口销毁竞态由
        // PostMessage 失败路径兜底
        auto* payload = new std::vector<ConsoleDevice>(catalog_build::build_catalog(
            DeviceEnumerator::scan(), msc_enum::scan(), serial_enum::scan()));
        if (!::PostMessageW(hwnd, kMsgCatalogDone, 0, reinterpret_cast<LPARAM>(payload)))
            delete payload;
    }).detach();
}

void ConsoleWindow::handle_scan_done(std::vector<ConsoleDevice>* catalog) {
    if (!catalog) return;
    m_catalog = std::move(*catalog);
    slog->info("扫描完成：目录设备 {} 台", m_catalog.size());
    refill();
    wchar_t done[48];
    ::swprintf(done, 48, L"扫描完成 · 双击设备行开会话");
    ::SendMessageW(m_status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(done));
}

unsigned ConsoleWindow::kind_mask() const noexcept {
    unsigned mask = 0;
    if (::SendMessageW(m_chkHid, BM_GETCHECK, 0, 0) == BST_CHECKED)
        mask |= kind_bits(DeviceKind::hid);
    if (::SendMessageW(m_chkSerial, BM_GETCHECK, 0, 0) == BST_CHECKED)
        mask |= kind_bits(DeviceKind::serial);
    if (::SendMessageW(m_chkUsb, BM_GETCHECK, 0, 0) == BST_CHECKED)
        mask |= kind_bits(DeviceKind::usb);
    if (::SendMessageW(m_chkMsc, BM_GETCHECK, 0, 0) == BST_CHECKED)
        mask |= kind_bits(DeviceKind::msc);
    return mask;   // 全不勾 = 0 = 协议全选（device_matches 口径）
}

void ConsoleWindow::refill() {
    wchar_t query[256];
    ::GetWindowTextW(m_search, query, 256);
    m_view = filter_devices(m_catalog, query, kind_mask());
    slog->debug("过滤刷新 关键词\"{}\" 命中 {}/{}", ustlog::w2u(query), m_view.size(),
                m_catalog.size());

    ::SendMessageW(m_list, LVM_DELETEALLITEMS, 0, 0);
    wchar_t num[16];
    for (size_t i = 0; i < m_view.size(); ++i) {
        const ConsoleDevice& d = m_view[i];
        const std::wstring vp = d.vidpid_text();   // 持有临时串，行文本指针须存活至消息返回
        const std::wstring kt = d.kind_text();
        ::swprintf(num, 16, L"%zu", i + 1);
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = static_cast<int>(i);
        it.pszText = num;
        ::SendMessageW(m_list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&it));
        ListView_SetItemText(m_list, static_cast<int>(i), 1,
                             const_cast<LPWSTR>(d.name.c_str()));
        ListView_SetItemText(m_list, static_cast<int>(i), 2, const_cast<LPWSTR>(vp.c_str()));
        ListView_SetItemText(m_list, static_cast<int>(i), 3, const_cast<LPWSTR>(kt.c_str()));
        ListView_SetItemText(m_list, static_cast<int>(i), 4,
                             const_cast<LPWSTR>(d.path.c_str()));
    }
    status_refresh();
}

void ConsoleWindow::on_activate_item() {
    const int i = static_cast<int>(
        ::SendMessageW(m_list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
    if (i < 0 || i >= static_cast<int>(m_view.size())) return;
    const ConsoleDevice& d = m_view[static_cast<size_t>(i)];
    slog->debug("双击选中设备 [{}] {} {}", i, ustlog::w2u(d.name), ustlog::w2u(d.vidpid_text()));

    std::unique_ptr<IChannel> ch = catalog_build::make_channel(d);
    if (!ch) {
        slog->warn("设备 [{}] 不支持开会话", ustlog::w2u(d.name));
        ::MessageBoxW(m_hwnd, L"该设备类型暂不支持开会话。", L"通信控制台",
                      MB_ICONINFORMATION);
        return;
    }
    std::wstring err;
    if (!ch->open(&err)) {
        slog->warn("打开会话失败: {}", ustlog::w2u(err));
        const std::wstring msg_ = L"打开会话失败：" + err;
        ::MessageBoxW(m_hwnd, msg_.c_str(), L"通信控制台", MB_ICONWARNING);
        return;
    }
    const std::wstring display = ch->desc().display;   // open 成功后描述才完整

    // 标签标题：会话: <display>，超长截尾保标签条可读
    std::wstring title = L"会话: " + display;
    if (title.size() > 28) title = title.substr(0, 27) + L"…";

    SessionPane* pane = SessionPane::create(m_hwnd, std::move(ch), std::move(title));
    if (!pane) {
        slog->warn("创建会话面板失败: {}", ustlog::w2u(display));
        ::MessageBoxW(m_hwnd, L"创建会话面板失败。", L"通信控制台", MB_ICONWARNING);
        return;
    }
    m_panes.emplace_back(pane);

    TCITEMW ti{};
    ti.mask = TCIF_TEXT;
    ti.pszText = const_cast<LPWSTR>(pane->title().c_str());
    const int idx = static_cast<int>(m_panes.size()) - 1;
    ::SendMessageW(m_tabs, TCM_INSERTITEMW, static_cast<WPARAM>(idx),
                   reinterpret_cast<LPARAM>(&ti));
    ::SendMessageW(m_tabs, TCM_SETCURSEL, static_cast<WPARAM>(idx), 0);
    on_tab_switch();
    layout();

    slog->info("会话已开: {}", ustlog::w2u(display));
    const std::wstring info = L"会话已开: " + display + L"（右键标签可关闭）";
    ::SendMessageW(m_status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(info.c_str()));
    status_refresh();
}

void ConsoleWindow::on_tab_switch() {
    const int cur = static_cast<int>(::SendMessageW(m_tabs, TCM_GETCURSEL, 0, 0));
    slog->debug("切换会话标签 -> {}", cur);
    for (size_t k = 0; k < m_panes.size(); ++k)
        m_panes[k]->show(static_cast<int>(k) == cur);   // 只显示当前标签的面板
}

void ConsoleWindow::on_tab_rclick() {
    const int cur = static_cast<int>(::SendMessageW(m_tabs, TCM_GETCURSEL, 0, 0));
    if (cur < 0 || cur >= static_cast<int>(m_panes.size())) return;
    HMENU menu = ::CreatePopupMenu();
    ::AppendMenuW(menu, MF_STRING, 1, L"关闭会话");
    POINT pt{};
    ::GetCursorPos(&pt);
    if (::TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, m_hwnd,
                         nullptr) == 1)
        close_session(cur);
    ::DestroyMenu(menu);
}

void ConsoleWindow::close_session(int idx) {
    if (idx < 0 || idx >= static_cast<int>(m_panes.size())) return;
    slog->info("关闭会话 [{}] {}", idx,
               ustlog::w2u(m_panes[static_cast<size_t>(idx)]->title()));
    m_panes.erase(m_panes.begin() + idx);   // ~SessionPane：断回调→销毁窗→close 通道
    ::SendMessageW(m_tabs, TCM_DELETEITEM, static_cast<WPARAM>(idx), 0);
    if (!m_panes.empty()) {
        const int cur = idx < static_cast<int>(m_panes.size()) ? idx
                                                               : static_cast<int>(m_panes.size()) - 1;
        ::SendMessageW(m_tabs, TCM_SETCURSEL, static_cast<WPARAM>(cur), 0);
    }
    on_tab_switch();
    layout();
    status_refresh();
}

void ConsoleWindow::status_refresh() {
    if (!m_status) return;
    wchar_t buf[96];
    ::swprintf(buf, 96, L"设备 %zu/%zu · 会话 %zu", m_view.size(), m_catalog.size(),
               m_panes.size());
    ::SendMessageW(m_status, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(buf));
}
