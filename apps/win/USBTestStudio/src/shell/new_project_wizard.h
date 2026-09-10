// new_project_wizard.h — 新建工程向导（WizardVM 纯逻辑 + 三步对话框；设计 §1.1 / 计划 MS0-T11）
//
// WizardVM 契约：
//   step 0=选模板（tpl_id 必选）→ 1=名称/目录（validate：名称非空、目录不存在或为空）
//   → 2=确认（can_finish）→ instantiate；
//   next/back 状态流转；目录已存在非空/名称空=validate 返回错误文案（对话框直接显示）。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../app/log.h"
#include "templates.h"

#include <string>
#include <vector>

namespace usts::shell {

struct WizardVM {
    int step = 0;                 // 0..2
    std::string tpl_id;
    std::wstring name;            // 工程名
    std::wstring dir;             // 目标目录
    std::vector<TemplateDef> templates;   // list() 结果缓存

    // 步内校验：空串=可进入下一步；非空=错误文案（显示在向导底部）
    std::wstring validate() const {
        if (step == 0) {
            if (tpl_id.empty()) return L"请选择一个模板";
        } else if (step == 1) {
            if (name.empty()) return L"工程名不能为空";
            if (dir.empty()) return L"目标目录不能为空";
            const DWORD attr = GetFileAttributesW(dir.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES) {
                // 存在：仅允许空目录
                WIN32_FIND_DATAW fd;
                const HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
                bool nonempty = false;   // 空目录 FindFirstFile 仍成功（. / .. 项）
                if (h != INVALID_HANDLE_VALUE) {
                    do {
                        if (wcscmp(fd.cFileName, L".") != 0 &&
                            wcscmp(fd.cFileName, L"..") != 0) { nonempty = true; break; }
                    } while (FindNextFileW(h, &fd));
                    FindClose(h);
                }
                if (nonempty) return L"目标目录已存在且非空";
            }
        }
        return L"";
    }
    bool next() {
        if (!validate().empty()) return false;
        if (step >= 2) return false;
        ++step;
        return true;
    }
    bool back() {
        if (step == 0) return false;
        --step;
        return true;
    }
    bool can_finish() const { return step == 2 && validate().empty() &&
                                     !tpl_id.empty() && !name.empty() && !dir.empty(); }
    // finish：找到模板并实例化（返回工程文件路径；空=失败，err 出参）
    std::wstring finish(const std::wstring& manifest_dir, std::string& err) const {
        auto log = ustlog::logger("shell.workspace");
        if (!can_finish()) { err = "向导状态未就绪"; return L""; }
        for (const auto& t : templates)
            if (t.id == tpl_id)
                return Templates::instantiate(t, manifest_dir, dir, name, err);
        err = "模板不存在: " + tpl_id;
        log->warn("wizard finish: {}", err);
        return L"";
    }
};

// 三步对话框（薄壳：模板列表/名称目录表单/确认摘要 + [上一步][下一步][完成][取消]）
class NewProjectWizard {
public:
    // 模态运行；返回 true=已实例化（proj_path 出参）。templates_root=manifest 所在目录。
    static bool run(HWND owner, const std::wstring& manifest_path,
                    std::wstring& proj_path);
};

} // namespace usts::shell
