// desc_parse.h — 反编译内核：二进制 → 模型（设计 §2.2 / 计划 MS1-T2）
//
// 输入口径（真机 GET_DESCRIPTOR 同构；MS1 源=文件，真机链路归 MS2）：
//   parse_device_desc：18B 设备描述符
//   parse_config_blob：GET_CONFIGURATION 全量（config 头 + 嵌套 if/iad/hid/ep）
//   parse_string_set：字符串描述符序列（[0]=LANGID，[1..N]=UTF-16LE）
//   attach_report：报告描述符（0x22）单独注入接口（模型 report_hex）
// 保真：不可识别 type / 长度异常块 → UnknownBlock raw（不失败）；bLength=0/越界=err。
#pragma once

#include "desc_model.h"

#include <string>
#include <vector>

namespace usts::shell::desc {

bool parse_device_desc(const uint8_t* p, size_t n, DeviceDesc& out, std::string& err);
// unknown_out：配置 blob 内无法识别的块（after="configs[0].<序>"）
bool parse_config_blob(const uint8_t* p, size_t n, ConfigDesc& out,
                       std::vector<UnknownBlock>& unknown_out, std::string& err);
bool parse_string_set(const uint8_t* p, size_t n, StringTable& out, std::string& err);
void attach_report(InterfaceDesc& ifc, const uint8_t* p, size_t n);   // report_hex 注入

namespace detail {

inline std::string to_hex_str(const uint8_t* p, size_t n) {
    static const char kHex[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s += kHex[p[i] >> 4];
        s += kHex[p[i] & 0xF];
    }
    return s;
}

inline uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

} // namespace detail

// ---------------------------------------------------------------------------
// 实现（header-only；模块日志 desc.parse）
// ---------------------------------------------------------------------------
inline bool parse_device_desc(const uint8_t* p, size_t n, DeviceDesc& out,
                              std::string& err) {
    auto log = ustlog::logger("desc.parse");
    if (n < 18 || p[0] != 18 || p[1] != kTypeDevice) {
        err = "设备描述符形状非法（长度/类型）";
        log->warn("parse_device_desc: {}", err);
        return false;
    }
    out.bcd_usb = detail::rd16(p + 2);
    out.dev_class = p[4]; out.sub_class = p[5]; out.protocol = p[6];
    out.max_packet0 = p[7];
    out.vid = detail::rd16(p + 8);
    out.pid = detail::rd16(p + 10);
    out.bcd_device = detail::rd16(p + 12);
    out.i_man = p[14]; out.i_prod = p[15]; out.i_serial = p[16];
    out.num_configs = p[17];
    log->debug("parse_device_desc: vid={:#06x} pid={:#06x} bcdUSB={:#06x}", out.vid,
               out.pid, out.bcd_usb);
    return true;
}

inline bool parse_config_blob(const uint8_t* p, size_t n, ConfigDesc& out,
                              std::vector<UnknownBlock>& unknown_out, std::string& err) {
    auto log = ustlog::logger("desc.parse");
    if (n < 9 || p[1] != kTypeConfiguration) {
        err = "配置描述符形状非法";
        return false;
    }
    const uint16_t total = detail::rd16(p + 2);
    if (total > n) { err = "wTotalLength 越界"; return false; }
    out.value = p[5]; out.i_cfg = p[6];
    out.attributes = p[7]; out.max_power = p[8];
    out.interfaces.clear();
    unknown_out.clear();

    size_t i = 9;
    int if_seq = 0;
    InterfaceDesc* cur = nullptr;
    while (i < total) {
        const uint8_t len = p[i], type = p[i + 1];
        if (len < 2 || i + len > total) { err = "子描述符 bLength 越界"; return false; }
        switch (type) {
        case kTypeInterface:
            if (len < 9) { err = "接口描述符长度不足"; return false; }
            out.interfaces.push_back({});
            cur = &out.interfaces.back();
            cur->number = p[i + 2]; cur->alt = p[i + 3];
            cur->if_class = p[i + 5]; cur->sub_class = p[i + 6]; cur->protocol = p[i + 7];
            cur->i_if = p[i + 8];
            ++if_seq;
            break;
        case kTypeEndpoint:
            if (!cur || len < 7) { err = "端点描述符出现在接口外/长度不足"; return false; }
            {
                EndpointDesc e;
                e.address = p[i + 2]; e.attributes = p[i + 3];
                e.max_packet = detail::rd16(p + i + 4);
                e.interval = p[i + 6];
                cur->endpoints.push_back(e);
            }
            break;
        case kTypeHid:
            if (!cur || len < 9) { err = "HID 描述符出现在接口外/长度不足"; return false; }
            {
                HidDesc h;
                h.bcd_hid = detail::rd16(p + i + 2);
                h.country = p[i + 4];
                h.report_hex.clear();   // 长度在 p[7..8]，报告本体经 attach_report 注入
                cur->hid = h;
            }
            break;
        case kTypeIad:
            if (len < 8) { err = "IAD 描述符长度不足"; return false; }
            {
                IadDesc d;
                d.first_if = p[i + 2]; d.if_count = p[i + 3];
                d.fn_class = p[i + 4]; d.fn_sub = p[i + 5]; d.fn_protocol = p[i + 6];
                d.i_function = p[i + 7];
                if (cur) cur->iad = d;   // 挂当前/下一接口（规范置首接口前，容错双挂）
                else if (!out.interfaces.empty())
                    out.interfaces.back().iad = d;
            }
            break;
        default: {
            // 未识别块：raw 保真（含 bLength/Type），挂 "configs[0].<描述符序>"
            UnknownBlock u;
            u.after = "configs[0]." + std::to_string(if_seq);
            u.raw_hex = detail::to_hex_str(p + i, len);
            unknown_out.push_back(std::move(u));
            log->debug("parse_config_blob: 未识别块 type={:#04x} len={} → raw 保真",
                       type, len);
            break;
        }
        }
        i += len;
    }
    log->debug("parse_config_blob: {} 接口 {} 未识别块 total={}", out.interfaces.size(),
               unknown_out.size(), total);
    return true;
}

inline bool parse_string_set(const uint8_t* p, size_t n, StringTable& out,
                             std::string& err) {
    auto log = ustlog::logger("desc.parse");
    out.langids.clear();
    out.table.clear();
    size_t i = 0;
    bool first = true;
    while (i < n) {
        const uint8_t len = p[i];
        if (len < 2 || i + len > n) { err = "字符串描述符 bLength 越界"; return false; }
        if (p[i + 1] != kTypeString) { err = "非字符串描述符类型"; return false; }
        if (first) {
            // [0] = LANGID 描述符：04 03 <langid 数组 LE>
            for (size_t k = 2; k + 1 < len; k += 2) {
                wchar_t buf[16];
                swprintf(buf, 16, L"0x%04X", detail::rd16(p + i + k));
                out.langids.push_back(buf);
                out.table[buf] = {};   // 每语言独立数组
            }
            first = false;
        } else {
            // UTF-16LE → 宽串（Windows wchar_t==2B 直转）
            std::wstring s(reinterpret_cast<const wchar_t*>(p + i + 2),
                           (len - 2) / 2);
            for (auto& [lang, arr] : out.table) arr.push_back(s);
        }
        i += len;
    }
    if (first) { err = "空字符串集"; return false; }
    log->debug("parse_string_set: {} 语言，首语言 {} 串", out.langids.size(),
               out.table.empty() ? 0 : out.table.begin()->second.size());
    return true;
}

inline void attach_report(InterfaceDesc& ifc, const uint8_t* p, size_t n) {
    auto log = ustlog::logger("desc.parse");
    if (!ifc.hid) ifc.hid = HidDesc{};
    ifc.hid->report_hex = detail::to_hex_str(p, n);
    log->debug("attach_report: {} 字节 → 接口 {}", n * 2 ? n : 0, ifc.number);
}

} // namespace usts::shell::desc
