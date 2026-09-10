// te_model.h — 计划模型（W5 Test Explorer / 计划 MS3-T1）
// 契约：字段名与 tools/usbtest 及本应用旧引擎同源（type/name/limits）；文件=JSON 形状
// （三端契约同名同义；YAML 解析归滚动——诚实账见合并计划）。
#pragma once

#include "../app/log.h"
#include "../framework/json_mini.h"
#include "../framework/win32_rai.h"

#include <string>
#include <vector>

namespace usts::shell::te {

struct PlanStep {
    std::string type;        // enumerate / hid_polling_rate / …（23 型契约）
    std::string name;
    // 通用参数桶（表单即数据的运行形态：mock/虚拟后端只消费 min_interfaces/min_hz 等
    // 少数键；完整 23 型分派归真机后端域）
    std::vector<std::pair<std::string, std::string>> params;
    // limits（min/max 数值键值对）
    std::vector<std::pair<std::string, double>> limits;
    std::string param(const std::string& k, const std::string& dft = "") const;
    double limit(const std::string& k, double dft = 0) const;
};

struct PlanModel {
    std::wstring path;       // 来源文件（空=内存构造）
    std::string name = "未命名计划";
    std::string backend = "mock";   // mock | virtual（真机=--legacy 域）
    std::vector<PlanStep> steps;
    std::string to_json() const;
    static bool from_json(const std::string& text, PlanModel& out, std::string& err);
};

// 计划目录扫描（*.json 含 "steps" 数组者视为计划；坏文件跳过并计数）
struct PlanScan {
    std::vector<PlanModel> plans;
    size_t skipped = 0;
};
PlanScan scan_plans(const std::wstring& dir);

// ---------------------------------------------------------------------------
// 实现（header-only；模块日志 te.model）
// ---------------------------------------------------------------------------
inline std::string PlanStep::param(const std::string& k, const std::string& dft) const {
    for (const auto& [key, v] : params)
        if (key == k) return v;
    return dft;
}
inline double PlanStep::limit(const std::string& k, double dft) const {
    for (const auto& [key, v] : limits)
        if (key == k) return v;
    return dft;
}

inline std::string PlanModel::to_json() const {
    wraii::json_writer w;
    w.begin_object();
    w.key(L"v").int_value(1);
    w.key(L"name").string_value(wraii::utf8_to_wide(name));
    w.key(L"backend").string_value(wraii::utf8_to_wide(backend));
    w.key(L"steps").begin_array();
    for (const auto& s : steps) {
        w.begin_object();
        w.key(L"type").string_value(wraii::utf8_to_wide(s.type));
        w.key(L"name").string_value(wraii::utf8_to_wide(s.name));
        if (!s.params.empty()) {
            w.key(L"params").begin_object();
            for (const auto& [k, v] : s.params)
                w.key(wraii::utf8_to_wide(k)).string_value(wraii::utf8_to_wide(v));
            w.end_object();
        }
        if (!s.limits.empty()) {
            w.key(L"limits").begin_object();
            for (const auto& [k, v] : s.limits)
                w.key(wraii::utf8_to_wide(k)).double_value(v);
            w.end_object();
        }
        w.end_object();
    }
    w.end_array();
    w.end_object();
    return wraii::wide_to_utf8(w.result());
}

inline bool PlanModel::from_json(const std::string& text, PlanModel& out,
                                 std::string& err) {
    auto log = ustlog::logger("te.model");
    const std::wstring wide = wraii::utf8_to_wide(text);
    minijson::Value v;
    std::wstring werr;
    if (!minijson::parse(wide, v, &werr) || !v.is_object()) {
        err = "计划 JSON 解析失败";
        return false;
    }
    out.name = wraii::wide_to_utf8(minijson::obj_str(v, L"name", L"未命名计划"));
    out.backend = wraii::wide_to_utf8(minijson::obj_str(v, L"backend", L"mock"));
    const auto* steps = v.find(L"steps");
    if (!steps || !steps->is_array()) { err = "计划缺 steps"; return false; }
    out.steps.clear();
    for (const auto& e : steps->array) {
        PlanStep s;
        s.type = wraii::wide_to_utf8(minijson::obj_str(e, L"type", L""));
        if (s.type.empty()) { err = "步骤缺 type"; return false; }
        s.name = wraii::wide_to_utf8(minijson::obj_str(e, L"name", wraii::utf8_to_wide(s.type)));
        if (const auto* ps = e.find(L"params"); ps && ps->is_object())
            for (const auto& [k, val] : ps->members)
                s.params.emplace_back(wraii::wide_to_utf8(k),
                                      wraii::wide_to_utf8(val.as_str(L"")));
        if (const auto* ls = e.find(L"limits"); ls && ls->is_object())
            for (const auto& [k, val] : ls->members)
                s.limits.emplace_back(wraii::wide_to_utf8(k), val.as_double(0));
        out.steps.push_back(std::move(s));
    }
    log->debug("计划加载: {} 步（backend={}）", out.steps.size(), out.backend);
    return true;
}

inline PlanScan scan_plans(const std::wstring& dir) {
    auto log = ustlog::logger("te.model");
    PlanScan out;
    WIN32_FIND_DATAW fd;
    const HANDLE h = ::FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring p = dir + L"\\" + fd.cFileName;
        std::wstring text, ferr;
        if (!wraii::read_text_file_utf8(p, text, &ferr)) { ++out.skipped; continue; }
        PlanModel m;
        std::string err;
        if (PlanModel::from_json(wraii::wide_to_utf8(text), m, err)) {
            m.path = p;
            out.plans.push_back(std::move(m));
        } else {
            ++out.skipped;
            log->debug("跳过非计划文件: {}", wraii::wide_to_utf8(p));
        }
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    log->debug("scan_plans: {} 计划 {} 跳过", out.plans.size(), out.skipped);
    return out;
}

} // namespace usts::shell::te
