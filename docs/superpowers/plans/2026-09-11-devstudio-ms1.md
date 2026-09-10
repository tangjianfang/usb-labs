# USB DevStudio MS1（描述符编译器）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 交付 W2 描述符台引擎——模型(.ustsdesc) ⇄ 二进制 ⇄ C(TinyUSB) 三向编译闭环 + Linter 首批 25 规 + 字段级 diff + W2 编辑器面板，替换 w2.descriptor 桩（设计 `apps/设计-USBDevStudio六工作台.md` §2，总纲 §七 MS1）。

**Architecture:** 纯逻辑内核五模块（`src/shell/desc_*.h`：模型/解析/构建/生成/Lint/diff）全部可离线自测，新自测靶 `desc_selftest.exe`（第七靶）；W2 编辑器薄壳（`desc_editor.h/.cpp`）接入面板注册表。**范围诚实账**：反编译输入=二进制文件/模型 JSON（真机 GET_DESCRIPTOR 控制传输与"下发虚拟 DUT"依赖设备访问层与 usbsim 设备实例，归 MS2——设计 §2.2 两处已注记，本计划不实现）。

**Tech Stack:** 同 MS0（Win32/C++20/零第三方/spdlog 经 log.h 门面/json_mini 读+wraii json_writer 写）。

## Global Constraints（沿 MS0 全文有效）

- /W4 /utf-8 /permissive- 零告警；spdlog 每公开 API 入口/出口 debug（模块名 `desc.model/parse/build/gen/lint/diff/ui.editor`）；纯逻辑分支全覆盖；**round-trip 幂等是硬属性**（model→json→model ≡、well-formed 下 model→bin→model ≡）；危险动作无（本里程碑无写盘外操作）；每任务一提交，T8 收口推送；validate.sh 0 断链。
- 二进制口径：USB 2.0 规范字段布局（§9.6.1 设备/§9.6.3 配置/§9.6.5 接口/§9.6.6 端点/§9.7 字符串/HID 类规范 1.11/IAD ECN）；**little-endian**（wValue/wTotalLength 等多字节低字节在前）。
- 测试数据：内置标准样例（Lab1 键鼠复合完整描述符二进制，与 `labs/lab1-hid-composite/firmware/src/usb_descriptors.c` 口径对齐——以 TinyUSB 生成的真实字节为基准手工构造）。

---

### Task 1: 描述符模型与 JSON 序列化（desc_model.h）

**Files:**
- Create: `apps/win/USBTestStudio/src/shell/desc_model.h`
- Create: `apps/win/USBTestStudio/src/app/desc_selftest.cpp`
- Modify: `CMakeLists.txt`（desc_selftest 第七靶，模式照抄 shell_selftest；链 setupapi shlwapi dbghelp comctl32 + 新增 desc 源仅本头=header-only）

**Interfaces（Produces）:**
```cpp
namespace usts::shell::desc {
constexpr uint8_t kTypeDevice = 0x01, kTypeConfiguration = 0x02, kTypeString = 0x03,
                  kTypeInterface = 0x04, kTypeEndpoint = 0x05, kTypeHid = 0x21,
                  kTypeReport = 0x22, kTypeIad = 0x0B;
struct UnknownBlock { std::string after; std::string raw_hex; };   // round-trip 保真
struct DeviceDesc { uint16_t bcd_usb=0x0200; uint8_t dev_class=0, sub_class=0, protocol=0,
    max_packet0=8; uint16_t vid=0, pid=0, bcd_device=0x0100; uint8_t i_man=1, i_prod=2,
    i_serial=3, num_configs=1; };
struct EndpointDesc { uint8_t address=0x81, attributes=0x03; uint16_t max_packet=64,
    interval=1; };
struct HidDesc { uint16_t bcd_hid=0x0111; uint8_t country=0;
    std::string report_hex;                                        // 报告描述符原始字节
};
struct IadDesc { uint8_t first_if=0, if_count=1, fn_class=0, fn_sub=0, fn_protocol=0,
    i_function=0; };
struct InterfaceDesc { uint8_t number=0, alt=0, if_class=3, sub_class=0, protocol=0,
    i_if=0; std::optional<IadDesc> iad; std::optional<HidDesc> hid;
    std::vector<EndpointDesc> endpoints; };
struct ConfigDesc { uint8_t value=1, i_cfg=0, attributes=0x80, max_power=50;   // 100mA
    std::vector<InterfaceDesc> interfaces; };
struct StringTable { std::vector<std::wstring> langids{L"0x0409"};
    std::map<std::wstring, std::vector<std::wstring>, std::less<>> table; };  // langid → [i0..iN]
struct DescModel {
    DeviceDesc device;
    std::vector<ConfigDesc> configs;      // MS1 单配置（多配置结构预留）
    StringTable strings;
    std::vector<UnknownBlock> unknown;    // 未识别块保真
    std::string to_json() const;
    static bool from_json(const std::string&, DescModel&, std::string& err);
};
}
```

- [ ] Step 1 失败测试（desc_selftest.cpp）：roundtrip 全字段覆盖（改每个字段→json→model 相等）；缺 v/坏 JSON=err；langid 表缺省兜底；unknown 块 hex 保留
- [ ] Step 2 跑红 → Step 3 实现（json_writer/minijson 同 MS0 模式；raw_hex 小写无分隔）→ Step 4 跑绿 → Step 5 Commit `feat(MS1-T1)`

### Task 2: 反编译内核——二进制→模型（desc_parse.h）

**Files:** Create `desc_parse.h`；Modify `desc_selftest.cpp`

**Interfaces:**
```cpp
// 输入：完整配置 blob（GET_CONFIGURATION 返回的全量：config+嵌套 if/ep/hid/iad）；
// 设备描述符单独 parse_device；字符串按 LANGID 描述符组解析。
// 不可识别 type / bLength=0 / bLength 越界 → 未知块 raw 收集（保真，不失败）。
bool parse_device_desc(const uint8_t* p, size_t n, DeviceDesc& out, std::string& err);
bool parse_config_blob(const uint8_t* p, size_t n, ConfigDesc& out, std::string& err);
bool parse_string_set(const uint8_t* p, size_t n, StringTable& out, std::string& err);
```

- [ ] Step 1 失败测试：内置 Lab1 样例（设备 18B/配置 blob 含 IAD+CDC×2? — 用 HID 复合: IAD 无, If0 键盘 HID+EP81+If1 鼠标 HID+EP82）解析逐字段断言；HidDesc wDescriptorLength 与报告描述符分离（报告描述符单独 blob 输入→report_hex）；未知 type 0xFF 块 → unknown 保真；bLength=0 → err；截断（bLength 越界）→ err；round-trip: build(parse(x))==x（借 T3 后置属性测试，本任务先钉 parse 正确性）
- [ ] Step 2 跑红 → Step 3 实现（walk 循环：i += p[i]；接口边界=遇非 Interface/IAD/HID/Endpoint 即止）→ Step 4 绿 → Step 5 Commit `feat(MS1-T2)`

### Task 3: 构建——模型→二进制（desc_build.h）

**Files:** Create `desc_build.h`；Modify `desc_selftest.cpp`

**Interfaces:**
```cpp
std::vector<uint8_t> build_device_desc(const DeviceDesc&);
std::vector<uint8_t> build_config_blob(const ConfigDesc&);   // 回填 wTotalLength/bNumInterfaces/bNumEndpoints/报告长度
std::vector<uint8_t> build_string_set(const StringTable&);   // LANGID 描述符(index0) + 各字符串 UTF-16LE
```

- [ ] Step 1 失败测试：Lab1 样例 build 后与黄金字节逐字节相等（黄金=parse 的输入同一份）；**性质测试**：parse(build(m))≡m（全部字段）+ build(parse(b))≡b（well-formed 输入）；wTotalLength==实际长度；字符串 UTF-16LE 编码（中文"实验室"→逐字节）
- [ ] Step 2 红 → Step 3 实现（HID 描述符 bNumDescriptors=1 常态；字符串按 LANGID 串接）→ Step 4 绿 → Step 5 Commit `feat(MS1-T3)`

### Task 4: C 代码生成（desc_gen_c.h）

**Files:** Create `desc_gen_c.h`；Modify `desc_selftest.cpp`

**Interfaces:**
```cpp
enum class GenTemplate { TinyUsb, PlainArrays };   // 设计 0.15/0.17 差异=命名注释级，MS1 收敛为 TinyUsb+通用两版（偏差在文件头注明）
struct GenOptions { GenTemplate tpl = GenTemplate::TinyUsb; const char* model_name = "device"; };
std::string generate_c(const DescModel&, const GenOptions&);   // 输出含: desc_device[]/desc_configuration[]/hid_report_descriptor[]/string_desc_arr[] + 生成头注释
```

- [ ] Step 1 失败测试：两模板输出含全部关键标识符与正确数组长度（`sizeof` 长度列=build 字节数）；HEX 行格式（每行 ≤16 字节 `0x..,`）；生成头含版本与模型名；报告描述符字节与 model hex 一致
- [ ] Step 2 红 → Step 3 实现（swprintf 组行）→ Step 4 绿 → Step 5 Commit `feat(MS1-T4)`

### Task 5: Linter 引擎 + 规则库 25 条（desc_lint.h + rules/desc_rules.json）

**Files:** Create `desc_lint.h`；Create `rules/desc_rules.json`；Modify `desc_selftest.cpp`

**Interfaces:**
```cpp
struct LintHit { std::string rule_id; int severity=0;   // 0 err 1 warn 2 info
                 std::string clause, message, fix; std::string path; };  // path=模型内位置 "configs[0].interfaces[1].endpoints[0].address"
class DescLinter {
public:
    static const std::vector<LintHit>& run(const DescModel&);   // 全规则，按 path 排序稳定
    static std::string rules_meta_json();                        // 25 条元信息（与 rules/desc_rules.json 同源生成）
};
```
**规则实现口径（25 条，检测函数 C++ 注册表按 id 挂接；元信息 json 数据驱动——外部规则 DLL 归 MS5 插件 ABI，此为设计 §2.2"可插件扩展"的第一步，偏差已注记）**：
D01 bLength==实际长度（build 一致性） · D02 bDescriptorType 合法 · D03 bcdUSB ∈ {0x0110,0x0200,0x0210} · D04 bMaxPacketSize0 ∈ {8,16,32,64}(§9.6.1) · D05 idVendor!=0 · D06 wTotalLength==合计(§9.6.3) · D07 bNumInterfaces==实际(§9.6.3) · D08 bConfigurationValue>=1 · D09 bmAttributes.bit7==1(§9.6.3) · D10 bMaxPower<=250 · D11 接口号连续且<bNumInterfaces(§9.6.5) · D12 alt 组内自 0 起 · D13 bNumEndpoints==实际(§9.6.5) · D14 bInterfaceClass ∈ 已知值表 · D15 端点号 1..15 且 EP0 禁用(§9.6.6) · D16 端点地址组内不重复 · D17 bmAttributes 高 2 位保留=0 且类型 ∈ 0..3 · D18 wMaxPacketSize FS 界（控制 8..64/批量中断<=64/同步<=1023+附加位）(§9.6.6) · D19 配置内禁控制端点 · D20 中断 bInterval 1..255 / FS 同步 1..16(§9.6.6) · D21 HID wDescriptorLength==报告长度 · D22 报告描述符 Item 栈平衡（MAIN PUSH/POP 配对、COLLECTION/END 配对） · D23 iXxx 索引 <= 字符串表长（0 除外） · D24 LANGID 表非空 · D25 IAD 组内接口连续且 first_if+count<=bNumInterfaces

- [ ] Step 1 失败测试：**每规则一正一负**（构造触发/不触发模型，断言 hits 含/不含该 id）；干净 Lab1 样例=0 err；severity 排序与 path 生成
- [ ] Step 2 红 → Step 3 实现（规则表驱动注册：`{id, fn}` 数组；fn(const DescModel&, std::vector<LintHit>&)）→ Step 4 绿（≥50 断言）→ Step 5 Commit `feat(MS1-T5)`（含 rules/desc_rules.json 生成对账：json 内 id 集 == 代码注册集，测试钉）

### Task 6: 模型字段级 diff（desc_diff.h）

**Files:** Create `desc_diff.h`；Modify `desc_selftest.cpp`

**Interfaces:**
```cpp
struct FieldDiff { std::string path; std::string a, b; };   // 值的显示形态（hex/十进制标注）
std::vector<FieldDiff> diff_models(const DescModel&, const DescModel&);   // 深度优先，序稳定
std::wstring diff_summary(size_t n);   // "N 处差异"（UI 顶部条）
```

- [ ] Step 1 失败测试：单字段改（vid）→ 恰 1 条 diff 且 a/b 正确；结构增删（加端点/删接口）→ 路径化差异；相同模型=空；summary 文案
- [ ] Step 2 红 → Step 3 实现 → Step 4 绿 → Step 5 Commit `feat(MS1-T6)`

### Task 7: W2 编辑器面板（desc_editor.h/.cpp）

**Files:** Create `desc_editor.h`、`desc_editor.cpp`；Modify `CMakeLists.txt`（主靶+desc_selftest 挂源）；Modify `panel_registry.h`（w2.descriptor 桩替换为真创建）

**Interfaces:**
```cpp
// 表单字段注册表（数据驱动：类型→字段列表；未知类型回退 raw hex 编辑——设计 §2.2）
struct FieldSpec { const char* key; const wchar_t* label; int kind; };  // 0 hex 1 dec 2 str
const std::vector<FieldSpec>& fields_for(uint8_t desc_type);           // device/config/if/ep/hid/iad
// 编辑器面板：左=树(描述符块) 中=字段表单(注册表渲染) 右=HEX(选中块 build 字节) 底=Linter 列表
// 工具条: [打开 .bin/.ustsdesc…] [✓检查] [生成 ▾(C/bin)] [diff ▾(与文件)]
class DescEditorPanel {
public:
    static HWND create_w2(void* host);            // 面板注册表工厂签名
    void load_model(DescModel m);                 // 树重建+检查+HEX 刷新
    const DescModel& model() const;
    void run_lint_and_show();
};
```

- [ ] Step 1 失败测试：fields_for 各类型非空且首字段合理（device→bcdUSB…）；hidden 烟测：create→load_model(Lab1)→树节点数==块数→run_lint→Destroy
- [ ] Step 2 红 → Step 3 实现（TreeView+表单静态行+HEX 静态多行+ListBox Linter；~350 行 Win32 薄壳）→ Step 4 绿 → Step 5 Commit `feat(MS1-T7)`

### Task 8: 集成收口——菜单/命令/版本/CI/推送

**Files:**
- Modify: `main_window_ds.cpp`（menu_table 增：文件=打开描述符…/另存描述符模型；工具=描述符检查·生成 C·生成 .bin（设计 §0.1 口径；菜单测试同步扩）；命令 handler 接真：检查/生成→DescEditorPanel 当前模型）
- Modify: `src/app/version.h` → 0.11.0
- Modify: `.github/workflows/usbtest-mock.yml`（desc_selftest 步 + 七靶）
- Modify: `apps/设计-USBDevStudio六工作台.md` §2.2 两处注记（真机反编译/下发虚拟 → MS2 归属明示）；README 段更新；ROADMAP P5 MS1 勾账

- [ ] Step 1 菜单/命令接线 + 菜单表测试扩（3 新行）→ Step 2 全七靶构建零告警 + 全自测（shell 29+desc ≥50）+ `--smoke` 0 退出 → Step 3 validate.sh → Step 4 Commit `feat(MS1)` → Step 5 Push

## 验收（MS1 DoD）

1. desc_selftest ≥50 例全绿（模型 roundtrip/解析黄金样例/build 逐字节/生成模板/Linter 25×2/diff）；
2. 三向闭环实证：`parse(build(m))≡m ∧ build(parse(b))≡b`（性质测试钉死）；
3. W2 面板替换桩（树/表单/HEX/Linter 可交互，`--smoke` 通过）；
4. 七靶全绿 + CI 更新 + validate 0 断链；
5. spdlog：desc.* 模块 debug ≥25 处。

## 自审记录

- 规格覆盖：设计 §2.1 布局→T7；§2.2 控件（反编译/Linter/生成▾/diff▾/树/表单/报告子编辑器=报告 Item 栈视图归 T5-D22+T7 HEX 展示，逐 Item 行编辑=MS2 顺带——已在 T7 注记）；§2.3 状态机→T7 load/lint 流。真机反编译+下发虚拟=MS2（架构节范围账）。
- 类型一致性：DescModel 族在 T2~T7 消费处签名一致；DescEditorPanel::create_w2 与 PanelCreateFn(HWND(*)(void*)) 匹配。
- 无占位：黄金样例字节/25 规则清单/两模板输出断言全部具体化。
