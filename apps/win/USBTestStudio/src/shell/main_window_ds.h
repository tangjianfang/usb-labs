// main_window_ds.h — DevStudio 主窗口壳（设计 §0.1/§0.2 / 计划 MS0-T10）
//
// 职责：IDE 形态主窗口——菜单（menu_table 数据驱动）/视角四段工具栏/三栏布局
// （左=工程树+设备目录桩 | 中=面板标签区 | 右=上下文栏）/状态栏六格。
// 面板内容由面板注册表装配（MS0 桩=静态文本占位，MS1+ 逐工作台替换）。
// 数据表（菜单/状态栏）与窗口操作分离——表驱动可离线自测，窗口烟测=隐藏创建/销毁。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "perspective.h"
#include "settings.h"

#include <string>
#include <utility>
#include <vector>

namespace usts::shell {

struct MenuDef {
    const wchar_t* menu;      // 顶层菜单名
    const wchar_t* item;      // 菜单项（L"-"=分隔线）
    const char* shortcut;     // 显示用（可空）
    const char* cmd_id;       // 命令 id（分隔线为空）
};

// 设计 §0.1 菜单逐项（自测与此表强一致——改菜单先改设计）
const std::vector<MenuDef>& menu_table();
// 状态栏六格（工程/设备/引擎/通知/日志/时钟）——(命令锚点, 显示名)
const std::vector<std::pair<const char*, const wchar_t*>>& status_cells_def();

class MainWindowDS {
public:
    // 菜单/工具栏/快捷键全部命令注册进 CommandRegistry（幂等）
    static void register_commands();

    bool create(HINSTANCE inst, const Settings& cfg, const LayoutState& layout);
    void switch_perspective(int perspective_index);   // 0..3（PerspectiveId 序值）
    void show(int nCmdShow);
    bool pump_quit();          // 消息循环；返回 true=收到 WM_QUIT
    void request_quit();

    HWND hwnd() const { return m_hwnd; }
    int current_perspective() const { return m_perspective; }

private:
    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void build_menu();
    void layout_children();     // 三栏 + 工具栏 + 状态栏按 client 尺寸摆放
    void apply_perspective();   // 装载当前视角面板桩

    HWND m_hwnd = nullptr;
    HWND m_hwnd_left = nullptr;      // 左栏容器
    HWND m_hwnd_central = nullptr;   // 中央标签区容器
    HWND m_hwnd_right = nullptr;     // 右上下文栏
    HWND m_hwnd_status = nullptr;    // 状态栏（六格文本自绘）
    int m_perspective = 0;
    HFONT m_font = nullptr;
};

} // namespace usts::shell
