// command_registry.h — 命令注册表（设计 §8 命令面板 / 计划 MS0-T5）
//
// 契约：
//   1. 菜单/工具栏/快捷键/命令面板共用同一命令 id——注册一处全端可达；
//   2. add 重复 id=覆盖定义；set_handler 未 add 的 id=拒绝（warn）；
//   3. invoke 无 handler/未知 id=false + warn（可恢复，不抛）。
#pragma once

#include "../app/log.h"
#include "fuzzy.h"
#include "../framework/win32_rai.h"

#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace usts::shell {

struct Command {
    std::string id;            // "view.refresh"
    std::wstring title;        // "刷新设备"（命令面板显示与模糊匹配文本）
    std::string shortcut;      // "F5"（可空；显示用，按键派发在主窗口快捷键表）
    std::string category;      // "视图"/"运行"/"工具"/"文件"…
};

class CommandRegistry {
public:
    static CommandRegistry& instance();

    void add(Command c);                       // 重复 id=覆盖（后注册者赢）
    std::vector<Command> all() const;          // 注册序
    std::vector<Command> by_category(const std::string& cat) const;

    void set_handler(const std::string& id, std::function<void()> fn);
    bool has_handler(const std::string& id) const;
    bool invoke(const std::string& id) const;

private:
    CommandRegistry() = default;
    mutable std::mutex m_mu;
    std::vector<Command> m_cmds;                          // 注册序（菜单生成用）
    std::map<std::string, std::function<void()>> m_handlers;
};

// 命令池模糊排序（fuzzy_rank 的命令特化；空查询=全量 score 0 按注册序）
inline std::vector<std::pair<Command, int>> fuzzy_rank(const std::wstring& q,
                                                       const std::vector<Command>& pool) {
    return fuzzy_rank_impl(q, pool, [](const Command& c) { return c.title; });
}

// ---------------------------------------------------------------------------
// 实现（header-only；模块日志 shell.cmd）
// ---------------------------------------------------------------------------
inline CommandRegistry& CommandRegistry::instance() {
    static CommandRegistry reg;
    return reg;
}

inline void CommandRegistry::add(Command c) {
    auto log = ustlog::logger("shell.cmd");
    std::lock_guard<std::mutex> lk(m_mu);
    for (auto& e : m_cmds) {
        if (e.id == c.id) {
            log->debug("add 覆盖: {}（title 重定义）", c.id);
            e = std::move(c);
            return;
        }
    }
    log->debug("add: {} [{}] {}", c.id, c.category, wraii::wide_to_utf8(c.title));
    m_cmds.push_back(std::move(c));
}

inline std::vector<Command> CommandRegistry::all() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_cmds;
}

inline std::vector<Command> CommandRegistry::by_category(const std::string& cat) const {
    std::lock_guard<std::mutex> lk(m_mu);
    std::vector<Command> out;
    for (const auto& c : m_cmds)
        if (c.category == cat) out.push_back(c);
    return out;
}

inline void CommandRegistry::set_handler(const std::string& id, std::function<void()> fn) {
    auto log = ustlog::logger("shell.cmd");
    std::lock_guard<std::mutex> lk(m_mu);
    bool known = false;
    for (const auto& c : m_cmds) known = known || (c.id == id);
    if (!known) {
        log->warn("set_handler: 未注册命令 {}（拒绝）", id);
        return;
    }
    m_handlers[id] = std::move(fn);
}

inline bool CommandRegistry::has_handler(const std::string& id) const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_handlers.count(id) > 0;
}

inline bool CommandRegistry::invoke(const std::string& id) const {
    auto log = ustlog::logger("shell.cmd");
    std::function<void()> fn;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        const auto it = m_handlers.find(id);
        if (it == m_handlers.end()) {
            log->warn("invoke: 无 handler（命令 {}）", id);
            return false;
        }
        fn = it->second;
    }
    log->debug("invoke: {}", id);
    fn();
    return true;
}

} // namespace usts::shell
