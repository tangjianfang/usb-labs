// version.h — 应用版本单一事实源（T11 安装包/关于页/日志头共用；
// 发版只改这里，CMake 侧如需 -D 覆盖用同名字符串宏）
#pragma once

#define UTS_VERSION_MAJOR 0
#define UTS_VERSION_MINOR 9
#define UTS_VERSION_PATCH 0

#define UTS_VERSION_U8 "0.9.0"

// 窄字符字面量场景（utf-8 编译单元）直用 UTS_VERSION_U8；
// 宽字符场景用 UTS_VERSION_W
#define UTSW2(x) L##x
#define UTS_VERSION_W UTSW2(UTS_VERSION_U8)
