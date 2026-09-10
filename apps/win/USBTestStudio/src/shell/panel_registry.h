// panel_registry.h — 面板注册表（设计 §10 契约的数据层 / 计划 MS0-T4）
//
// 契约（V2.0 第三方插件 ABI 的同一接口，MS0 先落地进程内静态注册）：
//   1. 新增面板零改框架——主窗口/视角装载/命令面板全部从注册表取面板；
//   2. register_panel 重复 id=拒绝（err 给出已存在项）；
//   3. 视角引用未注册面板 id=静默忽略（加载期兼容：面板可后装/可选 DLL）。
#pragma once

#include "../app/log.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

struct HWND__;   // 前向声明（不拉 windows.h，保持纯逻辑可测）

namespace usts::shell {

enum class DevKind { Usb, Hid, Com, Msc, Audio, Ble };   // 六源（设计 §1.2）

struct PanelInfo {
    std::string id;                    // "w5.explorer"（组.名称 点分）
    std::wstring title;                // "测试资源管理器"（中文，设计文案）
    int icon = 0;                      // usts::ui::tokens::Icon 序值
    bool central = true;               // true=中央标签面板 / false=侧栏面板
    std::vector<DevKind> supports;     // 空=通用面板
};

// 面板窗口工厂：(host=宿主容器 HWND) -> 面板 HWND。MS0 为桩（返回占位窗口），
// MS1+ 各工作台用真实现替换——注册表只存函数不关心实现。
using PanelCreateFn = std::function<HWND__*(void*)>;

class PanelRegistry {
public:
    static PanelRegistry& instance();  // 进程级单例

    bool register_panel(PanelInfo info, PanelCreateFn fn, std::string& err);
    // 桩→真实现替换（MS1 起：W2 描述符台等逐个换真；未注册 id=拒绝）
    bool set_factory(const std::string& id, PanelCreateFn fn);
    const std::vector<std::pair<PanelInfo, PanelCreateFn>>& all() const { return m_panels; }
    const PanelInfo* find(const std::string& id) const;
    // 视角装载用：按 id 列表取已注册面板（未注册忽略，顺序保持入参序）
    std::vector<PanelInfo> for_perspective(const std::vector<std::string>& ids) const;

private:
    PanelRegistry() = default;
    std::vector<std::pair<PanelInfo, PanelCreateFn>> m_panels;
};

// MS0 内置面板桩（幂等：重复调用直接返回）。真实现随 MS1+ 里程碑逐个替换。
// id 清单与视角默认面板（perspective.h）对齐：
//   w1.project_tree / w1.device_catalog / w2.descriptor / w3.vd / w3.console /
//   w3.inspector / w4.trace / w5.explorer / w5.runner / w5.pipeline / w5.history /
//   w6.report_list
void register_builtin_panels(PanelRegistry& r);

// ---------------------------------------------------------------------------
// 实现（header-only；模块日志 shell.panel）
// ---------------------------------------------------------------------------

inline PanelRegistry& PanelRegistry::instance() {
    static PanelRegistry reg;
    return reg;
}

inline bool PanelRegistry::register_panel(PanelInfo info, PanelCreateFn fn,
                                          std::string& err) {
    auto log = ustlog::logger("shell.panel");
    if (find(info.id)) {
        err = "面板 id 已注册: " + info.id;
        log->warn("register_panel 拒绝重复: {}", err);
        return false;
    }
    log->debug("register_panel: id={} central={} supports={}", info.id, info.central,
               info.supports.size());
    m_panels.emplace_back(std::move(info), std::move(fn));
    return true;
}

inline const PanelInfo* PanelRegistry::find(const std::string& id) const {
    for (const auto& [info, fn] : m_panels)
        if (info.id == id) return &info;
    return nullptr;
}

inline bool PanelRegistry::set_factory(const std::string& id, PanelCreateFn fn) {
    auto log = ustlog::logger("shell.panel");
    for (auto& [info, f] : m_panels) {
        if (info.id == id) { f = std::move(fn); log->debug("set_factory: {}", id); return true; }
    }
    log->warn("set_factory: 未注册 id {}", id);
    return false;
}

inline std::vector<PanelInfo> PanelRegistry::for_perspective(
    const std::vector<std::string>& ids) const {
    std::vector<PanelInfo> out;
    for (const auto& id : ids)
        if (const PanelInfo* p = find(id)) out.push_back(*p);
    return out;
}

namespace detail {

// MS0 内置面板定义表（title=设计 §0.2/各工作台标题文案）
struct BuiltinPanel { const char* id; const wchar_t* title; bool central; };
inline const std::vector<BuiltinPanel>& builtin_panel_table() {
    static const std::vector<BuiltinPanel> t = {
        {"w1.project_tree",  L"工程树",       false},
        {"w1.device_catalog", L"设备目录",    false},
        {"w2.descriptor",    L"描述符台",     true},
        {"w3.vd",            L"虚拟调试器",   true},
        {"w3.console",       L"通信控制台",   true},
        {"w3.inspector",     L"状态检查器",   false},
        {"w4.trace",         L"协议追踪台",   true},
        {"w5.explorer",      L"测试资源管理器", false},
        {"w5.runner",        L"产测执行",     true},
        {"w5.pipeline",      L"Pipeline",    true},
        {"w5.history",       L"运行历史",     true},
        {"w6.report_list",   L"报告中心",     true},
    };
    return t;
}

} // namespace detail

inline void register_builtin_panels(PanelRegistry& r) {
    auto log = ustlog::logger("shell.panel");
    for (const auto& b : detail::builtin_panel_table()) {
        if (r.find(b.id)) continue;   // 幂等
        PanelInfo pi;
        pi.id = b.id;
        pi.title = b.title;
        pi.central = b.central;
        std::string err;   // 幂等保护下不会失败，err 仅形式
        r.register_panel(std::move(pi), nullptr, err);
    }
    log->debug("register_builtin_panels: 在册 {} 项", r.all().size());
}

} // namespace usts::shell
