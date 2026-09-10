// templates.h — 工程模板库（设计 §1.1 新建工程向导 / 计划 MS0-T7）
//
// 契约：
//   1. manifest.json（templates/ 目录，随安装包分发）列出 7 个官方 lab 模板；
//   2. instantiate = 递归复制模板目录 → dest_dir（跳过 build 缓存/__pycache__/*.pyc），
//      生成 <工程名>.ustsproj（六组按模板目录结构预填引用）；
//   3. 模板源目录约定（labs/labN 结构）：hardware/ firmware/src/ host/ capture/。
#pragma once

#include "../app/log.h"
#include "workspace.h"

#include <cwchar>
#include <cstdlib>
#include <string>
#include <vector>

namespace usts::shell {

struct TemplateDef {
    std::string id;
    std::wstring name;
    std::wstring desc;
    std::wstring src_dir;   // 相对 manifest 所在目录
};

class Templates {
public:
    // manifest 解析失败/缺文件=err（空列表）；条目缺 id 或 src=跳过该条并 warn
    static std::vector<TemplateDef> list(const std::wstring& manifest_path, std::string& err);

    // 实例化：复制 src → dest_dir（目录须不存在或为空），生成 <name>.ustsproj；
    // 返回工程文件全路径；失败 err（已复制的内容不回滚——目标目录本就要求新建）。
    static std::wstring instantiate(const TemplateDef& t, const std::wstring& manifest_dir,
                                    const std::wstring& dest_dir,
                                    const std::wstring& proj_name, std::string& err);
};

// ---------------------------------------------------------------------------
// 实现（header-only；模块日志 shell.workspace）
// ---------------------------------------------------------------------------
namespace detail {

// 复制时跳过的相对目录/后缀（build 产物与缓存不入工程）
inline bool _skip_entry(const std::wstring& name, bool is_dir) {
    if (!is_dir) {
        static const wchar_t* kSkipExt[] = {L".pyc", L".uf2", L".elf", L".o", L".bin"};
        for (const auto* e : kSkipExt)
            if (name.size() >= wcslen(e) &&
                _wcsicmp(name.c_str() + name.size() - wcslen(e), e) == 0) return true;
        return false;
    }
    return name == L"build" || name == L"__pycache__" || name == L".git";
}

inline bool _copy_dir_recurse(const std::wstring& src, const std::wstring& dst,
                              size_t& copied, std::string& err) {
    if (!::CreateDirectoryW(dst.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) {
        err = "CreateDirectory 失败 GLE=" + std::to_string(::GetLastError()) + " path=" +
              wraii::wide_to_utf8(dst);
        return false;
    }
    WIN32_FIND_DATAW fd;
    const HANDLE h = ::FindFirstFileW((src + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        err = "FindFirstFile 失败 GLE=" + std::to_string(::GetLastError());
        return false;
    }
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        const bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (_skip_entry(name, is_dir)) continue;
        const std::wstring s = src + L"\\" + name, d = dst + L"\\" + name;
        if (is_dir) {
            if (!_copy_dir_recurse(s, d, copied, err)) { ::FindClose(h); return false; }
        } else if (!::CopyFileW(s.c_str(), d.c_str(), FALSE)) {
            err = "CopyFile 失败 GLE=" + std::to_string(::GetLastError()) + " file=" +
                  wraii::wide_to_utf8(name);
            ::FindClose(h);
            return false;
        }
        ++copied;
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    return true;
}

// 按模板目录结构预填六组引用（存在才引用——不同 lab 深度不一）
inline void _prefill_groups(Workspace& ws) {
    auto add_all = [](std::vector<FileRef>& group, const std::wstring& dir_rel,
                      const std::wstring& dir_full, RefKind kind) {
        WIN32_FIND_DATAW fd;
        const HANDLE h = ::FindFirstFileW((dir_full + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            group.push_back({dir_rel + L"/" + name, kind});
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    };
    add_all(ws.hardware, L"hardware", L"hardware", RefKind::Doc);
    add_all(ws.firmware, L"firmware/src", L"firmware\\src", RefKind::Text);
    add_all(ws.host_app, L"host", L"host", RefKind::Text);
    add_all(ws.captures, L"capture", L"capture", RefKind::Capture);
    // tests：host/autotest.yaml 即产测计划（存在则设为启动计划）
    DWORD attr = ::GetFileAttributesW(L"host\\autotest.yaml");
    if (attr != INVALID_FILE_ATTRIBUTES) {
        ws.tests.push_back({L"host/autotest.yaml", RefKind::Plan});
        ws.startup_plan = L"host/autotest.yaml";
    }
}

} // namespace detail

inline std::vector<TemplateDef> Templates::list(const std::wstring& manifest_path,
                                                std::string& err) {
    auto log = ustlog::logger("shell.workspace");
    std::vector<TemplateDef> out;
    std::wstring text, ferr;
    if (!wraii::read_text_file_utf8(manifest_path, text, &ferr)) {
        err = "manifest 不可读: " + wraii::wide_to_utf8(ferr);
        log->warn("Templates::list: {}", err);
        return out;
    }
    minijson::Value v;
    std::wstring werr;
    if (!minijson::parse(text, v, &werr) || !v.is_object()) {
        err = "manifest 解析失败: " + wraii::wide_to_utf8(werr);
        return out;
    }
    const auto* arr = v.find(L"templates");
    if (!arr || !arr->is_array()) { err = "manifest 缺 templates 数组"; return out; }
    for (const auto& e : arr->array) {
        if (!e.is_object()) continue;
        const std::wstring id = minijson::obj_str(e, L"id", L"");
        const std::wstring src = minijson::obj_str(e, L"src", L"");
        if (id.empty() || src.empty()) {
            log->warn("manifest 条目缺 id/src，跳过");
            continue;
        }
        TemplateDef t;
        t.id = wraii::wide_to_utf8(id);
        t.name = minijson::obj_str(e, L"name", id);
        t.desc = minijson::obj_str(e, L"desc", L"");
        const std::wstring mdir = manifest_path.substr(0, manifest_path.find_last_of(L'\\'));
        t.src_dir = mdir + L"\\" + src;
        out.push_back(std::move(t));
    }
    log->debug("Templates::list: {} 个模板（{}）", out.size(), wraii::wide_to_utf8(manifest_path));
    return out;
}

inline std::wstring Templates::instantiate(const TemplateDef& t,
                                           const std::wstring& manifest_dir,
                                           const std::wstring& dest_dir,
                                           const std::wstring& proj_name, std::string& err) {
    auto log = ustlog::logger("shell.workspace");
    log->info("实例化工程: 模板={} 目标={} 名称={}", t.id,
              wraii::wide_to_utf8(dest_dir), wraii::wide_to_utf8(proj_name));

    // 模板源探测：优先 <manifest_dir>/<t.src_dir 相对名>，回退 t.src_dir 绝对
    std::wstring src = manifest_dir + L"\\" + t.src_dir.substr(
        t.src_dir.find_last_of(L'\\') + 1);
    if (::GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) src = t.src_dir;
    if (::GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = "模板源目录不存在: " + wraii::wide_to_utf8(src);
        log->error("instantiate: {}", err);
        return L"";
    }
    // 目标目录：不存在则建；存在且非空=拒绝（防覆盖）
    if (::GetFileAttributesW(dest_dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (!::CreateDirectoryW(dest_dir.c_str(), nullptr)) {
            err = "目标目录创建失败 GLE=" + std::to_string(::GetLastError());
            log->error("instantiate: {}", err);
            return L"";
        }
    } else {
        WIN32_FIND_DATAW fd;
        const HANDLE h = ::FindFirstFileW((dest_dir + L"\\*").c_str(), &fd);
        bool nonempty = false;   // 空目录 FindFirstFile 仍成功（. / .. 项）
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (::wcscmp(fd.cFileName, L".") != 0 &&
                    ::wcscmp(fd.cFileName, L"..") != 0) { nonempty = true; break; }
            } while (::FindNextFileW(h, &fd));
            ::FindClose(h);
        }
        if (nonempty) {
            err = "目标目录非空，拒绝实例化";
            log->warn("instantiate: {}", err);
            return L"";
        }
    }
    size_t copied = 0;
    if (!detail::_copy_dir_recurse(src, dest_dir, copied, err)) {
        log->error("instantiate: {}", err);
        return L"";
    }
    // 生成 .ustsproj（相对 dest_dir 探测预填：预填内部用相对路径探测，切 cwd 实现）
    Workspace ws;
    ws.name = proj_name;
    ws.template_id = t.id;
    wchar_t old_cwd[MAX_PATH] = {};
    ::GetCurrentDirectoryW(MAX_PATH, old_cwd);
    ::SetCurrentDirectoryW(dest_dir.c_str());
    detail::_prefill_groups(ws);
    ::SetCurrentDirectoryW(old_cwd);
    const std::wstring proj_path = dest_dir + L"\\" + proj_name + L".ustsproj";
    if (!WorkspaceStore::save(ws, proj_path, err)) {
        log->error("instantiate: 工程文件写入失败: {}", err);
        return L"";
    }
    log->info("工程实例化完成: {} 项复制，工程文件={}", copied, wraii::wide_to_utf8(proj_path));
    return proj_path;
}

} // namespace usts::shell
