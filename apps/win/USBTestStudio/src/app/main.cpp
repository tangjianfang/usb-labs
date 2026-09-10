// main.cpp — wWinMain 入口（MS0 起：默认 = USB DevStudio 工作站壳）。
// 模式：默认 DevStudio（四视角 IDE 壳）· --legacy 旧产测模式 · --console EP-4 通信
// 控制台 · --smoke DevStudio 三秒烟测（创建→切视角→退出 0，CI/打包校验用）。
// 启动序列：ustlog → 崩溃 minidump 处理器 → 设置/布局加载（损坏重置+提示）→
// 新崩溃 dump 扫描（有则提示）→ 主窗口；退出时回存布局/设置。
#include "app/log.h"
#include "app/version.h"
#include "shell/crashdump.h"
#include "shell/desc_editor.h"
#include "shell/te_editor.h"
#include "shell/trace_editor.h"
#include "shell/vd_editor.h"
#include "shell/panel_registry.h"
#include "shell/main_window_ds.h"
#include "shell/perspective.h"
#include "shell/settings.h"
#include "ui/console_window.h"
#include "ui/main_window.h"

#include <commctrl.h>
#include <shellapi.h>

#include <cwchar>
#include <memory>
#include <string>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

namespace shell = usts::shell;

struct AppArgs {
    std::wstring plan_path;   // --legacy 产测模式：默认 exe 同目录 plan.json
    std::wstring dut_sn = L"AUTO";
    std::wstring station;
    bool auto_exit = false;
    bool console = false;     // EP-4 工程师通信控制台（设备发现）
    bool legacy = false;      // 旧产测主窗口（MS0 前默认形态，保留一版）
    bool smoke = false;       // DevStudio 烟测：创建→切视角→自动退出
};

AppArgs parse_command_line(std::shared_ptr<spdlog::logger> log) {
    AppArgs a;
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (!argv) {
        log->error("CommandLineToArgvW 失败 GetLastError=0x{:08X}", ::GetLastError());
        return a;
    }
    for (int i = 1; i < argc; ++i) {
        const wchar_t* arg = argv[i];
        if (::wcscmp(arg, L"--console") == 0) {
            a.console = true;
        } else if (::wcscmp(arg, L"--legacy") == 0) {
            a.legacy = true;                           // 旧产测模式（读 plan.json）
        } else if (::wcscmp(arg, L"--smoke") == 0) {
            a.smoke = true;                            // DevStudio 烟测（无交互）
        } else if (::wcscmp(arg, L"--auto") == 0) {
            a.auto_exit = true;                        // 完成后自动退出（产线，退出码进 MES）
        } else if (::wcscmp(arg, L"--plan") == 0 && i + 1 < argc) {
            a.plan_path = argv[++i];
        } else if (::wcscmp(arg, L"--dut-sn") == 0 && i + 1 < argc) {
            a.dut_sn = argv[++i];
        } else if (::wcscmp(arg, L"--station") == 0 && i + 1 < argc) {
            a.station = argv[++i];
        } else if (arg[0] != L'-') {
            a.plan_path = arg;                         // 位置参数 = 计划路径
        }
    }
    ::LocalFree(argv);
    log->info("命令行解析完成：console={} legacy={} smoke={} auto_exit={} plan={} dut_sn={} station={}",
              a.console, a.legacy, a.smoke, a.auto_exit, ustlog::w2u(a.plan_path),
              ustlog::w2u(a.dut_sn), ustlog::w2u(a.station));
    return a;
}

} // namespace

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    ustlog::init(/*also_stdout=*/false);
    auto log = ustlog::logger("app");
    log->info("USB DevStudio 启动（exe 目录 {}）", ustlog::w2u(wraii::exe_dir()));
    log->info("版本 {}，命令行 {}", UTS_VERSION_U8, ustlog::w2u(::GetCommandLineW()));

    ::HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    if (!::InitCommonControlsEx(&icc)) {
        log->error("InitCommonControlsEx 失败 GetLastError=0x{:08X}", ::GetLastError());
        return static_cast<int>(ExitCode::PlanError);
    }

    // RICHEDIT50W 窗口类在 Msftedit.dll 中，须先加载
    ::LoadLibraryW(L"Msftedit.dll");

    AppArgs args = parse_command_line(log);

    if (args.console) {
        // EP-4 工程师通信控制台：独立窗口类，其余模式不创建、不受影响
        log->info("进入 EP-4 通信控制台模式");
        if (!ConsoleWindow::register_class(hInstance)) {
            log->error("ConsoleWindow::register_class 失败 GetLastError=0x{:08X}", ::GetLastError());
            return static_cast<int>(ExitCode::PlanError);
        }
        ConsoleWindow* con = ConsoleWindow::create(hInstance, nCmdShow);
        if (!con) {
            log->error("ConsoleWindow::create 失败 GetLastError=0x{:08X}", ::GetLastError());
            return static_cast<int>(ExitCode::PlanError);
        }
        int code = con->run();
        log->info("控制台退出，退出码 {}", code);
        delete con;
        return code;
    }

    if (!args.legacy) {
        // —— DevStudio 工作站壳（MS0 默认）——
        shell::CrashDumpMgr::install_handler();   // 崩溃收集先于一切 UI（设计 §8）
        shell::Settings cfg;
        bool cfg_corrupt = false;
        shell::SettingsStore::load(cfg, cfg_corrupt);
        if (cfg_corrupt) {
            log->warn("settings.json 损坏，已重置默认（原文件保留策略见设计附录 E.4）");
            MessageBoxW(nullptr, L"设置文件损坏，已恢复默认值。", L"USB DevStudio",
                        MB_ICONWARNING | MB_OK);
        }
        shell::LayoutState layout;
        bool layout_corrupt = false;
        shell::LayoutStore::load(layout, layout_corrupt);
        if (layout_corrupt) log->warn("layout.json 损坏，已重置默认布局");
        // 退出时回存（窗口位置/视角变化）
        struct LayoutSave {
            shell::LayoutState st;
            ~LayoutSave() {
                std::string err;
                if (!shell::LayoutStore::save(st, err))
                    ustlog::logger("app")->warn("布局回存失败: {}", err);
            }
        } layout_save;
        layout_save.st = layout;

        // 新崩溃 dump 提示（设计 §8 崩溃恢复：三选对话框 MS3 完整化，MS0=提示）
        std::string cderr;
        shell::CrashDumpMgr::ensure_dir(cderr);
        const auto dumps = shell::CrashDumpMgr::scan();
        if (!dumps.empty() &&
            !shell::CrashDumpMgr::is_seen(shell::CrashDumpMgr::dir(), dumps.back())) {
            log->warn("检测到未读崩溃转储: {}", ustlog::w2u(dumps.back()));
            const std::wstring dump_msg =
                L"检测到上次异常退出留下的崩溃转储：\n" + dumps.back() +
                L"\n\n仅保留本地（稍后可在日志目录找到）？";
            if (MessageBoxW(nullptr, dump_msg.c_str(),
                            L"USB DevStudio", MB_ICONWARNING | MB_YESNO) == IDYES) {
                shell::CrashDumpMgr::mark_seen(shell::CrashDumpMgr::dir(), dumps.back());
            }
        }

        shell::PanelRegistry::instance().set_factory(
            "w2.descriptor", &shell::desc::DescEditorPanel::create_w2);   // MS1：W2 真面板
        shell::PanelRegistry::instance().set_factory(
            "w3.vd", &shell::vd::VdEditorPanel::create_w3);               // MS2：W3 真面板
        shell::PanelRegistry::instance().set_factory(
            "w5.runner", &shell::te::RunnerPanel::create_w5);             // MS3：W5 执行视图
        shell::PanelRegistry::instance().set_factory(
            "w6.report_list", &shell::te::ReportPanel::create_w6);        // MS3：W6 报告中心
        shell::PanelRegistry::instance().set_factory(
            "w4.trace", &shell::trace::TracePanel::create_w4);            // MS4：W4 追踪台
        shell::MainWindowDS::register_commands();
        shell::MainWindowDS win;
        if (!win.create(hInstance, cfg, layout)) {
            log->error("DevStudio 主窗口创建失败");
            return static_cast<int>(ExitCode::PlanError);
        }
        if (args.smoke) {
            // 烟测：隐藏创建→四视角轮切→退出 0（无视觉断言，重大版本前禁用视觉 review）
            log->info("DevStudio 烟测：四视角轮切后退出");
            win.show(SW_HIDE);
            for (int i = 0; i < 4; ++i) win.switch_perspective(i);
            win.switch_perspective(0);
            layout_save.st.current = shell::PerspectiveId::Dev;
            log->info("DevStudio 烟测通过，退出码 0");
            return 0;
        }
        win.show(nCmdShow);
        win.pump_quit();
        log->info("DevStudio 退出");
        return 0;
    }

    // —— 旧产测模式（--legacy；MS0 前的默认形态，保留一版过渡）——
    if (args.plan_path.empty()) args.plan_path = wraii::exe_dir() + L"\\plan.json";
    if (args.station.empty()) args.station = L"STN-01";   // 与 tools/usbtest DEFAULT_STATION 一致

    log->info("进入旧产测模式：plan={} dut_sn={} station={} auto_exit={}",
              ustlog::w2u(args.plan_path), ustlog::w2u(args.dut_sn),
              ustlog::w2u(args.station), args.auto_exit);

    if (!MainWindow::register_class(hInstance)) {
        log->error("MainWindow::register_class 失败 GetLastError=0x{:08X}", ::GetLastError());
        return static_cast<int>(ExitCode::PlanError);
    }

    MainWindow* win = MainWindow::create(hInstance, nCmdShow, args.plan_path, args.dut_sn,
                                         args.station, args.auto_exit);
    if (!win) {
        log->error("MainWindow::create 失败 GetLastError=0x{:08X}", ::GetLastError());
        return static_cast<int>(ExitCode::PlanError);
    }

    int code = win->run();
    log->info("产测退出，退出码 {}", code);
    delete win;
    return code;
}
