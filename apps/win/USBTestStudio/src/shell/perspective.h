// perspective.h — 视角系统与布局持久化（设计 §0.2 / 附录 E.7 C.5 / 计划 MS0-T3）
//
// 契约：
//   1. 四视角=布局预设不是功能边界（任何面板可在任何视角打开）；
//   2. 布局持久化 %APPDATA%\USBDevStudio\layout.json，信封 {"v":1,...}；
//   3. clamp() 边界夹取（左/右栏 180..420，窗口 ≥ 最小值）；损坏=重置默认+was_corrupt。
#pragma once

#include "../app/log.h"
#include "../framework/json_mini.h"
#include "../framework/win32_rai.h"
#include "../ui/tokens.h"

#include <algorithm>
#include <string>
#include <vector>

namespace usts::shell {

enum class PerspectiveId { Dev = 0, Debug = 1, Test = 2, Prod = 3 };

struct PerspectiveDef {
    PerspectiveId id;
    const wchar_t* name;            // 开发/调试/测试/产线
    const char* accelerator;        // "Ctrl+Alt+1".."4"
    std::vector<std::string> default_panels;
};

// 四视角默认面板（设计 §0.2 表；面板 id 以面板注册表为准，未注册面板加载时忽略）
inline const std::vector<PerspectiveDef>& perspectives() {
    static const std::vector<PerspectiveDef> ps = {
        {PerspectiveId::Dev,   L"开发", "Ctrl+Alt+1",
         {"w1.project_tree", "w1.device_catalog", "w2.descriptor", "w3.vd"}},
        {PerspectiveId::Debug, L"调试", "Ctrl+Alt+2",
         {"w3.vd", "w3.console", "w4.trace", "w3.inspector"}},
        {PerspectiveId::Test,  L"测试", "Ctrl+Alt+3",
         {"w5.explorer", "w5.runner", "w5.pipeline", "w5.history"}},
        {PerspectiveId::Prod,  L"产线", "Ctrl+Alt+4",
         {"w5.runner", "w6.report_list"}},
    };
    return ps;
}

inline const PerspectiveDef* find_perspective(PerspectiveId id) {
    for (const auto& p : perspectives())
        if (p.id == id) return &p;
    return nullptr;
}

// ---------------------------------------------------------------------------
// 布局状态（layout.json v1 全量字段）
// ---------------------------------------------------------------------------
struct LayoutState {
    PerspectiveId current = PerspectiveId::Dev;
    int left_w = 260;                 // 左栏宽（夹取 180..420）
    int right_w = 300;                // 右上下文栏宽（夹取 220..420，见设计 §0.2）
    bool context_visible = true;
    std::vector<std::string> pinned_panels;   // 跨视角钉住 📌
    int win_x = -1, win_y = -1;       // -1=首次默认居中
    int win_w = usts::ui::tokens::kWinDefW, win_h = usts::ui::tokens::kWinDefH;

    void clamp();                     // 边界夹取（左/右栏与窗口最小值）
    std::string to_json() const;
    static bool from_json(const std::string& text, LayoutState& out, std::string& err);
};

class LayoutStore {
public:
    static std::wstring path();       // %APPDATA%\USBDevStudio\layout.json
    // 缺文件=默认；损坏=默认+was_corrupt=true（UI 提示一次后照常可用）。
    static bool load(LayoutState& out, bool& was_corrupt);
    static bool save(const LayoutState& s, std::string& err);
};

// ---------------------------------------------------------------------------
// 实现（header-only；模块日志 shell.perspective）
// ---------------------------------------------------------------------------
namespace detail {

inline std::string layout_to_json(const LayoutState& s) {
    wraii::json_writer w;
    w.begin_object();
    w.key(L"v").int_value(1);
    w.key(L"current").int_value(static_cast<long long>(s.current));
    w.key(L"left_w").int_value(s.left_w);
    w.key(L"right_w").int_value(s.right_w);
    w.key(L"context_visible").bool_value(s.context_visible);
    w.key(L"pinned").begin_array();
    for (const auto& p : s.pinned_panels) w.string_value(wraii::utf8_to_wide(p));
    w.end_array();
    w.key(L"win_x").int_value(s.win_x);
    w.key(L"win_y").int_value(s.win_y);
    w.key(L"win_w").int_value(s.win_w);
    w.key(L"win_h").int_value(s.win_h);
    w.end_object();
    return wraii::wide_to_utf8(w.result());
}

inline bool layout_from_json(const std::string& text, LayoutState& out, std::string& err) {
    const std::wstring wide = wraii::utf8_to_wide(text);
    minijson::Value v;
    std::wstring werr;
    if (!minijson::parse(wide, v, &werr) || !v.is_object()) {
        err = "layout JSON 解析失败: " + wraii::wide_to_utf8(werr);
        return false;
    }
    if (const auto* x = v.find(L"v")) {
        if (x->as_int(1) != 1) { err = "layout 版本不识别"; return false; }
    }
    const long long cur = minijson::obj_int(v, L"current",
                                            static_cast<long long>(out.current));
    if (cur < 0 || cur > 3) { err = "layout current 越界"; return false; }
    out.current = static_cast<PerspectiveId>(cur);
    out.left_w = static_cast<int>(minijson::obj_int(v, L"left_w", out.left_w));
    out.right_w = static_cast<int>(minijson::obj_int(v, L"right_w", out.right_w));
    if (const auto* x = v.find(L"context_visible")) out.context_visible = x->boolean;
    out.pinned_panels.clear();
    if (const auto* arr = v.find(L"pinned"); arr && arr->is_array()) {
        for (const auto& e : arr->array)
            out.pinned_panels.push_back(wraii::wide_to_utf8(e.as_str(L"")));
        // 空串条目=脏数据，剔除
        out.pinned_panels.erase(
            std::remove(out.pinned_panels.begin(), out.pinned_panels.end(), std::string()),
            out.pinned_panels.end());
    }
    out.win_x = static_cast<int>(minijson::obj_int(v, L"win_x", out.win_x));
    out.win_y = static_cast<int>(minijson::obj_int(v, L"win_y", out.win_y));
    out.win_w = static_cast<int>(minijson::obj_int(v, L"win_w", out.win_w));
    out.win_h = static_cast<int>(minijson::obj_int(v, L"win_h", out.win_h));
    out.clamp();
    return true;
}

} // namespace detail

inline void LayoutState::clamp() {
    left_w = (std::max)(180, (std::min)(420, left_w));
    right_w = (std::max)(220, (std::min)(420, right_w));
    win_w = (std::max)(usts::ui::tokens::kWinMinW, win_w);
    win_h = (std::max)(usts::ui::tokens::kWinMinH, win_h);
}

inline std::string LayoutState::to_json() const {
    auto log = ustlog::logger("shell.perspective");
    const auto r = detail::layout_to_json(*this);
    log->debug("to_json: {} 字节（current={} pinned={}）", r.size(),
               static_cast<int>(current), pinned_panels.size());
    return r;
}

inline bool LayoutState::from_json(const std::string& text, LayoutState& out,
                                   std::string& err) {
    auto log = ustlog::logger("shell.perspective");
    log->debug("from_json: {} 字节", text.size());
    const bool ok = detail::layout_from_json(text, out, err);
    if (!ok) log->warn("from_json 失败: {}", err);
    return ok;
}

inline std::wstring LayoutStore::path() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    const std::wstring base = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : L".";
    return base + L"\\USBDevStudio\\layout.json";
}

inline bool LayoutStore::load(LayoutState& out, bool& was_corrupt) {
    auto log = ustlog::logger("shell.perspective");
    was_corrupt = false;
    std::wstring text, ferr;
    if (!wraii::read_text_file_utf8(path(), text, &ferr)) {
        log->debug("load: 文件不存在或不可读（按默认布局）");
        out = LayoutState{};
        return true;
    }
    std::string err;
    if (!LayoutState::from_json(wraii::wide_to_utf8(text), out, err)) {
        log->warn("load: 损坏（重置默认）: {}", err);
        out = LayoutState{};
        was_corrupt = true;
    } else {
        log->debug("load: 成功 current={} left_w={}", static_cast<int>(out.current), out.left_w);
    }
    return true;
}

inline bool LayoutStore::save(const LayoutState& s, std::string& err) {
    auto log = ustlog::logger("shell.perspective");
    const std::wstring p = path();
    const std::wstring dir = p.substr(0, p.find_last_of(L'\\'));
    if (!::CreateDirectoryW(dir.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) {
        err = "创建目录失败 GLE=" + std::to_string(::GetLastError());
        log->error("save: {}", err);
        return false;
    }
    const std::string json = s.to_json();
    const std::wstring tmp = p + L".tmp";
    std::wstring werr;
    if (!wraii::write_file_bytes(tmp, json.data(), json.size(), &werr) ||
        !::MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        err = "布局保存失败: " + wraii::wide_to_utf8(werr) +
              " GLE=" + std::to_string(::GetLastError());
        log->error("save: {}", err);
        return false;
    }
    log->debug("save: {} 字节 -> {}", json.size(), wraii::wide_to_utf8(p));
    return true;
}

} // namespace usts::shell
