// plugin_abi.h — 插件 manifest 模型 + 注册表（设计 §8 插件 ABI 四扩展点 / 计划 MS5-T11）
// MS5 落数据层：plugin.json 解析+注册（DLL 加载与调用归滚动——设计 §8 注记）。
// 四扩展点：panel（面板）/ step（步骤处理器）/ parser（解析器）/ device-template（设备模板）
#pragma once

#include "../app/log.h"
#include "command_registry.h"

#include <map>
#include <string>
#include <vector>

namespace usts::shell::plugin {

struct PluginEntry {
    std::string id;          // "com.example.msc-bench"
    std::string version;
    std::string kind;        // panel | step | parser | device-template
    std::string entry;       // DLL 导出名 / 脚本入口（MS5 仅记录）
    std::string title;
};

class PluginRegistry {
public:
    static PluginRegistry& instance();
    // manifest 文本（一个 plugin.json = 一个插件，extensions 数组可含多扩展点条目）
    // 非法 kind / 缺 id → false+err（整包拒绝——原子性）
    bool load_manifest(const std::string& json_text, std::string& err);
    const std::vector<PluginEntry>& all() const { return m_plugins; }
    const PluginEntry* find(const std::string& id) const;
    size_t count_of_kind(const std::string& kind) const;

private:
    std::vector<PluginEntry> m_plugins;
};

// ---------------------------------------------------------------------------
// 实现（header-only；模块日志 plugin）
// ---------------------------------------------------------------------------
inline PluginRegistry& PluginRegistry::instance() {
    static PluginRegistry r;
    return r;
}

inline bool PluginRegistry::load_manifest(const std::string& json_text,
                                          std::string& err) {
    auto log = ustlog::logger("plugin");
    const std::wstring wide = wraii::utf8_to_wide(json_text);
    minijson::Value v;
    std::wstring werr;
    if (!minijson::parse(wide, v, &werr) || !v.is_object()) {
        err = "plugin.json 解析失败";
        return false;
    }
    const std::string id = wraii::wide_to_utf8(minijson::obj_str(v, L"id", L""));
    const std::string version = wraii::wide_to_utf8(minijson::obj_str(v, L"version", L""));
    if (id.empty() || version.empty()) {
        err = "plugin.json 缺 id/version";
        return false;
    }
    if (find(id)) {
        err = "插件 id 已注册: " + id;
        return false;
    }
    static const char* kKinds[] = {"panel", "step", "parser", "device-template"};
    const auto* exts = v.find(L"extensions");
    if (!exts || !exts->is_array()) {
        err = "plugin.json 缺 extensions";
        return false;
    }
    std::vector<PluginEntry> batch;
    for (const auto& e : exts->array) {
        PluginEntry p;
        p.id = id;
        p.version = version;
        p.kind = wraii::wide_to_utf8(minijson::obj_str(e, L"kind", L""));
        p.entry = wraii::wide_to_utf8(minijson::obj_str(e, L"entry", L""));
        p.title = wraii::wide_to_utf8(minijson::obj_str(e, L"title", wraii::utf8_to_wide(p.id)));
        bool kind_ok = false;
        for (const char* k : kKinds)
            kind_ok = kind_ok || p.kind == k;
        if (!kind_ok) {
            err = "非法扩展点 kind: " + p.kind + "（插件 " + id + " 整包拒绝）";
            log->warn("load_manifest: {}", err);
            return false;
        }
        if (p.entry.empty()) {
            err = "扩展点缺 entry（插件 " + id + "）";
            return false;
        }
        batch.push_back(std::move(p));
    }
    for (auto& b : batch) m_plugins.push_back(std::move(b));
    log->info("插件装入: {}（{} 扩展点）", id, batch.size());
    return true;
}

inline const PluginEntry* PluginRegistry::find(const std::string& id) const {
    for (const auto& p : m_plugins)
        if (p.id == id) return &p;
    return nullptr;
}

inline size_t PluginRegistry::count_of_kind(const std::string& kind) const {
    size_t n = 0;
    for (const auto& p : m_plugins)
        if (p.kind == kind) ++n;
    return n;
}

} // namespace usts::shell::plugin
