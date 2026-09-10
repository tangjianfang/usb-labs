// shell_selftest.cpp — 第六自测靶：MS0 shell 模块（tokens/设置/视角/面板注册/
// 命令+模糊/工程模型/模板库/日志视图模型/崩溃管理，随任务逐个追加 RUN_TEST）。
// 运行：apps/win/build/Release/shell_selftest.exe（任意 CWD）。
#include "../src/app/log.h"
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

int main() {
    ustlog::init(true, L"shell-selftest");   // selftest 靶接 stdout（README §7）
    auto log = ustlog::logger("app.shell");
    log->info("shell_selftest 启动（MS0 第六靶）");

    RUN_TEST(test_tokens_values);
    RUN_TEST(test_tokens_icon_enum);
    RUN_TEST(test_settings_roundtrip);
    RUN_TEST(test_settings_defaults_and_corrupt);
    RUN_TEST(test_settings_store_io);

    const int rc = shell_test::run_all("shell_selftest");
    log->info("shell_selftest 结束 rc={}", rc);
    return rc;
}
