// shell_selftest.cpp — 第六自测靶：MS0 shell 模块（tokens/设置/视角/面板注册/
// 命令+模糊/工程模型/模板库/日志视图模型/崩溃管理，随任务逐个追加 RUN_TEST）。
// 运行：apps/win/build/Release/shell_selftest.exe（任意 CWD）。
#include "../src/app/log.h"
#include "../src/shell/shell_test.h"
#include "../src/ui/tokens.h"

namespace t = usts::ui::tokens;

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

int main() {
    ustlog::init(true, L"shell-selftest");   // selftest 靶接 stdout（README §7）
    auto log = ustlog::logger("app.shell");
    log->info("shell_selftest 启动（MS0 第六靶）");

    RUN_TEST(test_tokens_values);
    RUN_TEST(test_tokens_icon_enum);

    const int rc = shell_test::run_all("shell_selftest");
    log->info("shell_selftest 结束 rc={}", rc);
    return rc;
}
