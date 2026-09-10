// icons.h — 16px 单色图标库（设计附录 B.4：39 枚语义命名，1px 线宽，单色着色）
// 绘制=几何字形（GDI 线/矩形/圆），不依赖字体图标；调用方传色（危险=c.fail/禁用=c.off）。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "tokens.h"

namespace usts::ui {

// 在 (x,y) 起 16×16 视窗绘制 icon；color=线色；返回 false=未知枚举
bool draw_icon(HDC dc, tokens::Icon icon, int x, int y, COLORREF color);

} // namespace usts::ui
