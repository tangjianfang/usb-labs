// vd_selftest.cpp — 第八自测靶：MS2 虚拟调试器（内核/模板/脚本/协议/面板，逐步追加）。
#include "../src/app/log.h"
#include "../src/shell/desc_model.h"
#include "../src/shell/desc_parse.h"
#include "../src/shell/shell_test.h"
#include "../src/shell/vd_core.h"

namespace d = usts::shell::desc;
namespace vd = usts::shell::vd;

// 黄金模型（与 desc_selftest 同源——从黄金二进制 parse 而来，保证跨靶一致口径）
static d::DescModel golden_model() {
    static const uint8_t dev[18] = {
        0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08,
        0x41, 0x23, 0x02, 0x00, 0x00, 0x01, 0x01, 0x02, 0x03, 0x01,
    };
    static const uint8_t cfg[59] = {
        0x09, 0x02, 0x3B, 0x00, 0x02, 0x01, 0x00, 0xA0, 0x32,
        0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00,
        0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x3F, 0x00,
        0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x0A,
        0x09, 0x04, 0x01, 0x00, 0x01, 0x03, 0x01, 0x02, 0x00,
        0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x3F, 0x00,
        0x07, 0x05, 0x82, 0x03, 0x08, 0x00, 0x0A,
    };
    static const uint8_t str[] = {
        0x04, 0x03, 0x09, 0x04,
        0x10, 0x03, 'U', 0, 'S', 0, 'B', 0, '-', 0, 'L', 0, 'a', 0, 'b', 0,
        0x06, 0x03, 'K', 0, 'M', 0,
        0x0C, 0x03, 'S', 0, 'N', 0, '0', 0, '0', 0, '1', 0,
    };
    d::DescModel m;
    std::string err;
    std::vector<d::UnknownBlock> unk;
    d::parse_device_desc(dev, 18, m.device, err);
    m.configs.push_back({});
    d::parse_config_blob(cfg, 59, m.configs[0], unk, err);
    d::parse_string_set(str, sizeof(str), m.strings, err);
    return m;
}

static void setup(uint8_t* s, uint8_t rt, uint8_t req, uint16_t val, uint16_t len) {
    s[0] = rt; s[1] = req;
    s[2] = static_cast<uint8_t>(val & 0xFF); s[3] = static_cast<uint8_t>(val >> 8);
    s[4] = 0; s[5] = 0;
    s[6] = static_cast<uint8_t>(len & 0xFF); s[7] = static_cast<uint8_t>(len >> 8);
}

// ---------------------------------------------------------------------------
// T1 · 内核：枚举序列字节级 + 状态机 + 断点 + 注入
// ---------------------------------------------------------------------------
static void test_vd_enumeration_golden() {
    vd::VirtualDevice dev(golden_model());
    CHECK(dev.state() == vd::DevState::Attached);
    uint8_t s[8];
    // ① GET_DESCRIPTOR device（未配置先转 Default）
    setup(s, 0x80, 0x06, 0x0100, 18);
    auto r = dev.host_ctrl(s);
    CHECK(!r.stall);
    CHECK(r.data == d::build_device_desc(golden_model().device));   // 字节级=MS1 产物
    CHECK(dev.state() == vd::DevState::Default);
    // ② SET_ADDRESS 5
    setup(s, 0x00, 0x05, 5, 0);
    r = dev.host_ctrl(s);
    CHECK(!r.stall && dev.address() == 5 && dev.state() == vd::DevState::Addressed);
    // ③ GET_DESCRIPTOR config（全 59B）
    setup(s, 0x80, 0x06, 0x0200, 59);
    r = dev.host_ctrl(s);
    CHECK_EQ(r.data.size(), 59u);
    CHECK(r.data == d::build_config_blob(golden_model().configs[0]));
    // wLength 截断口径
    setup(s, 0x80, 0x06, 0x0200, 9);
    r = dev.host_ctrl(s);
    CHECK_EQ(r.data.size(), 9u);
    // ④ 字符串 0(LANGID)/1/3
    setup(s, 0x80, 0x06, 0x0300, 255);
    r = dev.host_ctrl(s);
    CHECK_EQ(r.data.size(), 4u);
    setup(s, 0x80, 0x06, 0x0303, 255);
    r = dev.host_ctrl(s);
    CHECK_EQ(r.data.size(), 12u);                       // "SN001"
    // ⑤ SET_CONFIGURATION 1 → Configured
    setup(s, 0x00, 0x09, 1, 0);
    r = dev.host_ctrl(s);
    CHECK(dev.state() == vd::DevState::Configured);
    // ⑥ 未知请求 → STALL
    setup(s, 0x80, 0x77, 0, 0);
    r = dev.host_ctrl(s);
    CHECK(r.stall);
    CHECK(dev.stats().stall >= 1);
}

static void test_vd_breakpoints() {
    vd::VirtualDevice dev(golden_model());
    uint8_t s[8];
    vd::Breakpoint bp;
    // on_ctrl(SET_ADDRESS)
    bp.kind = vd::Breakpoint::Kind::CtrlRequest;
    bp.bmReqType = 0x00; bp.bRequest = 0x05;
    setup(s, 0x80, 0x06, 0x0100, 18);
    dev.host_ctrl(s);
    CHECK(!dev.hit(bp));                       // GET_DESCRIPTOR 不中
    setup(s, 0x00, 0x05, 5, 0);
    dev.host_ctrl(s);
    CHECK(dev.hit(bp));                        // SET_ADDRESS 命中
    // on_state(Configured)
    vd::Breakpoint bs;
    bs.kind = vd::Breakpoint::Kind::State; bs.state = vd::DevState::Configured;
    CHECK(!dev.hit(bs));
    setup(s, 0x00, 0x09, 1, 0);
    dev.host_ctrl(s);
    CHECK(dev.hit(bs));
    // on_error_count(1)：先制造一次 STALL（未知请求）再判定
    vd::Breakpoint be;
    be.kind = vd::Breakpoint::Kind::ErrorCount; be.error_count = 1;
    CHECK(!dev.hit(be));
    setup(s, 0x80, 0x77, 0, 0);
    dev.host_ctrl(s);
    CHECK(dev.hit(be));
    // on_enum_step（事件 seq 精确匹配）
    vd::Breakpoint bn;
    bn.kind = vd::Breakpoint::Kind::EnumStep; bn.enum_step = 3;
    CHECK(dev.hit(bn) == (dev.events().back().seq == 3));
    // 禁用断点恒不中
    bn.enabled = false;
    CHECK(!dev.hit(bn));
}

static void test_vd_injections() {
    vd::VirtualDevice dev(golden_model());
    uint8_t s[8];
    // StallNext
    dev.inject(vd::Inject::StallNext, 1);
    setup(s, 0x80, 0x06, 0x0100, 18);
    CHECK(dev.host_ctrl(s).stall);
    setup(s, 0x80, 0x06, 0x0100, 18);
    CHECK(!dev.host_ctrl(s).stall);            // 只生效一次
    // 配置到 Configured 供 poll 测试
    setup(s, 0x00, 0x05, 5, 0); dev.host_ctrl(s);
    setup(s, 0x00, 0x09, 1, 0); dev.host_ctrl(s);
    dev.set_report_source([] { return std::vector<uint8_t>{0, 0, 4}; });   // 按 A
    CHECK_EQ(dev.poll_in_report(0x81).size(), 3u);
    // NakTimes 2
    dev.inject(vd::Inject::NakTimes, 2);
    CHECK(dev.poll_in_report(0x81).empty());
    CHECK(dev.poll_in_report(0x81).empty());
    CHECK_EQ(dev.poll_in_report(0x81).size(), 3u);   // 恢复
    CHECK(dev.stats().nak == 2);
    // DropPower → Attached
    dev.inject(vd::Inject::DropPower, 1);
    CHECK(dev.state() == vd::DevState::Attached);
    CHECK(dev.address() == 0);
    // ToggleErr/Babble → errors++ 且事件留痕
    dev.inject(vd::Inject::ToggleErr, 1);
    const auto errors_before = dev.stats().errors;
    dev.inject(vd::Inject::Babble, 1);
    CHECK(dev.stats().errors == errors_before + 1);
    bool has_inject_mark = false;
    for (const auto& e : dev.events())
        if (e.mark == 2) has_inject_mark = true;
    CHECK(has_inject_mark);
    // 事件 seq 单调
    for (size_t i = 1; i < dev.events().size(); ++i)
        CHECK(dev.events()[i].seq == dev.events()[i - 1].seq + 1);
}

static void test_vd_unconfigured_poll_naks() {
    vd::VirtualDevice dev(golden_model());
    dev.set_report_source([] { return std::vector<uint8_t>{1}; });
    CHECK(dev.poll_in_report(0x81).empty());   // 未配置 → NAK
    CHECK(dev.poll_in_report(0x81).empty());   // Attached 未过 Default？poll 不动状态机
    CHECK(dev.state() == vd::DevState::Attached);
}

int main() {
    ustlog::init(true, L"vd-selftest");
    auto log = ustlog::logger("app.shell");
    log->info("vd_selftest 启动（MS2 第八靶）");
    RUN_TEST(test_vd_enumeration_golden);
    RUN_TEST(test_vd_breakpoints);
    RUN_TEST(test_vd_injections);
    RUN_TEST(test_vd_unconfigured_poll_naks);
    const int rc = shell_test::run_all("vd_selftest");
    log->info("vd_selftest 结束 rc={}", rc);
    return rc;
}
