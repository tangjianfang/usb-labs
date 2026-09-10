// desc_lint.h — 描述符 Linter：25 条规则引擎（设计 §2.2 / 计划 MS1-T5）
//
// 架构（设计"规则库可插件扩展"的第一步——外部规则 DLL 归 MS5 插件 ABI，此处注记）：
//   元信息（id/severity/条款/修复建议）= 内置表，与 rules/desc_rules.json 同源
//   （json 供外部消费/文档化；测试钉"json id 集 == 代码注册集"防漂移）；
//   检测逻辑 = C++ 注册表 {id, fn(model, hits)} 挂接。
// severity：0=err（规范违反，USB3CV 大概率 FAIL）/ 1=warn（可疑）/ 2=info（提示）。
#pragma once

#include "desc_build.h"

#include <algorithm>
#include <string>
#include <vector>

namespace usts::shell::desc {

struct LintHit {
    std::string rule_id;     // "D15"
    int severity = 0;        // 0 err / 1 warn / 2 info
    std::string clause;      // "USB 2.0 §9.6.6"
    std::string message;     // 人话描述（含实测值）
    std::string fix;         // 修复建议
    std::string path;        // 模型内位置（与 diff 同口径）
};

class DescLinter {
public:
    static std::vector<LintHit> run(const DescModel& m);
    static std::string rules_meta_json();   // 25 条元信息（rules/desc_rules.json 同源）
    static size_t rule_count();
};

// ---------------------------------------------------------------------------
// 实现（header-only；模块日志 desc.lint）
// ---------------------------------------------------------------------------
namespace detail {

struct RuleDef {
    const char* id;
    int severity;
    const char* clause;
    const char* fix;
};

inline const std::vector<RuleDef>& rule_defs() {
    static const std::vector<RuleDef> t = {
        {"D01", 0, "USB 2.0 §9.6.1", "块长度与规范长度一致由构建器保证；手改 JSON 后重新生成"},
        {"D02", 0, "USB 2.0 §9.5", "使用规范描述符类型"},
        {"D03", 0, "USB 2.0 §9.6.1", "bcdUSB 取 0x0110/0x0200/0x0210"},
        {"D04", 0, "USB 2.0 §9.6.1", "bMaxPacketSize0 取 8/16/32/64"},
        {"D05", 0, "USB-IF", "idVendor 使用正式申请 VID（0 非法）"},
        {"D06", 0, "USB 2.0 §9.6.3", "wTotalLength 由构建器按实际回填"},
        {"D07", 0, "USB 2.0 §9.6.3", "bNumInterfaces 与接口列表一致"},
        {"D08", 1, "USB 2.0 §9.6.3", "bConfigurationValue ≥ 1（0 保留）"},
        {"D09", 0, "USB 2.0 §9.6.3", "bmAttributes bit7 必须置 1（保留位）"},
        {"D10", 0, "USB 2.0 §9.6.3", "bMaxPower ≤ 250（500mA/2mA 单位）"},
        {"D11", 0, "USB 2.0 §9.6.5", "接口号从 0 连续编号"},
        {"D12", 1, "USB 2.0 §9.6.5", "同一接口的 AlternateSetting 组从 0 起"},
        {"D13", 0, "USB 2.0 §9.6.5", "bNumEndpoints 与端点列表一致（构建器回填）"},
        {"D14", 1, "USB-IF class codes", "bInterfaceClass 使用已分配类码（0x00/0x01/0x02/0x03/0x08/0x0A/0x0E/0xEF/0xFF）"},
        {"D15", 0, "USB 2.0 §9.6.6", "端点号 1..15，EP0 不得出现在配置内"},
        {"D16", 0, "USB 2.0 §9.6.6", "同一接口内端点地址不重复"},
        {"D17", 0, "USB 2.0 §9.6.6", "bmAttributes 高 2 位保留为 0，传输类型 ∈ 控制/同步/批量/中断"},
        {"D18", 0, "USB 2.0 §9.6.6", "全速：批量/中断 ≤64B，同步 ≤1023B"},
        {"D19", 0, "USB 2.0 §9.6.6", "配置内不得定义控制端点（EP0 唯一）"},
        {"D20", 1, "USB 2.0 §9.6.6", "中断 bInterval 1..255；全速同步 1..16"},
        {"D21", 0, "HID 1.11 §7.1", "wDescriptorLength 与报告描述符字节数一致（构建器回填）"},
        {"D22", 0, "HID 1.11 §6.2.2", "报告描述符 COLLECTION/END_COLLECTION 配对且 Item 栈平衡"},
        {"D23", 1, "USB 2.0 §9.6.7", "iXxx 字符串索引不得超过字符串表长度"},
        {"D24", 0, "USB 2.0 §9.6.7", "LANGID 表非空（字符串索引 0 语义）"},
        {"D25", 1, "USB 2.0 IAD ECN", "IAD first_if+count 不越过接口总数且组内连续"},
    };
    return t;
}

inline void hit(std::vector<LintHit>& out, const RuleDef& def, const std::string& path,
                const std::string& message) {
    LintHit h;
    h.rule_id = def.id;
    h.severity = def.severity;
    h.clause = def.clause;
    h.fix = def.fix;
    h.path = path;
    h.message = message;
    out.push_back(std::move(h));
}

inline const RuleDef* find_def(const char* id) {
    for (const auto& d : rule_defs())
        if (std::string(d.id) == id) return &d;
    return nullptr;
}

inline std::string h8(uint32_t v) {
    char b[16];
    snprintf(b, sizeof(b), "%#04x", v);
    return b;
}

// HID 报告 Item 快速走查：Main(0x0C~0x0F mask) 的 COLLECTION(0xA1)/END(0xC0) 配对
inline int collection_balance(const std::string& report_hex) {
    const auto bytes = hex_bytes(report_hex);
    int depth = 0, min_depth = 0;
    bool has_item = false;
    for (size_t i = 0; i < bytes.size();) {
        const uint8_t item = bytes[i];
        const uint8_t type = item & 0xFC;
        if (type == 0xA0) { ++depth; has_item = true; }        // Main: COLLECTION 系列
        else if (type == 0xC0) { --depth; has_item = true; }   // Main: END_COLLECTION
        if (depth < min_depth) min_depth = depth;
        // 长度：后两 bit 编码 0/1/2/4 字节（长 Item 0xFE 另计——教学样例无）
        static const uint8_t kLen[4] = {0, 1, 2, 4};
        const uint8_t n = (item & 0x03) == 0x03 ? 4 : kLen[item & 0x03];
        i += 1 + n;
        if (i > bytes.size()) break;
    }
    return has_item ? (depth == 0 && min_depth >= 0 ? 0 : depth) : -1;   // 0=平衡
}

} // namespace detail

inline std::vector<LintHit> DescLinter::run(const DescModel& m) {
    auto log = ustlog::logger("desc.lint");
    std::vector<LintHit> out;
    const auto& defs = detail::rule_defs();
    const auto D = [&] (const char* id) { return detail::find_def(id); };

    // —— 设备级 ——
    if (D("D03") && m.device.bcd_usb != 0x0110 && m.device.bcd_usb != 0x0200 &&
        m.device.bcd_usb != 0x0210)
        detail::hit(out, *D("D03"), "device.bcdUSB",
                    "bcdUSB=" + detail::h8(m.device.bcd_usb) + " 非常见版本");
    if (D("D04") && m.device.max_packet0 != 8 && m.device.max_packet0 != 16 &&
        m.device.max_packet0 != 32 && m.device.max_packet0 != 64)
        detail::hit(out, *D("D04"), "device.bMaxPacketSize0",
                    "bMaxPacketSize0=" + detail::h8(m.device.max_packet0) + " 非法");
    if (D("D05") && m.device.vid == 0)
        detail::hit(out, *D("D05"), "device.idVendor", "idVendor=0（未申请 VID）");
    if (D("D24") && m.strings.langids.empty())
        detail::hit(out, *D("D24"), "strings.langids", "LANGID 表为空");

    // —— 字符串索引（设备级三索引）——
    {
        const std::wstring key = m.strings.langids.empty() ? L"" : m.strings.langids[0];
        const auto it = m.strings.table.find(key);
        const size_t nstr = (it == m.strings.table.end()) ? 0 : it->second.size();
        auto check_idx = [&] (uint8_t idx, const char* path) {
            if (D("D23") && idx > nstr)
                detail::hit(out, *D("D23"), path,
                            "索引 " + std::to_string(idx) + " > 字符串表 " +
                                std::to_string(nstr));
        };
        check_idx(m.device.i_man, "device.iManufacturer");
        check_idx(m.device.i_prod, "device.iProduct");
        check_idx(m.device.i_serial, "device.iSerialNumber");
    }

    for (size_t ci = 0; ci < m.configs.size(); ++ci) {
        const ConfigDesc& c = m.configs[ci];
        const std::string cpath = "configs[" + std::to_string(ci) + "]";
        if (D("D08") && c.value == 0)
            detail::hit(out, *D("D08"), cpath + ".bConfigurationValue", "配置值 0 保留");
        if (D("D09") && !(c.attributes & 0x80))
            detail::hit(out, *D("D09"), cpath + ".bmAttributes", "bit7 未置 1");
        if (D("D10") && c.max_power > 250)
            detail::hit(out, *D("D10"), cpath + ".bMaxPower",
                        std::to_string(c.max_power * 2) + "mA 超过 500mA 上限");

        // 接口号连续（D11）：收集 number 集合
        {
            std::vector<int> nums;
            for (const auto& f : c.interfaces) nums.push_back(f.number);
            std::sort(nums.begin(), nums.end());
            nums.erase(std::unique(nums.begin(), nums.end()), nums.end());
            bool contiguous = !nums.empty() && nums.front() == 0 &&
                              nums.size() == (nums.empty() ? 0 : nums.back() + 1);
            if (D("D11") && !contiguous)
                detail::hit(out, *D("D11"), cpath + ".interfaces",
                            "接口号非 0 起连续（实测 " + std::to_string(nums.size()) +
                                " 个唯一号）");
            // IAD（D25）
            int total_if = static_cast<int>(nums.empty() ? 0 : nums.back() + 1);
            for (size_t fi = 0; fi < c.interfaces.size(); ++fi) {
                const auto& f = c.interfaces[fi];
                if (f.iad && D("D25") &&
                    (f.iad->first_if + f.iad->if_count > total_if))
                    detail::hit(out, *D("D25"),
                                cpath + ".interfaces[" + std::to_string(fi) + "].iad",
                                "first_if+" + std::to_string(f.iad->if_count) +
                                    " 超过接口总数 " + std::to_string(total_if));
            }
        }

        // alt 分组从 0 起（D12）
        {
            std::vector<int> seen_numbers;
            for (size_t fi = 0; fi < c.interfaces.size(); ++fi) {
                const auto& f = c.interfaces[fi];
                const bool first_of_number =
                    std::find(seen_numbers.begin(), seen_numbers.end(), f.number) ==
                    seen_numbers.end();
                if (first_of_number) {
                    seen_numbers.push_back(f.number);
                    if (D("D12") && f.alt != 0)
                        detail::hit(out, *D("D12"),
                                    cpath + ".interfaces[" + std::to_string(fi) +
                                        "].bAlternateSetting",
                                    "接口 " + std::to_string(f.number) + " 首个 alt=" +
                                        std::to_string(f.alt) + " 应为 0");
                }
            }
        }

        for (size_t fi = 0; fi < c.interfaces.size(); ++fi) {
            const InterfaceDesc& f = c.interfaces[fi];
            const std::string fpath = cpath + ".interfaces[" + std::to_string(fi) + "]";
            if (D("D14")) {
                static const int kKnown[] = {0x00, 0x01, 0x02, 0x03, 0x06, 0x07,
                                             0x08, 0x09, 0x0A, 0x0B, 0x0E, 0xEF,
                                             0xDC, 0xFF};
                bool known = false;
                for (int k : kKnown) known = known || (f.if_class == k);
                if (!known)
                    detail::hit(out, *D("D14"), fpath + ".bInterfaceClass",
                                "类码 " + detail::h8(f.if_class) + " 未分配");
            }
            if (f.hid && D("D22")) {
                const int bal = detail::collection_balance(f.hid->report_hex);
                if (bal != 0)
                    detail::hit(out, *D("D22"), fpath + ".hid.report",
                                "COLLECTION 配对不平衡（深度差=" + std::to_string(bal) +
                                    "）");
            }
            std::vector<uint8_t> addrs;
            for (size_t ei = 0; ei < f.endpoints.size(); ++ei) {
                const EndpointDesc& e = f.endpoints[ei];
                const std::string epath = fpath + ".endpoints[" + std::to_string(ei) + "]";
                const uint8_t num = e.address & 0x0F;
                if (D("D15") && (num == 0 || num > 15))
                    detail::hit(out, *D("D15"), epath + ".bEndpointAddress",
                                "端点号 " + std::to_string(num) + " 非法");
                if (D("D16") && std::find(addrs.begin(), addrs.end(), e.address) != addrs.end())
                    detail::hit(out, *D("D16"), epath + ".bEndpointAddress",
                                "地址 " + detail::h8(e.address) + " 组内重复");
                addrs.push_back(e.address);
                const uint8_t xfer = e.attributes & 0x03;
                if (D("D17") && (e.attributes & 0xC0) != 0)
                    detail::hit(out, *D("D17"), epath + ".bmAttributes",
                                "保留位非零 " + detail::h8(e.attributes));
                if (D("D19") && xfer == 0)
                    detail::hit(out, *D("D19"), epath + ".bmAttributes",
                                "配置内出现控制端点");
                if (D("D18")) {
                    const uint16_t mp = e.max_packet & 0x07FF;
                    bool bad = false;
                    if (xfer == 2 || xfer == 3) bad = mp > 64;          // 批量/中断
                    else if (xfer == 1) bad = mp > 1023;                // 同步
                    if (bad)
                        detail::hit(out, *D("D18"), epath + ".wMaxPacketSize",
                                    std::to_string(mp) + "B 超全速上限");
                }
                if (D("D20")) {
                    if (xfer == 3 && (e.interval < 1 || e.interval > 255))
                        detail::hit(out, *D("D20"), epath + ".bInterval",
                                    "中断间隔 " + std::to_string(e.interval) + " 出界");
                    if (xfer == 1 && (e.interval < 1 || e.interval > 16))
                        detail::hit(out, *D("D20"), epath + ".bInterval",
                                    "全速同步间隔 " + std::to_string(e.interval) + " 出界");
                }
            }
        }
    }
    // 稳定排序：err < warn < info，同级按 path
    std::stable_sort(out.begin(), out.end(), [](const LintHit& a, const LintHit& b) {
        if (a.severity != b.severity) return a.severity < b.severity;
        return a.path < b.path;
    });
    log->debug("lint run: {} 命中（模型 configs={}）", out.size(), m.configs.size());
    return out;
}

inline size_t DescLinter::rule_count() { return detail::rule_defs().size(); }

inline std::string DescLinter::rules_meta_json() {
    wraii::json_writer w;
    w.begin_object();
    w.key(L"v").int_value(1);
    w.key(L"rules").begin_array();
    for (const auto& d : detail::rule_defs()) {
        w.begin_object();
        w.key(L"id").string_value(wraii::utf8_to_wide(d.id));
        w.key(L"severity").int_value(d.severity);
        w.key(L"clause").string_value(wraii::utf8_to_wide(d.clause));
        w.key(L"fix").string_value(wraii::utf8_to_wide(d.fix));
        w.end_object();
    }
    w.end_array();
    w.end_object();
    return wraii::wide_to_utf8(w.result());
}

} // namespace usts::shell::desc
