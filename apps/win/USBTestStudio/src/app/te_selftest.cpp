// te_selftest.cpp — 第九自测靶：MS3 测试台（模型/双后端/Pipeline/面板）+ MS5 切片。
#include "../src/app/log.h"
#include "../src/shell/shell_test.h"
#include "../src/shell/te_editor.h"
#include "../src/shell/te_model.h"
#include "../src/shell/te_runner.h"
#include "../src/shell/pipeline.h"
#include "../src/shell/vd_templates.h"

namespace te = usts::shell::te;
namespace vd = usts::shell::vd;

// ---------------------------------------------------------------------------
// T1 · 计划模型
// ---------------------------------------------------------------------------
static void test_plan_roundtrip_and_scan() {
    te::PlanModel p;
    p.name = "lab1 产测";
    p.backend = "virtual";
    te::PlanStep s1;
    s1.type = "enumerate";
    s1.name = "枚举检测";
    te::PlanStep s2;
    s2.type = "hid_polling_rate";
    s2.params = {{"seconds", "2"}};
    s2.limits = {{"min_hz", 900}};
    p.steps = {s1, s2};
    const std::string j = p.to_json();
    te::PlanModel o;
    std::string err;
    CHECK(te::PlanModel::from_json(j, o, err));
    CHECK_EQ(o.name, std::string("lab1 产测"));
    CHECK_EQ(o.steps.size(), 2u);
    CHECK_EQ(o.steps[1].param("seconds"), std::string("2"));
    CHECK_EQ(o.steps[1].limit("min_hz"), 900.0);
    CHECK(o.to_json() == j);
    // 坏输入
    CHECK(!te::PlanModel::from_json("{bad", o, err));
    CHECK(!te::PlanModel::from_json("{}", o, err));                 // 缺 steps
    CHECK(!te::PlanModel::from_json("{\"steps\":[{}]}", o, err));   // 缺 type
    // 扫描：临时目录两计划+一坏文件+一无关 json
    const std::wstring dir = shell_test::make_temp_dir();
    shell_test::write_file(dir + L"\\a.json", p.to_json().c_str());
    te::PlanModel q; q.name = "q";
    shell_test::write_file(dir + L"\\b.json", q.to_json().c_str());
    shell_test::write_file(dir + L"\\bad.json", "{oops");
    shell_test::write_file(dir + L"\\other.json", "{\"x\":1}");     // 无 steps=跳过
    const auto scan = te::scan_plans(dir);
    CHECK_EQ(scan.plans.size(), 2u);
    CHECK_EQ(scan.skipped, 2u);
    CHECK(!scan.plans[0].path.empty());
    shell_test::remove_temp_dir(dir);
}

// ---------------------------------------------------------------------------
// T2 · 双后端
// ---------------------------------------------------------------------------
static void test_run_mock_and_virtual() {
    te::PlanModel p;
    p.backend = "mock";
    te::PlanStep ok;
    ok.type = "enumerate"; ok.name = "枚举";
    te::PlanStep hz;
    hz.type = "hid_polling_rate"; hz.name = "回报率";
    hz.params = {{"seconds", "2"}};
    hz.limits = {{"min_hz", 900}};
    te::PlanStep out;
    out.type = "hid_output_write"; out.name = "LED";
    p.steps = {ok, hz, out};
    // mock：全 PASS（999.5 ≥ 900）
    auto r = te::run_plan(p, nullptr, te::Backend::Mock);
    CHECK(r.verdict());
    CHECK_EQ(r.passed(), 3u);
    // mock 限值失败演示：min_hz=1000 → FAIL 且不中断
    p.steps[1].limits = {{"min_hz", 1000}};
    r = te::run_plan(p, nullptr, te::Backend::Mock);
    CHECK(!r.verdict());
    CHECK_EQ(r.passed(), 2u);          // 失败不中断：3 步都执行
    // 虚拟后端：enumerate 对真 vd 设备跑通；output_write 如实 FAIL（无 OUT 路径）
    vd::VirtualDevice dev(vd::builtin_templates()[0].model);
    p.steps[1].limits = {{"min_hz", 50}};
    r = te::run_plan(p, &dev, te::Backend::Virtual);
    CHECK_EQ(r.steps.size(), 3u);
    CHECK(r.steps[0].pass);            // 枚举真跑 → Configured
    CHECK(r.steps[1].pass);            // poll 样本 200 拍 → 100Hz ≥ 50
    CHECK(!r.steps[2].pass);           // OUT 路径如实不可用
    CHECK(dev.state() == vd::DevState::Configured);
    // Virtual 后端无会话=mock 降级不炸
    r = te::run_plan(p, nullptr, te::Backend::Virtual);
    CHECK_EQ(r.steps.size(), 3u);
    CHECK(r.steps[0].note.find("降级") != std::string::npos);
}

// ---------------------------------------------------------------------------
// T3 · Pipeline
// ---------------------------------------------------------------------------
static void test_pipeline_run_and_roundtrip() {
    te::PlanModel plan;   // 空 steps 计划=verdict false（steps 空 → verdict()==false 口径）
    plan.name = "empty";
    te::PlanModel pass_plan;
    te::PlanStep e1; e1.type = "enumerate"; e1.name = "枚举";
    pass_plan.steps = {e1};
    te::PipelineModel f;
    te::FlowNode n1;
    n1.kind = te::FlowNode::Kind::RunPlan;
    n1.plan_json = pass_plan.to_json();
    te::FlowNode n2;
    n2.kind = te::FlowNode::Kind::Delay; n2.ticks = 3;
    te::FlowNode n3;
    n3.kind = te::FlowNode::Kind::AssertReport; n3.expect_pass = true;
    te::FlowNode n4;
    n4.kind = te::FlowNode::Kind::External; n4.command = "dfu-util -l";
    f.nodes = {n1, n2, n3, n4};
    // roundtrip
    const std::string j = f.to_json();
    te::PipelineModel o;
    std::string err;
    CHECK(te::PipelineModel::from_json(j, o, err));
    CHECK_EQ(o.nodes.size(), 4u);
    CHECK(o.nodes[1].kind == te::FlowNode::Kind::Delay && o.nodes[1].ticks == 3);
    CHECK(!te::PipelineModel::from_json("{}", o, err));
    // 执行（无 vd → run_plan 降级 mock → enumerate PASS → assert PASS）
    te::PipelineRunner runner;
    auto r = runner.run(f, nullptr);
    CHECK(r.verdict);
    CHECK_EQ(r.steps.size(), 4u);
    // 失败+abort：RunPlan(空计划=FAIL) → abort 截断
    te::PipelineModel bad;
    te::FlowNode b1;
    b1.kind = te::FlowNode::Kind::RunPlan;
    b1.plan_json = plan.to_json();
    te::FlowNode b2;
    b2.kind = te::FlowNode::Kind::Delay;
    bad.nodes = {b1, b2};
    auto rb = runner.run(bad, nullptr);
    CHECK(!rb.verdict);
    CHECK_EQ(rb.steps.size(), 1u);       // abort：第二节未执行
    CHECK(rb.steps[0].aborted);
    // continue 策略：全跑完
    bad.nodes[0].abort_on_fail = false;
    auto rc = runner.run(bad, nullptr);
    CHECK_EQ(rc.steps.size(), 2u);
    CHECK(!rc.steps[0].aborted);
}

// ---------------------------------------------------------------------------
// T4 · 面板烟测
// ---------------------------------------------------------------------------
static void test_te_panels_smoke() {
    HWND host = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 900, 500, nullptr,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
    CHECK(host != nullptr);
    HWND runner = te::RunnerPanel::create_w5(host);
    CHECK(runner != nullptr);
    te::PlanModel p;
    te::PlanStep s; s.type = "enumerate"; s.name = "枚举检测";
    p.steps = {s};
    te::RunnerPanel::instance()->run_mock(p);
    CHECK_EQ(ListBox_GetCount(te::RunnerPanel::instance()->list()), 1);
    // 失败计划渲染 FAIL 行
    te::PlanModel bad;
    te::PlanStep hz; hz.type = "hid_polling_rate"; hz.limits = {{"min_hz", 9999}};
    bad.steps = {hz};
    te::RunnerPanel::instance()->run_mock(bad);
    CHECK_EQ(ListBox_GetCount(te::RunnerPanel::instance()->list()), 1);
    // 报告中心：临时目录两 json（一 PASS 一 FAIL）
    const std::wstring dir = shell_test::make_temp_dir();
    shell_test::write_file(dir + L"\\r1.json", "{\"verdict\":\"PASS\"}");
    shell_test::write_file(dir + L"\\r2.json", "{\"verdict\":\"FAIL\"}");
    HWND report = te::ReportPanel::create_w6(host);
    CHECK(report != nullptr);
    te::ReportPanel::instance()->scan(dir);
    CHECK_EQ(te::ReportPanel::instance()->count(), 2u);
    shell_test::remove_temp_dir(dir);
    DestroyWindow(runner);
    DestroyWindow(report);
    DestroyWindow(host);
}

int main() {
    ustlog::init(true, L"te-selftest");
    auto log = ustlog::logger("app.shell");
    log->info("te_selftest 启动（MS3 第九靶）");
    RUN_TEST(test_plan_roundtrip_and_scan);
    RUN_TEST(test_run_mock_and_virtual);
    RUN_TEST(test_pipeline_run_and_roundtrip);
    RUN_TEST(test_te_panels_smoke);
    const int rc = shell_test::run_all("te_selftest");
    log->info("te_selftest 结束 rc={}", rc);
    return rc;
}
