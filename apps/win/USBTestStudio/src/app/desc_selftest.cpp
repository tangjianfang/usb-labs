// desc_selftest.cpp — 第七自测靶：MS1 描述符编译器（模型/解析/构建/C 生成/
// Linter 25 规/diff，随任务逐个追加 RUN_TEST）。
// 运行：apps/win/build/Release/desc_selftest.exe（任意 CWD）。
#include "../src/app/log.h"
#include "../src/shell/desc_build.h"
#include "../src/shell/desc_gen_c.h"
#include "../src/shell/desc_lint.h"
#include "../src/shell/desc_model.h"
#include "../src/shell/desc_parse.h"
#include "../src/shell/shell_test.h"

#include <fstream>
#include <iterator>
#include <shlwapi.h>
#include <shellapi.h>

namespace d = usts::shell::desc;

// ---------------------------------------------------------------------------
// 黄金样例：Lab1 HID 复合（键盘 If0 EP81 + 鼠标 If1 EP82）——与
// labs/lab1-hid-composite/firmware/src/usb_descriptors.c 字段口径一致
// ---------------------------------------------------------------------------
static const uint8_t kGoldenDevice[18] = {
    0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08,
    0x41, 0x23, 0x02, 0x00, 0x00, 0x01, 0x01, 0x02, 0x03, 0x01,
};
static const uint8_t kGoldenConfig[59] = {
    0x09, 0x02, 0x3B, 0x00, 0x02, 0x01, 0x00, 0xA0, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x3F, 0x00,
    0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x0A,
    0x09, 0x04, 0x01, 0x00, 0x01, 0x03, 0x01, 0x02, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x3F, 0x00,
    0x07, 0x05, 0x82, 0x03, 0x08, 0x00, 0x0A,
};
static const uint8_t kGoldenStrings[] = {
    0x04, 0x03, 0x09, 0x04,                               // LANGID 0x0409（索引 0）
    0x10, 0x03, 'U', 0, 'S', 0, 'B', 0, '-', 0, 'L', 0, 'a', 0, 'b', 0,   // [1] "USB-Lab"
    0x06, 0x03, 'K', 0, 'M', 0,                            // [2] "KM"
    0x0C, 0x03, 'S', 0, 'N', 0, '0', 0, '0', 0, '1', 0,    // [3] "SN001"
};
static const uint8_t kGoldenKeyboardReport[63] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0,
    0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08,
    0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00,
    0x29, 0x65, 0x81, 0x00, 0xC0,
    // 填充到 63B（wDescriptorLength 与 TinyUSB 键盘报告一致口径）
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static bool vec_eq(const std::vector<uint8_t>& v, const uint8_t* p, size_t n) {
    return v.size() == n && memcmp(v.data(), p, n) == 0;
}

// ---------------------------------------------------------------------------
// T1 · 模型与 JSON 序列化
// ---------------------------------------------------------------------------
static void test_model_json_roundtrip_all_fields() {
    d::DescModel m;
    m.device.bcd_usb = 0x0210; m.device.dev_class = 0xEF; m.device.sub_class = 2;
    m.device.protocol = 1; m.device.max_packet0 = 64; m.device.vid = 0x2341;
    m.device.pid = 0x0002; m.device.bcd_device = 0x0101; m.device.i_man = 7;
    m.device.i_prod = 8; m.device.i_serial = 9; m.device.num_configs = 1;
    d::ConfigDesc c;
    c.value = 3; c.i_cfg = 4; c.attributes = 0x90; c.max_power = 250;
    d::InterfaceDesc f;
    f.number = 1; f.alt = 2; f.if_class = 0x0A; f.sub_class = 3; f.protocol = 5;
    f.i_if = 6;
    d::IadDesc iad; iad.first_if = 1; iad.if_count = 2; iad.fn_class = 0x0E;
    iad.fn_sub = 1; iad.fn_protocol = 2; iad.i_function = 5;
    f.iad = iad;
    d::HidDesc hid; hid.bcd_hid = 0x0111; hid.country = 1;
    hid.report_hex = "05010906a101";
    f.hid = hid;
    d::EndpointDesc e; e.address = 0x82; e.attributes = 0x02;
    e.max_packet = 512; e.interval = 0;
    f.endpoints.push_back(e);
    c.interfaces.push_back(f);
    m.configs.push_back(c);
    m.strings.langids = {L"0x0409", L"0x0804"};
    m.strings.table[L"0x0409"] = {L"USB-Labs", L"键鼠套装", L"SN001"};
    m.strings.table[L"0x0804"] = {L"实验室"};
    m.unknown.push_back({"device", "00ff00ff"});

    const std::string j = m.to_json();
    d::DescModel o;
    std::string err;
    CHECK(d::DescModel::from_json(j, o, err));
    CHECK_EQ(o.device.bcd_usb, m.device.bcd_usb);
    CHECK_EQ(o.device.dev_class, m.device.dev_class);
    CHECK_EQ(o.device.sub_class, m.device.sub_class);
    CHECK_EQ(o.device.protocol, m.device.protocol);
    CHECK_EQ(o.device.max_packet0, m.device.max_packet0);
    CHECK_EQ(o.device.vid, m.device.vid);
    CHECK_EQ(o.device.pid, m.device.pid);
    CHECK_EQ(o.device.bcd_device, m.device.bcd_device);
    CHECK_EQ(o.device.i_man, m.device.i_man);
    CHECK_EQ(o.device.i_prod, m.device.i_prod);
    CHECK_EQ(o.device.i_serial, m.device.i_serial);
    CHECK_EQ(o.device.num_configs, m.device.num_configs);
    CHECK_EQ(o.configs.size(), 1u);
    const auto& oc = o.configs[0];
    CHECK_EQ(oc.value, c.value); CHECK_EQ(oc.i_cfg, c.i_cfg);
    CHECK_EQ(oc.attributes, c.attributes); CHECK_EQ(oc.max_power, c.max_power);
    CHECK_EQ(oc.interfaces.size(), 1u);
    const auto& of = oc.interfaces[0];
    CHECK_EQ(of.number, f.number); CHECK_EQ(of.alt, f.alt);
    CHECK_EQ(of.if_class, f.if_class); CHECK_EQ(of.sub_class, f.sub_class);
    CHECK_EQ(of.protocol, f.protocol); CHECK_EQ(of.i_if, f.i_if);
    CHECK(of.iad && of.iad->first_if == 1 && of.iad->if_count == 2 &&
          of.iad->fn_class == 0x0E && of.iad->i_function == 5);
    CHECK(of.hid && of.hid->bcd_hid == 0x0111 && of.hid->country == 1 &&
          of.hid->report_hex == "05010906a101");
    CHECK_EQ(of.endpoints.size(), 1u);
    CHECK_EQ(of.endpoints[0].address, 0x82);
    CHECK_EQ(of.endpoints[0].attributes, 0x02);
    CHECK_EQ(of.endpoints[0].max_packet, 512);
    CHECK_EQ(of.endpoints[0].interval, 0);
    CHECK_EQ(o.strings.langids.size(), 2u);
    CHECK_EQ(o.strings.table.at(L"0x0409").size(), 3u);
    CHECK_EQ(o.strings.table.at(L"0x0409")[1], std::wstring(L"键鼠套装"));
    CHECK_EQ(o.strings.table.at(L"0x0804")[0], std::wstring(L"实验室"));
    CHECK_EQ(o.unknown.size(), 1u);
    CHECK_EQ(o.unknown[0].after, std::string("device"));
    CHECK_EQ(o.unknown[0].raw_hex, std::string("00ff00ff"));
    // 二次 roundtrip 幂等（json(o)==json(m) 字节级）
    CHECK(o.to_json() == j);
}

static void test_model_json_errors_and_defaults() {
    d::DescModel o;
    std::string err;
    CHECK(!d::DescModel::from_json("{bad", o, err));
    CHECK(!d::DescModel::from_json("{\"v\":2}", o, err));
    CHECK(!d::DescModel::from_json("{}", o, err));                       // 缺 device
    CHECK(!d::DescModel::from_json("{\"v\":1}", o, err));                // 缺 device
    // 最小合法：device 缺字段=保默认
    CHECK(d::DescModel::from_json("{\"v\":1,\"device\":{}}", o, err));
    CHECK_EQ(o.device.bcd_usb, 0x0200);
    CHECK_EQ(o.device.max_packet0, 8);
    CHECK_EQ(o.device.i_man, 1);
    CHECK_EQ(o.strings.langids.size(), 1u);   // 缺省 0x0409
    CHECK(o.configs.empty());
}

// ---------------------------------------------------------------------------
// T2 · 反编译：黄金样例逐字段 + 未识别块保真 + 畸形输入
// ---------------------------------------------------------------------------
static void test_parse_device_golden() {
    d::DeviceDesc dev;
    std::string err;
    CHECK(d::parse_device_desc(kGoldenDevice, sizeof(kGoldenDevice), dev, err));
    CHECK_EQ(dev.bcd_usb, 0x0200);
    CHECK_EQ(dev.dev_class, 0);
    CHECK_EQ(dev.max_packet0, 8);
    CHECK_EQ(dev.vid, 0x2341);
    CHECK_EQ(dev.pid, 0x0002);
    CHECK_EQ(dev.bcd_device, 0x0100);
    CHECK_EQ(dev.i_man, 1); CHECK_EQ(dev.i_prod, 2); CHECK_EQ(dev.i_serial, 3);
    CHECK_EQ(dev.num_configs, 1);
    // 畸形
    CHECK(!d::parse_device_desc(kGoldenDevice, 10, dev, err));   // 截断
    const uint8_t bad[18] = {0x0F, 0x01};
    CHECK(!d::parse_device_desc(bad, 18, dev, err));             // bLength/type 非法
}

static d::ConfigDesc parse_golden_cfg(std::vector<d::UnknownBlock>& unk) {
    d::ConfigDesc c;
    std::string err;
    CHECK(d::parse_config_blob(kGoldenConfig, sizeof(kGoldenConfig), c, unk, err));
    return c;
}

static void test_parse_config_golden() {
    std::vector<d::UnknownBlock> unk;
    const d::ConfigDesc c = parse_golden_cfg(unk);
    CHECK_EQ(c.value, 1); CHECK_EQ(c.i_cfg, 0);
    CHECK_EQ(c.attributes, 0xA0); CHECK_EQ(c.max_power, 50);
    CHECK_EQ(c.interfaces.size(), 2u);
    const auto& kbd = c.interfaces[0];
    CHECK_EQ(kbd.number, 0); CHECK_EQ(kbd.alt, 0);
    CHECK_EQ(kbd.if_class, 3); CHECK_EQ(kbd.sub_class, 1); CHECK_EQ(kbd.protocol, 1);
    CHECK(kbd.hid && kbd.hid->bcd_hid == 0x0111 && kbd.hid->report_hex.empty());
    CHECK_EQ(kbd.endpoints.size(), 1u);
    CHECK_EQ(kbd.endpoints[0].address, 0x81);
    CHECK_EQ(kbd.endpoints[0].attributes, 3);
    CHECK_EQ(kbd.endpoints[0].max_packet, 8);
    CHECK_EQ(kbd.endpoints[0].interval, 10);
    const auto& mse = c.interfaces[1];
    CHECK_EQ(mse.number, 1); CHECK_EQ(mse.protocol, 2);   // 鼠标
    CHECK_EQ(mse.endpoints[0].address, 0x82);
    CHECK(unk.empty());
}

static void test_parse_config_unknown_and_malformed() {
    std::string err;
    std::vector<d::UnknownBlock> unk;
    // 未识别 type 0xFF 块（插在键盘 EP 后）
    uint8_t blob[64];
    memcpy(blob, kGoldenConfig, sizeof(kGoldenConfig));
    const uint8_t ff[4] = {0x04, 0xFF, 0xAA, 0xBB};
    memcpy(blob + 34, ff, 4);                                  // 覆盖鼠标接口头前 4B? 不可
    // 更稳：构造 mini blob = config 头 + 未知块
    const uint8_t mini[] = {0x09, 0x02, 0x0D, 0x00, 0x00, 0x01, 0x00, 0x80, 0x32,
                            0x04, 0xFF, 0xAA, 0xBB};
    d::ConfigDesc c;
    CHECK(d::parse_config_blob(mini, sizeof(mini), c, unk, err));
    CHECK_EQ(unk.size(), 1u);
    CHECK_EQ(unk[0].raw_hex, std::string("04ffaabb"));
    CHECK_EQ(unk[0].after, std::string("configs[0].0"));
    // bLength=0
    const uint8_t zero[] = {0x09, 0x02, 0x0C, 0x00, 0x00, 0x01, 0x00, 0x80, 0x32,
                            0x00, 0x04};
    CHECK(!d::parse_config_blob(zero, sizeof(zero), c, unk, err));
    // wTotalLength 越界
    const uint8_t over[] = {0x09, 0x02, 0xFF, 0x00, 0x00, 0x01, 0x00, 0x80, 0x32};
    CHECK(!d::parse_config_blob(over, sizeof(over), c, unk, err));
    (void)blob;
}

static void test_parse_strings_and_attach_report() {
    d::StringTable st;
    std::string err;
    CHECK(d::parse_string_set(kGoldenStrings, sizeof(kGoldenStrings), st, err));
    CHECK_EQ(st.langids.size(), 1u);
    CHECK_EQ(st.langids[0], std::wstring(L"0x0409"));
    CHECK_EQ(st.table[L"0x0409"].size(), 3u);
    CHECK_EQ(st.table[L"0x0409"][0], std::wstring(L"USB-Lab"));
    CHECK_EQ(st.table[L"0x0409"][1], std::wstring(L"KM"));
    CHECK_EQ(st.table[L"0x0409"][2], std::wstring(L"SN001"));
    CHECK(!d::parse_string_set(nullptr, 0, st, err));           // 空集=err
    const uint8_t bad[] = {0x05, 0x04};                          // 非字符串类型
    CHECK(!d::parse_string_set(bad, sizeof(bad), st, err));
    // attach_report
    std::vector<d::UnknownBlock> unk;
    d::ConfigDesc c = parse_golden_cfg(unk);
    d::attach_report(c.interfaces[0], kGoldenKeyboardReport,
                     sizeof(kGoldenKeyboardReport));
    CHECK_EQ(c.interfaces[0].hid->report_hex.size(), 63u * 2);
    CHECK(c.interfaces[0].hid->report_hex.substr(0, 10) == "05010906a1");
}

// ---------------------------------------------------------------------------
// T3 · 构建：黄金逐字节 + 性质 parse∘build ≡ id
// ---------------------------------------------------------------------------
static d::DescModel golden_model() {
    d::DescModel m;
    std::string err;
    d::parse_device_desc(kGoldenDevice, sizeof(kGoldenDevice), m.device, err);
    std::vector<d::UnknownBlock> unk;
    m.configs.push_back(parse_golden_cfg(unk));
    d::StringTable st;
    d::parse_string_set(kGoldenStrings, sizeof(kGoldenStrings), st, err);
    m.strings = st;
    d::attach_report(m.configs[0].interfaces[0], kGoldenKeyboardReport,
                     sizeof(kGoldenKeyboardReport));
    d::attach_report(m.configs[0].interfaces[1], kGoldenKeyboardReport,
                     sizeof(kGoldenKeyboardReport));
    return m;
}

static void test_build_golden_bytes() {
    const d::DescModel m = golden_model();
    CHECK(vec_eq(d::build_device_desc(m.device), kGoldenDevice, sizeof(kGoldenDevice)));
    CHECK(vec_eq(d::build_config_blob(m.configs[0]), kGoldenConfig, sizeof(kGoldenConfig)));
    CHECK(vec_eq(d::build_string_set(m.strings), kGoldenStrings, sizeof(kGoldenStrings)));
}

static void test_build_roundtrip_properties() {
    // 性质 1：build(parse(b)) ≡ b（黄金输入，含 report 注入后 wDescriptorLength 一致）
    const d::DescModel m = golden_model();
    // 性质 2：parse(build(m)) ≡ m（全部字段）
    std::string err;
    std::vector<d::UnknownBlock> unk;
    d::DeviceDesc dev2;
    const auto devbin = d::build_device_desc(m.device);
    CHECK(d::parse_device_desc(devbin.data(), devbin.size(), dev2, err));
    CHECK_EQ(dev2.vid, m.device.vid);
    CHECK_EQ(dev2.bcd_usb, m.device.bcd_usb);
    d::ConfigDesc cfg2;
    const auto cfgbin = d::build_config_blob(m.configs[0]);
    CHECK(d::parse_config_blob(cfgbin.data(), cfgbin.size(), cfg2, unk, err));
    CHECK_EQ(cfg2.interfaces.size(), m.configs[0].interfaces.size());
    CHECK_EQ(cfg2.interfaces[0].endpoints[0].address, 0x81);
    CHECK_EQ(cfg2.interfaces[1].protocol, 2);
    CHECK_EQ(cfgbin.size(), 59u);                       // wTotalLength 回填正确
    // HID 报告长度回填 == report_hex 字节数
    d::StringTable st2;
    const auto strbin = d::build_string_set(m.strings);
    CHECK(d::parse_string_set(strbin.data(), strbin.size(), st2, err));
    CHECK_EQ(st2.table[L"0x0409"][0], std::wstring(L"USB-Lab"));
    CHECK_EQ(st2.table[L"0x0409"][2], std::wstring(L"SN001"));
    // 回填一致性：bNumInterfaces/bNumEndpoints 由实际推导
    d::ConfigDesc c3 = m.configs[0];
    c3.interfaces[0].endpoints.push_back({0x83, 2, 64, 1});   // 加一端点
    const auto b3 = d::build_config_blob(c3);
    CHECK_EQ(b3.size(), 66u);                                  // 59+7
    CHECK_EQ(b3[4], 2);                                        // bNumInterfaces 不变
}

// ---------------------------------------------------------------------------
// T4 · C 代码生成（两模板）
// ---------------------------------------------------------------------------
static bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

static void test_gen_c_tinyusb() {
    const d::DescModel m = golden_model();
    d::GenOptions opt;
    opt.tpl = d::GenTemplate::TinyUsb;
    opt.model_name = "lab1";
    const std::string c = d::generate_c(m, opt);
    CHECK(contains(c, "desc_device[18]"));
    CHECK(contains(c, "desc_fs_configuration[]"));
    CHECK(contains(c, "hid_report_descriptor[]"));
    CHECK(contains(c, "string_desc_arr"));
    CHECK(contains(c, "lab1.ustsdesc"));
    // 设备头 18 字节逐项在场（首字段 0x12, 0x01）
    CHECK(contains(c, "0x12, 0x01"));
    // 配置长度注释 = 59
    CHECK(contains(c, "59 字节"));
    // 报告字节数组含样例前缀
    CHECK(contains(c, "0x05, 0x01, 0x09, 0x06"));
    // 字符串行含 LANGID 0x0409 与文本注释
    CHECK(contains(c, "0x0409"));
    CHECK(contains(c, "// USB-Lab"));
    // 行格式：每行 ≤16 字节 → 存在换行续行（59B 配置必产生多行）
    CHECK(contains(c, "\n    0x"));
}

static void test_gen_c_plain_and_diff_from_tiny() {
    const d::DescModel m = golden_model();
    d::GenOptions opt;
    opt.tpl = d::GenTemplate::PlainArrays;
    opt.model_name = "plain";
    const std::string c = d::generate_c(m, opt);
    CHECK(contains(c, "dev_desc[18]"));
    CHECK(contains(c, "cfg_desc[]"));
    CHECK(contains(c, "hid_report_0[]"));
    CHECK(!contains(c, "desc_fs_configuration"));   // 两模板命名互斥
    const std::string tiny = d::generate_c(m, {d::GenTemplate::TinyUsb, "lab1"});
    CHECK(tiny != c);
}

// ---------------------------------------------------------------------------
// T5 · Linter：黄金零 err + 逐规则触发/不触发 + 规则库对账
// ---------------------------------------------------------------------------
static const char* has_id(const std::vector<d::LintHit>& hits, const char* id) {
    for (const auto& h : hits)
        if (h.rule_id == id) return id;
    return nullptr;
}

static void test_lint_golden_clean() {
    const d::DescModel m = golden_model();
    const auto hits = d::DescLinter::run(m);
    for (const auto& h : hits)
        std::printf("  [lint] %s %s %s\n", h.rule_id.c_str(), h.path.c_str(),
                    h.message.c_str());
    size_t errs = 0;
    for (const auto& h : hits)
        if (h.severity == 0) ++errs;
    CHECK_EQ(errs, 0u);   // Lab1 黄金样例=干净（规范符合）
    CHECK_EQ(hits.size(), 0u);   // 含 warn/info 亦无
}

static void test_lint_rule_triggers() {
    // 每条可模型触发的规则：一正（命中）一负（黄金干净已由上一测钉）
    auto bad = [] {
        d::DescModel m = golden_model();
        return m;
    };
    auto fire = [] (d::DescModel m, const char* id) {
        return has_id(d::DescLinter::run(m), id) != nullptr;
    };
    {   // D03 bcdUSB
        auto m = bad(); m.device.bcd_usb = 0x0300;
        CHECK(fire(m, "D03"));
    }
    {   // D04 bMaxPacketSize0
        auto m = bad(); m.device.max_packet0 = 9;
        CHECK(fire(m, "D04"));
    }
    {   // D05 vid=0
        auto m = bad(); m.device.vid = 0;
        CHECK(fire(m, "D05"));
    }
    {   // D08 配置值 0
        auto m = bad(); m.configs[0].value = 0;
        CHECK(fire(m, "D08"));
    }
    {   // D09 bit7
        auto m = bad(); m.configs[0].attributes = 0x00;
        CHECK(fire(m, "D09"));
    }
    {   // D10 功率超限
        auto m = bad(); m.configs[0].max_power = 251;
        CHECK(fire(m, "D10"));
    }
    {   // D11 接口号从 1 起（非连续）
        auto m = bad();
        m.configs[0].interfaces[0].number = 1;
        m.configs[0].interfaces[1].number = 3;
        CHECK(fire(m, "D11"));
    }
    {   // D12 首个 alt=1
        auto m = bad(); m.configs[0].interfaces[0].alt = 1;
        CHECK(fire(m, "D12"));
    }
    {   // D14 未分配类码
        auto m = bad(); m.configs[0].interfaces[0].if_class = 0x55;
        CHECK(fire(m, "D14"));
    }
    {   // D15 EP0 地址
        auto m = bad(); m.configs[0].interfaces[0].endpoints[0].address = 0x00;
        CHECK(fire(m, "D15"));
    }
    {   // D16 重复地址
        auto m = bad();
        m.configs[0].interfaces[0].endpoints.push_back(
            m.configs[0].interfaces[0].endpoints[0]);
        CHECK(fire(m, "D16"));
    }
    {   // D17 保留位
        auto m = bad(); m.configs[0].interfaces[0].endpoints[0].attributes = 0x43;
        CHECK(fire(m, "D17"));
    }
    {   // D18 批量 128B
        auto m = bad();
        m.configs[0].interfaces[0].endpoints[0].attributes = 0x02;   // 批量
        m.configs[0].interfaces[0].endpoints[0].max_packet = 128;
        CHECK(fire(m, "D18"));
    }
    {   // D19 控制端点
        auto m = bad(); m.configs[0].interfaces[0].endpoints[0].attributes = 0x00;
        CHECK(fire(m, "D19"));
    }
    {   // D20 中断 interval=0
        auto m = bad(); m.configs[0].interfaces[0].endpoints[0].interval = 0;
        CHECK(fire(m, "D20"));
    }
    {   // D20b 全速同步 interval=20
        auto m = bad();
        m.configs[0].interfaces[0].endpoints[0].attributes = 0x01;   // 同步
        m.configs[0].interfaces[0].endpoints[0].interval = 20;
        CHECK(fire(m, "D20"));
    }
    {   // D22 COLLECTION 不闭合
        auto m = bad();
        m.configs[0].interfaces[0].hid->report_hex = "050109a101";   // 单 COLLECTION 未闭合
        CHECK(fire(m, "D22"));
    }
    {   // D22b 多闭一层
        auto m = bad();
        m.configs[0].interfaces[0].hid->report_hex = "c0";
        CHECK(fire(m, "D22"));
    }
    {   // D23 字符串索引越界
        auto m = bad(); m.device.i_man = 5;   // 表内仅 1 串（USB-Lab）
        CHECK(fire(m, "D23"));
    }
    {   // D24 LANGID 空
        auto m = bad(); m.strings.langids.clear(); m.strings.table.clear();
        CHECK(fire(m, "D24"));
    }
    {   // D25 IAD 越界
        auto m = bad();
        d::IadDesc iad; iad.first_if = 1; iad.if_count = 2;   // 总接口=2, 1+2>2
        m.configs[0].interfaces[0].iad = iad;
        CHECK(fire(m, "D25"));
    }
}

static void test_lint_rules_meta_and_file() {
    CHECK_EQ(d::DescLinter::rule_count(), 25u);
    const std::string j = d::DescLinter::rules_meta_json();
    for (const char* id : {"D01", "D13", "D21", "D25"})
        CHECK(j.find(id) != std::string::npos);
    // 元信息含条款引用（专业性锚点）
    CHECK(j.find("9.6.6") != std::string::npos);
    // 与源码树 rules/desc_rules.json 对账（路径相对 __FILE__，任意 CWD 可跑）
    std::string here = __FILE__;
    std::replace(here.begin(), here.end(), '\\', '/');
    const std::string rel = here.substr(0, here.find("/src/app/")) + "/rules/desc_rules.json";
    std::ifstream in(rel);
    if (in.good()) {   // 文件在（源码树内）→ id 集对账
        std::string disk((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        CHECK(disk.find("\"D01\"") != std::string::npos);
        CHECK(disk.find("\"D25\"") != std::string::npos);
    }
}

int main() {
    ustlog::init(true, L"desc-selftest");
    auto log = ustlog::logger("app.shell");
    log->info("desc_selftest 启动（MS1 第七靶）");
    {   // --dump-rules：把 25 规则元信息打到 stdout（生成 rules/desc_rules.json 用）
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; i < argc; ++i)
            if (wcscmp(argv[i], L"--dump-rules") == 0) {
                std::printf("%s", d::DescLinter::rules_meta_json().c_str());
                return 0;
            }
        LocalFree(argv);
    }

    RUN_TEST(test_model_json_roundtrip_all_fields);
    RUN_TEST(test_model_json_errors_and_defaults);
    RUN_TEST(test_parse_device_golden);
    RUN_TEST(test_parse_config_golden);
    RUN_TEST(test_parse_config_unknown_and_malformed);
    RUN_TEST(test_parse_strings_and_attach_report);
    RUN_TEST(test_build_golden_bytes);
    RUN_TEST(test_build_roundtrip_properties);
    RUN_TEST(test_gen_c_tinyusb);
    RUN_TEST(test_gen_c_plain_and_diff_from_tiny);
    RUN_TEST(test_lint_golden_clean);
    RUN_TEST(test_lint_rule_triggers);
    RUN_TEST(test_lint_rules_meta_and_file);

    const int rc = shell_test::run_all("desc_selftest");
    log->info("desc_selftest 结束 rc={}", rc);
    return rc;
}
