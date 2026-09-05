// main_window.h — 主窗口：原生 Win32 控件、PerMonitorV2 DPI 缩放、快捷键
// （F5 扫描 / Ctrl+R 运行 / Ctrl+S 导出）、WM_SIZE 布局、引擎事件渲染。
// 未真机编译，按 MSDN 口径编写。
#pragma once

#include "engine/test_engine.h"
#include "ui/log_view.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

class MainWindow {
public:
    static bool register_class(HINSTANCE hinst);
    static MainWindow* create(HINSTANCE hinst, int nCmdShow, const std::wstring& plan_path,
                              const std::wstring& dut_sn, const std::wstring& station,
                              bool auto_exit);
    ~MainWindow();

    // 消息循环（含 TranslateAccelerator），返回退出码（0 PASS / 1 FAIL / 2 ABORT / 3 计划错误）
    int run();
    bool pre_translate(MSG& msg);

private:
    MainWindow(const std::wstring& plan_path, const std::wstring& dut_sn,
               const std::wstring& station, bool auto_exit);

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept;
    LRESULT on_message(UINT msg, WPARAM wp, LPARAM lp);

    // 控件 / 布局
    void on_create();
    void layout();
    int  S(int px96) const noexcept { return MulDiv(px96, static_cast<int>(m_dpi), 96); }
    void make_fonts();
    void apply_fonts();
    void create_controls();

    // 动作
    bool try_load_plan(bool announce);
    void do_scan();
    void do_run();
    void do_export();
    void fill_device_views();
    void fill_steps_table(const std::wstring& pending_text);
    void set_running_ui(bool running);

    // 引擎事件（WM_APP+1..3）
    void handle_engine_log();
    void handle_engine_step(StepEvent* ev);
    void handle_engine_done(DoneEvent* ev);
    void handle_scan_done(std::vector<DeviceInfo>* devices);

    // 状态栏
    void status_set(int pane, const std::wstring& text);
    void status_refresh();

    // 控件 ID（按钮与加速器共用）
    enum {
        IDC_SCAN = 1001,
        IDC_RUN = 1002,
        IDC_EXPORT = 1003,
        IDC_COMBO = 2001,
        IDC_LVDEV = 2002,
        IDC_LVSTEP = 2003,
        IDC_PROG = 2004,
        IDC_STATUS = 2005,
        IDC_LOG = 2006,
    };

    std::wstring m_planPath;
    std::wstring m_dutSn;
    std::wstring m_station;
    bool m_autoExit = false;

    HWND m_hwnd = nullptr;
    HWND m_combo = nullptr;
    HWND m_btnScan = nullptr;
    HWND m_btnRun = nullptr;
    HWND m_btnExport = nullptr;
    HWND m_lvDev = nullptr;
    HWND m_lvStep = nullptr;
    HWND m_prog = nullptr;
    HWND m_status = nullptr;
    LogView m_log;

    HFONT m_font = nullptr;
    HACCEL m_accel = nullptr;
    UINT m_dpi = 96;

    std::vector<DeviceInfo> m_devices;
    std::wstring m_statusTexts[4];               // 状态栏 4 分栏文本缓存
    unsigned long long m_droppedTotal = 0;
    int m_lastExitCode = 0;
    std::atomic<bool> m_running{false};

    std::unique_ptr<MessagePumpEvents> m_pump;   // 先于引擎声明
    std::unique_ptr<TestEngine> m_engine;        // 最后声明 → 最先析构（先停线程）
};
