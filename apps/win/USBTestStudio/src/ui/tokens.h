// tokens.h — 视觉 token 单一事实源（设计文档附录 E.6；换主题/调密度只改这里）
//
// 规范：
//   1. 面板与控件代码只引用本文件语义 token，禁止内联颜色/尺寸字面量；
//   2. 值为 96dpi 基准，运行期随 PerMonitorV2 缩放；
//   3. 色板为浅色主题 V1.0 值，深色主题(P1)=同名常量换值（届时拆 tokens_light/dark）。
#pragma once

#include <cstdint>

namespace usts::ui::tokens {

// ---------------------------------------------------------------------------
// 色板（COLORREF 0x00BBGGRR；注释=设计 token 名 #RRGGBB）
// ---------------------------------------------------------------------------
constexpr uint32_t kPrimary = 0x00C05615;    // c.primary     #1565C0
constexpr uint32_t kPrimaryBg = 0x00FD2FE3;  // c.primary.bg  #E3F2FD
constexpr uint32_t kPass = 0x00327D2E;       // c.pass        #2E7D32
constexpr uint32_t kFail = 0x002828C6;       // c.fail        #C62828
constexpr uint32_t kRun = kPrimary;          // c.run         #1565C0（呼吸动画由控件层做）
constexpr uint32_t kWarn = 0x0025A8F9;       // c.warn        #F9A825
constexpr uint32_t kOff = 0x009E9E9E;        // c.off         #9E9E9E
constexpr uint32_t kBg = 0x00FFFFFF;         // c.bg          #FFFFFF
constexpr uint32_t kBgAlt = 0x00F5F5F5;      // c.bg.alt      #F5F5F5（斑马纹）
constexpr uint32_t kBorder = 0x0000E0E0;     // c.border      #E0E0E0
constexpr uint32_t kText = 0x00212121;       // c.text        #212121
constexpr uint32_t kTextDim = 0x00757575;    // c.text.dim    #757575
constexpr uint32_t kDangerZone = 0x00EEEBFF; // c.danger.zone #FFEBEE（危险对话框底）

// ---------------------------------------------------------------------------
// 尺寸（px@96dpi；间距只取 space.unit 的 1/2/3/4/6 倍 = 4/8/12/16/24）
// ---------------------------------------------------------------------------
constexpr int kSpaceUnit = 4;
constexpr int kCtrlH = 28;          // 控件高（紧凑 24）
constexpr int kCtrlHCompact = 24;
constexpr int kRowH = 24;           // 表格行高
constexpr int kTreeRowH = 22;       // 树节点高
constexpr int kMarginPanel = 12;    // 面板内边距
constexpr int kMarginGroup = 16;    // 分组间距
constexpr int kWinDefW = 1280, kWinDefH = 800;   // 默认窗口
constexpr int kWinMinW = 1024, kWinMinH = 640;   // 最小窗口
constexpr int kIconS = 16;          // 图标（大屏 64）
constexpr int kIconL = 64;

// ---------------------------------------------------------------------------
// 字体（font.ui 9pt；中文回退 Microsoft YaHei UI 由创建处 fallback 链处理）
// ---------------------------------------------------------------------------
constexpr wchar_t kFontUi[] = L"Segoe UI";
constexpr int kFontUiPt = 9;
constexpr wchar_t kFontUiCn[] = L"Microsoft YaHei UI";
constexpr wchar_t kFontMono[] = L"Consolas";
constexpr int kFontMonoPt = 9;
constexpr int kFontH1Pt = 12;           // 面板标题（semibold 由控件层设）
constexpr int kFontH2Pt = 11;           // 分组标题
constexpr int kFontOperatorPt = 48;     // 产线大屏 SN/verdict

// ---------------------------------------------------------------------------
// 图标 39 枚（命名=语义；16px 视窗/1px 线宽/单色，绘制实现于 src/ui/icons.cpp，
// 危险图标渲染以 c.fail 着色，禁用 c.off——见设计附录 B.4）
// ---------------------------------------------------------------------------
enum class Icon {
    Scan, Play, Stop, Pause, Gear, Fullscreen, Lock, Unlock, Warn, Bell,
    Camera, Record, Chip, Keyboard, Mouse, Plug, Disk, Video, Audio, Bt,
    Bolt, Download, Report, Diff, Folder, File, Copy, Trash, Refresh, Plus,
    Minus, Check, Cross, Filter, Search,
    Perspective, Breakpoint, Template, Pipeline
};

} // namespace usts::ui::tokens
