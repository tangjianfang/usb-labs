// main.cpp — wWinMain 入口：InitCommonControlsEx、加载 Msftedit.dll、
// 命令行解析（--console 进 EP-4 通信控制台，默认产测模式）、消息循环与退出码。
#include "app/log.h"
#include "app/version.h"
#include "ui/console_window.h"
#include "ui/main_window.h"

#include <commctrl.h>
#include <shellapi.h>

#include <cwchar>
#include <memory>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

struct AppArgs {
    std::wstring plan_path;   // 默认 exe 同目录 plan.json
    std::wstring dut_sn = L"AUTO";
    std::wstring station;
    bool auto_exit = false;
    bool console = false;     // EP-4 工程师通信控制台（设备发现）
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
            a.console = true;                          // EP-4 通信控制台（设备发现），产测模式旁路
        } else if (::wcscmp(arg, L"--auto") == 0) {
            a.auto_exit = true;                        // 完成后自动退出（产线模式，退出码进 MES）
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
    log->info("命令行解析完成：console={} auto_exit={} plan={} dut_sn={} station={}",
              a.console, a.auto_exit, ustlog::w2u(a.plan_path),
              ustlog::w2u(a.dut_sn), ustlog::w2u(a.station));
    return a;
}

} // namespace

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    ustlog::init(/*also_stdout=*/false);
    auto log = ustlog::logger("app");
    log->info("USBTestStudio 启动（console 构建目录 {}）", ustlog::w2u(wraii::exe_dir()));
    log->info("版本 {}，命令行 {}", UTS_VERSION_U8,
              ustlog::w2u(::GetCommandLineW()));

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
        // EP-4 工程师通信控制台：独立窗口类，产测主窗口不创建、不受影响
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

    if (args.plan_path.empty()) args.plan_path = wraii::exe_dir() + L"\\plan.json";
    if (args.station.empty()) args.station = L"STN-01";   // 与 tools/usbtest DEFAULT_STATION 一致

    log->info("进入产测模式：plan={} dut_sn={} station={} auto_exit={}",
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
