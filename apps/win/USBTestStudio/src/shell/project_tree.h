// project_tree.h — 工程树面板（薄壳；设计 §1.1 / 计划 MS0-T11）
// TreeView 六组节点+引用行；缺文件=⚠ 前缀（WorkspaceOps::check_missing 数据）；
// 双击=ShellExecute 打开（系统关联程序，MS0 不内置编辑器——设计 §1.1 查看器 MS1+）。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>

#include "../app/log.h"
#include "workspace.h"

namespace usts::shell {

class ProjectTree {
public:
    bool create(HWND parent, int x, int y, int w, int h) {
        auto log = ustlog::logger("ui.shell");
        m_hwnd = CreateWindowExW(0, WC_TREEVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | TVS_HASLINES | TVS_LINESATROOT |
                                     TVS_HASBUTTONS,
                                 x, y, w, h, parent,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(900)),
                                 reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE)),
                                 nullptr);
        if (!m_hwnd) {
            log->error("工程树创建失败 GLE=0x{:08X}", GetLastError());
            return false;
        }
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(parent));
        log->debug("工程树已创建");
        return true;
    }

    void set_workspace(const Workspace& ws, const std::wstring& base_dir) {
        auto log = ustlog::logger("ui.shell");
        TreeView_DeleteAllItems(m_hwnd);
        auto missing = WorkspaceOps::check_missing(ws, base_dir);
        auto add_root = [this] (const wchar_t* title) {
            TVINSERTSTRUCTW ti = {};
            ti.hParent = TVI_ROOT;
            ti.item.mask = TVIF_TEXT;
            ti.item.pszText = const_cast<LPWSTR>(title);
            return TreeView_InsertItem(m_hwnd, &ti);
        };
        const struct { const wchar_t* name; const std::vector<FileRef>* refs; } groups[] = {
            {L"硬件", &ws.hardware}, {L"固件", &ws.firmware}, {L"描述符", &ws.descriptors},
            {L"主机程序", &ws.host_app}, {L"测试", &ws.tests}, {L"抓包", &ws.captures},
        };
        size_t idx = 0, miss = 0;
        for (const auto& g : groups) {
            const HTREEITEM root = add_root(g.name);
            for (const auto& r : *g.refs) {
                const bool ok = idx < missing.size() ? missing[idx].second : true;
                if (!ok) ++miss;
                std::wstring label = (ok ? L"" : L"⚠ ") + r.rel_path;
                TVINSERTSTRUCTW ti = {};
                ti.hParent = root;
                ti.item.mask = TVIF_TEXT;
                ti.item.pszText = label.data();
                TreeView_InsertItem(m_hwnd, &ti);
                ++idx;
            }
        }
        log->debug("工程树装载: {} 引用，缺失 {}（工程={}）", idx, miss,
                   wraii::wide_to_utf8(ws.name));
    }

    HWND hwnd() const { return m_hwnd; }

private:
    HWND m_hwnd = nullptr;
};

} // namespace usts::shell
