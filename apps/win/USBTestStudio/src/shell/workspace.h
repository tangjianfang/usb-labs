// workspace.h — 工程模型 .ustsproj（设计 §1.1/§三 / 计划 MS0-T6）
//
// 契约：
//   1. .ustsproj = 工程清单：对产品全栈文件（六组）的"引用"而非拷贝（工程可迁移）；
//   2. 信封 {"v":1,...}，字段只增不改；name 必填（缺失=err）；
//   3. 引用路径一律相对工程目录（relativize 落盘时归一）；跨盘=保留绝对路径；
//   4. remove_ref 只解除引用不删文件；add_ref 重复路径=拒绝。
#pragma once

#include "../app/log.h"
#include "../framework/json_mini.h"
#include "../framework/win32_rai.h"

#include <shlwapi.h>   // PathRelativePathToW

#include <string>
#include <utility>
#include <vector>

namespace usts::shell {

enum class RefKind { Text, Binary, Model, Scenario, Plan, Capture, Doc };

inline const wchar_t* ref_kind_key(RefKind k) {
    switch (k) {
        case RefKind::Binary:   return L"binary";
        case RefKind::Model:    return L"model";
        case RefKind::Scenario: return L"scenario";
        case RefKind::Plan:     return L"plan";
        case RefKind::Capture:  return L"capture";
        case RefKind::Doc:      return L"doc";
        default:                return L"text";
    }
}
inline RefKind ref_kind_from(const std::wstring& s) {
    if (s == L"binary")   return RefKind::Binary;
    if (s == L"model")    return RefKind::Model;
    if (s == L"scenario") return RefKind::Scenario;
    if (s == L"plan")     return RefKind::Plan;
    if (s == L"capture")  return RefKind::Capture;
    if (s == L"doc")      return RefKind::Doc;
    return RefKind::Text;   // 未知/缺省
}

struct FileRef {
    std::wstring rel_path;        // 相对工程目录（'/' 分隔持久化）
    RefKind kind = RefKind::Text;
};

struct Workspace {
    std::wstring name;            // 必填
    std::string template_id;      // 可空（从哪个模板实例化）
    std::vector<FileRef> hardware, firmware, descriptors, host_app, tests, captures;
    std::wstring station = L"STN-01";
    std::wstring startup_plan;    // 启动计划 rel_path（可空）

    std::string to_json() const;
    static bool from_json(const std::string& text, Workspace& out, std::string& err);
};

class WorkspaceOps {
public:
    // 六组名（add_ref 的 group 入参取值；顺序=all_refs 拼接序）
    static const std::vector<std::wstring>& group_names();

    static std::vector<FileRef> all_refs(const Workspace& ws);
    // (引用, 文件是否存在)——工程树 ⚠ 徽章数据源
    static std::vector<std::pair<FileRef, bool>> check_missing(const Workspace& ws,
                                                               const std::wstring& base_dir);
    static bool add_ref(Workspace& ws, const std::wstring& group, FileRef ref,
                        std::string& err);
    static bool remove_ref(Workspace& ws, const std::wstring& rel_path, std::string& err);
    // 同盘→相对路径（'/' 分隔）；跨盘→原样绝对路径
    static std::wstring relativize(const std::wstring& base, const std::wstring& target);
};

class WorkspaceStore {
public:
    static bool save(const Workspace& ws, const std::wstring& ustsproj_path,
                     std::string& err);
    // 缺文件/损坏=默认+was_corrupt=true（新建工程入口靠它区分"打开失败"）
    static bool load(const std::wstring& ustsproj_path, Workspace& out, bool& was_corrupt);
};

// ---------------------------------------------------------------------------
// 实现（header-only；模块日志 shell.workspace）
// ---------------------------------------------------------------------------
namespace detail {

inline std::string workspace_to_json(const Workspace& ws) {
    auto write_group = [](wraii::json_writer& w, const wchar_t* key,
                          const std::vector<FileRef>& refs) {
        w.key(key).begin_array();
        for (const auto& r : refs) {
            w.begin_object();
            w.key(L"path").string_value(r.rel_path);
            w.key(L"kind").string_value(ref_kind_key(r.kind));
            w.end_object();
        }
        w.end_array();
    };
    wraii::json_writer w;
    w.begin_object();
    w.key(L"v").int_value(1);
    w.key(L"name").string_value(ws.name);
    if (!ws.template_id.empty()) w.key(L"template").string_value(wraii::utf8_to_wide(ws.template_id));
    w.key(L"groups").begin_object();
    write_group(w, L"hardware", ws.hardware);
    write_group(w, L"firmware", ws.firmware);
    write_group(w, L"descriptors", ws.descriptors);
    write_group(w, L"host", ws.host_app);
    write_group(w, L"tests", ws.tests);
    write_group(w, L"captures", ws.captures);
    w.end_object();
    w.key(L"production").begin_object();
    w.key(L"station").string_value(ws.station);
    w.end_object();
    if (!ws.startup_plan.empty()) w.key(L"startup_plan").string_value(ws.startup_plan);
    w.end_object();
    return wraii::wide_to_utf8(w.result());
}

inline bool read_group(const minijson::Value& groups, const wchar_t* key,
                       std::vector<FileRef>& out, std::string& err) {
    out.clear();
    const auto* arr = groups.find(key);
    if (!arr || !arr->is_array()) return true;   // 缺组=空（前向兼容）
    for (const auto& e : arr->array) {
        if (!e.is_object()) { err = "groups 条目必须是对象"; return false; }
        FileRef r;
        r.rel_path = minijson::obj_str(e, L"path", L"");
        if (r.rel_path.empty()) { err = "groups 条目缺 path"; return false; }
        r.kind = ref_kind_from(minijson::obj_str(e, L"kind", L"text"));
        // 持久化统一 '/'；内存统一保持原样（Windows 下 '\' 输入也接受）
        out.push_back(std::move(r));
    }
    return true;
}

inline bool workspace_from_json(const std::string& text, Workspace& out, std::string& err) {
    const std::wstring wide = wraii::utf8_to_wide(text);
    minijson::Value v;
    std::wstring werr;
    if (!minijson::parse(wide, v, &werr) || !v.is_object()) {
        err = "ustsproj JSON 解析失败: " + wraii::wide_to_utf8(werr);
        return false;
    }
    if (const auto* x = v.find(L"v")) {
        if (x->as_int(1) != 1) { err = "ustsproj 版本不识别"; return false; }
    }
    const auto* name = v.find(L"name");
    if (!name || name->string.empty()) { err = "ustsproj 缺 name"; return false; }
    out.name = name->string;
    out.template_id = wraii::wide_to_utf8(minijson::obj_str(v, L"template", L""));
    const auto* groups = v.find(L"groups");
    if (groups && groups->is_object()) {
        if (!read_group(*groups, L"hardware", out.hardware, err) ||
            !read_group(*groups, L"firmware", out.firmware, err) ||
            !read_group(*groups, L"descriptors", out.descriptors, err) ||
            !read_group(*groups, L"host", out.host_app, err) ||
            !read_group(*groups, L"tests", out.tests, err) ||
            !read_group(*groups, L"captures", out.captures, err))
            return false;
    }
    if (const auto* prod = v.find(L"production"); prod && prod->is_object())
        out.station = minijson::obj_str(*prod, L"station", out.station);
    out.startup_plan = minijson::obj_str(v, L"startup_plan", L"");
    return true;
}

} // namespace detail

inline std::string Workspace::to_json() const {
    auto log = ustlog::logger("shell.workspace");
    const auto r = detail::workspace_to_json(*this);
    log->debug("to_json: {} 字节（refs={}）", r.size(), WorkspaceOps::all_refs(*this).size());
    return r;
}

inline bool Workspace::from_json(const std::string& text, Workspace& out,
                                 std::string& err) {
    auto log = ustlog::logger("shell.workspace");
    log->debug("from_json: {} 字节", text.size());
    const bool ok = detail::workspace_from_json(text, out, err);
    if (!ok) log->warn("from_json 失败: {}", err);
    return ok;
}

inline const std::vector<std::wstring>& WorkspaceOps::group_names() {
    static const std::vector<std::wstring> g = {L"hardware", L"firmware", L"descriptors",
                                                L"host", L"tests", L"captures"};
    return g;
}

inline std::vector<FileRef> WorkspaceOps::all_refs(const Workspace& ws) {
    std::vector<FileRef> all;
    all.reserve(ws.hardware.size() + ws.firmware.size() + ws.descriptors.size() +
                ws.host_app.size() + ws.tests.size() + ws.captures.size());
    auto append = [&all](const std::vector<FileRef>& v) {
        all.insert(all.end(), v.begin(), v.end());
    };
    append(ws.hardware); append(ws.firmware); append(ws.descriptors);
    append(ws.host_app); append(ws.tests); append(ws.captures);
    return all;
}

inline std::vector<std::pair<FileRef, bool>> WorkspaceOps::check_missing(
    const Workspace& ws, const std::wstring& base_dir) {
    auto log = ustlog::logger("shell.workspace");
    std::vector<std::pair<FileRef, bool>> out;
    size_t missing = 0;
    for (const auto& r : all_refs(ws)) {
        const std::wstring full = base_dir.empty() ? r.rel_path
                                                   : base_dir + L"\\" + r.rel_path;
        const DWORD attr = ::GetFileAttributesW(full.c_str());
        const bool exists = attr != INVALID_FILE_ATTRIBUTES &&
                            !(attr & FILE_ATTRIBUTE_DIRECTORY);
        if (!exists) ++missing;
        out.emplace_back(r, exists);
    }
    log->debug("check_missing: {} 引用，缺失 {}（base={}）", out.size(), missing,
               wraii::wide_to_utf8(base_dir));
    return out;
}

inline std::vector<FileRef>* _group_of(Workspace& ws, const std::wstring& group) {
    if (group == L"hardware")    return &ws.hardware;
    if (group == L"firmware")    return &ws.firmware;
    if (group == L"descriptors") return &ws.descriptors;
    if (group == L"host")        return &ws.host_app;
    if (group == L"tests")       return &ws.tests;
    if (group == L"captures")    return &ws.captures;
    return nullptr;
}

inline bool WorkspaceOps::add_ref(Workspace& ws, const std::wstring& group, FileRef ref,
                                  std::string& err) {
    auto log = ustlog::logger("shell.workspace");
    auto* g = _group_of(ws, group);
    if (!g) { err = "未知分组: " + wraii::wide_to_utf8(group); return false; }
    for (const auto& r : all_refs(ws)) {
        if (r.rel_path == ref.rel_path) {
            err = "引用已存在: " + wraii::wide_to_utf8(ref.rel_path);
            log->warn("add_ref 拒绝重复: {}", err);
            return false;
        }
    }
    log->debug("add_ref: [{}] {}", wraii::wide_to_utf8(group),
               wraii::wide_to_utf8(ref.rel_path));
    g->push_back(std::move(ref));
    return true;
}

inline bool WorkspaceOps::remove_ref(Workspace& ws, const std::wstring& rel_path,
                                     std::string& err) {
    auto log = ustlog::logger("shell.workspace");
    for (const auto& g : group_names()) {
        auto* v = _group_of(ws, g);
        for (auto it = v->begin(); it != v->end(); ++it) {
            if (it->rel_path == rel_path) {
                log->debug("remove_ref: [{}] {}", wraii::wide_to_utf8(g),
                           wraii::wide_to_utf8(rel_path));
                v->erase(it);   // 只解除引用，不删文件（设计 §1.1）
                return true;
            }
        }
    }
    err = "引用不存在: " + wraii::wide_to_utf8(rel_path);
    return false;
}

inline std::wstring WorkspaceOps::relativize(const std::wstring& base,
                                             const std::wstring& target) {
    auto log = ustlog::logger("shell.workspace");
    wchar_t out[MAX_PATH] = {};
    // PathRelativePathToW 坑：基目录无尾反斜杠时，末段不计入公共前缀（C:\p 对
    // C:\p\sub\f.txt 会得 p\sub\f.txt）——统一补尾斜杠
    std::wstring basedir = base;
    if (!basedir.empty() && basedir.back() != L'\\') basedir += L'\\';
    if (!basedir.empty() && !target.empty() &&
        ::PathRelativePathToW(out, basedir.c_str(), FILE_ATTRIBUTE_DIRECTORY,
                              target.c_str(), FILE_ATTRIBUTE_NORMAL)) {
        std::wstring rel = out;
        // 持久化统一 '/' 分隔（跨平台可读；内存同步归一）
        for (auto& c : rel) if (c == L'\\') c = L'/';
        // PathRelativePathToW 返回带 ".\" 前缀（.\apps\win）——剥掉
        if (rel.size() >= 2 && rel[0] == L'.' && rel[1] == L'/') rel.erase(0, 2);
        if (!rel.empty() && rel[0] != L'/') {   // PathRelative 前导 ".."=同盘成功
            log->debug("relativize: {} -> {}", wraii::wide_to_utf8(target),
                       wraii::wide_to_utf8(rel));
            return rel;
        }
    }
    log->debug("relativize: 跨盘或失败，保留绝对 {}", wraii::wide_to_utf8(target));
    return target;   // 跨盘（如 "D:\…"）或 API 失败=原样
}

inline bool WorkspaceStore::save(const Workspace& ws, const std::wstring& ustsproj_path,
                                 std::string& err) {
    auto log = ustlog::logger("shell.workspace");
    const std::wstring dir = ustsproj_path.substr(0, ustsproj_path.find_last_of(L'\\'));
    if (!dir.empty() && !::CreateDirectoryW(dir.c_str(), nullptr) &&
        ::GetLastError() != ERROR_ALREADY_EXISTS) {
        err = "创建目录失败 GLE=" + std::to_string(::GetLastError());
        log->error("save: {}", err);
        return false;
    }
    const std::string json = ws.to_json();
    std::wstring werr;
    if (!wraii::write_file_bytes(ustsproj_path, json.data(), json.size(), &werr)) {
        err = "写工程文件失败: " + wraii::wide_to_utf8(werr);
        log->error("save: {}", err);
        return false;
    }
    log->debug("save: {} 字节 -> {}", json.size(), wraii::wide_to_utf8(ustsproj_path));
    return true;
}

inline bool WorkspaceStore::load(const std::wstring& ustsproj_path, Workspace& out,
                                 bool& was_corrupt) {
    auto log = ustlog::logger("shell.workspace");
    was_corrupt = false;
    std::wstring text, ferr;
    if (!wraii::read_text_file_utf8(ustsproj_path, text, &ferr)) {
        log->debug("load: 不可读（按损坏口径）: {}", wraii::wide_to_utf8(ferr));
        out = Workspace{};
        was_corrupt = true;
        return true;
    }
    std::string err;
    if (!Workspace::from_json(wraii::wide_to_utf8(text), out, err)) {
        log->warn("load: 损坏: {}", err);
        out = Workspace{};
        was_corrupt = true;
    } else {
        log->debug("load: 成功 name={} refs={}", wraii::wide_to_utf8(out.name),
                   WorkspaceOps::all_refs(out).size());
    }
    return true;
}

} // namespace usts::shell
