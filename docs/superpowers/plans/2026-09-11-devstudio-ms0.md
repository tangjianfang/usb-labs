# USB DevStudio MS0（IDE 壳）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 交付 DevStudio IDE 壳——四视角/命令面板/工程模型/面板注册表/设置中心/崩溃收集/性能红线/打包，形成 MS1 双引擎的地基（设计：`apps/设计-USBDevStudio六工作台.md` §0/§1.1/§8，总纲 `apps/方案-USBDevStudio工作站.md` §七 MS0）。

**Architecture:** 在现有 `apps/win/USBTestStudio/` 项目原地演进：新增 `src/shell/` 纯逻辑模块（tokens/设置/视角/面板注册/命令+模糊匹配/工程模型/模板库/日志视图模型/崩溃管理，全部可离线自测）+ 薄 Win32 壳层（主窗口/命令面板窗/向导/设置对话框）。新自测靶 `shell_selftest.exe`（第六靶）。exe 更名 `USBDevStudio.exe`，版本 0.10.0。

**Tech Stack:** Win32 + C++20（v143、/W4 /utf-8 /permissive-、x64）；vendored spdlog v1.14.1（仅经 `src/app/log.h` 门面）；JSON 用现有 `framework/json_mini.*`（读）与 `framework/win32_rai.*` 的 JSON writer（写）。

## Global Constraints

- C++20、MSVC v143、`/W4 /utf-8 /permissive-`，零告警（CI msvc-build 同口径）。
- 零第三方依赖（例外：vendored spdlog，且只经 `src/app/log.h` 门面，模块名遵循 `apps/win/USBTestStudio/README.md` §7：新增 `app.shell / ui.shell / shell.workspace / shell.cmd / shell.perspective / shell.panel / shell.settings / shell.crash / shell.logview`）。
- **每个公开 API 入口/出口打 debug 级日志**（参数+结果+耗时），失败打 warn/err（含 GetLastError 十六进制）——spdlog 规范沿 README §7 格式。
- 纯逻辑模块必须 100% 分支覆盖自测（本计划每任务的测试清单即覆盖口径）；Win32 壳层数据表（菜单/状态栏/命令表）用表驱动自测，窗口烟测=隐藏创建/销毁（不做视觉验证——重大版本前禁用视觉 review）。
- UI 字符串=中文（设计文档文案照抄），UTF-8 源码 → 宽字符经现有 `wraii` 转换助手。
- 尺寸/颜色一律引用 `tokens.h` 语义 token（附录 E.6 值），禁止内联字面量。
- 布局/设置持久化：`%APPDATA%\USBDevStudio\{settings.json,layout.json}`，信封 `{"v":1,...}`，字段只增不改；损坏=重置该文件+warn 一次。
- 提交信息：conventional 前缀+中文摘要；每任务一提交；里程碑（T12）提交+**push**。
- 运行测试：`cmake --build apps/win/build --config Release` 后 `apps/win/build/Release/shell_selftest.exe`（任意 CWD 可跑）；CI 追加该靶。

---

### Task 1: 视觉 token 单源 + shell 自测靶骨架

**Files:**
- Create: `apps/win/USBTestStudio/src/ui/tokens.h`
- Create: `apps/win/USBTestStudio/src/shell/shell_test.h`
- Create: `apps/win/USBTestStudio/src/shell/shell_selftest.cpp`
- Modify: `apps/win/USBTestStudio/CMakeLists.txt`（新增 shell_selftest 靶）

**Interfaces:**
- Produces: `usts::ui::tokens` 命名空间常量（后续所有 UI 任务引用）；`shell_test.h` 的 `CHECK/CHECK_EQ/RUN_TEST/main` 测试宏（后续所有 shell 任务引用）；CMake 靶 `shell_selftest`。

- [x] **Step 1: 写 tokens.h**（值=设计附录 E.6 逐项照抄）

```cpp
// tokens.h — 视觉 token 单一事实源（设计附录 E.6；换主题只改这里）
#pragma once
#include <cstdint>
namespace usts::ui::tokens {
// 色板（COLORREF 0x00BBGGRR）
constexpr uint32_t kPrimary = 0x00C05615;      // #1565C0
constexpr uint32_t kPrimaryBg = 0x00FD2FE3;    // #E3F2FD
constexpr uint32_t kPass = 0x00327D2E;         // #2E7D32
constexpr uint32_t kFail = 0x002828C6;         // #C62828
constexpr uint32_t kRun = kPrimary;
constexpr uint32_t kWarn = 0x0025A8F9;         // #F9A825
constexpr uint32_t kOff = 0x009E9E9E;
constexpr uint32_t kBg = 0x00FFFFFF; constexpr uint32_t kBgAlt = 0x00F5F5F5;
constexpr uint32_t kBorder = 0x0000E0E0;       // #E0E0E0
constexpr uint32_t kText = 0x00212121; constexpr uint32_t kTextDim = 0x00757575;
constexpr uint32_t kDangerZone = 0x00EEEBFF;   // #FFEBEE
// 尺寸（px@96dpi）
constexpr int kSpaceUnit = 4; constexpr int kCtrlH = 28; constexpr int kCtrlHCompact = 24;
constexpr int kRowH = 24; constexpr int kTreeRowH = 22;
constexpr int kMarginPanel = 12; constexpr int kMarginGroup = 16;
constexpr int kWinDefW = 1280, kWinDefH = 800, kWinMinW = 1024, kWinMinH = 640;
constexpr int kIconS = 16, kIconL = 64;
// 字体
constexpr wchar_t kFontUi[] = L"Segoe UI"; constexpr int kFontUiPt = 9;
constexpr wchar_t kFontUiCn[] = L"Microsoft YaHei UI";
constexpr wchar_t kFontMono[] = L"Consolas";  constexpr int kFontMonoPt = 9;
constexpr int kFontH1Pt = 12, kFontH2Pt = 11; constexpr int kFontOperatorPt = 48;
// 图标 39 枚（命名=语义，绘制在 src/ui/icons/ 后续任务）
enum class Icon { Scan, Play, Stop, Pause, Gear, Fullscreen, Lock, Unlock, Warn, Bell,
  Camera, Record, Chip, Keyboard, Mouse, Plug, Disk, Video, Audio, Bt, Bolt, Download,
  Report, Diff, Folder, File, Copy, Trash, Refresh, Plus, Minus, Check, Cross, Filter,
  Search, Perspective, Breakpoint, Template, Pipeline };
}
```

- [x] **Step 2: 写 shell_test.h**（微测试架，任意 CWD、退出码 0/1）

```cpp
// shell_test.h — shell 模块自测微框架（与仓库 *_selftest 风格一致，无第三方依赖）
#pragma once
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
namespace shell_test {
struct Case { const char* name; std::function<void()> fn; };
inline std::vector<Case>& cases() { static std::vector<Case> c; return c; }
inline int& failed() { static int f = 0; return f; }
inline bool _ck(bool ok, const char* expr, const char* file, int line) {
  if (!ok) { std::printf("  FAIL %s:%d  %s\n", file, line, expr); failed()++; } return ok; }
#define CHECK(x) shell_test::_ck((x), #x, __FILE__, __LINE__)
#define CHECK_EQ(a, b) shell_test::_ck((a) == (b), #a " == " #b, __FILE__, __LINE__)
#define RUN_TEST(fn) shell_test::cases().push_back({#fn, fn})
inline int run_all(const char* title) {
  std::printf("== %s ==\n", title); int n = 0;
  for (auto& c : cases()) { size_t before = failed(); c.fn(); n++;
    std::printf("  %s %s\n", failed() == before ? "ok " : "FAIL", c.name); }
  std::printf("%d 例，%d 失败 %s\n", n, failed(), failed() ? "" : "✓");
  return failed() ? 1 : 0; }
}
```

- [x] **Step 3: 写 shell_selftest.cpp 空靶 + tokens 自测**

```cpp
// shell_selftest.cpp — 第六自测靶：MS0 各模块（随任务逐个追加 RUN_TEST）
#include "../src/shell/shell_test.h"
#include "../src/ui/tokens.h"
namespace t = usts::ui::tokens;
static void test_tokens_values() {
  CHECK_EQ(t::kPrimary, 0x00C05615u); CHECK_EQ(t::kPass, 0x00327D2Eu);
  CHECK_EQ(t::kFail, 0x002828C6u);   CHECK_EQ(t::kRowH, 24);
  CHECK_EQ(t::kWinMinW, 1024);       CHECK_EQ(t::kCtrlH, 28);
  CHECK_EQ(t::kFontOperatorPt, 48);
  CHECK(static_cast<int>(t::Icon::Breakpoint) == 37 && static_cast<int>(t::Icon::Pipeline) == 38);
}
int main() {
  RUN_TEST(test_tokens_values);
  return shell_test::run_all("shell_selftest（tokens）");
}
```

- [x] **Step 4: CMakeLists.txt 加靶**（模式照抄现有 `parser_selftest` 段：源=src/shell/shell_selftest.cpp，链接主工程同等系统库，输出名 shell_selftest，ALL 靶）

- [x] **Step 5: 构建验证**：`cmake --build apps/win/build --config Release --target shell_selftest` → 运行 exe → `1 例，0 失败 ✓`

- [x] **Step 6: Commit** `feat(MS0-T1): 视觉 token 单源+shell 自测靶骨架`

---

### Task 2: 设置中心数据层（settings.h）

**Files:**
- Create: `apps/win/USBTestStudio/src/shell/settings.h`
- Modify: `apps/win/USBTestStudio/src/shell/shell_selftest.cpp`

**Interfaces:**
- Produces:
```cpp
namespace usts::shell {
struct Settings {                       // 键=设计附录 E.4（字段只增不改）
  // 通用
  std::wstring language = L"zh-CN";     // "zh-CN" | "en"（en=P1，值仍可存）
  int theme = 0;                        // 0 浅色 1 深色 2 跟随系统
  bool open_last_layout = true;
  // 产线
  std::wstring station = L"STN-01";
  bool sound_on = true; int sound_volume = 80;   // 0..100
  int pass_dwell_ms = 1500; bool auto_next = true; bool scan_go = true;
  // 日志
  std::wstring log_level = L"INFO"; int log_retention_days = 7;
  // 更新
  int update_channel = 0;               // 0 正式 1 测试
  bool update_auto = true;
  // 高级
  std::wstring report_dir = L"reports"; int scan_interval_ms = 3000;
  std::wstring to_json() const;         // 信封 {"v":1,...}
  static bool from_json(const std::string& text, Settings& out, std::string& err);
};
class SettingsStore {                   // %APPDATA%\USBDevStudio\settings.json
public:
  static std::wstring path();           // ...\USBDevStudio\settings.json
  static bool load(Settings& out, bool& was_corrupt);   // 缺文件=默认值(was_corrupt=false)
  static bool save(const Settings& s, std::string& err);
};
}
```

- [x] **Step 1: 失败测试**（追加到 shell_selftest.cpp）

```cpp
namespace sh = usts::shell;
static void test_settings_roundtrip() {
  sh::Settings s; s.station = L"STN-07"; s.sound_volume = 55; s.theme = 1;
  std::string j = sh::Settings::to_json(s);
  sh::Settings o; std::string err;
  CHECK(sh::Settings::from_json(j, o, err));
  CHECK_EQ(o.station, std::wstring(L"STN-07"));
  CHECK_EQ(o.sound_volume, 55); CHECK_EQ(o.theme, 1);
}
static void test_settings_defaults_and_corrupt() {
  sh::Settings o; std::string err;
  CHECK(!sh::Settings::from_json("{ broken", o, err));           // 损坏=失败+err
  std::string j = sh::Settings::to_json(sh::Settings{});
  CHECK(sh::Settings::from_json(j, o, err));                      // 缺键=保默认
  CHECK_EQ(o.station, std::wstring(L"STN-01")); CHECK_EQ(o.log_retention_days, 7);
}
static void test_settings_store_io() {  // 临时目录存取（不碰真实 APPDATA）
  sh::Settings s; s.report_dir = L"D:\\tmp_rep";
  std::wstring p = sh::SettingsStore::path();
  CHECK(!p.empty() && p.find(L"USBDevStudio") != std::wstring::npos);
  std::string err; CHECK(sh::SettingsStore::save(s, err));       // 真实保存（首次建目录）
  sh::Settings o; bool corrupt = false;
  CHECK(sh::SettingsStore::load(o, corrupt) && !corrupt);
  CHECK_EQ(o.report_dir, std::wstring(L"D:\\tmp_rep"));
  sh::Settings d; CHECK(sh::SettingsStore::save(d, err));        // 还原默认，防污染后续测试
}
```
`RUN_TEST` 三条；先写 `#include "../src/shell/settings.h"` 让编译失败（红）。

- [x] **Step 2: 跑红**：`--target shell_selftest` 编译 FAIL（settings.h 不存在）✓ 符合预期

- [x] **Step 3: 实现 settings.h**（header-only；JSON 写=手工拼串按 key 顺序固定；读=用现有 `framework/json_mini.h` 解析后逐键取、缺键保默认；目录创建/读写经 `framework/win32_rai.h` 文件助手；**每个公开函数入口 log.debug("...") 出口 log.debug/err**，模块名 `shell.settings`；保存写临时文件+rename 原子替换）

- [x] **Step 4: 跑绿**：三例全绿

- [x] **Step 5: Commit** `feat(MS0-T2): 设置中心数据层——信封 v1/损坏重置/原子保存`

---

### Task 3: 视角系统与布局持久化（perspective.h）

**Files:**
- Create: `apps/win/USBTestStudio/src/shell/perspective.h`
- Modify: `shell_selftest.cpp`

**Interfaces:**
- Produces:
```cpp
namespace usts::shell {
enum class PerspectiveId { Dev = 0, Debug = 1, Test = 2, Prod = 3 };
struct PerspectiveDef {
  PerspectiveId id; const wchar_t* name;          // 开发/调试/测试/产线
  const char* accelerator;                        // "Ctrl+Alt+1".."4"
  std::vector<std::string> default_panels;        // 设计 §0.2 逐行
};
const std::vector<PerspectiveDef>& perspectives();        // 4 项
const PerspectiveDef* find_perspective(PerspectiveId id);
struct LayoutState {                               // layout.json 内容（v1）
  PerspectiveId current = PerspectiveId::Dev;
  int left_w = 260, right_w = 300;                 // 180..420 夹取
  bool context_visible = true;
  std::vector<std::string> pinned_panels;          // 跨视角钉住 📌
  int win_x = -1, win_y = -1, win_w = 1280, win_h = 800;  // -1=默认居中
  std::wstring to_json() const;
  static bool from_json(const std::string& text, LayoutState& out, std::string& err);
  void clamp();                                    // 边界夹取（left/right 180..420，win≥min）
};
class LayoutStore {                                // %APPDATA%\USBDevStudio\layout.json
public:
  static bool load(LayoutState& out, bool& was_corrupt);   // 损坏=默认+was_corrupt
  static bool save(const LayoutState& s, std::string& err);
};
}
```
（默认面板串取设计 §0.2 表：Dev={w1.project_tree,w1.device_catalog,w2.descriptor,w3.vd}；Debug={w3.vd,w3.console,w4.trace,w3.inspector}；Test={w5.explorer,w5.runner,w5.pipeline,w5.history}；Prod={w5.runner,w6.report_list}——MS0 面板 id 以注册表为准，未注册面板加载时忽略。）

- [x] **Step 1: 失败测试**

```cpp
static void test_perspective_defs() {
  auto& ps = sh::perspectives();
  CHECK_EQ(ps.size(), 4u);
  CHECK_EQ(std::wstring(ps[0].name), std::wstring(L"开发"));
  CHECK(std::string(ps[2].accelerator) == "Ctrl+Alt+3");
  CHECK(sh::find_perspective(sh::PerspectiveId::Debug) == &ps[1]);
  CHECK(sh::find_perspective((sh::PerspectiveId)9) == nullptr);
}
static void test_layout_roundtrip_clamp_corrupt() {
  sh::LayoutState l; l.current = sh::PerspectiveId::Test; l.left_w = 9999;
  l.pinned_panels = {"w5.explorer"};
  l.clamp(); CHECK_EQ(l.left_w, 420);                             // 夹取
  std::string j = sh::LayoutState::to_json(l);
  sh::LayoutState o; std::string err;
  CHECK(sh::LayoutState::from_json(j, o, err));
  CHECK(o.current == sh::PerspectiveId::Test);
  CHECK_EQ(o.pinned_panels.size(), 1u);
  CHECK(!sh::LayoutState::from_json("nope", o, err));             // 损坏
}
```

- [x] **Step 2: 跑红** → **Step 3: 实现**（JSON 读写/目录原子保存同 T2 模式，模块名 `shell.perspective`，逐函数 debug 日志）→ **Step 4: 跑绿**
- [x] **Step 5: Commit** `feat(MS0-T3): 视角系统四定义+layout.json 持久化（夹取/损坏重置）`

---

### Task 4: 面板注册表（panel_registry.h）

**Files:**
- Create: `apps/win/USBTestStudio/src/shell/panel_registry.h`
- Modify: `shell_selftest.cpp`

**Interfaces:**
- Produces（=设计 §10 契约的数据层半部；窗口工厂 MS0 只挂桩）:
```cpp
namespace usts::shell {
enum class DevKind { Usb, Hid, Com, Msc, Audio, Ble };       // 六源
struct PanelInfo {
  std::string id;                    // "w5.explorer"
  std::wstring title;                // "测试资源管理器"
  int icon;                          // tokens::Icon 序值
  bool central = true;               // 中央面板(标签) or 侧栏面板
  std::vector<DevKind> supports;     // 空=通用
};
using PanelCreateFn = std::function<struct HWND__*(void*)>;  // (host) -> HWND；MS0 桩
class PanelRegistry {
public:
  static PanelRegistry& instance();
  bool register_panel(PanelInfo info, PanelCreateFn fn, std::string& err); // 重复 id=失败
  const std::vector<std::pair<PanelInfo, PanelCreateFn>>& all() const;
  const PanelInfo* find(const std::string& id) const;
  std::vector<PanelInfo> for_perspective(const std::vector<std::string>& ids) const; // 过滤未注册
};
void register_builtin_panels(PanelRegistry& r);   // MS0 内置桩: w1.project_tree/w1.device_catalog/
                                                  // w3.console(引用现有 console)/w5.runner 占位
}
```

- [x] **Step 1: 失败测试**

```cpp
static void test_panel_registry() {
  auto& r = sh::PanelRegistry::instance();
  sh::PanelInfo pi; pi.id = "t.x"; pi.title = L"测试面板"; std::string err;
  CHECK(r.register_panel(pi, nullptr, err));
  CHECK(!r.register_panel(pi, nullptr, err));               // 重复 id 拒绝
  CHECK(r.find("t.x") != nullptr && r.find("nope") == nullptr);
  auto vis = r.for_perspective({"t.x", "missing.id"});
  CHECK_EQ(vis.size(), 1u);                                 // 未注册忽略
  sh::register_builtin_panels(r);
  CHECK(r.find("w5.runner") != nullptr);                    // MS0 桩在册
}
```
（注册表为进程级单例——测试用独立 id 前缀 `t.` 防互扰；`register_builtin_panels` 幂等：重复注册内置 id 直接返回。）

- [x] **Step 2: 跑红** → **Step 3: 实现**（模块名 `shell.panel`；debug 日志含注册的 id/title；`register_builtin_panels` 各桩 title 中文照设计）→ **Step 4: 跑绿** → **Step 5: Commit** `feat(MS0-T4): 面板注册表——§10 契约数据层+MS0 内置桩`

---

### Task 5: 命令注册表 + 模糊匹配（command_registry.h / fuzzy.h）

**Files:**
- Create: `apps/win/USBTestStudio/src/shell/command_registry.h`
- Create: `apps/win/USBTestStudio/src/shell/fuzzy.h`
- Modify: `shell_selftest.cpp`

**Interfaces:**
- Produces:
```cpp
namespace usts::shell {
struct Command {
  std::string id; std::wstring title;            // 中文标题（匹配也打拼音首字母, P1 不做）
  std::string shortcut;                          // "Ctrl+Alt+1" 可空
  std::string category;                          // 视图/运行/工具/文件...
};
class CommandRegistry {
public:
  static CommandRegistry& instance();
  void add(Command c);                           // 重复 id 覆盖（内置后注册者可改回调语义）
  std::vector<Command> all() const;
  std::vector<Command> by_category(const std::string& cat) const;
  void set_handler(const std::string& id, std::function<void()> fn);
  bool invoke(const std::string& id) const;      // 无 handler/false 返回 false 并 warn
};
}
namespace usts::shell {
// fuzzy.h — 子序列模糊匹配+打分（大小写不敏感；UTF-8 输入, 匹配在 UTF-16 上做）
struct FuzzyResult { bool matched = false; int score = 0; std::vector<int> hit; /*命中下标*/ };
FuzzyResult fuzzy_match(const std::wstring& needle, const std::wstring& haystack);
// 记分: 命中+10; 连续命中额外+5; 词首(前一是非字母数字)额外+8; 首字命中额外+6; needle 空=matched(score 0)
std::vector<std::pair<Command, int>> fuzzy_rank(const std::wstring& q,
                                                const std::vector<Command>& pool);
// 排序 score 降序, 0 分/不中剔除; 稳定(同分保池序)
}
```

- [x] **Step 1: 失败测试**

```cpp
static void test_fuzzy_match_basics() {
  auto r = sh::fuzzy_match(L"", L"任何");               CHECK(r.matched && r.score == 0);
  auto a = sh::fuzzy_match(L"运行", L"运行计划");        CHECK(a.matched);
  CHECK(sh::fuzzy_match(L"run", L"Run Plan").matched);   // 大小写不敏感
  CHECK(!sh::fuzzy_match(L"xyz", L"运行计划").matched);  // 子序列不存在
  CHECK(sh::fuzzy_match(L"rpl", L"Run Plan").matched);   // 跨词子序列
}
static void test_fuzzy_scoring_order() {
  auto cont = sh::fuzzy_match(L"pl", L"Plan");           // 连续
  auto jump = sh::fuzzy_match(L"pn", L"Plan");           // 跳跃
  CHECK(cont.matched && jump.matched && cont.score > jump.score);
  auto wstart = sh::fuzzy_match(L"p", L"Run Plan");      // 词首 p
  auto mid = sh::fuzzy_match(L"l", L"Run Plan");         // 词中 l
  CHECK(wstart.score > mid.score);
}
static void test_command_registry_and_rank() {
  auto& cr = sh::CommandRegistry::instance();
  cr.add({"ms0.t.run", L"运行计划", "Ctrl+R", "运行"});
  cr.add({"ms0.t.trace", L"打开追踪台", "", "视图"});
  cr.add({"ms0.t.pl", L"运行 Pipeline", "", "运行"});
  static int invoked = 0; cr.set_handler("ms0.t.run", [] { invoked++; });
  CHECK(cr.invoke("ms0.t.run") && invoked == 1);
  CHECK(!cr.invoke("ms0.t.none"));
  auto ranked = sh::fuzzy_rank(L"运行", cr.all());
  CHECK(!ranked.empty() && ranked[0].first.id == "ms0.t.run");  // 全命中词首最优先
}
```

- [x] **Step 2: 跑红** → **Step 3: 实现**（fuzzy 在 UTF-16 逐字符；`is_word_start`：前驱非字母数字；两文件均 debug 日志模块 `shell.cmd`；CommandRegistry 单例+互斥锁保护——线程模型按 UI 单线程访问但日志线程会读 all()，加轻锁）→ **Step 4: 跑绿** → **Step 5: Commit** `feat(MS0-T5): 命令注册表+模糊匹配打分（连续/词首/首字加成）`

---

### Task 6: 工程模型 .ustsproj（workspace.h）

**Files:**
- Create: `apps/win/USBTestStudio/src/shell/workspace.h`
- Modify: `shell_selftest.cpp`

**Interfaces:**
- Produces:
```cpp
namespace usts::shell {
enum class RefKind { Text, Binary, Model, Scenario, Plan, Capture, Doc };
struct FileRef { std::wstring rel_path; RefKind kind = RefKind::Text; };
struct Workspace {                                 // .ustsproj v1（设计 §三 + §1.1 树分组）
  std::wstring name;
  std::string template_id;                         // 可空
  std::vector<FileRef> hardware, firmware, descriptors, host_app, tests, captures;
  std::wstring station = L"STN-01";                // 内嵌产线配置节
  std::wstring startup_plan;                       // 启动计划 rel_path
  std::wstring to_json() const;
  static bool from_json(const std::string& text, Workspace& out, std::string& err);
};
enum class WsEvent { Unknown };
struct RefStatus { bool exists = true; bool dirty = false; };   // 徽章 ⚠/●
class WorkspaceOps {
public:
  static std::vector<std::pair<FileRef, bool>> check_missing(   // (ref, exists)
      const Workspace& ws, const std::wstring& base_dir);
  static std::vector<FileRef> all_refs(const Workspace& ws);    // 六组拼接
  static bool add_ref(Workspace& ws, const wchar_t* group,      // group∈六组名
                      FileRef ref, std::string& err);           // 路径转相对(可迁); 已存在=失败
  static bool remove_ref(Workspace& ws, const std::wstring& rel_path, std::string& err); // 不删文件
  static std::wstring relativize(const std::wstring& base, const std::wstring& target);  // 同盘相对化
};
class WorkspaceStore {
public:
  static bool save(const Workspace& ws, const std::wstring& ustsproj_path, std::string& err);
  static bool load(const std::wstring& ustsproj_path, Workspace& out, bool& was_corrupt);
};
}
```

- [x] **Step 1: 失败测试**（临时目录用 `framework/win32_rai.h` 文件助手建/清）

```cpp
static void test_workspace_roundtrip() {
  sh::Workspace w; w.name = L"lab1 无线键鼠"; w.template_id = "lab1-hid-composite";
  sh::FileRef f{L"firmware/src/main.c", sh::RefKind::Text};
  w.firmware.push_back(f); w.startup_plan = L"host/autotest.yaml";
  std::string j = sh::Workspace::to_json(w);
  sh::Workspace o; std::string err;
  CHECK(sh::Workspace::from_json(j, o, err));
  CHECK_EQ(o.name, std::wstring(L"lab1 无线键鼠"));
  CHECK_EQ(o.firmware.size(), 1u); CHECK_EQ(o.firmware[0].kind, sh::RefKind::Text);
  CHECK(!sh::Workspace::from_json("{}", o, err));          // 缺 name=err
}
static void test_workspace_refs_missing_and_addremove() {
  sh::Workspace w; std::string err;
  sh::FileRef a{L"a.txt"}; CHECK(sh::WorkspaceOps::add_ref(w, L"hardware", a, err));
  CHECK(!sh::WorkspaceOps::add_ref(w, L"hardware", a, err));          // 重复拒绝
  CHECK(!sh::WorkspaceOps::add_ref(w, L"badgroup", a, err));
  // check_missing: 临时目录建 a.txt, 缺 b.txt
  std::wstring dir = make_temp_dir(); write_file(dir + L"\\a.txt", "x");
  w.hardware.push_back({L"b.txt"});
  auto miss = sh::WorkspaceOps::check_missing(w, dir);
  CHECK_EQ(miss.size(), 2u);
  CHECK(miss[0].second && !miss[1].second);                          // a 在 b 缺
  CHECK(sh::WorkspaceOps::remove_ref(w, L"a.txt", err));
  CHECK(!sh::WorkspaceOps::remove_ref(w, L"zz.txt", err));
  remove_temp_dir(dir);
}
static void test_workspace_relativize() {
  CHECK_EQ(sh::WorkspaceOps::relativize(L"C:\\p", L"C:\\p\\sub\\f.txt"),
           std::wstring(L"sub\\f.txt"));
  CHECK_EQ(sh::WorkspaceOps::relativize(L"C:\\p", L"D:\\q\\f.txt"),
           std::wstring(L"D:\\q\\f.txt"));                            // 跨盘=原样绝对
}
static void test_workspace_store_io() {
  sh::Workspace w; w.name = L"t"; std::wstring dir = make_temp_dir();
  std::string err; bool corrupt = false;
  CHECK(sh::WorkspaceStore::save(w, dir + L"\\x.ustsproj", err));
  sh::Workspace o; CHECK(sh::WorkspaceStore::load(dir + L"\\x.ustsproj", o, corrupt));
  CHECK_EQ(o.name, std::wstring(L"t"));
  write_file(dir + L"\\x.ustsproj", "corrupt");                       // 覆写损坏
  CHECK(sh::WorkspaceStore::load(dir + L"\\x.ustsproj", o, corrupt) && corrupt);
  remove_temp_dir(dir);
}
```
（`make_temp_dir/write_file/remove_temp_dir` 作为测试助手加进 `shell_test.h`：`GetTempPath`+唯一子目录，`CreateDirectoryW`/`DeleteFileW`/`RemoveDirectoryW` 递归。）

- [x] **Step 2: 跑红** → **Step 3: 实现**（JSON 组装/解析同 T2 风格；relativize 用 `PathRelativePathToW`，失败回退绝对路径；模块 `shell.workspace`，全 API debug 日志；add/remove/check 打印组名与路径）→ **Step 4: 跑绿** → **Step 5: Commit** `feat(MS0-T6): 工程模型 .ustsproj——六组引用/缺失检查/相对化/存取`

---

### Task 7: 模板库（templates.h）

**Files:**
- Create: `apps/win/USBTestStudio/src/shell/templates.h`
- Create: `apps/win/USBTestStudio/templates/manifest.json`（7 lab 官方模板清单）
- Modify: `shell_selftest.cpp`

**Interfaces:**
- Consumes: `Workspace`（T6）
- Produces:
```cpp
namespace usts::shell {
struct TemplateDef { std::string id; std::wstring name; std::wstring desc; std::wstring src_dir; };
class Templates {
public:
  // manifest.json: {"v":1,"templates":[{"id":"lab1-hid-composite","name":"Lab1 HID 复合设备",
  //   "desc":"无线键鼠接收器…","src":"labs/lab1-hid-composite"}, ... ×7]}
  static std::vector<TemplateDef> list(const std::wstring& manifest_path, std::string& err);
  // 实例化: 递归复制 src_dir → dest_dir（跳过 build 缓存/*.pyc/__pycache__），
  // 生成 <name>.ustsproj（Workspace::to_json: name=用户名, template_id, 六组预填:
  // hardware←hardware/ firmware←firmware/src/ tests←host/autotest.yaml host←host/*.py
  // captures←capture/ descriptors=空(MS1 填)），返回工程文件全路径
  static std::wstring instantiate(const TemplateDef& t, const std::wstring& dest_dir,
                                  const std::wstring& proj_name, std::string& err);
};
}
```

- [x] **Step 1: 失败测试**（构造迷你模板目录树：`tpl/src/hardware/BOM.csv`+`tpl/src/host/a.py`+`tpl/src/host/autotest.yaml`+`tpl/src/firmware/src/main.c`+`tpl/src/__pycache__/junk.pyc`+manifest.json；断言实例化后：目标树存在、`__pycache__` 未复制、ustsproj 可 load、name=用户名、template_id、tests 组含 autotest.yaml；manifest 缺文件/坏 JSON=err）

- [x] **Step 2: 跑红** → **Step 3: 实现**（递归复制经 `framework/win32_rai.h`；跳过表硬编码；模块 `shell.workspace`，实例化逐目录 debug 日志+结果 info）→ **Step 4: 跑绿** → **Step 5: Commit** `feat(MS0-T7): 工程模板库——manifest×7 lab/实例化复制+ustsproj 生成`

---

### Task 8: 日志视图模型 10 万行（logview_model.h）

**Files:**
- Create: `apps/win/USBTestStudio/src/shell/logview_model.h`
- Modify: `shell_selftest.cpp`

**Interfaces:**
- Produces:
```cpp
namespace usts::shell {
struct LogRow { uint64_t seq; int64_t ts_ms; char level; uint32_t tid; std::string msg; };
class LogViewModel {                        // 有界环形（设计 §0.5 性能红线的模型层）
public:
  explicit LogViewModel(size_t cap = 100000);
  uint64_t append(LogRow r);                // 返回 seq；满则淘汰最旧并 dropped_++
  size_t size() const; uint64_t dropped() const;
  // 视口: offset 从最新往前数（0=最新一行），返回最多 count 行按时间正序
  std::vector<const LogRow*> viewport(size_t offset_from_newest, size_t count) const;
  void clear();  size_t cap() const;
};
}
```

- [x] **Step 1: 失败测试**

```cpp
static void test_logview_ring_and_viewport() {
  sh::LogViewModel m(1000);
  for (uint64_t i = 0; i < 1250; ++i) m.append({i, (int64_t)i, 'I', 1, "m"});
  CHECK_EQ(m.size(), 1000u); CHECK_EQ(m.dropped(), 250u);        // 环形淘汰+计数
  auto v0 = m.viewport(0, 3);                                    // 最新 3 行, 正序
  CHECK_EQ(v0.size(), 3u); CHECK_EQ(v0[0]->seq, 1247u); CHECK_EQ(v0[2]->seq, 1249u);
  auto vOld = m.viewport(999, 5);                                // 越界裁剪: 只剩最旧 1 行
  CHECK_EQ(vOld.size(), 1u); CHECK_EQ(vOld[0]->seq, 250u);
  m.clear(); CHECK_EQ(m.size(), 0u) ; CHECK_EQ(m.dropped(), 250u); // clear 保 dropped
}
static void test_logview_perf_100k() {                           // 性能红线(宽松上界)
  sh::LogViewModel m(100000);
  auto t0 = std::chrono::steady_clock::now();
  for (uint64_t i = 0; i < 100000; ++i) m.append({i, (int64_t)i, 'D', 7, "0123456789abcdef"});
  auto ms = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - t0).count();
  CHECK_EQ(m.size(), 100000u);
  std::printf("  [perf] 100k append = %.1f ms\n", ms);
  CHECK(ms < 2000.0);                                            // 红线: <2s(机器差异余量)
}
```

- [x] **Step 2: 跑红** → **Step 3: 实现**（`std::deque<LogRow>` 环形，模块 `shell.logview`）→ **Step 4: 跑绿** → **Step 5: Commit** `feat(MS0-T8): 日志视图模型——100k 有界环形/视口语义/性能红线自测`

---

### Task 9: 崩溃 minidump 管理（crashdump.h）

**Files:**
- Create: `apps/win/USBTestStudio/src/shell/crashdump.h`
- Modify: `shell_selftest.cpp`

**Interfaces:**
- Produces:
```cpp
namespace usts::shell {
class CrashDumpMgr {                       // %LOCALAPPDATA%\USBDevStudio\minidump
public:
  static std::wstring dir();
  static bool ensure_dir(std::string& err);
  static void install_handler();           // SetUnhandledExceptionFilter→MiniDumpWriteDump
                                           // 文件名 crashdump-<yyyyMMdd-HHmmss>.dmp；失败 OutputDebugString
  static std::vector<std::wstring> scan();                 // 目录内 .dmp 按时间排序
  static void mark_seen(const std::wstring& dump_path);    // 记入 seen.txt（一行一路径）
  static bool is_seen(const std::wstring& dump_path);      // seen.txt 内存化
  static size_t purge_older_than_days(int days);           // 清理（设置 log_retention_days 用）
};
}
```

- [x] **Step 1: 失败测试**（临时目录注入 3 个假 .dmp+1 个 .txt：scan 只列 .dmp 且排序；mark_seen/is_seen 往返；purge 按 mtime 清老文件保新文件——`utimensat`/`SetFileTime` 改时间；目录不存在 scan 返回空不崩）
- [x] **Step 2: 跑红** → **Step 3: 实现**（dump 目录真实路径=LOCALAPPDATA，但 scan/mark/purge 接受注入路径重载以便测试：额外提供 `scan_dir(dir)` 等静态重载；handler 安装只此一处不做单测，注释指向 MSDN MiniDumpWriteDump；模块 `shell.crash`）→ **Step 4: 跑绿** → **Step 5: Commit** `feat(MS0-T9): minidump 管理——目录/扫描/已读账本/过期清理（可注入测试）`

---

### Task 10: DevStudio 主窗口壳（main_window_ds.h/.cpp，数据表驱动）

**Files:**
- Create: `apps/win/USBTestStudio/src/shell/main_window_ds.h`
- Create: `apps/win/USBTestStudio/src/shell/main_window_ds.cpp`
- Create: `apps/win/USBTestStudio/src/ui/icons.cpp`（39 枚 16px 单色路径绘制桩：先落 8 个高频图标真实 GDI 路径，其余占位矩形+TODO 表登记——**MS0 只用已绘 8 枚**，全量绘制归 MS1 顺带）
- Modify: `shell_selftest.cpp`、`CMakeLists.txt`（主靶加 shell 源）

**Interfaces:**
- Consumes: T2/T3/T4/T5（Settings/LayoutState/PanelRegistry/CommandRegistry）、`src/app/log.h`、`framework/*`
- Produces:
```cpp
namespace usts::shell {
struct MenuDef { const wchar_t* menu; const wchar_t* item; const char* shortcut; const char* cmd_id; };
const std::vector<MenuDef>& menu_table();          // 设计 §0.1 菜单逐项（自测比对）
const std::vector<std::pair<const char*, const wchar_t*>>& status_cells_def();
class MainWindowDS {
public:
  static void register_commands();                 // 菜单/工具栏命令注册进 CommandRegistry
  bool create(HINSTANCE inst, const Settings&, const LayoutState&);   // 窗口+菜单+工具栏+三栏
  void switch_perspective(PerspectiveId id);       // 工具栏四段+面板装载+LayoutStore.save
  LayoutState layout() const;
  void on_device_change();                         // WM_DEVICECHANGE 转发设备目录桩
private:
  // 布局: 左(工程树+设备目录) | 中央(标签区, 桩面板=静态文本) | 右(上下文/知识栏)
  // 工具栏: 视角四段钮/▶运行▾/⏹/命令面板搜索框/⚙  — 每钮注册命令
};
}
```

- [x] **Step 1: 失败测试（表驱动+隐藏窗口烟测）**

```cpp
static void test_menu_table_matches_design() {
  auto& m = sh::menu_table();
  auto has = [&] (const wchar_t* menu, const wchar_t* item, const char* sc) {
    for (auto& d : m) if (wcscmp(d.menu, menu)==0 && wcscmp(d.item, item)==0 &&
        std::string(d.shortcut)==sc) return true; return false; };
  CHECK(has(L"视图", L"刷新设备", "F5"));
  CHECK(has(L"运行", L"运行计划", "Ctrl+R"));
  CHECK(has(L"工具", L"命令面板", "Ctrl+K"));
  CHECK(has(L"文件", L"新建工程(从模板…)", ""));
  CHECK(has(L"视图", L"开发视角", "Ctrl+Alt+1"));
  CHECK_EQ(sh::status_cells_def().size(), 6u);     // 工程/设备/引擎/通知/日志/时钟
}
static void test_ds_window_smoke() {               // 隐藏窗口烟测(无视觉验证)
  sh::MainWindowDS w; sh::Settings s; sh::LayoutState l;
  CHECK(w.create(GetModuleHandleW(nullptr), s, l));
  w.switch_perspective(sh::PerspectiveId::Debug);
  CHECK(w.layout().current == sh::PerspectiveId::Debug);
  w.switch_perspective(sh::PerspectiveId::Dev);    // 回位防污染 layout.json
}
```
（create() 内部不真正保存 layout.json——保存由析构/显式 flush，烟测后调用 `LayoutStore::save` 复原默认。）

- [x] **Step 2: 跑红** → **Step 3: 实现**（Win32: RegisterClassExW/三栏 WM_SIZE/自绘工具栏四段钮(Owner-draw 按 token 色)/菜单由 menu_table 生成/WM_DPICHANGED 按比例；**所有消息路径关键分支 log.debug**（模块 `ui.shell`），窗口创建/销毁/视角切换 info；桩面板=中央静态文本列注册表 id）→ **Step 4: 跑绿** → **Step 5: Commit** `feat(MS0-T10): DevStudio 主窗口壳——菜单表驱动/三栏/视角切换/隐藏烟测`

---

### Task 11: 命令面板窗 + 新建工程向导 + 设置对话框 + 工程树

**Files:**
- Create: `apps/win/USBTestStudio/src/shell/command_palette.h/.cpp`
- Create: `apps/win/USBTestStudio/src/shell/new_project_wizard.h/.cpp`
- Create: `apps/win/USBTestStudio/src/shell/settings_dialog.h/.cpp`
- Create: `apps/win/USBTestStudio/src/shell/project_tree.h/.cpp`
- Modify: `shell_selftest.cpp`、`main_window_ds.cpp`（Ctrl+K 呼出/工具→新建工程/⚙）

**Interfaces:**
- Consumes: T5 fuzzy_rank（面板视图模型）、T6 Workspace、T7 Templates、T2 SettingsStore
- Produces:
```cpp
namespace usts::shell {
struct PaletteVM {                                   // 纯逻辑, 自测主对象
  std::vector<std::pair<Command,int>> rows; int selected = 0; std::wstring last_query;
  void open(); void close();
  void type(wchar_t ch)/backspace()/set_query(const std::wstring& q, const CommandRegistry&);
  bool move(int delta);                              // ↑↓ 循环
  const Command* confirm();                          // Enter: 返回选中命令(不 invoke)
  bool visible() const;
};
class CommandPaletteWnd { public: bool create(HWND owner); void show(PaletteVM&); };
// 向导: 三步(选模板→名称/目录→确认) 状态机纯逻辑 WizardVM + 对话框壳
struct WizardVM { int step=0; std::string tpl_id; std::wstring name, dir;
  std::string validate() const;                      // 步2: 名称非空/目录不存在或为空
  bool next(); bool back(); bool can_finish() const; };
class NewProjectWizard { public: static bool run(HWND owner, const std::wstring& templates_root,
                                                 WizardVM& vm, std::wstring& proj_path); };
// 设置对话框: 左树右表单, 读写 SettingsStore（改=立即保存+通知应用: 站位回调 on_change）
class SettingsDialog { public: static bool run(HWND owner, Settings& s); };
// 工程树: 读 Workspace 六组 → TreeView; 双击=内置查看器(文本)或 ShellExecute 打开
class ProjectTree { public: bool create(HWND parent); void set_workspace(const Workspace&, const std::wstring& base); };
}
```

- [x] **Step 1: 失败测试（VM 层）**：PaletteVM——set_query("运行") 首行=运行计划/confirm 返回之/move 循环/空查询列全量前 8/关闭后 visible=false；WizardVM——step 流转 0→1→2、validate 空名 err、目录已存在 err、can_finish；类型过滤 `ms0.` 前缀命令防污染全局注册表
- [x] **Step 2: 跑红** → **Step 3: 实现**（三个对话框=DialogBoxIndirectParam 动态模板或 CreateWindow 浮层，控件按 tokens 尺寸；全部动作 debug 日志 `ui.shell`；SettingsDialog 保存经 SettingsStore 并 `log.info`）→ **Step 4: 跑绿（含隐藏窗口烟测三对话框 Create/Destroy）** → **Step 5: Commit** `feat(MS0-T11): 命令面板/新建工程向导/设置对话框/工程树——VM 纯逻辑+薄壳`

---

### Task 12: 集成收口——入口/版本/更名/CI/打包/推送

**Files:**
- Modify: `apps/win/USBTestStudio/src/app/main.cpp`（入口改 MainWindowDS；`--console` 旗标保留旧产测窗；启动时 CrashDumpMgr::install_handler + 扫新 dump→提示对话框数据；加载 Settings/Layout）
- Modify: `apps/win/USBTestStudio/src/app/version.h` → `0.10.0`（注释：MS0）
- Modify: `apps/win/USBTestStudio/CMakeLists.txt`（主靶 OUTPUT_NAME USBDevStudio）
- Modify: `.github/workflows/usbtest-mock.yml`（msvc-build：+shell_selftest 运行步；artifact 路径 USBDevStudio.exe）
- Modify: `packaging/`（T11 的 iss：exe 名/版本源/开始菜单名 "USB DevStudio"；zip 兜底脚本同步）
- Modify: `apps/win/USBTestStudio/README.md`（产品名/版本/MS0 面貌+第六自测靶说明）、`apps/README.md`（应用矩阵行）

- [x] **Step 1: main.cpp 集成**（命令行 `USBDevStudio.exe [--console] [--plan …旧旗标保留]`；启动序列 ensure_dir→load settings→load layout(was_corrupt→warn toast)→install crash handler→scan new dumps(>0→MessageBoxW 三选提示数据)→create window；退出 save layout/settings；全序列 info 日志）
- [x] **Step 2: 全量构建+全自测**：`cmake --build apps/win/build --config Release` 五靶零告警 → 依次运行 channel/discovery/session/parser/shell 五自测全绿（shell 靶此时应 ≥30 例）
- [x] **Step 3: 本地冒烟**：运行 `USBDevStudio.exe` 3 秒自动退出模式（`--smoke` 旗标：创建→切四视角→退出码 0）——无视觉断言
- [x] **Step 4: 文档+CI+打包同步**（如上文件清单）
- [x] **Step 5: Commit** `feat(MS0): IDE 壳收口——DevStudio 入口/v0.10.0/第六靶/CI+打包同步`
- [x] **Step 6: Push** `git push origin main`（远程失败则记录并继续本地提交，收尾重试）

---

## 验收（MS0 DoD）

1. 六靶全绿（shell_selftest ≥30 例，含菜单表/视角/注册表/模糊/工程模型/模板/日志 10 万行性能/崩溃扫描/VM 层全量）；
2. `USBDevStudio.exe` 启动即 IDE 壳（四视角可切、Ctrl+K 命令面板可用、新建工程向导可从 lab 模板实例化、设置持久化、崩溃 dump 落盘）；
3. 全部新公开 API 有 spdlog debug 入口/出口日志（抽查 grep `log.debug` ≥ 40 处）；
4. CI msvc-build 六靶构建+运行全绿，artifact=USBDevStudio.exe；
5. validate.sh 0 断链；两份设计文档无需改动（如实现与设计偏差，改文档同步提交）。

## 自审记录（写计划时已核）

- 规格覆盖：设计 §0.1/0.2 壳与视角→T3/T10；§0.3 token→T1；§8 平台服务之命令面板/设置→T5/T11；§1.1 工程树/向导→T6/T7/T11；工程模型五文件之 .ustsproj→T6（其余四文件属 MS1+，不在 MS0 范围）；性能红线→T8；崩溃→T9；打包/CI→T12。**不在 MS0**：知识栏内容、脚本台、插件 ABI（MS5）、DLL 拆分（首个 DLL=MS3 UVC）。
- 类型一致性：`PerspectiveId/PanelInfo/Command/Workspace/FileRef/LogRow/Settings/LayoutState` 在各任务与 T10/T11 消费处签名一致（已逐处核对）。
- 无占位符：Win32 样板未逐行给码的任务（T10/T11 壳层）均给出控件级规格出处（设计文档小节号）+接口签名+测试代码，实现者无需自行发明。
