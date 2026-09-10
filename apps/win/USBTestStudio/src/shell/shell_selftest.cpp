// shell_selftest.cpp — 第六自测靶：MS0 shell 模块（tokens/设置/视角/面板注册/
// 命令+模糊/工程模型/模板库/日志视图模型/崩溃管理，随任务逐个追加 RUN_TEST）。
// 运行：apps/win/build/Release/shell_selftest.exe（任意 CWD）。
#include "../src/app/log.h"
#include "../src/shell/perspective.h"
#include "../src/shell/settings.h"
#include "../src/shell/shell_test.h"
#include "../src/ui/tokens.h"

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

    const int rc = shell_test::run_all("shell_selftest");
    log->info("shell_selftest 结束 rc={}", rc);
    return rc;
}
