// console_window.cpp — EP-4 S2 设备发现窗口实现。未真机编译，按 MSDN 口径编写。
#include "ui/console_window.h"

#include "discovery/catalog_build.h"

#include <commctrl.h>

#include <cwchar>
#include <thread>

#pragma comment(lib, "comctl32.lib")

namespace {

constexpr wchar_t kClassName[] = L"USBTestStudio_ConsoleWnd";
constexpr wchar_t kWindowTitle[] = L"USBTestStudio — 工程师通信控制台（设备发现）";

// WM_APP+1：后台扫描完成（LPARAM: new std::vector<ConsoleDevice>，UI 侧 delete）
constexpr UINT kMsgCatalogDone = WM_APP + 1;

// 目录列（96dpi 基准宽；layout 按窗口 DPI 重新应用）
const wchar_t* const kColTitles[] = {L"#", L"名称", L"VID:PID", L"协议", L"端口路径"};
constexpr int kColWidths[] = {40, 210, 90, 52, 460};

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
    return ::RegisterClassExW(&wc) != 0;
}

ConsoleWindow::~ConsoleWindow() {
    // m_sessions 逆序析构即逐个 close（读线程先 join），无需额外收口
    if (m_accel) ::DestroyAcceleratorTable(m_accel);
    if (m_font) ::DeleteObject(m_font);
}

ConsoleWindow* ConsoleWindow::create(HINSTANCE hinst, int nCmdShow) {
    ConsoleWindow* w = new ConsoleWindow();
    w->m_hwnd = ::CreateWindowExW(0, kClassName, kWindowTitle, WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                  nullptr, nullptr, hinst, w);
    if (!w->m_hwnd) {
        delete w;
        return nullptr;
    }
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
            return 0;
        }

        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            if (id == IDC_REFRESH) do_scan();
            else if (id == IDC_SEARCH && code == EN_CHANGE) refill();   // 即时过滤：输入即收敛
            else if ((id == IDC_CHK_HID || id == IDC_CHK_SERIAL || id == IDC_CHK_USB)
                     && code == BN_CLICKED)
                refill();
            return 0;
        }

        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<NMHDR*>(lp);
            if (nm->idFrom == IDC_LIST && nm->code == NM_DBLCLK) on_activate_item();
            return 0;
        }

        case kMsgCatalogDone: {
            std::unique_ptr<std::vector<ConsoleDevice>> ev(
                reinterpret_cast<std::vector<ConsoleDevice>*>(lp));
            handle_scan_done(ev.get());
            return 0;
        }

        case WM_DESTROY:
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
    for (HWND h : {m_chkHid, m_chkSerial, m_chkUsb})
        ::SendMessageW(h, BM_SETCHECK, BST_CHECKED, 0);   // 默认协议全选
    m_btnRefresh = create_control(m_hwnd, L"Button", L"刷新 (F5)",
                                  BS_PUSHBUTTON | WS_TABSTOP, IDC_REFRESH);
    m_list = create_control(m_hwnd, L"SysListView32", L"",
                            LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER
                                | LVS_NOSORTHEADER,
                            IDC_LIST);
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
    HWND ctrls[] = {m_search, m_chkHid, m_chkSerial, m_chkUsb, m_btnRefresh, m_list, m_status};
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
    const int chkW = S(56);
    for (HWND chk : {m_chkHid, m_chkSerial, m_chkUsb}) {
        ::MoveWindow(chk, x, rowY, chkW, ctlH, TRUE);
        x += chkW;
    }
    const int btnW = S(110);
    ::MoveWindow(m_btnRefresh, x, rowY, btnW, ctlH, TRUE);

    ::SendMessageW(m_status, WM_SIZE, 0, 0);   // 状态栏自行贴底
    RECT sr{};
    ::GetWindowRect(m_status, &sr);
    const int statusH = sr.bottom - sr.top;

    const int row2Y = rowY + ctlH + m;
    ::MoveWindow(m_list, m, row2Y, w - 2 * m, h - row2Y - statusH - m, TRUE);

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
    if (m_status) {
        const wchar_t* s = L"扫描中…";
        ::SendMessageW(m_status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(s));
    }
    HWND hwnd = m_hwnd;
    std::jthread([hwnd]() {
        // 只读枚举（SetupDi + SERIALCOMM 注册表）在后台线程完成，目录经堆载
        // payload 投递回 UI 线程；窗口销毁竞态由 PostMessage 失败路径兜底
        auto* payload = new std::vector<ConsoleDevice>(catalog_build::build_catalog(
            DeviceEnumerator::scan(), serial_enum::scan()));
        if (!::PostMessageW(hwnd, kMsgCatalogDone, 0, reinterpret_cast<LPARAM>(payload)))
            delete payload;
    }).detach();
}

void ConsoleWindow::handle_scan_done(std::vector<ConsoleDevice>* catalog) {
    if (!catalog) return;
    m_catalog = std::move(*catalog);
    refill();
    wchar_t done[48];
    ::swprintf(done, 48, L"扫描完成 · 双击设备行开会话（收发台见 S3）");
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
    return mask;   // 全不勾 = 0 = 协议全选（device_matches 口径）
}

void ConsoleWindow::refill() {
    wchar_t query[256];
    ::GetWindowTextW(m_search, query, 256);
    m_view = filter_devices(m_catalog, query, kind_mask());

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

    std::unique_ptr<IChannel> ch = catalog_build::make_channel(d);
    if (!ch) {
        ::MessageBoxW(m_hwnd, L"通用 USB 接口：WinUSB / MSC 通道在 S5 切片提供。",
                      L"通信控制台", MB_ICONINFORMATION);
        return;
    }
    std::wstring err;
    if (!ch->open(&err)) {
        const std::wstring msg_ = L"打开会话失败：" + err;
        ::MessageBoxW(m_hwnd, msg_.c_str(), L"通信控制台", MB_ICONWARNING);
        return;
    }
    const std::wstring display = ch->desc().display;   // open 成功后描述才完整
    m_sessions.push_back(std::move(ch));
    const std::wstring info = L"会话已开: " + display;
    ::SendMessageW(m_status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(info.c_str()));
    status_refresh();
}

void ConsoleWindow::status_refresh() {
    if (!m_status) return;
    wchar_t buf[96];
    ::swprintf(buf, 96, L"设备 %zu/%zu · 会话 %zu", m_view.size(), m_catalog.size(),
               m_sessions.size());
    ::SendMessageW(m_status, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(buf));
}
