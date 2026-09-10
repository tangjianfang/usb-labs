// desc_selftest.cpp — 第七自测靶：MS1 描述符编译器（模型/解析/构建/C 生成/
// Linter 25 规/diff，随任务逐个追加 RUN_TEST）。
// 运行：apps/win/build/Release/desc_selftest.exe（任意 CWD）。
#include "../src/app/log.h"
#include "../src/shell/desc_model.h"
#include "../src/shell/shell_test.h"

namespace d = usts::shell::desc;

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

int main() {
    ustlog::init(true, L"desc-selftest");
    auto log = ustlog::logger("app.shell");
    log->info("desc_selftest 启动（MS1 第七靶）");

    RUN_TEST(test_model_json_roundtrip_all_fields);
    RUN_TEST(test_model_json_errors_and_defaults);

    const int rc = shell_test::run_all("desc_selftest");
    log->info("desc_selftest 结束 rc={}", rc);
    return rc;
}
