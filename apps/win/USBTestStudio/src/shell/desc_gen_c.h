// desc_gen_c.h — 模型 → C 源码生成（设计 §2.2 生成▾ / 计划 MS1-T4）
//
// 模板（设计写 0.15/0.17 两版——实际差异仅命名注释级，MS1 收敛为两档并注明：
//   TinyUsb   = TinyUSB 设备栈数组风格（desc_device/desc_configuration/hid_report_descriptor/
//               string_desc_utf16 数组，0.15 与 0.17 通用，版本差异在回调注册处不在数组）
//   PlainArrays = 通用 const uint8_t 数组（无 TinyUSB 命名约定，自研栈/教学用）
#pragma once

#include "desc_build.h"

#include <string>

namespace usts::shell::desc {

enum class GenTemplate { TinyUsb, PlainArrays };

struct GenOptions {
    GenTemplate tpl = GenTemplate::TinyUsb;
    const char* model_name = "device";   // 生成头注释与数组前缀来源
};

std::string generate_c(const DescModel& m, const GenOptions& opt);

// ---------------------------------------------------------------------------
// 实现（header-only；模块日志 desc.gen）
// ---------------------------------------------------------------------------
namespace detail {

inline std::string hex_lines(const std::vector<uint8_t>& v, int indent,
                             const char* tail_last) {
    std::string out, pad(indent, ' ');
    char buf[16];
    for (size_t i = 0; i < v.size(); ++i) {
        if (i % 16 == 0) out += pad;
        snprintf(buf, sizeof(buf), "0x%02X", v[i]);
        out += buf;
        const bool last = i + 1 == v.size();
        out += last ? tail_last : ", ";
        if (i % 16 == 15 && !last) out += "\n";
    }
    if (!v.empty()) out += "\n";
    return out;
}

inline std::string wide_to_utf8_escaped(const std::wstring& s) {
    // C 源内字符串以 UTF-8 字面量输出（TinyUSB 字符串走 UTF-16 数组，此处用于注释）
    return wraii::wide_to_utf8(s);
}

} // namespace detail

inline std::string generate_c(const DescModel& m, const GenOptions& opt) {
    auto log = ustlog::logger("desc.gen");
    std::string c;
    const bool tiny = opt.tpl == GenTemplate::TinyUsb;
    c += "// 由 USB DevStudio 生成 —— 模型: ";
    c += opt.model_name;
    c += ".ustsdesc（TinyUSB 0.15/0.17 数组通用；手改会被下次生成覆盖）\n";
    c += "#pragma once\n\n#include <stdint.h>\n\n";

    // —— 设备描述符 ——
    const auto dev = build_device_desc(m.device);
    c += tiny ? "const uint8_t desc_device[18] = {\n"
              : "const uint8_t dev_desc[18] = {\n";
    c += detail::hex_lines(dev, 4, "");
    c += "};\n\n";

    // —— 配置 blob ——
    if (!m.configs.empty()) {
        const auto cfg = build_config_blob(m.configs[0]);
        c += tiny ? "const uint8_t desc_fs_configuration[] = {\n"
                  : "const uint8_t cfg_desc[] = {\n";
        c += detail::hex_lines(cfg, 4, "");
        c += "};  // ";
        char n[32];
        snprintf(n, sizeof(n), "%zu 字节\n\n", cfg.size());
        c += n;
    }

    // —— HID 报告描述符（每 HID 接口一根）——
    int hid_idx = 0;
    for (const auto& f : m.configs.empty() ? std::vector<InterfaceDesc>{}
                                           : m.configs[0].interfaces) {
        if (!f.hid || f.hid->report_hex.empty()) continue;
        const auto rpt = detail::hex_bytes(f.hid->report_hex);
        c += tiny ? "// HID 报告描述符（接口 " : "// 接口 ";
        c += std::to_string(f.number);
        c += tiny ? "，wDescriptorLength=" : " 报告描述符 ";
        c += std::to_string(rpt.size());
        c += " 字节）\n";
        c += tiny ? "const uint8_t hid_report_descriptor[] = {\n"
                  : "const uint8_t hid_report_";
        if (!tiny) c += std::to_string(hid_idx) + "[] = {\n";
        c += detail::hex_lines(rpt, 4, "");
        c += "};\n\n";
        ++hid_idx;
    }

    // —— 字符串（TinyUSB 风格 UTF-16 数组：[0]=LANGID 后各串）——
    if (!m.strings.langids.empty()) {
        c += "// 字符串描述符（索引 0=LANGID；1..N 对应 iManufacturer/iProduct/iSerial）\n";
        c += tiny ? "const uint16_t string_desc_arr[][16] = {\n"
                  : "const uint16_t str_desc[][16] = {\n";
        std::string lang = "(uint16_t)0x";
        const std::wstring l0 = m.strings.langids[0];
        lang += wraii::wide_to_utf8(l0.substr(l0.size() >= 2 ? 2 : 0));   // 去 "0x"
        c += "    { " + lang + " },\n";
        const auto it = m.strings.table.find(l0);
        if (it != m.strings.table.end())
            for (const auto& s : it->second) {
                std::string row = "    { ";
                for (size_t k = 0; k < s.size() && k < 15; ++k) {
                    char b[10];
                    snprintf(b, sizeof(b), "0x%04X, ", static_cast<unsigned>(s[k]));
                    row += b;
                }
                c += row + "},  // " + detail::wide_to_utf8_escaped(s) + "\n";
            }
        c += "};\n";
    }
    log->debug("generate_c: {} 字节（模板={}）", c.size(),
               tiny ? "TinyUsb" : "PlainArrays");
    return c;
}

} // namespace usts::shell::desc
