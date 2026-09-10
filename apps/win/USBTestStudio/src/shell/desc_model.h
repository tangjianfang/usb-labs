// desc_model.h — 描述符模型 .ustsdesc（设计 §2 W2 / 计划 MS1-T1）
//
// 契约：
//   1. 字段=USB 2.0 规范口径（§9.6.1 设备/§9.6.3 配置/§9.6.5 接口/§9.6.6 端点/
//      字符串 §9.7/HID 类 1.11/IAD ECN）；多字节字段模型内按数值存，二进制时 LE；
//   2. round-trip 保真：未识别描述符块以 raw_hex 收集（模型→JSON→模型 无损；
//      well-formed 输入下 模型→bin→模型 无损——T2/T3 属性测试钉死）；
//   3. 信封 {"v":1,...}，字段只增不改；strings.table 按 langid 键存字符串数组
//      （索引 1..N 对应 iManufacturer/iProduct 等）。
#pragma once

#include "../app/log.h"
#include "../framework/json_mini.h"
#include "../framework/win32_rai.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace usts::shell::desc {

constexpr uint8_t kTypeDevice = 0x01;
constexpr uint8_t kTypeConfiguration = 0x02;
constexpr uint8_t kTypeString = 0x03;
constexpr uint8_t kTypeInterface = 0x04;
constexpr uint8_t kTypeEndpoint = 0x05;
constexpr uint8_t kTypeHid = 0x21;
constexpr uint8_t kTypeReport = 0x22;
constexpr uint8_t kTypeIad = 0x0B;

struct UnknownBlock {
    std::string after;     // 挂载点："device" / "configs[0].interfaces[1]" 等
    std::string raw_hex;   // 小写无分隔（含 bLength/bDescriptorType 整块）
};

struct DeviceDesc {
    uint16_t bcd_usb = 0x0200;
    uint8_t dev_class = 0, sub_class = 0, protocol = 0;
    uint8_t max_packet0 = 8;
    uint16_t vid = 0, pid = 0, bcd_device = 0x0100;
    uint8_t i_man = 1, i_prod = 2, i_serial = 3;
    uint8_t num_configs = 1;
};

struct EndpointDesc {
    uint8_t address = 0x81;          // bit7=IN
    uint8_t attributes = 0x03;       // 传输类型低 2 位（3=中断）
    uint16_t max_packet = 64;
    uint8_t interval = 1;
};

struct HidDesc {
    uint16_t bcd_hid = 0x0111;
    uint8_t country = 0;
    std::string report_hex;          // 类描述符 0x22 原始字节（长度=parse/build 一致性 D21）
};

struct IadDesc {
    uint8_t first_if = 0, if_count = 1;
    uint8_t fn_class = 0, fn_sub = 0, fn_protocol = 0;
    uint8_t i_function = 0;
};

struct InterfaceDesc {
    uint8_t number = 0, alt = 0;
    uint8_t if_class = 3, sub_class = 0, protocol = 0;   // 3=HID（Lab1 基准）
    uint8_t i_if = 0;
    std::optional<IadDesc> iad;
    std::optional<HidDesc> hid;
    std::vector<EndpointDesc> endpoints;
};

struct ConfigDesc {
    uint8_t value = 1, i_cfg = 0;
    uint8_t attributes = 0x80;       // bit7 必置 1（§9.6.3 保留位）
    uint8_t max_power = 50;          // 2mA 单位（100mA）
    std::vector<InterfaceDesc> interfaces;
};

struct StringTable {
    // langid（显示 "0x0409"）→ 字符串数组；数组下标 0 恒为空（索引 0=LANGID 描述符）
    std::vector<std::wstring> langids{L"0x0409"};
    std::map<std::wstring, std::vector<std::wstring>> table;
};

struct DescModel {
    DeviceDesc device;
    std::vector<ConfigDesc> configs;          // MS1 单配置（多配置结构预留）
    StringTable strings;
    std::vector<UnknownBlock> unknown;

    std::string to_json() const;
    static bool from_json(const std::string& text, DescModel& out, std::string& err);
};

// ---------------------------------------------------------------------------
// 实现（header-only；模块日志 desc.model）
// ---------------------------------------------------------------------------
namespace detail {

inline std::string hex_of(uint32_t v) {
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%02X", v);
    return buf;
}

inline void write_dev(wraii::json_writer& w, const DeviceDesc& d) {
    w.begin_object();
    w.key(L"bcdUSB").string_value(wraii::utf8_to_wide(hex_of(d.bcd_usb)));
    w.key(L"bDeviceClass").int_value(d.dev_class);
    w.key(L"bDeviceSubClass").int_value(d.sub_class);
    w.key(L"bDeviceProtocol").int_value(d.protocol);
    w.key(L"bMaxPacketSize0").int_value(d.max_packet0);
    w.key(L"idVendor").string_value(wraii::utf8_to_wide(hex_of(d.vid)));
    w.key(L"idProduct").string_value(wraii::utf8_to_wide(hex_of(d.pid)));
    w.key(L"bcdDevice").string_value(wraii::utf8_to_wide(hex_of(d.bcd_device)));
    w.key(L"iManufacturer").int_value(d.i_man);
    w.key(L"iProduct").int_value(d.i_prod);
    w.key(L"iSerialNumber").int_value(d.i_serial);
    w.key(L"bNumConfigurations").int_value(d.num_configs);
    w.end_object();
}

inline void write_ep(wraii::json_writer& w, const EndpointDesc& e) {
    w.begin_object();
    w.key(L"bEndpointAddress").string_value(wraii::utf8_to_wide(hex_of(e.address)));
    w.key(L"bmAttributes").int_value(e.attributes);
    w.key(L"wMaxPacketSize").int_value(e.max_packet);
    w.key(L"bInterval").int_value(e.interval);
    w.end_object();
}

inline void write_if(wraii::json_writer& w, const InterfaceDesc& f) {
    w.begin_object();
    w.key(L"bInterfaceNumber").int_value(f.number);
    w.key(L"bAlternateSetting").int_value(f.alt);
    w.key(L"bInterfaceClass").int_value(f.if_class);
    w.key(L"bInterfaceSubClass").int_value(f.sub_class);
    w.key(L"bInterfaceProtocol").int_value(f.protocol);
    w.key(L"iInterface").int_value(f.i_if);
    if (f.iad) {
        w.key(L"iad").begin_object();
        w.key(L"bFirstInterface").int_value(f.iad->first_if);
        w.key(L"bInterfaceCount").int_value(f.iad->if_count);
        w.key(L"bFunctionClass").int_value(f.iad->fn_class);
        w.key(L"bFunctionSubClass").int_value(f.iad->fn_sub);
        w.key(L"bFunctionProtocol").int_value(f.iad->fn_protocol);
        w.key(L"iFunction").int_value(f.iad->i_function);
        w.end_object();
    }
    if (f.hid) {
        w.key(L"hid").begin_object();
        w.key(L"bcdHID").string_value(wraii::utf8_to_wide(hex_of(f.hid->bcd_hid)));
        w.key(L"bCountryCode").int_value(f.hid->country);
        w.key(L"report").string_value(wraii::utf8_to_wide(f.hid->report_hex));
        w.end_object();
    }
    w.key(L"endpoints").begin_array();
    for (const auto& e : f.endpoints) write_ep(w, e);
    w.end_array();
    w.end_object();
}

inline void write_cfg(wraii::json_writer& w, const ConfigDesc& c) {
    w.begin_object();
    w.key(L"bConfigurationValue").int_value(c.value);
    w.key(L"iConfiguration").int_value(c.i_cfg);
    w.key(L"bmAttributes").int_value(c.attributes);
    w.key(L"bMaxPower").int_value(c.max_power);
    w.key(L"interfaces").begin_array();
    for (const auto& f : c.interfaces) write_if(w, f);
    w.end_array();
    w.end_object();
}

inline void write_strings(wraii::json_writer& w, const StringTable& st) {
    w.begin_object();
    w.key(L"langids").begin_array();
    for (const auto& l : st.langids) w.string_value(l);
    w.end_array();
    w.key(L"table").begin_object();
    for (const auto& [lang, arr] : st.table) {
        w.key(lang).begin_array();
        w.string_value(L"");                     // 索引 0 占位（LANGID 语义）
        for (const auto& s : arr) w.string_value(s);
        w.end_array();
    }
    w.end_object();
    w.end_object();
}

inline std::string model_to_json(const DescModel& m) {
    wraii::json_writer w;
    w.begin_object();
    w.key(L"v").int_value(1);
    w.key(L"device"); write_dev(w, m.device);
    w.key(L"configurations").begin_array();
    for (const auto& c : m.configs) write_cfg(w, c);
    w.end_array();
    w.key(L"strings"); write_strings(w, m.strings);
    if (!m.unknown.empty()) {
        w.key(L"unknown").begin_array();
        for (const auto& u : m.unknown) {
            w.begin_object();
            w.key(L"after").string_value(wraii::utf8_to_wide(u.after));
            w.key(L"raw").string_value(wraii::utf8_to_wide(u.raw_hex));
            w.end_object();
        }
        w.end_array();
    }
    w.end_object();
    return wraii::wide_to_utf8(w.result());
}

inline int parse_hex(const std::wstring& s, int dft) {
    if (s.empty()) return dft;
    return static_cast<int>(wcstoul(s.c_str(), nullptr, 0));   // 0x 前缀/十进制通吃
}

inline DeviceDesc read_dev(const minijson::Value& v, const DeviceDesc& dft) {
    DeviceDesc d = dft;
    d.bcd_usb = static_cast<uint16_t>(parse_hex(minijson::obj_str(v, L"bcdUSB", L""), d.bcd_usb));
    d.dev_class = static_cast<uint8_t>(minijson::obj_int(v, L"bDeviceClass", d.dev_class));
    d.sub_class = static_cast<uint8_t>(minijson::obj_int(v, L"bDeviceSubClass", d.sub_class));
    d.protocol = static_cast<uint8_t>(minijson::obj_int(v, L"bDeviceProtocol", d.protocol));
    d.max_packet0 = static_cast<uint8_t>(minijson::obj_int(v, L"bMaxPacketSize0", d.max_packet0));
    d.vid = static_cast<uint16_t>(parse_hex(minijson::obj_str(v, L"idVendor", L""), d.vid));
    d.pid = static_cast<uint16_t>(parse_hex(minijson::obj_str(v, L"idProduct", L""), d.pid));
    d.bcd_device = static_cast<uint16_t>(parse_hex(minijson::obj_str(v, L"bcdDevice", L""), d.bcd_device));
    d.i_man = static_cast<uint8_t>(minijson::obj_int(v, L"iManufacturer", d.i_man));
    d.i_prod = static_cast<uint8_t>(minijson::obj_int(v, L"iProduct", d.i_prod));
    d.i_serial = static_cast<uint8_t>(minijson::obj_int(v, L"iSerialNumber", d.i_serial));
    d.num_configs = static_cast<uint8_t>(minijson::obj_int(v, L"bNumConfigurations", d.num_configs));
    return d;
}

inline EndpointDesc read_ep(const minijson::Value& v, const EndpointDesc& dft) {
    EndpointDesc e = dft;
    e.address = static_cast<uint8_t>(parse_hex(minijson::obj_str(v, L"bEndpointAddress", L""), e.address));
    e.attributes = static_cast<uint8_t>(minijson::obj_int(v, L"bmAttributes", e.attributes));
    e.max_packet = static_cast<uint16_t>(minijson::obj_int(v, L"wMaxPacketSize", e.max_packet));
    e.interval = static_cast<uint8_t>(minijson::obj_int(v, L"bInterval", e.interval));
    return e;
}

inline InterfaceDesc read_if(const minijson::Value& v, const InterfaceDesc& dft) {
    InterfaceDesc f = dft;
    f.number = static_cast<uint8_t>(minijson::obj_int(v, L"bInterfaceNumber", f.number));
    f.alt = static_cast<uint8_t>(minijson::obj_int(v, L"bAlternateSetting", f.alt));
    f.if_class = static_cast<uint8_t>(minijson::obj_int(v, L"bInterfaceClass", f.if_class));
    f.sub_class = static_cast<uint8_t>(minijson::obj_int(v, L"bInterfaceSubClass", f.sub_class));
    f.protocol = static_cast<uint8_t>(minijson::obj_int(v, L"bInterfaceProtocol", f.protocol));
    f.i_if = static_cast<uint8_t>(minijson::obj_int(v, L"iInterface", f.i_if));
    if (const auto* iad = v.find(L"iad")) {
        IadDesc d{};
        d.first_if = static_cast<uint8_t>(minijson::obj_int(*iad, L"bFirstInterface", 0));
        d.if_count = static_cast<uint8_t>(minijson::obj_int(*iad, L"bInterfaceCount", 1));
        d.fn_class = static_cast<uint8_t>(minijson::obj_int(*iad, L"bFunctionClass", 0));
        d.fn_sub = static_cast<uint8_t>(minijson::obj_int(*iad, L"bFunctionSubClass", 0));
        d.fn_protocol = static_cast<uint8_t>(minijson::obj_int(*iad, L"bFunctionProtocol", 0));
        d.i_function = static_cast<uint8_t>(minijson::obj_int(*iad, L"iFunction", 0));
        f.iad = d;
    }
    if (const auto* hid = v.find(L"hid")) {
        HidDesc d{};
        d.bcd_hid = static_cast<uint16_t>(parse_hex(minijson::obj_str(*hid, L"bcdHID", L""), 0x0111));
        d.country = static_cast<uint8_t>(minijson::obj_int(*hid, L"bCountryCode", 0));
        d.report_hex = wraii::wide_to_utf8(minijson::obj_str(*hid, L"report", L""));
        f.hid = d;
    }
    f.endpoints.clear();
    if (const auto* eps = v.find(L"endpoints"); eps && eps->is_array())
        for (const auto& e : eps->array) f.endpoints.push_back(read_ep(e, {}));
    return f;
}

inline ConfigDesc read_cfg(const minijson::Value& v, const ConfigDesc& dft) {
    ConfigDesc c = dft;
    c.value = static_cast<uint8_t>(minijson::obj_int(v, L"bConfigurationValue", c.value));
    c.i_cfg = static_cast<uint8_t>(minijson::obj_int(v, L"iConfiguration", c.i_cfg));
    c.attributes = static_cast<uint8_t>(minijson::obj_int(v, L"bmAttributes", c.attributes));
    c.max_power = static_cast<uint8_t>(minijson::obj_int(v, L"bMaxPower", c.max_power));
    c.interfaces.clear();
    if (const auto* ifs = v.find(L"interfaces"); ifs && ifs->is_array())
        for (const auto& f : ifs->array) c.interfaces.push_back(read_if(f, {}));
    return c;
}

inline StringTable read_strings(const minijson::Value& v) {
    StringTable st;
    if (const auto* ls = v.find(L"langids"); ls && ls->is_array()) {
        st.langids.clear();
        for (const auto& l : ls->array) st.langids.push_back(l.as_str(L"0x0409"));
    }
    if (const auto* tb = v.find(L"table"); tb && tb->is_object()) {
        for (const auto& [lang, arr] : tb->members) {
            std::vector<std::wstring> ss;
            if (arr.is_array())
                for (size_t i = 0; i < arr.array.size(); ++i) {
                    const auto s = arr.array[i].as_str(L"");
                    if (i == 0 && s.empty()) continue;   // 索引 0 占位不入（写回时补）
                    ss.push_back(s);
                }
            st.table[lang] = std::move(ss);
        }
    }
    return st;
}

inline bool model_from_json(const std::string& text, DescModel& out, std::string& err) {
    const std::wstring wide = wraii::utf8_to_wide(text);
    minijson::Value v;
    std::wstring werr;
    if (!minijson::parse(wide, v, &werr) || !v.is_object()) {
        err = "ustsdesc JSON 解析失败: " + wraii::wide_to_utf8(werr);
        return false;
    }
    if (const auto* x = v.find(L"v")) {
        if (x->as_int(1) != 1) { err = "ustsdesc 版本不识别"; return false; }
    }
    const auto* dev = v.find(L"device");
    if (!dev || !dev->is_object()) { err = "ustsdesc 缺 device"; return false; }
    out.device = read_dev(*dev, out.device);
    out.configs.clear();
    if (const auto* cs = v.find(L"configurations"); cs && cs->is_array())
        for (const auto& c : cs->array) out.configs.push_back(read_cfg(c, {}));
    if (const auto* ss = v.find(L"strings")) out.strings = read_strings(*ss);
    out.unknown.clear();
    if (const auto* us = v.find(L"unknown"); us && us->is_array())
        for (const auto& u : us->array) {
            UnknownBlock b;
            b.after = wraii::wide_to_utf8(minijson::obj_str(u, L"after", L""));
            b.raw_hex = wraii::wide_to_utf8(minijson::obj_str(u, L"raw", L""));
            if (!b.raw_hex.empty()) out.unknown.push_back(std::move(b));
        }
    return true;
}

} // namespace detail

inline std::string DescModel::to_json() const {
    auto log = ustlog::logger("desc.model");
    const auto r = detail::model_to_json(*this);
    log->debug("to_json: {} 字节（configs={} unknown={}）", r.size(), configs.size(),
               unknown.size());
    return r;
}

inline bool DescModel::from_json(const std::string& text, DescModel& out,
                                 std::string& err) {
    auto log = ustlog::logger("desc.model");
    log->debug("from_json: {} 字节", text.size());
    const bool ok = detail::model_from_json(text, out, err);
    if (!ok) log->warn("from_json 失败: {}", err);
    return ok;
}

} // namespace usts::shell::desc
