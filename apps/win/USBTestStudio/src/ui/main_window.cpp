// main_window.cpp — 主窗口实现。未真机编译，按 MSDN 口径编写。
#include "ui/main_window.h"

#include <commctrl.h>
#include <commdlg.h>

#include <cwchar>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")

namespace {

constexpr wchar_t kClassName[] = L"USBTestStudio_MainWnd";
constexpr wchar_t kWindowTitle[] = L"USBTestStudio — USB 产线测试上位机";

// WM_APP+4：后台扫描完成（LPARAM: new std::vector<DeviceInfo>，UI 侧 delete）
constexpr UINT kMsgScanDone = WM_APP + 4;

constexpr int kLogChildId = 1234;

HWND create_control(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style,
                    int child_id) {
    return ::CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(child_id)),
                             ::GetModuleHandleW(nullptr), nullptr);
}

void listview_add_column(HWND lv, int index, const wchar_t* title, int width_px96) {
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.iSubItem = index;
    col.pszText = const_cast<LPWSTR>(title);
    col.cx = width_px96;
    ::SendMessageW(lv, LVM_INSERTCOLUMNW, static_cast<WPARAM>(index),
                   reinterpret_cast<LPARAM>(&col));
}

} // namespace

// ---------------------------------------------------------------------------
// 注册 / 创建
// ---------------------------------------------------------------------------
bool MainWindow::register_class(HINSTANCE hinst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MainWindow::wnd_proc;
    wc.hInstance = hinst;
    wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = ::LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kClassName;
    return ::RegisterClassExW(&wc) != 0;
}

MainWindow::MainWindow(const std::wstring& plan_path, const std::wstring& dut_sn,
                       const std::wstring& station, bool auto_exit)
    : m_planPath(plan_path), m_dutSn(dut_sn), m_station(station), m_autoExit(auto_exit) {}

MainWindow::~MainWindow() {
    if (m_engine) m_engine->stop_and_join();   // 先停引擎线程（成员逆序析构前显式收口）
    if (m_accel) ::DestroyAcceleratorTable(m_accel);
    if (m_font) ::DeleteObject(m_font);
}

MainWindow* MainWindow::create(HINSTANCE hinst, int nCmdShow, const std::wstring& plan_path,
                               const std::wstring& dut_sn, const std::wstring& station,
                               bool auto_exit) {
    MainWindow* w = new MainWindow(plan_path, dut_sn, station, auto_exit);
    w->m_hwnd = ::CreateWindowExW(0, kClassName, kWindowTitle, WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                  nullptr, nullptr, hinst, w);
    if (!w->m_hwnd) {
        delete w;
        return nullptr;
    }
    // 初始尺寸按窗口 DPI 缩放并居中
    w->m_dpi = ::GetDpiForWindow(w->m_hwnd);
    int cw = w->S(1180), ch = w->S(720);
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

LRESULT CALLBACK MainWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* w = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w));
        w->m_hwnd = hwnd;   // WM_CREATE 之前先回填句柄，on_create 依赖它
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* self = reinterpret_cast<MainWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return ::DefWindowProcW(hwnd, msg, wp, lp);
    return self->on_message(msg, wp, lp);
}

// ---------------------------------------------------------------------------
// 控件创建 / 字体 / 布局
// ---------------------------------------------------------------------------
void MainWindow::on_create() {
    m_dpi = ::GetDpiForWindow(m_hwnd);
    create_controls();
    make_fonts();
    apply_fonts();

    // 加速键：F5 扫描 / Ctrl+R 运行 / Ctrl+S 导出
    ACCEL acc[3] = {
        {FVIRTKEY, VK_F5, IDC_SCAN},
        {static_cast<BYTE>(FVIRTKEY | FCONTROL), 'R', IDC_RUN},
        {static_cast<BYTE>(FVIRTKEY | FCONTROL), 'S', IDC_EXPORT},
    };
    m_accel = ::CreateAcceleratorTableW(acc, 3);

    // 引擎与事件泵（窗口存在后创建，泵需要 HWND）
    m_pump = std::make_unique<MessagePumpEvents>(m_hwnd);
    m_engine = std::make_unique<TestEngine>(*m_pump);

    status_set(0, L"就绪");
    status_set(1, m_station.empty() ? L"工位 -" : (L"工位 " + m_station));
    status_set(2, L"丢弃日志 0");
    status_set(3, L"");
    status_refresh();

    if (m_dutSn.empty()) m_dutSn = L"AUTO";
    try_load_plan(true);
    do_scan();   // 启动即扫一次
}

void MainWindow::create_controls() {
    m_combo = create_control(m_hwnd, L"ComboBox", L"",
                             CBS_DROPDOWNLIST | CBS_SORT | WS_VSCROLL | WS_TABSTOP, IDC_COMBO);
    m_btnScan = create_control(m_hwnd, L"Button", L"扫描 (F5)",
                               BS_PUSHBUTTON | WS_TABSTOP, IDC_SCAN);
    m_btnRun = create_control(m_hwnd, L"Button", L"运行 (Ctrl+R)",
                              BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_RUN);
    m_btnExport = create_control(m_hwnd, L"Button", L"导出报告 (Ctrl+S)",
                                 BS_PUSHBUTTON | WS_TABSTOP, IDC_EXPORT);
    m_lvDev = create_control(m_hwnd, L"SysListView32", L"",
                             LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER,
                             IDC_LVDEV);
    m_lvStep = create_control(m_hwnd, L"SysListView32", L"",
                              LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER,
                              IDC_LVSTEP);
    m_prog = create_control(m_hwnd, L"msctls_progress32", L"", PBS_SMOOTH, IDC_PROG);
    ::SendMessageW(m_prog, PBM_SETRANGE32, 0, 100);
    ::SendMessageW(m_prog, PBM_SETSTEP, 1, 0);
    m_status = create_control(m_hwnd, L"msctls_statusbar32", L"", SBARS_SIZEGRIP, IDC_STATUS);

    ListView_SetExtendedListViewStyleEx(m_lvDev, LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT,
                                        LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT);
    ListView_SetExtendedListViewStyleEx(m_lvStep, LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT,
                                        LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT);

    // 列（宽度为 96dpi 基准，font/布局阶段统一按 DPI 缩放列宽过小的问题，这里先给基准值）
    listview_add_column(m_lvDev, 0, L"类型", 52);
    listview_add_column(m_lvDev, 1, L"VID:PID", 100);
    listview_add_column(m_lvDev, 2, L"名称", 170);
    listview_add_column(m_lvDev, 3, L"实例路径", 190);
    listview_add_column(m_lvDev, 4, L"设备路径", 420);

    listview_add_column(m_lvStep, 0, L"#", 40);
    listview_add_column(m_lvStep, 1, L"类型", 130);
    listview_add_column(m_lvStep, 2, L"名称", 130);
    listview_add_column(m_lvStep, 3, L"结果", 56);
    listview_add_column(m_lvStep, 4, L"指标 / 说明", 430);

    if (!m_log.create(m_hwnd, ::GetModuleHandleW(nullptr), kLogChildId, m_dpi))
        ::MessageBoxW(m_hwnd, L"RichEdit（Msftedit.dll）初始化失败", L"USBTestStudio",
                      MB_ICONERROR);
}

void MainWindow::make_fonts() {
    if (m_font) ::DeleteObject(m_font);
    m_font = ::CreateFontW(-MulDiv(9, static_cast<int>(m_dpi), 72), 0, 0, 0, FW_NORMAL, FALSE,
                           FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

void MainWindow::apply_fonts() {
    if (!m_font) return;
    HWND ctrls[] = {m_combo, m_btnScan, m_btnRun, m_btnExport, m_lvDev, m_lvStep, m_prog, m_status};
    for (HWND h : ctrls)
        if (h) ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
    m_log.apply_dpi(m_dpi);
}

void MainWindow::layout() {
    RECT rc{};
    ::GetClientRect(m_hwnd, &rc);
    const int w = rc.right, h = rc.bottom;
    const int m = S(8), ctlH = S(26);
    const int rowY = m;

    const int comboW = (w - 2 * m) * 4 / 10;
    const int btnW = S(130);
    int x = m;
    ::MoveWindow(m_combo, x, rowY + S(3), comboW, ctlH, TRUE);
    x += comboW + m;
    ::MoveWindow(m_btnScan, x, rowY, btnW, ctlH, TRUE);
    x += btnW + m;
    ::MoveWindow(m_btnRun, x, rowY, btnW, ctlH, TRUE);
    x += btnW + m;
    ::MoveWindow(m_btnExport, x, rowY, btnW, ctlH, TRUE);
    x += btnW + m;
    ::MoveWindow(m_prog, x, rowY + (ctlH - S(16)) / 2, w - x - m, S(16), TRUE);

    const int row2Y = rowY + ctlH + m;
    int remainH = h - row2Y - m;
    ::SendMessageW(m_status, WM_SIZE, 0, 0);   // 状态栏自行贴底
    RECT sr{};
    ::GetWindowRect(m_status, &sr);
    const int statusH = sr.bottom - sr.top;
    remainH -= statusH;

    const int listH = remainH * 42 / 100;
    const int halfW = (w - 3 * m) / 2;
    ::MoveWindow(m_lvDev, m, row2Y, halfW, listH, TRUE);
    ::MoveWindow(m_lvStep, m * 2 + halfW, row2Y, w - 3 * m - halfW, listH, TRUE);

    const int logY = row2Y + listH + m;
    ::MoveWindow(m_log.hwnd(), m, logY, w - 2 * m, h - logY - statusH - m, TRUE);

    // 状态栏 4 分栏（3 个右边界）；重设分栏后需重发缓存文本（SB_SETPARTS 会重绘清空显示）
    int parts[3] = {(int)(w * 0.42), (int)(w * 0.62), (int)(w * 0.82)};
    ::SendMessageW(m_status, SB_SETPARTS, 3, reinterpret_cast<LPARAM>(parts));
    for (int i = 0; i < 4; ++i)
        ::SendMessageW(m_status, SB_SETTEXTW, static_cast<WPARAM>(i),
                       reinterpret_cast<LPARAM>(m_statusTexts[i].c_str()));
}

// ---------------------------------------------------------------------------
// 消息处理
// ---------------------------------------------------------------------------
LRESULT MainWindow::on_message(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            on_create();
            return 0;

        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) layout();
            return 0;

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = S(760);
            mmi->ptMinTrackSize.y = S(480);
            return 0;
        }

        case WM_DPICHANGED: {
            m_dpi = HIWORD(wp);
            const RECT* sug = reinterpret_cast<const RECT*>(lp);
            ::SetWindowPos(m_hwnd, nullptr, sug->left, sug->top, sug->right - sug->left,
                           sug->bottom - sug->top,
                           SWP_NOZORDER | SWP_NOACTIVATE);
            make_fonts();
            apply_fonts();
            layout();
            return 0;
        }

        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == IDC_SCAN) do_scan();
            else if (id == IDC_RUN) do_run();
            else if (id == IDC_EXPORT) do_export();
            else if (id == IDC_COMBO && HIWORD(wp) == CBN_SELCHANGE) {
                int sel = static_cast<int>(::SendMessageW(m_combo, CB_GETCURSEL, 0, 0));
                if (sel >= 0) {
                    int di = static_cast<int>(::SendMessageW(m_combo, CB_GETITEMDATA, sel, 0));
                    if (di >= 0 && di < static_cast<int>(m_devices.size()))
                        status_set(0, m_devices[static_cast<size_t>(di)].path);
                }
            }
            return 0;
        }

        case WM_APP + 1: handle_engine_log(); return 0;
        case WM_APP + 2: {
            std::unique_ptr<StepEvent> ev(reinterpret_cast<StepEvent*>(lp));
            handle_engine_step(ev.get());
            return 0;
        }
        case WM_APP + 3: {
            std::unique_ptr<DoneEvent> ev(reinterpret_cast<DoneEvent*>(lp));
            handle_engine_done(ev.get());
            return 0;
        }
        case kMsgScanDone: {
            std::unique_ptr<std::vector<DeviceInfo>> ev(
                reinterpret_cast<std::vector<DeviceInfo>*>(lp));
            handle_scan_done(ev.get());
            return 0;
        }

        case WM_DESTROY:
            if (m_engine && m_engine->running()) {
                m_engine->request_stop();
                m_lastExitCode = static_cast<int>(ExitCode::Aborted);
            }
            ::PostQuitMessage(m_lastExitCode);
            return 0;

        default:
            return ::DefWindowProcW(m_hwnd, msg, wp, lp);
    }
}

int MainWindow::run() {
    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!pre_translate(msg)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}

bool MainWindow::pre_translate(MSG& msg) {
    return m_accel && ::TranslateAcceleratorW(m_hwnd, m_accel, &msg) != 0;
}

// ---------------------------------------------------------------------------
// 计划加载
// ---------------------------------------------------------------------------
bool MainWindow::try_load_plan(bool announce) {
    if (!m_engine) return false;
    if (m_engine->has_plan()) return true;
    std::wstring path = m_planPath;
    if (path.empty()) return false;
    std::wstring err;
    if (m_engine->load_plan(path, &err)) {
        if (announce) {
            m_log.append(wraii::LogLevel::Info, L"计划已加载: " + path);
            const TestPlan& p = m_engine->plan();
            m_station = p.station;
            m_dutSn = p.dut_sn;
            m_log.append(wraii::LogLevel::Info,
                         wraii::fmt_v(L"计划[%s] 工位[%s] DUT[%s]，共 %u 步", p.name.c_str(),
                                      p.station.c_str(), p.dut_sn.c_str(),
                                      static_cast<unsigned>(p.steps.size())));
        }
        status_set(1, L"工位 " + m_station + L" · " + m_engine->plan().name);
        return true;
    }
    m_log.append(wraii::LogLevel::Error, L"计划加载失败: " + err);
    if (m_autoExit) ::PostQuitMessage(static_cast<int>(ExitCode::PlanError));
    return false;
}

// ---------------------------------------------------------------------------
// 动作
// ---------------------------------------------------------------------------
void MainWindow::do_scan() {
    m_log.append(wraii::LogLevel::Info, L"扫描 USB/HID 设备…");
    HWND hwnd = m_hwnd;
    std::jthread([hwnd]() {
        std::vector<DeviceInfo> devices = DeviceEnumerator::scan();
        auto* payload = new std::vector<DeviceInfo>(std::move(devices));
        if (!::PostMessageW(hwnd, kMsgScanDone, 0, reinterpret_cast<LPARAM>(payload)))
            delete payload;   // 窗口已销毁：就地释放
    }).detach();
    // detach 说明：扫描线程仅做 SetupDi/CM 只读枚举 + PostMessage，不触碰窗口句柄以外资源；
    // 窗口销毁竞态下的 payload 已在 PostMessage 失败路径释放（PostMessage 成功但未处理的情况
    // 理论上会造成一次小分配泄漏，见 README 已知限制）。
}

void MainWindow::handle_scan_done(std::vector<DeviceInfo>* devices) {
    if (!devices) return;
    m_devices = *devices;
    fill_device_views();
    m_log.append(wraii::LogLevel::Info,
                 wraii::fmt_v(L"扫描完成：%u 台设备（USB + HID）",
                              static_cast<unsigned>(m_devices.size())));
    status_refresh();
}

void MainWindow::fill_device_views() {
    ::SendMessageW(m_combo, CB_RESETCONTENT, 0, 0);
    ::SendMessageW(m_lvDev, LVM_DELETEALLITEMS, 0, 0);
    for (size_t i = 0; i < m_devices.size(); ++i) {
        const DeviceInfo& d = m_devices[i];

        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<LPWSTR>(d.class_name.c_str());
        ::SendMessageW(m_lvDev, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
        wchar_t vidpid[32];
        ::swprintf(vidpid, 32, L"%04X:%04X", d.vid, d.pid);
        ListView_SetItemText(m_lvDev, static_cast<int>(i), 1, vidpid);
        ListView_SetItemText(m_lvDev, static_cast<int>(i), 2,
                             const_cast<LPWSTR>(d.name.c_str()));
        ListView_SetItemText(m_lvDev, static_cast<int>(i), 3,
                             const_cast<LPWSTR>(d.instance_path.c_str()));
        ListView_SetItemText(m_lvDev, static_cast<int>(i), 4, const_cast<LPWSTR>(d.path.c_str()));

        int idx = static_cast<int>(::SendMessageW(
            m_combo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(DeviceEnumerator::summary(d).c_str())));
        if (idx >= 0)
            ::SendMessageW(m_combo, CB_SETITEMDATA, idx, static_cast<LPARAM>(i));
    }
    if (!m_devices.empty()) ::SendMessageW(m_combo, CB_SETCURSEL, 0, 0);
}

void MainWindow::fill_steps_table(const std::wstring& pending_text) {
    ::SendMessageW(m_lvStep, LVM_DELETEALLITEMS, 0, 0);
    if (!m_engine || !m_engine->has_plan()) return;
    const std::vector<PlanStep>& steps = m_engine->plan().steps;
    for (size_t i = 0; i < steps.size(); ++i) {
        wchar_t num[16];
        ::swprintf(num, 16, L"%zu", i + 1);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = num;
        ::SendMessageW(m_lvStep, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
        ListView_SetItemText(m_lvStep, static_cast<int>(i), 1,
                             const_cast<LPWSTR>(steps[i].type.c_str()));
        ListView_SetItemText(m_lvStep, static_cast<int>(i), 2,
                             const_cast<LPWSTR>(steps[i].name.c_str()));
        ListView_SetItemText(m_lvStep, static_cast<int>(i), 3,
                             const_cast<LPWSTR>(pending_text.c_str()));
        ListView_SetItemText(m_lvStep, static_cast<int>(i), 4, const_cast<LPWSTR>(L""));
    }
}

void MainWindow::do_run() {
    if (m_running.load()) return;
    if (!try_load_plan(true)) {
        ::MessageBoxW(m_hwnd,
                      (L"未找到测试计划：\n" + m_planPath +
                       L"\n\n请将 plan.json 放在 exe 同目录，或用 --plan <路径> 指定。"),
                      L"USBTestStudio", MB_ICONWARNING);
        return;
    }
    fill_steps_table(L"…");
    ::SendMessageW(m_prog, PBM_SETPOS, 0, 0);
    set_running_ui(true);
    status_set(0, L"测试运行中…");
    m_engine->start();
}

void MainWindow::do_export() {
    if (!m_engine || !m_engine->has_plan()) {
        ::MessageBoxW(m_hwnd, L"尚未加载测试计划，无报告可导出。", L"USBTestStudio",
                      MB_ICONINFORMATION);
        return;
    }
    wchar_t file[MAX_PATH] = L"";
    std::wstring def = m_engine->suggest_report_name();
    ::wcsncat_s(file, MAX_PATH, def.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFilter = L"JSON 报告 (*.json)\0*.json\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (!::GetSaveFileNameW(&ofn)) return;   // 用户取消（CommDlgExtendedError 可查，忽略）

    std::wstring err;
    if (m_engine->write_report_to(file, &err)) {
        m_log.append(wraii::LogLevel::Info, L"报告已导出: " + std::wstring(file));
        status_set(0, L"报告已导出");
    } else {
        m_log.append(wraii::LogLevel::Error, L"导出失败: " + err);
    }
}

void MainWindow::set_running_ui(bool running) {
    m_running.store(running);
    ::EnableWindow(m_btnRun, !running);
    ::EnableWindow(m_btnExport, !running);
}

// ---------------------------------------------------------------------------
// 引擎事件处理（WM_APP+1..3）
// ---------------------------------------------------------------------------
void MainWindow::handle_engine_log() {
    unsigned long long dropped = 0;
    bool more = true;
    while (more) {
        more = m_pump->drain_logs(
            [this](wraii::LogLevel lv, const std::wstring& t) { m_log.append(lv, t); }, &dropped);
        if (dropped != 0) {
            m_droppedTotal += dropped;
            dropped = 0;
            status_refresh();
        }
    }
}

void MainWindow::handle_engine_step(StepEvent* ev) {
    if (!ev) return;
    int i = ev->index;
    wchar_t result[8];
    ::swprintf(result, 8, L"%s", ev->result.pass ? L"PASS" : L"FAIL");
    ListView_SetItemText(m_lvStep, i, 3, result);

    std::wstring metrics;
    for (const auto& kv : ev->result.measured) {
        wchar_t buf[64];
        switch (kv.second.kind) {
            case MeasValue::Kind::Int:
                ::swprintf(buf, 64, L"%s=%lld", kv.first.c_str(), kv.second.i);
                break;
            case MeasValue::Kind::Dbl:
                ::swprintf(buf, 64, L"%s=%.6g", kv.first.c_str(), kv.second.d);
                break;
            default:
                metrics += kv.first + L"=" + kv.second.s + L" ";
                continue;
        }
        metrics += std::wstring(buf) + L" ";
    }
    std::wstring detail = metrics + ev->result.note;
    ListView_SetItemText(m_lvStep, i, 4, const_cast<LPWSTR>(detail.c_str()));

    ::SendMessageW(m_prog, PBM_SETPOS, ev->total > 0 ? (i + 1) * 100 / ev->total : 100, 0);
    status_set(0, wraii::fmt_v(L"步骤 %d/%d %s", i + 1, ev->total,
                               ev->result.pass ? L"PASS" : L"FAIL"));
}

void MainWindow::handle_engine_done(DoneEvent* ev) {
    if (!ev) return;
    set_running_ui(false);
    m_lastExitCode = ev->exit_code;

    // 收尾：冲干净残余日志再显示结论（保证日志顺序）
    unsigned long long dropped = 0;
    while (m_pump->drain_logs(
        [this](wraii::LogLevel lv, const std::wstring& t) { m_log.append(lv, t); }, &dropped)) {
        if (dropped != 0) {
            m_droppedTotal += dropped;
            dropped = 0;
        }
    }
    m_droppedTotal += dropped;

    wchar_t verdict[16];
    ::swprintf(verdict, 16, L"%s（退出码 %d）", ev->all_pass ? L"PASS" : L"FAIL", ev->exit_code);
    status_set(3, verdict);
    status_set(0, L"完成");
    status_refresh();
    ::SendMessageW(m_prog, PBM_SETPOS, 100, 0);
    if (m_autoExit) ::PostQuitMessage(ev->exit_code);
}

// ---------------------------------------------------------------------------
// 状态栏
// ---------------------------------------------------------------------------
void MainWindow::status_set(int pane, const std::wstring& text) {
    if (pane < 0 || pane > 3) return;
    m_statusTexts[pane] = text;   // 缓存：SB_SETPARTS / DPI 变化后统一重发
    if (m_status)
        ::SendMessageW(m_status, SB_SETTEXTW, static_cast<WPARAM>(pane),
                       reinterpret_cast<LPARAM>(text.c_str()));
}

void MainWindow::status_refresh() {
    status_set(2, wraii::fmt_v(L"设备 %u · 丢弃日志 %llu",
                               static_cast<unsigned>(m_devices.size()), m_droppedTotal));
}
