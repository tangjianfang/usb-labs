// settings.h — 设置中心数据层（设计附录 E.4 / 计划 MS0-T2）
//
// 契约：
//   1. 持久化 %APPDATA%\USBDevStudio\settings.json，信封 {"v":1,...}，字段只增不改；
//   2. 损坏/缺键=保默认（损坏时 was_corrupt=true 供 UI 提示一次）；
//   3. 保存=临时文件+MoveFileEx 原子替换，失败返回 err（不抛异常）。
#pragma once

#include "../app/log.h"
#include "../framework/json_mini.h"
#include "../framework/win32_rai.h"

#include <string>

namespace usts::shell {

struct Settings {
    // —— 通用 ——
    std::wstring language = L"zh-CN";   // "zh-CN" | "en"（en 界面=P1，值仍可存）
    int theme = 0;                      // 0 浅色 1 深色 2 跟随系统
    bool open_last_layout = true;
    // —— 产线 ——
    std::wstring station = L"STN-01";
    bool sound_on = true;
    int sound_volume = 80;              // 0..100
    int pass_dwell_ms = 1500;
    bool auto_next = true;
    bool scan_go = true;
    // —— 日志 ——
    std::wstring log_level = L"INFO";
    int log_retention_days = 7;
    // —— 更新 ——
    int update_channel = 0;             // 0 正式 1 测试
    bool update_auto = true;
    // —— 高级 ——
    std::wstring report_dir = L"reports";
    int scan_interval_ms = 3000;

    std::string to_json() const;
    static bool from_json(const std::string& text, Settings& out, std::string& err);
};

class SettingsStore {
public:
    static std::wstring path();   // %APPDATA%\USBDevStudio\settings.json
    // 缺文件=默认值（was_corrupt=false）；存在但解析失败=默认值+was_corrupt=true。
    static bool load(Settings& out, bool& was_corrupt);
    static bool save(const Settings& s, std::string& err);
};

// ---------------------------------------------------------------------------
// 实现（header-only，单 TU 包含即可；模块日志 shell.settings）
// ---------------------------------------------------------------------------
namespace detail {

inline std::string settings_to_json(const Settings& s) {
    wraii::json_writer w;
    w.begin_object();
    w.key(L"v").int_value(1);
    w.key(L"language").string_value(s.language);
    w.key(L"theme").int_value(s.theme);
    w.key(L"open_last_layout").bool_value(s.open_last_layout);
    w.key(L"station").string_value(s.station);
    w.key(L"sound_on").bool_value(s.sound_on);
    w.key(L"sound_volume").int_value(s.sound_volume);
    w.key(L"pass_dwell_ms").int_value(s.pass_dwell_ms);
    w.key(L"auto_next").bool_value(s.auto_next);
    w.key(L"scan_go").bool_value(s.scan_go);
    w.key(L"log_level").string_value(s.log_level);
    w.key(L"log_retention_days").int_value(s.log_retention_days);
    w.key(L"update_channel").int_value(s.update_channel);
    w.key(L"update_auto").bool_value(s.update_auto);
    w.key(L"report_dir").string_value(s.report_dir);
    w.key(L"scan_interval_ms").int_value(s.scan_interval_ms);
    w.end_object();
    return wraii::wide_to_utf8(w.result());
}

inline bool settings_from_json(const std::string& text, Settings& out, std::string& err) {
    const std::wstring wide = wraii::utf8_to_wide(text);
    minijson::Value v;
    std::wstring werr;
    if (!minijson::parse(wide, v, &werr) || !v.is_object()) {
        err = "settings JSON 解析失败: " + wraii::wide_to_utf8(werr);
        return false;
    }
    if (const auto* x = v.find(L"v")) {
        if (x->as_int(1) != 1) { err = "settings 版本不识别"; return false; }
    }
    out.language = minijson::obj_str(v, L"language", out.language);
    out.theme = static_cast<int>(minijson::obj_int(v, L"theme", out.theme));
    out.open_last_layout = v.find(L"open_last_layout")
        ? v.find(L"open_last_layout")->boolean : out.open_last_layout;
    out.station = minijson::obj_str(v, L"station", out.station);
    if (const auto* x = v.find(L"sound_on")) out.sound_on = x->boolean;
    out.sound_volume = static_cast<int>(minijson::obj_int(v, L"sound_volume", out.sound_volume));
    out.pass_dwell_ms = static_cast<int>(minijson::obj_int(v, L"pass_dwell_ms", out.pass_dwell_ms));
    if (const auto* x = v.find(L"auto_next")) out.auto_next = x->boolean;
    if (const auto* x = v.find(L"scan_go")) out.scan_go = x->boolean;
    out.log_level = minijson::obj_str(v, L"log_level", out.log_level);
    out.log_retention_days =
        static_cast<int>(minijson::obj_int(v, L"log_retention_days", out.log_retention_days));
    out.update_channel =
        static_cast<int>(minijson::obj_int(v, L"update_channel", out.update_channel));
    if (const auto* x = v.find(L"update_auto")) out.update_auto = x->boolean;
    out.report_dir = minijson::obj_str(v, L"report_dir", out.report_dir);
    out.scan_interval_ms =
        static_cast<int>(minijson::obj_int(v, L"scan_interval_ms", out.scan_interval_ms));
    return true;
}

} // namespace detail

inline std::string Settings::to_json() const {
    auto log = ustlog::logger("shell.settings");
    auto r = detail::settings_to_json(*this);
    log->debug("to_json: {} 字节（station={} theme={}）", r.size(),
               wraii::wide_to_utf8(station), theme);
    return r;
}

inline bool Settings::from_json(const std::string& text, Settings& out, std::string& err) {
    auto log = ustlog::logger("shell.settings");
    log->debug("from_json: {} 字节", text.size());
    const bool ok = detail::settings_from_json(text, out, err);
    if (!ok) log->warn("from_json 失败: {}", err);
    return ok;
}

inline std::wstring SettingsStore::path() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    const std::wstring base = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : L".";
    return base + L"\\USBDevStudio\\settings.json";
}

inline bool SettingsStore::load(Settings& out, bool& was_corrupt) {
    auto log = ustlog::logger("shell.settings");
    const std::wstring p = path();
    was_corrupt = false;
    std::wstring text, ferr;
    if (!wraii::read_text_file_utf8(p, text, &ferr)) {
        log->debug("load: 文件不存在或不可读（按默认值）: {}", wraii::wide_to_utf8(ferr));
        out = Settings{};
        return true;   // 缺文件=默认，非错误
    }
    std::string err;
    if (!Settings::from_json(wraii::wide_to_utf8(text), out, err)) {
        log->warn("load: 损坏（重置默认）: {}", err);
        out = Settings{};
        was_corrupt = true;
    } else {
        log->debug("load: 成功 station={}", wraii::wide_to_utf8(out.station));
    }
    return true;
}

inline bool SettingsStore::save(const Settings& s, std::string& err) {
    auto log = ustlog::logger("shell.settings");
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
    if (!wraii::write_file_bytes(tmp, json.data(), json.size(), &werr)) {
        err = "写临时文件失败: " + wraii::wide_to_utf8(werr);
        log->error("save: {}", err);
        return false;
    }
    if (!::MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        err = "原子替换失败 GLE=" + std::to_string(::GetLastError());
        log->error("save: {}", err);
        return false;
    }
    log->debug("save: {} 字节 -> {}", json.size(), wraii::wide_to_utf8(p));
    return true;
}

} // namespace usts::shell
