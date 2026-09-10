// desc_editor.h — W2 描述符台编辑器面板（设计 §2.1 布局 / 计划 MS1-T7）
//
// 薄壳：左=描述符树(TreeView) 中=字段表单(注册表渲染静态行) 右=HEX 预览(静态多行)
//       底=Linter 列表(ListBox) 工具条=[打开 .ustsdesc…][✓检查][生成 C]
// 表单字段注册表（数据驱动）：fields_for(type)——编辑写回 MS2 完整化（MS1 只读编辑
// 模型经 load_model 注入；设计 §2.2 逐 Item 行编辑归 MS2，此注记）。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>

#include "../app/log.h"
#include "desc_build.h"
#include "desc_lint.h"
#include "desc_model.h"

#include <string>
#include <vector>

namespace usts::shell::desc {

// 表单字段注册表（kind：0=hex 1=dec 2=str）
struct FieldSpec {
    const char* key;
    const wchar_t* label;
    int kind;
};
const std::vector<FieldSpec>& fields_for(uint8_t desc_type);

class DescEditorPanel {
public:
    // 面板注册表工厂（PanelCreateFn 签名：(void*)host -> HWND）
    static HWND__* create_w2(void* host);
    static DescEditorPanel* instance();

    void load_model(DescModel m);          // 树重建 + 检查 + HEX 刷新
    const DescModel& model() const { return m_model; }
    HWND tree() const { return m_tree; }       // 烟测断言用
    HWND lint_list() const { return m_lint; }
    void run_lint_and_show();
    HWND hwnd() const { return m_hwnd; }

private:
    static LRESULT CALLBACK panel_proc(HWND, UINT, WPARAM, LPARAM);
    void rebuild_tree();
    void show_hex_for(const std::string& path);   // 选中块 → 右侧 HEX
    std::vector<uint8_t> hex_of_block(const std::string& path) const;

    HWND m_hwnd = nullptr;
    HWND m_tree = nullptr;
    HWND m_form = nullptr;      // 静态多行（MS2 换真表单控件）
    HWND m_hex = nullptr;
    HWND m_lint = nullptr;
    DescModel m_model;
};

// ---------------------------------------------------------------------------
// 实现（薄壳+数据表；模块日志 ui.editor）
// ---------------------------------------------------------------------------
inline const std::vector<FieldSpec>& fields_for(uint8_t t) {
    static const std::vector<FieldSpec> dev = {
        {"bcdUSB", L"USB 版本", 0}, {"bDeviceClass", L"设备类", 1},
        {"bDeviceSubClass", L"子类", 1}, {"bDeviceProtocol", L"协议", 1},
        {"bMaxPacketSize0", L"EP0 包大小", 1}, {"idVendor", L"VID", 0},
        {"idProduct", L"PID", 0}, {"bcdDevice", L"设备版本", 0},
        {"iManufacturer", L"厂商串索引", 1}, {"iProduct", L"产品串索引", 1},
        {"iSerialNumber", L"序列串索引", 1}, {"bNumConfigurations", L"配置数", 1},
    };
    static const std::vector<FieldSpec> cfg = {
        {"bConfigurationValue", L"配置值", 1}, {"iConfiguration", L"配置串索引", 1},
        {"bmAttributes", L"属性", 0}, {"bMaxPower", L"最大电流(×2mA)", 1},
    };
    static const std::vector<FieldSpec> iface = {
        {"bInterfaceNumber", L"接口号", 1}, {"bAlternateSetting", L"备用设置", 1},
        {"bInterfaceClass", L"接口类", 1}, {"bInterfaceSubClass", L"子类", 1},
        {"bInterfaceProtocol", L"协议", 1}, {"iInterface", L"接口串索引", 1},
    };
    static const std::vector<FieldSpec> ep = {
        {"bEndpointAddress", L"端点地址", 0}, {"bmAttributes", L"属性", 0},
        {"wMaxPacketSize", L"最大包", 1}, {"bInterval", L"轮询间隔", 1},
    };
    static const std::vector<FieldSpec> hid = {
        {"bcdHID", L"HID 版本", 0}, {"bCountryCode", L"国家码", 1},
        {"report", L"报告描述符(hex)", 2},
    };
    static const std::vector<FieldSpec> none;
    switch (t) {
    case kTypeDevice: return dev;
    case kTypeConfiguration: return cfg;
    case kTypeInterface: return iface;
    case kTypeEndpoint: return ep;
    case kTypeHid: return hid;
    default: return none;
    }
}

inline DescEditorPanel* DescEditorPanel::instance() {
    static DescEditorPanel p;
    return &p;
}

inline HWND__* DescEditorPanel::create_w2(void* host) {
    auto log = ustlog::logger("ui.editor");
    auto* self = instance();
    if (self->m_hwnd && IsWindow(self->m_hwnd)) return self->m_hwnd;
    HWND parent = static_cast<HWND>(host);
    self->m_hwnd = CreateWindowExW(0, L"STATIC", L"",
                                   WS_CHILD | WS_VISIBLE | SS_NOTIFY,
                                   0, 0, 100, 100, parent, nullptr,
                                   reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE)),
                                   nullptr);
    if (!self->m_hwnd) {
        log->error("W2 面板容器创建失败 GLE=0x{:08X}", GetLastError());
        return nullptr;
    }
    const HINSTANCE inst =
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE));
    auto mk = [&] (const wchar_t* cls, DWORD style, int x, int y, int w, int h,
                   HMENU id) {
        return CreateWindowExW(WS_EX_CLIENTEDGE, cls, L"", WS_CHILD | WS_VISIBLE | style,
                               x, y, w, h, self->m_hwnd, id, inst, nullptr);
    };
    self->m_tree = mk(WC_TREEVIEWW, TVS_HASLINES | TVS_HASBUTTONS | TVS_LINESATROOT,
                      0, 32, 260, 300, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1)));
    self->m_form = mk(L"STATIC", 0, 264, 32, 300, 300,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(2)));
    self->m_hex = mk(L"STATIC", 0, 568, 32, 260, 300,
                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(3)));
    self->m_lint = mk(L"LISTBOX", LBS_NOTIFY | WS_VSCROLL, 0, 336, 828, 120,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(4)));
    CreateWindowExW(0, L"BUTTON", L"✓ 检查", WS_CHILD | WS_VISIBLE,
                    0, 2, 80, 26, self->m_hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(10)), inst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"生成 C", WS_CHILD | WS_VISIBLE,
                    84, 2, 80, 26, self->m_hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(11)), inst, nullptr);
    log->info("W2 描述符台面板已创建");
    return self->m_hwnd;
}

inline void DescEditorPanel::rebuild_tree() {
    auto log = ustlog::logger("ui.editor");
    TreeView_DeleteAllItems(m_tree);
    auto add = [this] (const wchar_t* text, HTREEITEM parent) {
        TVINSERTSTRUCTW ti = {};
        ti.hParent = parent;
        ti.item.mask = TVIF_TEXT;
        ti.item.pszText = const_cast<LPWSTR>(text);
        return TreeView_InsertItem(m_tree, &ti);
    };
    add(L"设备描述符", TVI_ROOT);
    if (!m_model.configs.empty()) {
        const HTREEITEM cfg = add(L"配置 1", TVI_ROOT);
        const auto& c = m_model.configs[0];
        for (size_t i = 0; i < c.interfaces.size(); ++i) {
            const auto& f = c.interfaces[i];
            wchar_t name[64];
            swprintf(name, 64, L"接口 %u（类 %02Xh）", f.number, f.if_class);
            const HTREEITEM fi = add(name, cfg);
            if (f.hid) {
                const HTREEITEM hi = add(L"HID", fi);
                (void)hi;
            }
            for (size_t e = 0; e < f.endpoints.size(); ++e) {
                wchar_t en[64];
                swprintf(en, 64, L"端点 %02Xh", f.endpoints[e].address);
                add(en, fi);
            }
        }
    }
    add(L"字符串描述符", TVI_ROOT);
    log->debug("rebuild_tree: {} 节点", TreeView_GetCount(m_tree));
}

inline void DescEditorPanel::run_lint_and_show() {
    auto log = ustlog::logger("ui.editor");
    const auto hits = DescLinter::run(m_model);
    ListBox_ResetContent(m_lint);
    for (const auto& h : hits) {
        const std::wstring line = wraii::utf8_to_wide(
            (h.severity == 0 ? "✗ " : h.severity == 1 ? "⚠ " : "ℹ ") + h.rule_id +
            "  " + h.path + " — " + h.message + "（" + h.clause + "）");
        ListBox_AddString(m_lint, line.c_str());
    }
    log->info("检查完成：{} 命中", hits.size());
}

inline void DescEditorPanel::load_model(DescModel m) {
    auto log = ustlog::logger("ui.editor");
    m_model = std::move(m);
    rebuild_tree();
    // 表单区（设备字段，MS1 静态呈现）
    const auto& fs = fields_for(kTypeDevice);
    std::wstring form;
    for (const auto& f : fs) form += std::wstring(f.label) + L"\n";
    SetWindowTextW(m_form, form.c_str());
    // HEX：设备块
    show_hex_for("device");
    run_lint_and_show();
    log->info("模型已装载：{} 接口", m_model.configs.empty()
                                         ? 0
                                         : m_model.configs[0].interfaces.size());
}

inline void DescEditorPanel::show_hex_for(const std::string& path) {
    const auto bytes = hex_of_block(path);
    std::wstring hex;
    wchar_t buf[8];
    for (size_t i = 0; i < bytes.size(); ++i) {
        swprintf(buf, 8, L"%02X ", bytes[i]);
        hex += buf;
        if (i % 16 == 15) hex += L"\n";
    }
    SetWindowTextW(m_hex, hex.c_str());
}

inline std::vector<uint8_t> DescEditorPanel::hex_of_block(const std::string& path) const {
    if (path == "device") return build_device_desc(m_model.device);
    if (!m_model.configs.empty() && path.rfind("configs", 0) == 0)
        return build_config_blob(m_model.configs[0]);
    return {};
}

} // namespace usts::shell::desc
