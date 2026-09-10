// icons.cpp — 39 枚 16px 单色图标几何绘制（设计附录 B.4 / 计划 MS0-T10）
// 字形原则：可辨识优先于美观（线框 1px；语义分组的形状族：播放=三角、停止=方块、
// 检查=对钩、错误=叉、视角=四格…）。调用方负责 DC 状态（这里自管 pen/brush 还原）。
#include "icons.h"

namespace usts::ui {

namespace {

struct GdiGuard {
    ~GdiGuard() {
        if (pen) { SelectObject(dc, old_pen); DeleteObject(pen); }
        if (brush) { SelectObject(dc, old_brush); DeleteObject(brush); }
    }
    HDC dc = nullptr;
    HPEN pen = nullptr;
    HPEN old_pen = nullptr;
    HBRUSH brush = nullptr;
    HBRUSH old_brush = nullptr;
};

void line(HDC dc, int x1, int y1, int x2, int y2) {
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
}

} // namespace

bool draw_icon(HDC dc, tokens::Icon icon, int x, int y, COLORREF color) {
    using I = tokens::Icon;
    GdiGuard g;
    g.dc = dc;
    g.pen = CreatePen(PS_SOLID, 1, color);
    g.old_pen = (HPEN)SelectObject(dc, g.pen);
    g.brush = CreateSolidBrush(color);
    g.old_brush = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    const int r = x + 16, b = y + 16, cx = x + 8, cy = y + 8;

    switch (icon) {
    case I::Play:        // 实心三角▶
        SelectObject(dc, g.brush);
        { POINT pts[3] = {{x + 5, y + 3}, {x + 5, b - 3}, {r - 4, cy}}; Polygon(dc, pts, 3); }
        break;
    case I::Stop:        // 实心方块
        SelectObject(dc, g.brush);
        Rectangle(dc, x + 4, y + 4, r - 3, b - 3);
        break;
    case I::Pause:       // 双竖条
        SelectObject(dc, g.brush);
        Rectangle(dc, x + 4, y + 3, x + 7, b - 3);
        Rectangle(dc, x + 9, y + 3, x + 12, b - 3);
        break;
    case I::Refresh:     // 循环箭头（圆+缺口）
        Arc(dc, x + 2, y + 2, r - 2, b - 2, cx, y, cx, b);
        line(dc, cx, y - 1, cx + 4, y + 3);
        line(dc, cx, y - 1, cx - 1, y + 4);
        break;
    case I::Check:       // 对钩
        line(dc, x + 3, cy, x + 7, b - 4);
        line(dc, x + 7, b - 4, r - 3, y + 4);
        break;
    case I::Cross:       // 叉
        line(dc, x + 4, y + 4, r - 4, b - 4);
        line(dc, r - 4, y + 4, x + 4, b - 4);
        break;
    case I::Warn:        // 三角+竖线
        { POINT pts[3] = {{cx, y + 2}, {r - 2, b - 2}, {x + 2, b - 2}}; Polygon(dc, pts, 3); }
        line(dc, cx, cy, cx, b - 5);
        line(dc, cx, b - 3, cx, b - 3);
        break;
    case I::Plus: case I::Minus:
        line(dc, x + 3, cy, r - 3, cy);
        if (icon == I::Plus) line(dc, cx, y + 3, cx, b - 3);
        break;
    case I::Search:      // 圆+柄
        Ellipse(dc, x + 2, y + 2, x + 11, y + 11);
        line(dc, x + 10, y + 10, r - 3, b - 3);
        break;
    case I::Filter:      // 漏斗
        { POINT pts[4] = {{x + 2, y + 3}, {r - 2, y + 3}, {cx + 1, cy + 1}, {cx + 1, b - 3}}; Polygon(dc, pts, 4); }
        break;
    case I::Perspective: // 四格（视角）
        Rectangle(dc, x + 2, y + 2, cx + 1, cy + 1);
        Rectangle(dc, cx + 1, y + 2, r - 2, cy + 1);
        Rectangle(dc, x + 2, cy + 1, cx + 1, b - 2);
        Rectangle(dc, cx + 1, cy + 1, r - 2, b - 2);
        break;
    case I::Breakpoint:  // 实心圆（调试断点）
        SelectObject(dc, g.brush);
        Ellipse(dc, x + 4, y + 4, r - 3, b - 3);
        break;
    case I::Template:    // 页+折角
        Rectangle(dc, x + 3, y + 2, r - 3, b - 2);
        line(dc, x + 6, y + 6, r - 6, y + 6);
        line(dc, x + 6, cy, r - 6, cy);
        break;
    case I::Pipeline:    // 三节点双箭头链
        Rectangle(dc, x + 1, y + 5, x + 6, y + 11);
        Rectangle(dc, cx - 1, y + 5, cx + 4, y + 11);
        Rectangle(dc, r - 6, y + 5, r - 1, y + 11);
        line(dc, x + 6, cy, cx - 1, cy);
        line(dc, cx + 4, cy, r - 6, cy);
        break;
    case I::Chip:        // 芯片：方框+四脚
        Rectangle(dc, x + 4, y + 4, r - 4, b - 4);
        line(dc, cx, y + 1, cx, y + 4);
        line(dc, cx, b - 4, cx, b - 1);
        line(dc, x + 1, cy, x + 4, cy);
        line(dc, r - 4, cy, r - 1, cy);
        break;
    case I::Folder:      // 文件夹
        Rectangle(dc, x + 2, y + 4, r - 2, b - 3);
        line(dc, x + 2, y + 4, x + 5, y + 1);
        line(dc, x + 5, y + 1, x + 9, y + 1);
        line(dc, x + 9, y + 1, x + 11, y + 4);
        break;
    case I::File:
        Rectangle(dc, x + 4, y + 1, r - 4, b - 1);
        line(dc, x + 6, y + 4, r - 6, y + 4);
        line(dc, x + 6, cy, r - 6, cy);
        line(dc, x + 6, b - 4, r - 8, b - 4);
        break;
    case I::Copy:        // 双页
        Rectangle(dc, x + 2, y + 2, x + 11, b - 2);
        Rectangle(dc, x + 5, y + 5, r - 1, b - 5);
        break;
    case I::Trash:       // 垃圾桶
        line(dc, x + 4, y + 4, r - 4, y + 4);
        Rectangle(dc, x + 5, y + 4, r - 5, b - 2);
        line(dc, cx - 3, y + 1, cx + 3, y + 1);
        break;
    case I::Gear:        // 齿轮=圆+齿
        Ellipse(dc, x + 4, y + 4, r - 4, b - 4);
        Ellipse(dc, x + 6, y + 6, r - 6, b - 6);
        line(dc, cx, y + 1, cx, y + 4);
        line(dc, cx, b - 4, cx, b - 1);
        line(dc, x + 1, cy, x + 4, cy);
        line(dc, r - 4, cy, r - 1, cy);
        break;
    case I::Lock: case I::Unlock:
        Arc(dc, x + 4, y + 1, r - 4, y + 9, x + 4, y + 5, r - 4, y + 5);
        Rectangle(dc, x + 3, y + 7, r - 3, b - 2);
        if (icon == I::Unlock) line(dc, r - 4, y + 5, r - 1, y + 8);
        break;
    case I::Fullscreen:  // 四角框
        line(dc, x + 2, y + 6, x + 2, y + 2);
        line(dc, x + 2, y + 2, x + 6, y + 2);
        line(dc, r - 6, y + 2, r - 2, y + 2);
        line(dc, r - 2, y + 2, r - 2, y + 6);
        line(dc, x + 2, b - 6, x + 2, b - 2);
        line(dc, x + 2, b - 2, x + 6, b - 2);
        line(dc, r - 6, b - 2, r - 2, b - 2);
        line(dc, r - 2, b - 2, r - 2, b - 6);
        break;
    case I::Bell:
        Arc(dc, x + 3, y + 3, r - 3, b - 3, r - 3, cy, x + 3, cy);
        line(dc, x + 2, cy, r - 2, cy);
        line(dc, cx, y + 2, cx, y + 4);
        Ellipse(dc, cx - 1, b - 4, cx + 3, b - 1);
        break;
    case I::Scan:        // 扫描：方框+横线
        Rectangle(dc, x + 2, y + 2, r - 2, b - 2);
        line(dc, x + 4, cy, r - 4, cy);
        break;
    case I::Camera:
        Rectangle(dc, x + 2, y + 5, r - 2, b - 3);
        Ellipse(dc, x + 5, y + 7, r - 5, b - 5);
        line(dc, cx - 2, y + 5, cx - 1, y + 2);
        line(dc, cx - 1, y + 2, cx + 4, y + 2);
        line(dc, cx + 4, y + 2, cx + 4, y + 5);
        break;
    case I::Record:      // 实心圆
        SelectObject(dc, g.brush);
        Ellipse(dc, x + 4, y + 4, r - 4, b - 4);
        break;
    case I::Keyboard:    // 键盘：框+键点
        Rectangle(dc, x + 1, y + 5, r - 1, b - 4);
        for (int i = 0; i < 4; ++i) line(dc, x + 3 + i * 3, y + 7, x + 4 + i * 3, y + 7);
        line(dc, x + 4, b - 6, r - 4, b - 6);
        break;
    case I::Mouse:       // 鼠标：椭圆+中线
        Ellipse(dc, x + 4, y + 2, r - 4, b - 2);
        line(dc, cx, y + 2, cx, y + 7);
        line(dc, x + 4, y + 7, r - 4, y + 7);
        break;
    case I::Plug:        // 串口：D 形+针
        Rectangle(dc, x + 4, y + 5, r - 4, b - 2);
        line(dc, x + 6, y + 5, x + 6, y + 2);
        line(dc, cx, y + 5, cx, y + 2);
        line(dc, r - 6, y + 5, r - 6, y + 2);
        break;
    case I::Disk:        // 盘：圆柱
        Ellipse(dc, x + 3, y + 2, r - 3, y + 7);
        line(dc, x + 3, y + 4, x + 3, b - 4);
        line(dc, r - 3, y + 4, r - 3, b - 4);
        Arc(dc, x + 3, b - 8, r - 3, b - 1, r - 3, b - 4, x + 3, b - 4);
        break;
    case I::Video:       // 摄像头：梯形+三角
        Rectangle(dc, x + 1, y + 4, cx + 3, b - 3);
        { POINT pts[3] = {{cx + 4, y + 6}, {r - 2, y + 3}, {r - 2, b - 2}}; Polygon(dc, pts, 3); }
        break;
    case I::Audio:       // 音箱
        Rectangle(dc, x + 2, y + 6, x + 6, b - 5);
        { POINT pts[3] = {{x + 6, cy}, {x + 10, y + 3}, {x + 10, b - 3}}; Polygon(dc, pts, 3); }
        Arc(dc, x + 9, y + 4, r - 1, b - 4, r - 1, cy, x + 9, cy);
        break;
    case I::Bt:          // 蓝牙：菱形+双叉
        line(dc, cx, y + 1, cx, b - 1);
        line(dc, cx, y + 1, cx + 4, y + 5);
        line(dc, cx, cy, cx + 4, cy + 4);
        line(dc, cx, b - 1, cx + 4, cy + 3);
        line(dc, cx, cy, cx + 4, cy - 4);
        break;
    case I::Bolt:        // 闪电 PD
        { POINT pts[5] = {{cx + 3, y + 1}, {x + 4, cy + 2}, {cx, cy + 2}, {cx - 3, b - 1}, {r - 4, y + 6}}; Polygon(dc, pts, 5); }
        break;
    case I::Download:    // 下箭头+托盘
        line(dc, cx, y + 2, cx, b - 5);
        line(dc, cx - 3, b - 7, cx, b - 4);
        line(dc, cx + 3, b - 7, cx, b - 4);
        line(dc, x + 3, b - 2, r - 3, b - 2);
        break;
    case I::Report:      // 报告：页+柱图
        Rectangle(dc, x + 3, y + 1, r - 3, b - 1);
        line(dc, x + 5, b - 4, x + 5, b - 4);
        line(dc, x + 6, b - 5, x + 6, b - 3);
        line(dc, cx, b - 7, cx, b - 3);
        line(dc, r - 6, b - 4, r - 6, b - 3);
        break;
    case I::Diff:        // 对比：双页+中线
        Rectangle(dc, x + 1, y + 3, cx, b - 2);
        Rectangle(dc, cx + 1, y + 3, r - 1, b - 2);
        line(dc, cx, y + 3, cx, b - 2);
        break;
    default:             // 未细化枚举（MS1+ 逐个精化）：外框占位
        Rectangle(dc, x + 2, y + 2, r - 2, b - 2);
        line(dc, x + 2, y + 2, r - 2, b - 2);
        break;
    }
    return true;
}

} // namespace usts::ui
