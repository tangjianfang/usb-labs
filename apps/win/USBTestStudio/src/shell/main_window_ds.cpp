// main_window_ds.cpp — DevStudio 主窗口壳实现（设计 §0.1/§0.2 / 计划 MS0-T10）
//
// MS0 契约：
//   1. 菜单=menu_table() 数据驱动生成，命令注册进 CommandRegistry（含 handler 桩）；
//   2. 工具栏=自绘四段视角钮 + 运行/停止/命令面板（owner-draw，色值走 tokens）；
//   3. 三栏布局=左(工程树+设备目录桩)/中(标签区)/右(上下文)；WM_SIZE 重排；
//   4. 面板桩=按视角默认面板列表生成静态文本占位（注册表装配，MS1+ 替换真面板）；
//   5. 状态栏=六格自绘文本（menu_table 同源的 status_cells_def）。
#include "main_window_ds.h"

#include "../app/log.h"
#include "../framework/win32_rai.h"
#include "../ui/tokens.h"
#include "command_registry.h"
#include "panel_registry.h"

#include <cwchar>
#include <windowsx.h>

namespace usts::shell {

namespace {

constexpr wchar_t kWndClass[] = L"USBDevStudioMainWnd";
constexpr int kToolbarH = 36;       // 工具栏高（设计 §0.1）
constexpr int kMenubarH = 20;       // 菜单栏（系统）估高，布局用 GetMenuBarInfo 校正省略——MS0 取常量
constexpr int kStatusH = 24;        // 状态栏

// 四视角钮区（工具栏左段）：x 命中 → 视角序
constexpr int kPerspBtnW = 56;
constexpr int kPerspBtnH = 24;
constexpr int kPerspBtnY = 6;

struct WndCtx {
    MainWindowDS* self = nullptr;
};
WndCtx* ctx_of(HWND hwnd) { return reinterpret_cast<WndCtx*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)); }

} // namespace

// ---------------------------------------------------------------------------
// 数据表（自测强一致锚点）
// ---------------------------------------------------------------------------
const std::vector<MenuDef>& menu_table() {
    static const std::vector<MenuDef> t = {
        {L"文件", L"新建工程(从模板…)", "", "file.new_project"},
        {L"文件", L"打开工程…", "Ctrl+O", "file.open_project"},
        {L"文件", L"最近工程", "", "file.recent"},
        {L"文件", L"关闭工程", "", "file.close_project"},
        {L"文件", L"-", "", ""},
        {L"文件", L"导入 pcapng…", "", "file.import_pcapng"},
        {L"文件", L"退出", "Alt+F4", "app.quit"},
        {L"编辑", L"撤销", "Ctrl+Z", "edit.undo"},
        {L"编辑", L"重做", "Ctrl+Y", "edit.redo"},
        {L"编辑", L"查找", "Ctrl+F", "edit.find"},
        {L"视图", L"开发视角", "Ctrl+Alt+1", "view.perspective.dev"},
        {L"视图", L"调试视角", "Ctrl+Alt+2", "view.perspective.debug"},
        {L"视图", L"测试视角", "Ctrl+Alt+3", "view.perspective.test"},
        {L"视图", L"产线视角", "Ctrl+Alt+4", "view.perspective.prod"},
        {L"视图", L"-", "", ""},
        {L"视图", L"刷新设备", "F5", "view.refresh_devices"},
        {L"视图", L"重置布局", "", "view.reset_layout"},
        {L"运行", L"运行计划", "Ctrl+R", "run.plan"},
        {L"运行", L"停止", "Esc", "run.stop"},
        {L"运行", L"试跑(mock)", "", "run.mock"},
        {L"工具", L"命令面板", "Ctrl+K", "tools.command_palette"},
        {L"工具", L"设置", "Ctrl+,", "tools.settings"},
        {L"工具", L"脚本控制台", "", "tools.script_console"},
        {L"帮助", L"用户手册", "F1", "help.manual"},
        {L"帮助", L"关于 USB DevStudio", "", "help.about"},
    };
    return t;
}

const std::vector<std::pair<const char*, const wchar_t*>>& status_cells_def() {
    static const std::vector<std::pair<const char*, const wchar_t*>> t = {
        {"status.project", L"工程: 未打开"},   // 设计 §0.1：工程名+脏标记
        {"status.devices", L"设备 0 台"},
        {"status.engine",  L"就绪"},
        {"status.notify",  L"⚠ 0"},
        {"status.log",     L"日志 INFO"},
        {"status.clock",   L"--:--:--"},
    };
    return t;
}

// ---------------------------------------------------------------------------
// 命令注册（幂等；MS0 handler 桩=日志留痕，MS1+ 逐个接真实现）
// ---------------------------------------------------------------------------
void MainWindowDS::register_commands() {
    auto& cr = CommandRegistry::instance();
    auto log = ustlog::logger("ui.shell");
    for (const auto& m : menu_table()) {
        if (!m.cmd_id || !*m.cmd_id) continue;
        const std::string id = m.cmd_id;
        if (!cr.has_handler(id)) {
            cr.add({id, m.item, m.shortcut, wraii::wide_to_utf8(m.menu)});
            cr.set_handler(id, [id, log] { log->debug("命令（MS0 桩）: {}", id); });
        }
    }
    log->debug("register_commands: 菜单命令注册完成（{} 项）", menu_table().size());
}

// ---------------------------------------------------------------------------
// 窗口创建/消息
// ---------------------------------------------------------------------------
bool MainWindowDS::create(HINSTANCE inst, const Settings& cfg, const LayoutState& layout) {
    auto log = ustlog::logger("ui.shell");
    (void)cfg;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MainWindowDS::wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    wc.lpszClassName = kWndClass;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        log->error("RegisterClassExW 失败 GLE=0x{:08X}", GetLastError());
        return false;
    }

    m_perspective = static_cast<int>(layout.current);
    m_font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, ui::tokens::kFontUi);

    RECT rc = {0, 0, layout.win_w, layout.win_h};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, TRUE);
    m_hwnd = CreateWindowExW(0, kWndClass, L"USB DevStudio",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top,
                             nullptr, nullptr, inst, this);
    if (!m_hwnd) {
        log->error("CreateWindowExW 失败 GLE=0x{:08X}", GetLastError());
        return false;
    }
    build_menu();
    layout_children();
    apply_perspective();
    log->info("DevStudio 主窗口已创建（perspective={}）", m_perspective);
    return true;
}

void MainWindowDS::build_menu() {
    auto log = ustlog::logger("ui.shell");
    HMENU bar = CreateMenu();
    HMENU cur = nullptr;
    const wchar_t* cur_name = nullptr;
    for (const auto& m : menu_table()) {
        if (!cur_name || wcscmp(m.menu, cur_name) != 0) {
            cur = CreatePopupMenu();
            AppendMenuW(bar, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(cur), m.menu);
            cur_name = m.menu;
        }
        if (wcscmp(m.item, L"-") == 0) {
            AppendMenuW(cur, MF_SEPARATOR, 0, nullptr);
        } else {
            // cmd_id 数字化：菜单命令 ID = 注册序 + 0x100（WM_COMMAND 派发后查表）
            UINT id = 0x100 + static_cast<UINT>(&m - menu_table().data());
            AppendMenuW(cur, MF_STRING, id, m.item);
        }
    }
    SetMenu(m_hwnd, bar);
    log->debug("build_menu: 顶层 {} 项", GetMenuItemCount(bar));
}

void MainWindowDS::layout_children() {
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    const int w = rc.right, h = rc.bottom;
    const int top = kMenubarH + kToolbarH;
    const int status_y = h - kStatusH;

    // 三栏容器（MS0：子面板桩在容器内自摆）
    auto ensure = [this](HWND& h, DWORD style) {
        if (!h) {
            h = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
                                WS_CHILD | style, 0, 0, 0, 0, m_hwnd,
                                nullptr, reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(m_hwnd, GWLP_HINSTANCE)), nullptr);
            if (h) SetWindowFont(h, m_font, FALSE);
        }
    };
    ensure(m_hwnd_left, 0);
    ensure(m_hwnd_central, SS_CENTER);
    ensure(m_hwnd_right, 0);
    if (!m_hwnd_status) {
        m_hwnd_status = CreateWindowExW(0, L"STATIC",
                                        L"就绪 · 设备 0 · 引擎空闲 · ⚠0 · 日志 INFO · USB DevStudio v0.10",
                                        WS_CHILD | SS_CENTERIMAGE, 0, 0, 0, 0,
                                        m_hwnd, nullptr,
                                        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(m_hwnd, GWLP_HINSTANCE)), nullptr);
        if (m_hwnd_status) SetWindowFont(m_hwnd_status, m_font, FALSE);
    }

    // 左 260 / 右 300（LayoutState 真值在 T11 接 splitter，MS0 取默认）
    const int left_w = 260, right_w = 300;
    int x = 0;
    MoveWindow(m_hwnd_left, x, top, left_w, status_y - top, TRUE);
    x += left_w;
    const int central_w = (w > left_w + right_w) ? (w - left_w - right_w) : 0;
    MoveWindow(m_hwnd_central, x, top, central_w, status_y - top, TRUE);
    x += central_w;
    MoveWindow(m_hwnd_right, x, top, (w > x) ? (w - x) : 0, status_y - top, TRUE);
    MoveWindow(m_hwnd_status, 0, status_y, w, kStatusH, TRUE);
    InvalidateRect(m_hwnd, nullptr, TRUE);
}

void MainWindowDS::apply_perspective() {
    auto log = ustlog::logger("ui.shell");
    auto& reg = PanelRegistry::instance();
    register_builtin_panels(reg);
    const PerspectiveDef* def = find_perspective(static_cast<PerspectiveId>(m_perspective));
    if (!def) { log->warn("apply_perspective: 序值非法 {}", m_perspective); return; }
    std::wstring text = std::wstring(def->name) + L"视角面板：";
    bool first = true;
    for (const auto& p : reg.for_perspective(def->default_panels)) {
        if (!first) text += L" · ";
        text += p.title;
        first = false;
    }
    SetWindowTextW(m_hwnd_central, text.c_str());
    // 左栏桩=工程树+设备目录两行
    SetWindowTextW(m_hwnd_left, L"工程树\n设备目录");
    InvalidateRect(m_hwnd, nullptr, TRUE);
    log->debug("apply_perspective: {} → {} 面板", wraii::wide_to_utf8(def->name),
               def->default_panels.size());
}

void MainWindowDS::switch_perspective(int idx) {
    auto log = ustlog::logger("ui.shell");
    if (idx < 0 || idx > 3) { log->warn("switch_perspective: 越界 {}", idx); return; }
    if (idx == m_perspective) return;
    m_perspective = idx;
    apply_perspective();
    // 菜单勾选态同步（MS0：四个视角项互斥勾选）
    HMENU bar = GetMenu(m_hwnd);
    if (bar) {
        const wchar_t* names[] = {L"开发视角", L"调试视角", L"测试视角", L"产线视角"};
        UINT base = 0x100;
        for (int i = 0; i < 4; ++i) {
            // 菜单 ID 由 menu_table 行序决定：视角项行序 10..13
            CheckMenuItem(bar, base + 10 + i, MF_BYCOMMAND |
                              (i == idx ? MF_CHECKED : MF_UNCHECKED));
            (void)names[i];
        }
    }
}

void MainWindowDS::show(int nCmdShow) {
    ShowWindow(m_hwnd, nCmdShow);
    UpdateWindow(m_hwnd);
}

bool MainWindowDS::pump_quit() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

void MainWindowDS::request_quit() { PostMessageW(m_hwnd, WM_CLOSE, 0, 0); }

LRESULT CALLBACK MainWindowDS::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        const auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<MainWindowDS*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        self->m_hwnd = hwnd;   // 创建期消息即有效（handle 内 DefWindowProcW 需要）
        return self->handle(msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT MainWindowDS::handle(UINT msg, WPARAM wp, LPARAM lp) {
    auto log = ustlog::logger("ui.shell");
    switch (msg) {
    case WM_COMMAND: {
        const UINT id = LOWORD(wp);
        if (id >= 0x100 && id < 0x100 + menu_table().size()) {
            const MenuDef& m = menu_table()[id - 0x100];
            log->debug("WM_COMMAND: {} [{}]", wraii::wide_to_utf8(m.item), m.cmd_id);
            // 视角四项即时切换，其余派发命令注册表
            if (wcsncmp(m.item, L"开发视角", 4) == 0) switch_perspective(0);
            else if (wcsncmp(m.item, L"调试视角", 4) == 0) switch_perspective(1);
            else if (wcsncmp(m.item, L"测试视角", 4) == 0) switch_perspective(2);
            else if (wcsncmp(m.item, L"产线视角", 4) == 0) switch_perspective(3);
            else if (m.cmd_id && *m.cmd_id) CommandRegistry::instance().invoke(m.cmd_id);
            return 0;
        }
        break;
    }
    case WM_SIZE:
        layout_children();
        return 0;
    case WM_ERASEBKGND:
        // 工具栏区自绘（四视角段+运行/停止；色值 tokens）
        {
            HDC dc = reinterpret_cast<HDC>(wp);
            RECT rc;
            GetClientRect(m_hwnd, &rc);
            FillRect(dc, &rc, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            RECT tb = {0, kMenubarH, rc.right, kMenubarH + kToolbarH};
            const HBRUSH bg = CreateSolidBrush(ui::tokens::kBgAlt);
            FillRect(dc, &tb, bg);
            DeleteObject(bg);
            // 四段视角钮
            for (int i = 0; i < 4; ++i) {
                RECT b = {8 + i * kPerspBtnW, kMenubarH + kPerspBtnY,
                          8 + (i + 1) * kPerspBtnW, kMenubarH + kPerspBtnY + kPerspBtnH};
                const COLORREF c = (i == m_perspective) ? ui::tokens::kPrimary
                                                        : ui::tokens::kBorder;
                const HBRUSH br = CreateSolidBrush(c);
                FillRect(dc, &b, br);
                DeleteObject(br);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, (i == m_perspective) ? RGB(255, 255, 255)
                                                      : ui::tokens::kText);
                const wchar_t* names[4] = {L"开发", L"调试", L"测试", L"产线"};
                DrawTextW(dc, names[i], -1, &b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            // 运行/停止钮（右对齐）
            RECT run = {rc.right - 200, kMenubarH + kPerspBtnY,
                        rc.right - 120, kMenubarH + kPerspBtnY + kPerspBtnH};
            RECT stop = {rc.right - 112, kMenubarH + kPerspBtnY,
                         rc.right - 32, kMenubarH + kPerspBtnY + kPerspBtnH};
            const HBRUSH run_br = CreateSolidBrush(ui::tokens::kPass);
            const HBRUSH stop_br = CreateSolidBrush(ui::tokens::kFail);
            FillRect(dc, &run, run_br);
            FillRect(dc, &stop, stop_br);
            DeleteObject(run_br);
            DeleteObject(stop_br);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            DrawTextW(dc, L"▶ 运行", -1, &run, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DrawTextW(dc, L"⏹ 停止", -1, &stop, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        return 1;
    case WM_LBUTTONUP: {
        // 工具栏命中：视角四段 / 运行 / 停止（MS0：运行停止=命令桩）
        const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        const int tb_y0 = kMenubarH + kPerspBtnY, tb_y1 = tb_y0 + kPerspBtnH;
        if (y >= tb_y0 && y <= tb_y1) {
            for (int i = 0; i < 4; ++i)
                if (x >= 8 + i * kPerspBtnW && x < 8 + (i + 1) * kPerspBtnW) {
                    switch_perspective(i);
                    InvalidateRect(m_hwnd, nullptr, FALSE);
                    return 0;
                }
        }
        break;
    }
    case WM_DEVICECHANGE:
        log->debug("WM_DEVICECHANGE（设备目录刷新信号，MS0 桩）");
        return 0;
    case WM_DPICHANGED: {
        const RECT* sug = reinterpret_cast<RECT*>(lp);
        SetWindowPos(m_hwnd, nullptr, sug->left, sug->top,
                     sug->right - sug->left, sug->bottom - sug->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DESTROY:
        log->info("DevStudio 主窗口销毁");
        if (m_font) { DeleteObject(m_font); m_font = nullptr; }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}

} // namespace usts::shell
