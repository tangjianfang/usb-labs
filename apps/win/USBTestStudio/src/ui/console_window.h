// console_window.h — EP-4 工程师通信控制台·S2 设备发现窗口（设计 §2 上区）：
// 搜索框 + 协议复选框 + 目录表格 + F5 刷新；双击设备行 = 开会话（S2 打开
// IChannel 并持有，收发台 UI 是 S3）。独立窗口类，`--console` 进入，产测
// 主窗口（main_window）不受影响。未真机编译，按 MSDN 口径编写。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // HWND/HINSTANCE：本头被 main.cpp 首位包含，不依赖传递包含

#include "channel/channel.h"
#include "discovery/device_catalog.h"

#include <memory>
#include <vector>

class ConsoleWindow {
public:
    static bool register_class(HINSTANCE hinst);
    static ConsoleWindow* create(HINSTANCE hinst, int nCmdShow);
    ~ConsoleWindow();

    int run();   // 消息循环，返回退出码

private:
    ConsoleWindow() = default;

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
    void do_scan();                       // 后台枚举（SetupDi/注册表只读）
    void handle_scan_done(std::vector<ConsoleDevice>* catalog);
    unsigned kind_mask() const noexcept;  // 复选框 → DeviceKind 位掩码（0=全选）
    void refill();                        // 即时过滤 + 重填表格（输入即收敛）
    void on_activate_item();              // 双击选中行 → 开会话
    void status_refresh();

    // 控件 ID（按钮与加速器共用）
    enum {
        IDC_REFRESH = 1101,
        IDC_SEARCH = 1102,
        IDC_CHK_HID = 1103,
        IDC_CHK_SERIAL = 1104,
        IDC_CHK_USB = 1105,
        IDC_LIST = 1201,
        IDC_STATUS = 1202,
    };

    HWND m_hwnd = nullptr;
    HWND m_search = nullptr;
    HWND m_chkHid = nullptr, m_chkSerial = nullptr, m_chkUsb = nullptr;
    HWND m_btnRefresh = nullptr;
    HWND m_list = nullptr;
    HWND m_status = nullptr;
    HFONT m_font = nullptr;
    HACCEL m_accel = nullptr;
    UINT m_dpi = 96;

    std::vector<ConsoleDevice> m_catalog;               // 全量目录（最近一次扫描）
    std::vector<ConsoleDevice> m_view;                  // 当前过滤视图（行 ↔ 条目）
    std::vector<std::unique_ptr<IChannel>> m_sessions;  // 双击已开的会话（收发台 S3）
};
