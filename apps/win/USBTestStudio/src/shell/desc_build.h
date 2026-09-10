// desc_build.h — 构建：模型 → 二进制（设计 §2.2 生成▾ / 计划 MS1-T3）
//
// 契约：
//   1. build_config_blob 回填一致性字段：wTotalLength/bNumInterfaces/bNumEndpoints/
//      HID bNumDescriptors+wDescriptorLength（与模型 report_hex 字节数一致）；
//   2. 字符串：build_string_set 产出 [LANGID 描述符][串1][串2]…（UTF-16LE，含中文）；
//   3. 性质（测试钉死）：parse(build(m)) ≡ m；build(parse(b)) ≡ b（well-formed）。
#pragma once

#include "desc_model.h"
#include "desc_parse.h"

#include <string>
#include <vector>

namespace usts::shell::desc {

std::vector<uint8_t> build_device_desc(const DeviceDesc& d);
std::vector<uint8_t> build_config_blob(const ConfigDesc& c);
std::vector<uint8_t> build_string_set(const StringTable& st);

namespace detail {

inline void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>(x >> 8));
}

inline std::vector<uint8_t> hex_bytes(const std::string& hex) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        const auto nib = [] (char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) break;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

} // namespace detail

// ---------------------------------------------------------------------------
// 实现（header-only；模块日志 desc.build）
// ---------------------------------------------------------------------------
inline std::vector<uint8_t> build_device_desc(const DeviceDesc& d) {
    auto log = ustlog::logger("desc.build");
    std::vector<uint8_t> v;
    v.reserve(18);
    v.push_back(18); v.push_back(kTypeDevice);
    detail::put16(v, d.bcd_usb);
    v.push_back(d.dev_class); v.push_back(d.sub_class); v.push_back(d.protocol);
    v.push_back(d.max_packet0);
    detail::put16(v, d.vid);
    detail::put16(v, d.pid);
    detail::put16(v, d.bcd_device);
    v.push_back(d.i_man); v.push_back(d.i_prod); v.push_back(d.i_serial);
    v.push_back(d.num_configs);
    log->debug("build_device_desc: 18 字节 vid={:#06x}", d.vid);
    return v;
}

inline std::vector<uint8_t> build_config_blob(const ConfigDesc& c) {
    auto log = ustlog::logger("desc.build");
    std::vector<uint8_t> v;
    // —— 配置头（wTotalLength 先占位，尾回填）——
    v.push_back(9); v.push_back(kTypeConfiguration);
    const size_t total_pos = v.size();
    detail::put16(v, 0);                     // wTotalLength 占位
    v.push_back(static_cast<uint8_t>(c.interfaces.size()));   // bNumInterfaces=实际
    v.push_back(c.value); v.push_back(c.i_cfg);
    v.push_back(c.attributes); v.push_back(c.max_power);
    for (const auto& f : c.interfaces) {
        if (f.iad) {
            const auto& a = *f.iad;
            v.push_back(8); v.push_back(kTypeIad);
            v.push_back(a.first_if); v.push_back(a.if_count);
            v.push_back(a.fn_class); v.push_back(a.fn_sub); v.push_back(a.fn_protocol);
            v.push_back(a.i_function);
        }
        v.push_back(9); v.push_back(kTypeInterface);
        v.push_back(f.number); v.push_back(f.alt);
        v.push_back(static_cast<uint8_t>(f.endpoints.size()));   // bNumEndpoints=实际
        v.push_back(f.if_class); v.push_back(f.sub_class); v.push_back(f.protocol);
        v.push_back(f.i_if);
        if (f.hid) {
            const auto report = detail::hex_bytes(f.hid->report_hex);
            v.push_back(9); v.push_back(kTypeHid);
            detail::put16(v, f.hid->bcd_hid);
            v.push_back(f.hid->country);
            v.push_back(1);                       // bNumDescriptors（报告 1 个）
            v.push_back(kTypeReport);
            detail::put16(v, static_cast<uint16_t>(report.size()));   // =实际长度
        }
        for (const auto& e : f.endpoints) {
            v.push_back(7); v.push_back(kTypeEndpoint);
            v.push_back(e.address); v.push_back(e.attributes);
            detail::put16(v, e.max_packet);
            v.push_back(e.interval);
        }
    }
    // 回填 wTotalLength
    v[total_pos] = static_cast<uint8_t>(v.size() & 0xFF);
    v[total_pos + 1] = static_cast<uint8_t>(v.size() >> 8);
    log->debug("build_config_blob: {} 字节 {} 接口", v.size(), c.interfaces.size());
    return v;
}

inline std::vector<uint8_t> build_string_set(const StringTable& st) {
    auto log = ustlog::logger("desc.build");
    std::vector<uint8_t> v;
    // [0] LANGID 描述符（各语言并行表；MS1 单语言主流，多语言=首语言串集 + 全 LANGID 罗列）
    std::vector<uint8_t> langs;
    for (const auto& l : st.langids) {
        const uint16_t id = static_cast<uint16_t>(wcstoul(l.c_str(), nullptr, 0));
        langs.push_back(static_cast<uint8_t>(id & 0xFF));
        langs.push_back(static_cast<uint8_t>(id >> 8));
    }
    v.push_back(static_cast<uint8_t>(2 + langs.size()));
    v.push_back(kTypeString);
    v.insert(v.end(), langs.begin(), langs.end());
    // 串 1..N（取首个语言；全语言表=MS2 多语言编辑器一起做）
    const std::wstring key = st.langids.empty() ? L"0x0409" : st.langids[0];
    const auto it = st.table.find(key);
    if (it != st.table.end()) {
        for (const auto& s : it->second) {
            const size_t bytes = s.size() * 2;
            if (bytes + 2 > 255) continue;              // bLength 上界（超长串跳过+留痕）
            v.push_back(static_cast<uint8_t>(bytes + 2));
            v.push_back(kTypeString);
            v.insert(v.end(), reinterpret_cast<const uint8_t*>(s.data()),
                     reinterpret_cast<const uint8_t*>(s.data()) + bytes);
        }
    }
    log->debug("build_string_set: {} 字节（{} 语言）", v.size(), st.langids.size());
    return v;
}

} // namespace usts::shell::desc
