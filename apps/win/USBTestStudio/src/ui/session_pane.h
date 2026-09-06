// session_pane.h — EP-4 S3 会话台后半·每会话一面板（设计 §2 会话标签页内容）：
// 发送区（发送框智能识别 + 编码锁定组合框 + Ctrl+↵/按钮发送 + 周期 ms）与
// 接收区（等宽只读多行、暂停/清屏/Hex↔ASCII/相对↔绝对切换）。面板自持
// IChannel + SessionCore + RenderCursor：接收回调在读线程经 PostMessage 载荷
// 投递 UI 线程入账渲染（channel.h 线程约定），周期到期由 ConsoleWindow 的
// 100ms 定时器统一驱动 tick()。ConsoleWindow 建标签后托管本面板，切标签即
// show/hide。纯 UI 层：逻辑全部在 session_core/session_view，随整片真机验收。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // HWND：与 console_window.h 同口径，不依赖传递包含

#include "channel/channel.h"
#include "session/session_core.h"
#include "session/session_view.h"

#include <memory>
#include <string>

class SessionPane {
public:
    // title 用作标签页文字（调用方可截断）；ch 须已 open 成功
    static SessionPane* create(HWND parent, std::unique_ptr<IChannel> ch,
                               std::wstring title);
    ~SessionPane();

    HWND hwnd() const noexcept { return m_hwnd; }
    const std::wstring& title() const noexcept { return m_title; }

    void show(bool visible) noexcept;
    void place(int x, int y, int w, int h) noexcept;   // ConsoleWindow::layout 调用
    void relayout(UINT dpi) noexcept;                  // DPI 变更：重建字体重排
    void tick(unsigned long long now_ms);              // 周期节拍驱动（100ms 一次）

private:
    SessionPane() = default;

    static bool register_class();   // 首次 create 惰性注册（幂等，main 无需感知）
    static LRESULT CALLBACK pane_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept;
    LRESULT on_message(UINT msg, WPARAM wp, LPARAM lp);

    // 控件 / 布局
    void on_create();
    void create_controls();
    void make_fonts();
    void apply_fonts();
    void layout();
    int  S(int px96) const noexcept { return MulDiv(px96, static_cast<int>(m_dpi), 96); }

    // 动作
    void do_send();                    // 发送当前发送框内容（按钮/Ctrl+↵/周期共用）
    void recall(bool up);              // ↑↓ 历史回选（草稿态语义见 session_core）
    void on_rx(std::vector<uint8_t>* payload);   // UI 线程：入账 + 渲染增量
    void render_poll();                // poll 增量 → 追加接收区（含滚底）
    void render_rebuild();             // rebuild 全量 → 替换接收区文本
    void append_lines(const std::vector<std::wstring>& lines);
    void trim_receive();               // 接收区行数封顶（显示侧防膨胀，账面不受限）
    void status_refresh();             // 面板状态行：计数 + 周期 + 模式

    // 小工具
    static LRESULT CALLBACK send_edit_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                               UINT_PTR id, DWORD_PTR ref);
    std::wstring fetch_text(HWND edit) const;          // 控件文本（动态长度）
    void set_send_text(const std::wstring& s);         // 屏蔽 EN_CHANGE 回灌的光标末尾置文本
    unsigned parse_interval() const;                   // 周期编辑框 → ms（钳制交 core）
    unsigned long long now_ms() const noexcept;        // 与 channel.h t_ms 同源
    void scroll_to_end() noexcept;

    // 控件 ID（面板内子控件，WM_COMMAND 分发用）
    enum {
        IDC_SEND_EDIT = 1,      // 发送框（子类化：↑↓ 历史 / Ctrl+↵ 发送）
        IDC_ENCODING = 2,       // 组合框：自动识别 / Hex 锁定 / ASCII 锁定
        IDC_BTN_SEND = 3,       // 发送
        IDC_CHK_PERIODIC = 4,   // 周期 开/关
        IDC_EDIT_INTERVAL = 5,  // 周期间隔 ms
        IDC_CHK_PAUSE = 6,      // 暂停（继续后台收）
        IDC_BTN_CLEAR = 7,      // 清屏（不清账）
        IDC_CHK_HEX = 8,        // Hex↔ASCII 视图
        IDC_CHK_ABSTS = 9,      // 相对↔绝对时间戳
        IDC_PANE_STATUS = 10,   // 面板状态行
        IDC_RX = 11,            // 接收区
    };

    HWND m_hwnd = nullptr;
    HWND m_send = nullptr, m_encoding = nullptr, m_btnSend = nullptr;
    HWND m_chkPeriodic = nullptr, m_interval = nullptr, m_labelMs = nullptr;
    HWND m_chkPause = nullptr, m_btnClear = nullptr, m_chkHex = nullptr, m_chkAbsTs = nullptr;
    HWND m_rx = nullptr, m_status = nullptr;
    HFONT m_font = nullptr, m_mono = nullptr;
    UINT m_dpi = 96;

    std::wstring m_title;
    std::unique_ptr<IChannel> m_channel;
    session_core::SessionCore m_core;
    session_view::RenderCursor m_view;
    std::wstring m_note;           // 面板状态行的最近一次结果/错误

    // 历史回选的草稿镜像（TxHistory 游标在 core，草稿文本在 UI 侧）
    std::wstring m_draft;
    bool m_in_history = false;     // 游标处于历史中（下一次 push 前草稿不再覆写）
    bool m_setting_text = false;   // 回选/程序化置文本时屏蔽 EN_CHANGE 回灌
};
