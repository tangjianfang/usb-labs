// shell_selftest.cpp — 第六自测靶：MS0 shell 模块（tokens/设置/视角/面板注册/
// 命令+模糊/工程模型/模板库/日志视图模型/崩溃管理，随任务逐个追加 RUN_TEST）。
// 运行：apps/win/build/Release/shell_selftest.exe（任意 CWD）。
#include "../src/app/log.h"
#include "../src/shell/command_palette.h"
#include "../src/shell/command_registry.h"
#include "../src/shell/crashdump.h"
#include "../src/shell/fuzzy.h"
#include "../src/shell/logview_model.h"
#include "../src/shell/main_window_ds.h"
#include "../src/shell/new_project_wizard.h"
#include "../src/shell/panel_registry.h"
#include "../src/shell/perspective.h"
#include "../src/shell/project_tree.h"
#include "../src/shell/settings.h"
#include "../src/shell/shell_test.h"
#include "../src/shell/templates.h"
#include "../src/shell/workspace.h"
#include "../src/ui/icons.h"
#include "../src/ui/tokens.h"

#include <chrono>

namespace t = usts::ui::tokens;
namespace sh = usts::shell;

// ---------------------------------------------------------------------------
// T1 · tokens 单源（值=设计附录 E.6，改 token 前先改设计）
// ---------------------------------------------------------------------------
static void test_tokens_values() {
    CHECK_EQ(t::kPrimary, 0x00C05615u);
    CHECK_EQ(t::kPrimaryBg, 0x00FD2FE3u);
    CHECK_EQ(t::kPass, 0x00327D2Eu);
    CHECK_EQ(t::kFail, 0x002828C6u);
    CHECK_EQ(t::kWarn, 0x0025A8F9u);
    CHECK_EQ(t::kOff, 0x009E9E9Eu);
    CHECK_EQ(t::kBgAlt, 0x00F5F5F5u);
    CHECK_EQ(t::kBorder, 0x0000E0E0u);
    CHECK_EQ(t::kText, 0x00212121u);
    CHECK_EQ(t::kDangerZone, 0x00EEEBFFu);
    CHECK_EQ(t::kSpaceUnit, 4);
    CHECK_EQ(t::kCtrlH, 28);
    CHECK_EQ(t::kCtrlHCompact, 24);
    CHECK_EQ(t::kRowH, 24);
    CHECK_EQ(t::kTreeRowH, 22);
    CHECK_EQ(t::kWinDefW, 1280);
    CHECK_EQ(t::kWinDefH, 800);
    CHECK_EQ(t::kWinMinW, 1024);
    CHECK_EQ(t::kWinMinH, 640);
    CHECK_EQ(t::kIconS, 16);
    CHECK_EQ(t::kFontOperatorPt, 48);
    CHECK(std::wcscmp(t::kFontUi, L"Segoe UI") == 0);
    CHECK(std::wcscmp(t::kFontMono, L"Consolas") == 0);
    CHECK(std::wcscmp(t::kFontUiCn, L"Microsoft YaHei UI") == 0);
}

static void test_tokens_icon_enum() {
    // 39 枚语义图标（附录 B.4 清单；增删图标=设计先行）
    CHECK(static_cast<int>(t::Icon::Scan) == 0);
    CHECK(static_cast<int>(t::Icon::Search) == 34);
    CHECK(static_cast<int>(t::Icon::Perspective) == 35);
    CHECK(static_cast<int>(t::Icon::Breakpoint) == 36);
    CHECK(static_cast<int>(t::Icon::Template) == 37);
    CHECK(static_cast<int>(t::Icon::Pipeline) == 38);
    CHECK(static_cast<int>(t::Icon::Pipeline) - static_cast<int>(t::Icon::Scan) == 38);
}

// ---------------------------------------------------------------------------
// T2 · 设置中心数据层
// ---------------------------------------------------------------------------
static void test_settings_roundtrip() {
    sh::Settings s;
    s.station = L"STN-07"; s.sound_volume = 55; s.theme = 1;
    s.language = L"en"; s.update_channel = 1; s.scan_interval_ms = 500;
    const std::string j = s.to_json();
    sh::Settings o; std::string err;
    CHECK(sh::Settings::from_json(j, o, err));
    CHECK_EQ(o.station, std::wstring(L"STN-07"));
    CHECK_EQ(o.sound_volume, 55);
    CHECK_EQ(o.theme, 1);
    CHECK_EQ(o.language, std::wstring(L"en"));
    CHECK_EQ(o.update_channel, 1);
    CHECK_EQ(o.scan_interval_ms, 500);
}

static void test_settings_defaults_and_corrupt() {
    sh::Settings o; std::string err;
    CHECK(!sh::Settings::from_json("{ broken", o, err));       // 解析失败=err 非空
    CHECK(!err.empty());
    CHECK(sh::Settings::from_json("{}", o, err));              // 空对象无 v=按默认全收
    CHECK_EQ(o.station, std::wstring(L"STN-01"));
    CHECK_EQ(o.sound_volume, 80);
    CHECK_EQ(o.log_retention_days, 7);
    CHECK_EQ(o.pass_dwell_ms, 1500);
    CHECK_EQ(o.report_dir, std::wstring(L"reports"));
    // 版本不识别
    CHECK(!sh::Settings::from_json("{\"v\":99}", o, err));
    // 部分键缺失=保留默认（覆盖一半）
    sh::Settings h; h.station = L"STN-09";
    CHECK(sh::Settings::from_json(h.to_json(), o, err));
    CHECK_EQ(o.station, std::wstring(L"STN-09"));
    CHECK_EQ(o.theme, 0);
}

static void test_settings_store_io() {
    sh::Settings s; s.report_dir = L"usts-t2-verify"; s.theme = 1;
    std::string err;
    CHECK(sh::SettingsStore::save(s, err));                    // 建目录+原子保存
    const std::wstring p = sh::SettingsStore::path();
    CHECK(!p.empty() && p.find(L"USBDevStudio") != std::wstring::npos);
    sh::Settings o; bool corrupt = true;
    CHECK(sh::SettingsStore::load(o, corrupt));
    CHECK(!corrupt);
    CHECK_EQ(o.report_dir, std::wstring(L"usts-t2-verify"));
    CHECK_EQ(o.theme, 1);
    CHECK(sh::SettingsStore::save(sh::Settings{}, err));       // 还原默认，防污染后续
}

// ---------------------------------------------------------------------------
// T3 · 视角系统与布局持久化
// ---------------------------------------------------------------------------
static void test_perspective_defs() {
    const auto& ps = sh::perspectives();
    CHECK_EQ(ps.size(), 4u);
    CHECK_EQ(std::wstring(ps[0].name), std::wstring(L"开发"));
    CHECK_EQ(std::wstring(ps[1].name), std::wstring(L"调试"));
    CHECK_EQ(std::wstring(ps[2].name), std::wstring(L"测试"));
    CHECK_EQ(std::wstring(ps[3].name), std::wstring(L"产线"));
    CHECK(std::string(ps[0].accelerator) == "Ctrl+Alt+1");
    CHECK(std::string(ps[1].accelerator) == "Ctrl+Alt+2");
    CHECK(std::string(ps[2].accelerator) == "Ctrl+Alt+3");
    CHECK(std::string(ps[3].accelerator) == "Ctrl+Alt+4");
    CHECK(sh::find_perspective(sh::PerspectiveId::Debug) == &ps[1]);
    CHECK(sh::find_perspective(static_cast<sh::PerspectiveId>(9)) == nullptr);
    CHECK(!ps[0].default_panels.empty() && ps[0].default_panels[0] == "w1.project_tree");
    CHECK_EQ(ps[3].default_panels.size(), 2u);   // 产线视角两面板
}

static void test_layout_roundtrip_clamp_corrupt() {
    sh::LayoutState l;
    l.current = sh::PerspectiveId::Test;
    l.left_w = 9999; l.right_w = 100;            // 双越界
    l.pinned_panels = {"w5.explorer", ""};       // 含脏空串
    l.clamp();
    CHECK_EQ(l.left_w, 420);
    CHECK_EQ(l.right_w, 220);
    l.pinned_panels.erase(
        std::remove(l.pinned_panels.begin(), l.pinned_panels.end(), std::string()),
        l.pinned_panels.end());
    CHECK_EQ(l.pinned_panels.size(), 1u);
    const std::string j = l.to_json();
    sh::LayoutState o; std::string err;
    CHECK(sh::LayoutState::from_json(j, o, err));
    CHECK(o.current == sh::PerspectiveId::Test);
    CHECK_EQ(o.left_w, 420);
    CHECK_EQ(o.pinned_panels.size(), 1u);
    CHECK_EQ(o.pinned_panels[0], std::string("w5.explorer"));
    CHECK(!sh::LayoutState::from_json("nope", o, err));           // 解析失败
    CHECK(!sh::LayoutState::from_json("{\"v\":2}", o, err));      // 版本不识别
    CHECK(!sh::LayoutState::from_json("{\"current\":7}", o, err)); // current 越界
    // 下界夹取 + 窗口最小值
    sh::LayoutState low; low.left_w = 50; low.win_w = 100; low.clamp();
    CHECK_EQ(low.left_w, 180);
    CHECK_EQ(low.win_w, t::kWinMinW);
}

static void test_layout_store_io() {
    sh::LayoutState l; l.left_w = 333; l.context_visible = false;
    std::string err;
    CHECK(sh::LayoutStore::save(l, err));
    sh::LayoutState o; bool corrupt = true;
    CHECK(sh::LayoutStore::load(o, corrupt));
    CHECK(!corrupt);
    CHECK_EQ(o.left_w, 333);
    CHECK(!o.context_visible);
    CHECK(sh::LayoutStore::save(sh::LayoutState{}, err));   // 还原默认
}

// ---------------------------------------------------------------------------
// T4 · 面板注册表
// ---------------------------------------------------------------------------
static void test_panel_registry() {
    auto& r = sh::PanelRegistry::instance();
    const size_t before = r.all().size();
    sh::PanelInfo pi; pi.id = "t.x"; pi.title = L"测试面板"; pi.central = false;
    pi.supports = {sh::DevKind::Hid, sh::DevKind::Com};
    std::string err;
    CHECK(r.register_panel(pi, nullptr, err));
    CHECK_EQ(r.all().size(), before + 1);
    CHECK(!r.register_panel(pi, nullptr, err));          // 重复 id 拒绝
    CHECK(!err.empty() && err.find("t.x") != std::string::npos);
    const sh::PanelInfo* f = r.find("t.x");
    CHECK(f != nullptr && f->central == false && f->supports.size() == 2);
    CHECK(r.find("nope") == nullptr);
    auto vis = r.for_perspective({"t.x", "missing.id"});
    CHECK_EQ(vis.size(), 1u);                            // 未注册忽略
    CHECK_EQ(vis[0].id, std::string("t.x"));
}

static void test_builtin_panels_and_perspective_align() {
    auto& r = sh::PanelRegistry::instance();
    const size_t before = r.all().size();
    sh::register_builtin_panels(r);                      // 幂等：二调不增
    sh::register_builtin_panels(r);
    CHECK_EQ(r.all().size(), before + 12);
    // 内置 12 项全部在册
    for (const char* id : {"w1.project_tree", "w1.device_catalog", "w2.descriptor",
                           "w3.vd", "w3.console", "w3.inspector", "w4.trace",
                           "w5.explorer", "w5.runner", "w5.pipeline", "w5.history",
                           "w6.report_list"})
        CHECK(r.find(id) != nullptr);
    // 视角默认面板全部已注册（装配闭环：perspective.h 引用 ⊆ 注册表）
    for (const auto& p : sh::perspectives()) {
        const auto panels = r.for_perspective(p.default_panels);
        CHECK_EQ(panels.size(), p.default_panels.size());
    }
}

// ---------------------------------------------------------------------------
// T5 · 命令注册表 + 模糊匹配
// ---------------------------------------------------------------------------
static void test_fuzzy_match_basics() {
    const auto empty = sh::fuzzy_match(L"", L"任何");
    CHECK(empty.matched && empty.score == 0);             // 空查询=全量
    CHECK(sh::fuzzy_match(L"运行", L"运行计划").matched);   // 中文子序列
    CHECK(sh::fuzzy_match(L"run", L"Run Plan").matched);   // 大小写不敏感
    CHECK(!sh::fuzzy_match(L"xyz", L"运行计划").matched);   // 无子序列
    CHECK(sh::fuzzy_match(L"rpl", L"Run Plan").matched);   // 跨词子序列
    CHECK(!sh::fuzzy_match(L"planx", L"Run Plan").matched);
    const auto hits = sh::fuzzy_match(L"rp", L"Run Plan");
    CHECK(hits.matched && hits.hit.size() == 2 && hits.hit[0] == 0 && hits.hit[1] == 4);
}

static void test_fuzzy_scoring_order() {
    const auto cont = sh::fuzzy_match(L"pl", L"Plan");     // 连续（P0 l1）
    const auto jump = sh::fuzzy_match(L"pn", L"Plan");     // 跳跃
    CHECK(cont.matched && jump.matched);
    CHECK(cont.score > jump.score);
    const auto wstart = sh::fuzzy_match(L"p", L"Run Plan"); // 词首 p
    const auto mid = sh::fuzzy_match(L"l", L"Run Plan");    // 词中 l
    CHECK(wstart.matched && mid.matched && wstart.score > mid.score);
    const auto first = sh::fuzzy_match(L"r", L"Run Plan");  // 首字
    const auto later = sh::fuzzy_match(L"n", L"Run Plan");  // 词中 n
    CHECK(first.score > later.score);
}

static void test_command_registry_and_rank() {
    auto& cr = sh::CommandRegistry::instance();
    const auto before = cr.all().size();
    cr.add({"ms0.t.run", L"运行计划", "Ctrl+R", "运行"});
    cr.add({"ms0.t.trace", L"打开追踪台", "", "视图"});
    cr.add({"ms0.t.pl", L"运行 Pipeline", "", "运行"});
    CHECK_EQ(cr.all().size(), before + 3);
    cr.add({"ms0.t.run", L"运行计划(覆盖)", "Ctrl+R", "运行"});   // 覆盖不加行
    CHECK_EQ(cr.all().size(), before + 3);
    CHECK_EQ(cr.by_category("视图").size(), 1u);
    static int invoked = 0;
    cr.set_handler("ms0.t.run", [] { ++invoked; });
    CHECK(cr.has_handler("ms0.t.run"));
    CHECK(cr.invoke("ms0.t.run") && invoked == 1);
    CHECK(!cr.invoke("ms0.t.none"));                       // 未知=失败不抛
    cr.set_handler("ms0.t.unknown", [] {});                // 未注册 id=拒绝（静默）
    CHECK(!cr.has_handler("ms0.t.unknown"));
    // 模糊排序：全命中前缀并列时按注册序，ms0.t.run 先注册居首
    const auto ranked = sh::fuzzy_rank(L"运行", cr.all());
    CHECK(!ranked.empty());
    CHECK_EQ(ranked[0].first.id, std::string("ms0.t.run"));
    CHECK_EQ(ranked.size(), 2u);                           // trace 不含"运行"
}

// ---------------------------------------------------------------------------
// T6 · 工程模型 .ustsproj
// ---------------------------------------------------------------------------
static void test_workspace_roundtrip() {
    sh::Workspace w;
    w.name = L"lab1 无线键鼠";
    w.template_id = "lab1-hid-composite";
    w.firmware.push_back({L"firmware/src/main.c", sh::RefKind::Text});
    w.firmware.push_back({L"firmware/build/app.uf2", sh::RefKind::Binary});
    w.tests.push_back({L"host/autotest.yaml", sh::RefKind::Plan});
    w.startup_plan = L"host/autotest.yaml";
    const std::string j = w.to_json();
    sh::Workspace o; std::string err;
    CHECK(sh::Workspace::from_json(j, o, err));
    CHECK_EQ(o.name, std::wstring(L"lab1 无线键鼠"));
    CHECK_EQ(o.template_id, std::string("lab1-hid-composite"));
    CHECK_EQ(o.firmware.size(), 2u);
    CHECK_EQ(o.firmware[0].kind, sh::RefKind::Text);
    CHECK_EQ(o.firmware[1].kind, sh::RefKind::Binary);
    CHECK_EQ(o.tests[0].kind, sh::RefKind::Plan);
    CHECK_EQ(o.startup_plan, std::wstring(L"host/autotest.yaml"));
    CHECK_EQ(o.station, std::wstring(L"STN-01"));      // 缺省产线节
    // 必填与版本
    CHECK(!sh::Workspace::from_json("{}", o, err));     // 缺 name
    CHECK(!sh::Workspace::from_json("{\"name\":\"x\",\"v\":2}", o, err));
    // 未知 kind=回退 text（前向兼容）
    CHECK(sh::Workspace::from_json("{\"name\":\"x\",\"groups\":{\"firmware\":"
           "[{\"path\":\"a\",\"kind\":\"future-kind\"}]}}", o, err));
    CHECK_EQ(o.firmware[0].kind, sh::RefKind::Text);
}

static void test_workspace_refs_missing_and_addremove() {
    sh::Workspace w; std::string err;
    CHECK(sh::WorkspaceOps::add_ref(w, L"hardware", {L"a.txt"}, err));
    CHECK(!sh::WorkspaceOps::add_ref(w, L"hardware", {L"a.txt"}, err));   // 重复拒绝
    CHECK(!err.empty());
    err.clear();
    CHECK(!sh::WorkspaceOps::add_ref(w, L"badgroup", {L"z"}, err));       // 未知组
    CHECK_EQ(sh::WorkspaceOps::all_refs(w).size(), 1u);
    // check_missing：a.txt 在、b.txt 缺
    const std::wstring dir = shell_test::make_temp_dir();
    shell_test::write_file(dir + L"\\a.txt", "x");
    w.hardware.push_back({L"b.txt"});
    const auto miss = sh::WorkspaceOps::check_missing(w, dir);
    CHECK_EQ(miss.size(), 2u);
    CHECK(miss[0].second && !miss[1].second);
    // remove：命中删除 / 未命中失败；不删物理文件
    CHECK(sh::WorkspaceOps::remove_ref(w, L"a.txt", err));
    CHECK(!sh::WorkspaceOps::remove_ref(w, L"zz.txt", err));
    CHECK(sh::WorkspaceOps::all_refs(w).size() == 1);
    const DWORD attr = ::GetFileAttributesW((dir + L"\\a.txt").c_str());
    CHECK(attr != INVALID_FILE_ATTRIBUTES);           // 文件仍在
    shell_test::remove_temp_dir(dir);
}

static void test_workspace_relativize() {
    // 同盘→相对，'/' 分隔（持久化约定）
    CHECK_EQ(sh::WorkspaceOps::relativize(L"C:\\p", L"C:\\p\\sub\\f.txt"),
             std::wstring(L"sub/f.txt"));
    CHECK_EQ(sh::WorkspaceOps::relativize(L"C:\\p", L"C:\\p\\f.txt"),
             std::wstring(L"f.txt"));
    // 跨盘→原样绝对
    CHECK_EQ(sh::WorkspaceOps::relativize(L"C:\\p", L"D:\\q\\f.txt"),
             std::wstring(L"D:\\q\\f.txt"));
}

static void test_workspace_store_io() {
    const std::wstring dir = shell_test::make_temp_dir();
    const std::wstring p = dir + L"\\x.ustsproj";
    sh::Workspace w; w.name = L"测试工程"; w.tests.push_back({L"t.yaml", sh::RefKind::Plan});
    std::string err; bool corrupt = false;
    CHECK(sh::WorkspaceStore::save(w, p, err));
    sh::Workspace o;
    CHECK(sh::WorkspaceStore::load(p, o, corrupt) && !corrupt);
    CHECK_EQ(o.name, std::wstring(L"测试工程"));
    CHECK_EQ(o.tests.size(), 1u);
    shell_test::write_file(p, "corrupt");               // 覆写损坏
    CHECK(sh::WorkspaceStore::load(p, o, corrupt) && corrupt);
    CHECK_EQ(o.name, std::wstring(L""));                // 损坏=默认
    shell_test::remove_temp_dir(dir);
}

// ---------------------------------------------------------------------------
// T7 · 工程模板库
// ---------------------------------------------------------------------------
static sh::TemplateDef make_mini_template(const std::wstring& root) {
    // 迷你模板目录树：hardware/BOM.csv · firmware/src/main.c · host/a.py ·
    // host/autotest.yaml · firmware/build/app.uf2(应跳过) · __pycache__/junk.pyc(应跳过)
    const std::wstring src = root + L"\\tpl\\labX";
    CreateDirectoryW((root + L"\\tpl").c_str(), nullptr);
    CreateDirectoryW(src.c_str(), nullptr);
    CreateDirectoryW((src + L"\\firmware").c_str(), nullptr);   // 逐级建（CreateDirectoryW 不递归）
    shell_test::write_file(root + L"\\tpl\\manifest.json",
        "{\"v\":1,\"templates\":[{\"id\":\"labX\",\"name\":\"LabX 模板\",\"desc\":\"测试\","
        "\"src\":\"labX\"}]}");
    CreateDirectoryW((src + L"\\hardware").c_str(), nullptr);
    CreateDirectoryW((src + L"\\firmware\\src").c_str(), nullptr);
    CreateDirectoryW((src + L"\\firmware\\build").c_str(), nullptr);
    CreateDirectoryW((src + L"\\__pycache__").c_str(), nullptr);
    CreateDirectoryW((src + L"\\host").c_str(), nullptr);
    shell_test::write_file(src + L"\\hardware\\BOM.csv", "bom");
    shell_test::write_file(src + L"\\firmware\\src\\main.c", "int main(){}");
    shell_test::write_file(src + L"\\firmware\\build\\app.uf2", "bin");
    shell_test::write_file(src + L"\\__pycache__\\junk.pyc", "x");
    shell_test::write_file(src + L"\\host\\a.py", "print()");
    shell_test::write_file(src + L"\\host\\autotest.yaml", "name: t");
    sh::TemplateDef t;
    t.id = "labX"; t.name = L"LabX 模板"; t.src_dir = src;
    return t;
}

static void test_templates_list() {
    const std::wstring root = shell_test::make_temp_dir();
    make_mini_template(root);
    std::string err;
    const auto ts = sh::Templates::list(root + L"\\tpl\\manifest.json", err);
    CHECK_EQ(ts.size(), 1u);
    CHECK_EQ(ts[0].id, std::string("labX"));
    CHECK_EQ(ts[0].name, std::wstring(L"LabX 模板"));
    CHECK(!ts[0].src_dir.empty());
    // 损坏/缺文件
    CHECK(sh::Templates::list(root + L"\\nope.json", err).empty() && !err.empty());
    shell_test::write_file(root + L"\\tpl\\manifest.json", "{bad");
    err.clear();
    CHECK(sh::Templates::list(root + L"\\tpl\\manifest.json", err).empty() && !err.empty());
    shell_test::remove_temp_dir(root);
}

static void test_templates_instantiate() {
    const std::wstring root = shell_test::make_temp_dir();
    const sh::TemplateDef t = make_mini_template(root);
    std::string err;
    const std::wstring proj = sh::Templates::instantiate(t, root + L"\\tpl",
                                                         root + L"\\proj", L"我的工程", err);
    CHECK(!proj.empty()) /* err 已打印于日志 */;
    CHECK(proj.find(L"我的工程.ustsproj") != std::wstring::npos);
    // 跳过项未复制
    CHECK(GetFileAttributesW((root + L"\\proj\\firmware\\build\\app.uf2").c_str())
          == INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesW((root + L"\\proj\\__pycache__").c_str())
          == INVALID_FILE_ATTRIBUTES);
    // 复制项在 + 工程文件可解析 + 预填正确
    CHECK(GetFileAttributesW((root + L"\\proj\\firmware\\src\\main.c").c_str())
          != INVALID_FILE_ATTRIBUTES);
    sh::Workspace ws; bool corrupt = true;
    CHECK(sh::WorkspaceStore::load(proj, ws, corrupt) && !corrupt);
    CHECK_EQ(ws.name, std::wstring(L"我的工程"));
    CHECK_EQ(ws.template_id, std::string("labX"));
    CHECK_EQ(ws.hardware.size(), 1u);          // BOM.csv
    CHECK_EQ(ws.firmware.size(), 1u);          // main.c（build 跳过）
    CHECK_EQ(ws.host_app.size(), 2u);          // a.py + autotest.yaml
    CHECK_EQ(ws.tests.size(), 1u);
    CHECK_EQ(ws.startup_plan, std::wstring(L"host/autotest.yaml"));
    // 缺文件检查全绿（引用皆真实）
    CHECK_EQ(sh::WorkspaceOps::check_missing(ws, root + L"\\proj").size(), ws.tests.size()
             + ws.host_app.size() + ws.firmware.size() + ws.hardware.size());
    // 目标非空=拒绝
    err.clear();
    CHECK(sh::Templates::instantiate(t, root + L"\\tpl", root + L"\\proj", L"again", err)
          .empty());
    shell_test::remove_temp_dir(root);
}

// ---------------------------------------------------------------------------
// T8 · 日志视图模型（10 万行性能红线）
// ---------------------------------------------------------------------------
static void test_logview_ring_and_viewport() {
    sh::LogViewModel m(1000);
    CHECK_EQ(m.cap(), 1000u);
    for (uint64_t i = 0; i < 1250; ++i) m.append({i, static_cast<int64_t>(i), 'I', 1, "m"});
    CHECK_EQ(m.size(), 1000u);
    CHECK_EQ(m.dropped(), 250u);                       // 环形淘汰+计数
    const auto v0 = m.viewport(0, 3);                  // 最新 3 行正序
    CHECK_EQ(v0.size(), 3u);
    CHECK_EQ(v0[0]->seq, 1247u);
    CHECK_EQ(v0[2]->seq, 1249u);
    const auto vOld = m.viewport(999, 5);              // 越界裁剪：只剩最旧 1 行
    CHECK_EQ(vOld.size(), 1u);
    CHECK_EQ(vOld[0]->seq, 250u);
    CHECK(m.viewport(1000, 3).empty());                // 完全越界=空
    CHECK(m.viewport(0, 0).empty());                   // count=0=空
    m.clear();
    CHECK_EQ(m.size(), 0u);
    CHECK_EQ(m.dropped(), 250u);                       // clear 保 dropped
    CHECK(m.viewport(0, 5).empty());                   // 空模型视口=空
}

static void test_logview_perf_100k() {
    sh::LogViewModel m(100000);
    const auto t0 = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < 100000; ++i)
        m.append({i, static_cast<int64_t>(i), 'D', 7, "0123456789abcdef"});
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    CHECK_EQ(m.size(), 100000u);
    CHECK_EQ(m.dropped(), 0u);
    std::printf("  [perf] 100k append = %.1f ms\n", ms);
    CHECK(ms < 2000.0);                                // 红线 <2s（机器差异余量）
    const auto vp = m.viewport(0, 100);                // 尾部视口 O(count)
    CHECK_EQ(vp.size(), 100u);
    CHECK_EQ(vp.back()->seq, 99999u);
}

// ---------------------------------------------------------------------------
// T9 · 崩溃 minidump 管理（可注入重载）
// ---------------------------------------------------------------------------
static void test_crashdump_scan_seen_purge() {
    const std::wstring dir = shell_test::make_temp_dir();
    // 3 个 dmp + 1 个无关 txt
    shell_test::write_file(dir + L"\\crashdump-20260910-010101.dmp", "a");
    shell_test::write_file(dir + L"\\crashdump-20260910-020202.dmp", "b");
    shell_test::write_file(dir + L"\\crashdump-20260910-030303.dmp", "c");
    shell_test::write_file(dir + L"\\note.txt", "x");
    const auto list = sh::CrashDumpMgr::scan_dir(dir);
    CHECK_EQ(list.size(), 3u);                          // 只列 .dmp
    CHECK_EQ(list[0], dir + L"\\crashdump-20260910-010101.dmp");   // 按名=按时间排序
    CHECK(sh::CrashDumpMgr::scan_dir(dir + L"\\noexist").empty()); // 目录不存在=空
    // seen 账本往返 + 幂等
    CHECK(!sh::CrashDumpMgr::is_seen(dir, list[0]));
    CHECK(sh::CrashDumpMgr::mark_seen(dir, list[0]));
    CHECK(sh::CrashDumpMgr::mark_seen(dir, list[0]));   // 二次=幂等
    CHECK(sh::CrashDumpMgr::is_seen(dir, list[0]));
    CHECK(!sh::CrashDumpMgr::is_seen(dir, list[2]));
    // purge：新鲜文件不清理（days=1 应清零——文件刚建）
    CHECK_EQ(sh::CrashDumpMgr::purge_older_than_days(dir, 1), 0u);
    CHECK_EQ(sh::CrashDumpMgr::scan_dir(dir).size(), 3u);
    // 真实目录路径形状
    const std::wstring real = sh::CrashDumpMgr::dir();
    CHECK(real.find(L"USBDevStudio") != std::wstring::npos
          && real.find(L"minidump") != std::wstring::npos);
    std::string err;
    CHECK(sh::CrashDumpMgr::ensure_dir(err));
    shell_test::remove_temp_dir(dir);
}

// ---------------------------------------------------------------------------
// T10 · DevStudio 主窗口壳（数据表 + 隐藏窗口烟测；不做视觉验证——重大版本前禁用）
// ---------------------------------------------------------------------------
static void test_menu_table_matches_design() {
    const auto& m = sh::menu_table();
    auto has = [&] (const wchar_t* menu, const wchar_t* item, const char* sc) {
        for (const auto& d : m)
            if (wcscmp(d.menu, menu) == 0 && wcscmp(d.item, item) == 0 &&
                std::string(d.shortcut) == sc)
                return true;
        return false;
    };
    CHECK(has(L"文件", L"新建工程(从模板…)", ""));
    CHECK(has(L"文件", L"打开工程…", "Ctrl+O"));
    CHECK(has(L"视图", L"开发视角", "Ctrl+Alt+1"));
    CHECK(has(L"视图", L"刷新设备", "F5"));
    CHECK(has(L"运行", L"运行计划", "Ctrl+R"));
    CHECK(has(L"工具", L"命令面板", "Ctrl+K"));
    CHECK(has(L"工具", L"设置", "Ctrl+,"));
    CHECK(has(L"帮助", L"用户手册", "F1"));
    CHECK_EQ(sh::status_cells_def().size(), 6u);   // 工程/设备/引擎/通知/日志/时钟
    // 全表 cmd_id 唯一（分隔线除外）
    size_t real = 0;
    for (const auto& d : m) {
        if (!*d.cmd_id) continue;
        ++real;
        size_t dup = 0;
        for (const auto& e : m)
            if (*e.cmd_id && std::string(e.cmd_id) == std::string(d.cmd_id)) ++dup;
        CHECK_EQ(dup, 1u);
    }
    CHECK(real >= 20);
}

static void test_ds_window_smoke() {
    sh::MainWindowDS::register_commands();
    auto& cr = sh::CommandRegistry::instance();
    CHECK(cr.has_handler("view.refresh_devices"));   // 菜单命令全注册
    CHECK(cr.has_handler("tools.command_palette"));
    CHECK(cr.invoke("view.refresh_devices"));        // 桩 handler 可执行
    // 隐藏创建/切换/销毁（无视觉断言）
    sh::MainWindowDS w;
    sh::Settings cfg; sh::LayoutState layout;
    layout.win_w = 1100; layout.win_h = 700;        // ≥最小值
    CHECK(w.create(GetModuleHandleW(nullptr), cfg, layout));
    CHECK(w.hwnd() != nullptr);
    CHECK_EQ(w.current_perspective(), 0);
    w.switch_perspective(2);
    CHECK_EQ(w.current_perspective(), 2);
    w.switch_perspective(9);                        // 越界=不变
    CHECK_EQ(w.current_perspective(), 2);
    w.switch_perspective(0);
    w.show(SW_HIDE);
    w.request_quit();                               // WM_CLOSE → Destroy → PostQuitMessage
    MSG msg;
    int got = 0;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { ++got; break; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CHECK_EQ(got, 1);                               // 干净退出路径
}

static void test_icons_draw_smoke() {
    // 39 枚全部在内存 DC 上绘制不崩（几何字形完整性）
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bm = CreateCompatibleBitmap(screen, 32, 32);
    const auto old = (HBITMAP)SelectObject(mem, bm);
    for (int i = 0; i <= 38; ++i)
        CHECK(usts::ui::draw_icon(mem, static_cast<t::Icon>(i), 8, 8, t::kText));
    SelectObject(mem, old);
    DeleteObject(bm);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

// ---------------------------------------------------------------------------
// T11 · 命令面板 VM / 新建工程向导 VM（纯逻辑层）
// ---------------------------------------------------------------------------
static void test_palette_vm() {
    auto& cr = sh::CommandRegistry::instance();
    const size_t before = cr.all().size();
    cr.add({"ms0.p1", L"面板命令甲", "Ctrl+P", "工具"});
    cr.add({"ms0.p2", L"面板命令乙", "", "工具"});
    sh::PaletteVM vm;
    vm.open();
    CHECK(vm.visible);
    CHECK_EQ(vm.rows.size(), (std::min)(size_t{8}, before + 2));   // 空查询前 8
    CHECK_EQ(vm.selected, 0);
    vm.type(L'甲');                       // 过滤到 1 行
    CHECK_EQ(vm.rows.size(), 1u);
    CHECK_EQ(vm.rows[0].first.id, std::string("ms0.p1"));
    const sh::Command* c = vm.confirm();
    CHECK(c != nullptr && c->id == "ms0.p1");
    vm.backspace();
    vm.backspace();                       // 退成空串（backspace 空查询=保持空）
    vm.set_query(L"");
    CHECK_EQ(vm.rows.size(), (std::min)(size_t{8}, before + 2));
    vm.move(1); vm.move(-1);              // 循环移动
    CHECK_EQ(vm.selected, 0);
    vm.close();
    CHECK(!vm.visible);
    CHECK(vm.confirm() == nullptr);       // 关闭后确认=nullptr
    vm.set_query(L"乙");
    CHECK_EQ(vm.rows.size(), 1u);
    vm.move(5);                           // 循环取模不越界
    CHECK(vm.selected == 0 || vm.selected == -0);
}

static void test_wizard_vm() {
    sh::WizardVM w;
    w.templates.push_back({"labX", L"LabX", L"", L""});
    // step0：未选模板
    CHECK(!w.next());
    CHECK(w.validate() == std::wstring(L"请选择一个模板"));
    w.tpl_id = "labX";
    CHECK(w.next());
    // step1：名称/目录校验
    CHECK_EQ(w.step, 1);
    CHECK(!w.next());
    CHECK(w.validate() == std::wstring(L"工程名不能为空"));
    w.name = L"我的工程";
    CHECK(!w.next());
    CHECK(w.validate() == std::wstring(L"目标目录不能为空"));
    const std::wstring dir = shell_test::make_temp_dir();
    shell_test::write_file(dir + L"\\占用.txt", "x");
    w.dir = dir;
    CHECK(!w.next());
    CHECK(w.validate() == std::wstring(L"目标目录已存在且非空"));
    shell_test::remove_temp_dir(dir);
    CreateDirectoryW(dir.c_str(), nullptr);           // 空目录=允许
    CHECK(w.next());
    CHECK(w.can_finish());
    // back 流转
    CHECK(w.back());
    CHECK_EQ(w.step, 1);
    CHECK(w.next());
    // finish：模板在列表 → 实例化成功（目录空）
    std::string err;
    // 模板 src 放 dir\src，目标改 dir\proj（独立子目录——src 不能在 dest 里）
    const std::wstring src = dir + L"\\src";
    CreateDirectoryW(src.c_str(), nullptr);
    w.templates[0].src_dir = src;
    w.dir = dir + L"\\proj";
    const std::wstring proj = w.finish(dir, err);
    CHECK(!proj.empty());
    CHECK(proj.find(L"我的工程.ustsproj") != std::wstring::npos);
    shell_test::remove_temp_dir(dir);
    // 模板不在列表=finish 失败
    sh::WizardVM w2;
    w2.step = 2; w2.tpl_id = "nope"; w2.name = L"n"; w2.dir = dir;
    CHECK(w2.finish(dir, err).empty());
}

static void test_palette_window_smoke() {
    sh::CommandPaletteWnd wnd;
    CHECK(sh::CommandPaletteWnd::register_class(GetModuleHandleW(nullptr)));
    CHECK(wnd.create(GetModuleHandleW(nullptr), nullptr));
    CHECK(wnd.hwnd() != nullptr);
    DestroyWindow(wnd.hwnd());
}

static void test_project_tree_smoke() {
    // 宿主静态窗 + 树创建 + 装载（含缺失 ⚠ 分支）
    HWND host = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 200, 300,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    CHECK(host != nullptr);
    sh::ProjectTree tree;
    CHECK(tree.create(host, 0, 0, 200, 300));
    sh::Workspace ws;
    ws.name = L"t";
    ws.hardware.push_back({L"有.txt"});
    ws.hardware.push_back({L"缺.txt"});
    const std::wstring dir = shell_test::make_temp_dir();
    shell_test::write_file(dir + L"\\有.txt", "x");
    tree.set_workspace(ws, dir);
    CHECK_EQ(TreeView_GetCount(tree.hwnd()), 8u);   // 6 组根（含空组）+ 2 引用
    DestroyWindow(host);
    shell_test::remove_temp_dir(dir);
}

int main() {
    ustlog::init(true, L"shell-selftest");   // selftest 靶接 stdout（README §7）
    auto log = ustlog::logger("app.shell");
    log->info("shell_selftest 启动（MS0 第六靶）");

    RUN_TEST(test_tokens_values);
    RUN_TEST(test_tokens_icon_enum);
    RUN_TEST(test_settings_roundtrip);
    RUN_TEST(test_settings_defaults_and_corrupt);
    RUN_TEST(test_settings_store_io);
    RUN_TEST(test_perspective_defs);
    RUN_TEST(test_layout_roundtrip_clamp_corrupt);
    RUN_TEST(test_layout_store_io);
    RUN_TEST(test_panel_registry);
    RUN_TEST(test_builtin_panels_and_perspective_align);
    RUN_TEST(test_fuzzy_match_basics);
    RUN_TEST(test_fuzzy_scoring_order);
    RUN_TEST(test_command_registry_and_rank);
    RUN_TEST(test_workspace_roundtrip);
    RUN_TEST(test_workspace_refs_missing_and_addremove);
    RUN_TEST(test_workspace_relativize);
    RUN_TEST(test_workspace_store_io);
    RUN_TEST(test_templates_list);
    RUN_TEST(test_templates_instantiate);
    RUN_TEST(test_logview_ring_and_viewport);
    RUN_TEST(test_logview_perf_100k);
    RUN_TEST(test_crashdump_scan_seen_purge);
    RUN_TEST(test_menu_table_matches_design);
    RUN_TEST(test_ds_window_smoke);
    RUN_TEST(test_icons_draw_smoke);
    RUN_TEST(test_palette_vm);
    RUN_TEST(test_wizard_vm);
    RUN_TEST(test_palette_window_smoke);
    RUN_TEST(test_project_tree_smoke);

    const int rc = shell_test::run_all("shell_selftest");
    log->info("shell_selftest 结束 rc={}", rc);
    return rc;
}
