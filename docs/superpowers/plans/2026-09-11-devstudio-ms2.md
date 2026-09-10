# USB DevStudio MS2（虚拟调试器）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** W3 虚拟调试器——C++ 原生虚拟 USB 设备内核（EP0 枚举状态机+断点五类+错误注入+场景脚本）+ ustsvd 管道协议 + W3 面板，替换 w3.vd 桩（设计 §3，总纲 §七 MS2）。

**Architecture 诚实账（立账即注记，同步设计文档）**：Python usbsim 是完整协议教学仿真器；W3 在零依赖 C++ 单 exe 内运行，故 MS2 内核为 **C++ 新写 vd_core**（范围=虚拟设备最小可用：USB 状态机 Attached→Default→Addressed→Configured + EP0 标准请求子集 + HID 中断 IN 报告流 + 断点/注入钩点），**复用 MS1 desc_build 产物服务 GET_DESCRIPTOR**；BOT/PD 等全协议仿真与 Python usbsim 桥接归后续里程碑。ustsvd：MS2 落**管道协议+进程内直连**（GUI↔vd 同进程）；hidapi 形状 DLL 导出薄层若上下文/周期允许则一并落，否则 MS3 补（偏差实时注记）。

**Tech Stack:** 同 MS0/MS1。新自测并入 desc_selftest? 否——`vd_selftest.exe` 第八靶。

## Global Constraints（沿 MS0/MS1 全文）

/W4 零告警；spdlog 模块 `vd.core/templates/script/host/ui.editor`；纯逻辑分支全覆盖；每任务一提交；T6 收口推送；validate.sh 0 断链。

---

### Task 1: 虚拟设备内核 vd_core.h（状态机+EP0+断点+注入+事件流）

**Files:** Create `src/shell/vd_core.h`；Create `src/app/vd_selftest.cpp`；Modify CMake（第八靶）

**Interfaces:**
```cpp
namespace usts::shell::vd {
enum class DevState { Attached, Default, Addressed, Configured };
enum class Inject { None, StallNext, NakTimes, DropPower, ToggleErr, Babble };
struct VdEvent { uint64_t seq; uint32_t ts_ms; bool host_to_dev; std::string summary;
                 std::vector<uint8_t> data; int mark; };  // mark: 0 普通 1 断点 2 注入 3 错误
struct Breakpoint {
    enum class Kind { CtrlRequest, EnumStep, EpXfer, State, ErrorCount } kind;
    uint8_t bmReqType=0, bRequest=0; uint16_t wValue=0;    // CtrlRequest 匹配
    int enum_step=0; uint8_t ep=0, dir=0;                   // EnumStep/EpXfer
    DevState state=DevState::Attached; int error_count=0;   // State/ErrorCount
    bool enabled=true;
};
class VirtualDevice {
public:
    explicit VirtualDevice(desc::DescModel m);
    // 主机侧 EP0：setup 包 → 应答（stall=true 或 data）；内部驱动状态机+事件流+断点+注入
    struct CtrlResult { bool stall; std::vector<uint8_t> data; bool breakpoint_hit; };
    CtrlResult host_ctrl(const uint8_t setup[8]);
    // 配置态后：取一份 HID IN 报告（键盘扫描码等——模板行为注入 report_gen）
    void set_report_source(std::function<std::vector<uint8_t>()> gen);
    std::vector<uint8_t> poll_in_report(uint8_t ep);      // IN 事务（NAK 注入=空）
    void inject(Inject kind, int count);                  // 错误注入
    void reset();                                         // 掉电/重置 → Attached
    DevState state() const; uint8_t address() const;
    const std::vector<VdEvent>& events() const; void clear_events();
    bool hit(const Breakpoint& bp) const;                 // 断点判定（最近事件后）
    // 统计：事务数/NAK/STALL/CRC 错
    struct Stats { uint32_t xfers=0, nak=0, stall=0, errors=0; };
    const Stats& stats() const;
};
}
```

- [ ] Step 1 失败测试：完整枚举序列（黄金模型）——GET_DESCRIPTOR device/config/string(0..3)/SET_ADDRESS/SET_CONFIGURATION 逐事务断言应答字节=MS1 build 产物；状态机迁移 Attached→Default(首次 ctrl)→Addressed→Configured；未配置时 poll=空；断点：on_ctrl(SET_ADDRESS) 命中 flag；on_state(Configured) 命中；注入 StallNext→下一 ctrl stall；NakTimes→poll 空并 nak++；DropPower→reset 状态回 Attached；unknown 请求→stall；事件流 seq 单调+方向
- [ ] Step 2 红 → Step 3 实现（setup 解析→switch 标准请求；descriptor 查表复用 build_device_desc/build_config_blob/build_string_set）→ Step 4 绿 → Step 5 Commit `feat(MS2-T1)`

### Task 2: 设备模板 vd_templates.h

**Files:** Create `src/shell/vd_templates.h`；Modify vd_selftest

**Interfaces:** `struct VdTemplate { std::string id; std::wstring name; desc::DescModel model; std::vector<std::string> key_seq; };` + `builtin_templates()`（HID 键盘 / HID 复合键鼠——两模板起步, MSC/PD 归 MS3 顺带, 注记）+ 键盘报告生成器（key_seq→扫描码报告 8B modifier+reserved+6keys）

- [ ] 测试：模板清单/模型可 build/GOLDEN 键序列报告字节（'A'→04）/序列循环回绕 → Commit `feat(MS2-T2)`

### Task 3: 场景脚本 vd_script.h（.ustssim）

**Files:** Create `src/shell/vd_script.h`；Modify vd_selftest

**Interfaces:** `struct ScriptEvent { enum class Kind { Wait, Inject, Assert, Delay, Loop } kind; std::string cond; Inject inject; int count; std::string assert_expr; int delay_ms; };` + `Script{to_json/from_json}` + `ScriptRunner::step(VirtualDevice&) → 进度/断言结果`

- [ ] 测试：事件表 roundtrip/等待枚举完成条件/断言失败回报/延时与循环计数 → Commit `feat(MS2-T3)`

### Task 4: ustsvd 协议 + 进程内会话 vd_host.h

**Files:** Create `src/shell/vd_host.h`；Modify vd_selftest

**Interfaces:** 附录 D 消息（vd_start/stop/event/break/resume/step/inject/client）的编码/解码（JSON-lines 同信封）+ `VdSession`（GUI 侧：持 VirtualDevice，把 EP0/报告流翻译为消息回调；管道传输层=内存队列，命名管道监听若落 DLL 时接入——MS2 进程内直连，注记）

- [ ] 测试：消息 roundtrip 字段级/会话驱动枚举产生 vd_event 序列/断点消息 → Commit `feat(MS2-T4)`

### Task 5: W3 面板 desc→vd_editor（w3.vd 真替换）

**Files:** Create `src/shell/vd_editor.h`；Modify CMake/panel registry 接线（main.cpp set_factory w3.vd）

薄壳：[模板▾][▶启动][⏹][⟲] 工具条 + 事件流列表 + 状态检查器静态卡 + 注入按钮排 + 断点启用列表。VM 逻辑=事件列表渲染/选中态。

- [ ] 测试：隐藏烟测（启动→喂 3 个枚举事务→事件行数→注入→停止）→ Commit `feat(MS2-T5)`

### Task 6: 收口——版本/CI/文档/推送

v0.12.0；CI 第九靶；设计文档 §3 usbsim→vd_core 架构注记；ROADMAP MS2 勾；全九靶构建+全自测+smoke；commit+push。

## 验收（MS2 DoD）

vd_selftest ≥25 例；枚举全序列字节级=MS1 产物；断点五类可命中；注入五型行为正确；W3 面板真替换；九靶全绿；spdlog vd.* ≥15 处。
