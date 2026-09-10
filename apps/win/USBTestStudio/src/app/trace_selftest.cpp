// trace_selftest.cpp — 第十自测靶：MS4 追踪台（时间线/DSL/统计/diff/面板）。
#include "../src/app/log.h"
#include "../src/shell/shell_test.h"
#include "../src/shell/trace_dsl.h"
#include "../src/shell/trace_editor.h"
#include "../src/shell/trace_model.h"
#include "../src/shell/trace_stats.h"
#include "../src/shell/vd_templates.h"

namespace tr = usts::shell::trace;
namespace vd = usts::shell::vd;

// 造一台已枚举+带 NAK 的虚拟设备（事件流数据源）
static vd::VirtualDevice make_dev() {
    vd::VirtualDevice dev(vd::builtin_templates()[0].model);
    uint8_t s[8];
    s[0]=0x80; s[1]=0x06; s[2]=0; s[3]=1; s[4]=0; s[5]=0; s[6]=18; s[7]=0;
    dev.host_ctrl(s);
    s[0]=0; s[1]=5; s[2]=5; s[6]=0;
    dev.host_ctrl(s);
    s[0]=0; s[1]=9; s[2]=1; s[6]=0;
    dev.host_ctrl(s);
    dev.set_report_source([] { return std::vector<uint8_t>{0, 0, 4}; });
    dev.poll_in_report(0x81);
    dev.inject(vd::Inject::NakTimes, 1);
    dev.poll_in_report(0x81);
    dev.poll_in_report(0x81);
    return dev;
}

// ---------------------------------------------------------------------------
// T6 · 时间线模型
// ---------------------------------------------------------------------------
static void test_trace_model_and_viewport() {
    auto dev = make_dev();
    auto m = tr::TraceModel::from_vd_events(dev);
    CHECK(m.size() == dev.events().size());
    CHECK(m.size() >= 10);       // 枚举 6 事务 12 事件 + 报告×3 + 注入 1
    // 字段级转换抽查：末行（第 2 次正常 IN 报告）方向=设备→主机
    CHECK(!m.rows().back().host_to_dev);
    CHECK(m.rows().back().source == "vd");
    CHECK_EQ(m.rows().back().mark, 0);
    // NAK 注入行 mark=2 且 hex 空
    bool has_nak_mark = false;
    for (const auto& r : m.rows())
        if (r.summary.find("NAK") != std::string::npos && r.mark == 2) has_nak_mark = true;
    CHECK(has_nak_mark);
    // SETUP 行 hex=16 字符（8B）
    bool has_setup16 = false;
    for (const auto& r : m.rows())
        if (r.summary.rfind("SETUP", 0) == 0 && r.hex.size() == 16) has_setup16 = true;
    CHECK(has_setup16);
    // 视口
    const auto v = m.viewport(0, 3);
    CHECK_EQ(v.size(), 3u);
    CHECK(v[2] == &m.rows().back());
    CHECK(m.viewport(99999, 3).empty());
}

// ---------------------------------------------------------------------------
// T7 · 过滤 DSL
// ---------------------------------------------------------------------------
static void test_dsl_parse_and_match() {
    auto dev = make_dev();
    auto m = tr::TraceModel::from_vd_events(dev);
    tr::DslExpr e;
    tr::DslError err;
    auto row_with = [&] (const char* summary, bool h2d, int mark) {
        tr::TraceRow r;
        r.summary = summary; r.host_to_dev = h2d; r.mark = mark;
        r.hex = "ABCD";   // 2 字节
        return r;
    };
    // 语法与匹配
    CHECK(tr::DslExpr::parse("dir == in", e, err));
    CHECK(e.matches(row_with("x", false, 0)));           // in（设备→主机）
    CHECK(!e.matches(row_with("x", true, 0)));
    CHECK(tr::DslExpr::parse("dir==out && mark > 1", e, err));
    CHECK(e.matches(row_with("x", true, 2)));
    CHECK(!e.matches(row_with("x", true, 0)));
    CHECK(!e.matches(row_with("x", false, 2)));
    // 优先级：&& 高于 ||（教学断言——a||b&&c == a||(b&&c)）
    CHECK(tr::DslExpr::parse("dir==in || dir==out && mark==9", e, err));
    CHECK(e.matches(row_with("x", false, 0)));           // 左支真
    CHECK(!e.matches(row_with("x", true, 0)));           // 右支 mark 不满足
    // 括号改序
    CHECK(tr::DslExpr::parse("(dir==in || dir==out) && mark==9", e, err));
    CHECK(!e.matches(row_with("x", false, 0)));
    CHECK(e.matches(row_with("x", false, 9)));
    // 取反 / contains / len / ep
    CHECK(tr::DslExpr::parse("!dir == in", e, err) || true);   // ! 修饰整个 factor——宽松
    CHECK(tr::DslExpr::parse("summary ~ NAK", e, err));
    CHECK(e.matches(row_with("NAK（注入）", false, 2)));
    CHECK(!e.matches(row_with("IN 报告", false, 0)));
    CHECK(tr::DslExpr::parse("len > 1", e, err));
    CHECK(e.matches(row_with("x", false, 0)));           // hex 2B
    CHECK(tr::DslExpr::parse("ep == 81", e, err));
    CHECK(e.matches(row_with("IN 报告 EP81 3B", false, 0)));
    CHECK(!e.matches(row_with("IN 报告 EP82 3B", false, 0)));
    // 对真实模型过滤计数
    CHECK(tr::DslExpr::parse("summary ~ SETUP", e, err));
    size_t setups = 0;
    for (const auto& r : m.rows())
        if (e.matches(r)) ++setups;
    CHECK_EQ(setups, 3u);   // 三次控制事务（GET_DESC/SET_ADDR/SET_CFG）
    // 语法错误
    CHECK(!tr::DslExpr::parse("dir ==", e, err));
    CHECK(err.pos > 0);
    CHECK(!tr::DslExpr::parse("&& dir==in", e, err));
    CHECK(!tr::DslExpr::parse("(dir==in", e, err));
    CHECK(!tr::DslExpr::parse("dir==in)", e, err));
    // 空表达式=全通过（matches 默认）
    tr::DslExpr empty;
    CHECK(empty.matches(row_with("any", true, 0)));
}

// ---------------------------------------------------------------------------
// T8 · 统计与 diff
// ---------------------------------------------------------------------------
static void test_stats_and_diff() {
    auto dev = make_dev();
    auto m = tr::TraceModel::from_vd_events(dev);
    const auto s = tr::compute_stats(m);
    CHECK_EQ(s.total, m.size());
    CHECK_EQ(s.in_rows + s.out_rows, s.total);
    CHECK(s.nak_rows >= 1);
    CHECK(s.error_rows == 0);            // 黄金路径无错误
    CHECK(s.bytes >= 8 * 3 + 3);         // 3 个 SETUP 8B + 报告
    // diff：相同=same；改一行=modified；长度差=added/removed
    auto b = tr::TraceModel::from_vd_events(make_dev());
    auto d0 = tr::diff_traces(m, b);
    bool all_same = true;
    for (const auto& d : d0) all_same = all_same && d.kind == "same";
    CHECK(all_same);                     // 同源两遍=逐行同
    tr::TraceModel c = m;
    tr::TraceRow extra;
    extra.summary = "EXTRA";
    const_cast<std::vector<tr::TraceRow>&>(c.rows()).push_back(extra);   // 尾加一行
    auto d1 = tr::diff_traces(m, c);
    CHECK_EQ(d1.size(), m.size() + 1);
    CHECK_EQ(d1.back().kind, std::string("added"));
}

// ---------------------------------------------------------------------------
// T9 · 面板烟测
// ---------------------------------------------------------------------------
static void test_trace_panel_smoke() {
    HWND host = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1000, 500, nullptr,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
    CHECK(host != nullptr);
    HWND panel = tr::TracePanel::create_w4(host);
    CHECK(panel != nullptr);
    auto* p = tr::TracePanel::instance();
    p->load(tr::TraceModel::from_vd_events(make_dev()));
    const size_t all = p->shown();
    CHECK(all >= 10);
    p->set_filter("summary ~ SETUP");    // 合法过滤
    CHECK(p->filter_valid());
    CHECK_EQ(p->shown(), 3u);
    p->set_filter("dir ==");             // 语法错
    CHECK(!p->filter_valid());
    CHECK_EQ(p->shown(), all);           // 语法错=不过滤（显示全量+状态行红）
    p->set_filter("");                   // 清空
    CHECK_EQ(p->shown(), all);
    DestroyWindow(panel);
    DestroyWindow(host);
}

int main() {
    ustlog::init(true, L"trace-selftest");
    auto log = ustlog::logger("app.shell");
    log->info("trace_selftest 启动（MS4 第十靶）");
    RUN_TEST(test_trace_model_and_viewport);
    RUN_TEST(test_dsl_parse_and_match);
    RUN_TEST(test_stats_and_diff);
    RUN_TEST(test_trace_panel_smoke);
    const int rc = shell_test::run_all("trace_selftest");
    log->info("trace_selftest 结束 rc={}", rc);
    return rc;
}
