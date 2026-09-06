// parser_selftest.cpp — EP-4 S4 解析面板前半·离线自测（无真机即可跑）：
// hid_parser 纯逻辑行为面——键盘页键码表（0x04~0x65 对照 HUT-1.3 §10）、
// 修饰键位图（HID-1.11 bit0 LCtrl…bit7 RGui）、boot 键盘 8 字节报告行格式、
// ErrorRollOver 错误码、鼠标按钮位图/X·Y 有符号位移/滚轮/宽轴 LE、
// 消费页用量名（§15.7/§15.9：B0 Play…EA Vol−）、Report ID 剥离、
// AsciiParser 委托 session_codec 口径一致。S4 后半 UI 面板接线随整片真机验收。
#include "parser/hid_parser.h"

#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

using hid_parser::format_ascii;
using hid_parser::format_consumer;
using hid_parser::format_keyboard;
using hid_parser::format_mouse;
using hid_parser::guess_mouse_format;
using hid_parser::keycode_name;
using hid_parser::modifier_names;
using hid_parser::parse_hid_report;
using hid_parser::MouseFormat;

static int g_pass = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        printf("FAIL: %s (passed %d before failure)\n", what, g_pass);
        exit(1);
    }
    ++g_pass;
}
static void expect(const std::wstring& got, const wchar_t* want, const char* what) {
    check(got == want, what);
}
static std::wstring kb(std::initializer_list<uint8_t> b) {
    std::vector<uint8_t> v(b);
    return format_keyboard(v.data(), v.size());
}
static std::wstring ms(std::initializer_list<uint8_t> b, MouseFormat f = {}) {
    std::vector<uint8_t> v(b);
    return format_mouse(v.data(), v.size(), f);
}
static std::wstring cn(std::initializer_list<uint8_t> b) {
    std::vector<uint8_t> v(b);
    return format_consumer(v.data(), v.size());
}

int main() {
    // —— 键盘页键码表（HUT-1.3 §10 原文核对） ——
    check(keycode_name(0x04) && wcscmp(keycode_name(0x04), L"A") == 0, "keycode 04=A");
    check(keycode_name(0x1D) && wcscmp(keycode_name(0x1D), L"Z") == 0, "keycode 1D=Z");
    check(keycode_name(0x1E) && wcscmp(keycode_name(0x1E), L"1") == 0, "keycode 1E=1");
    check(keycode_name(0x27) && wcscmp(keycode_name(0x27), L"0") == 0, "keycode 27=0");
    check(keycode_name(0x28) && wcscmp(keycode_name(0x28), L"Enter") == 0, "keycode 28=Enter");
    check(keycode_name(0x29) && wcscmp(keycode_name(0x29), L"Esc") == 0, "keycode 29=Esc");
    check(keycode_name(0x2C) && wcscmp(keycode_name(0x2C), L"Space") == 0, "keycode 2C=Space");
    check(keycode_name(0x35) && wcscmp(keycode_name(0x35), L"`") == 0, "keycode 35=`");
    check(keycode_name(0x39) && wcscmp(keycode_name(0x39), L"CapsLk") == 0, "keycode 39=CapsLk");
    check(keycode_name(0x3A) && wcscmp(keycode_name(0x3A), L"F1") == 0, "keycode 3A=F1");
    check(keycode_name(0x45) && wcscmp(keycode_name(0x45), L"F12") == 0, "keycode 45=F12");
    check(keycode_name(0x46) && wcscmp(keycode_name(0x46), L"PrtSc") == 0, "keycode 46=PrtSc");
    check(keycode_name(0x4F) && wcscmp(keycode_name(0x4F), L"Right") == 0, "keycode 4F=Right");
    check(keycode_name(0x52) && wcscmp(keycode_name(0x52), L"Up") == 0, "keycode 52=Up");
    check(keycode_name(0x53) && wcscmp(keycode_name(0x53), L"NumLk") == 0, "keycode 53=NumLk");
    check(keycode_name(0x58) && wcscmp(keycode_name(0x58), L"KPEnter") == 0, "keycode 58=KPEnter");
    check(keycode_name(0x5D) && wcscmp(keycode_name(0x5D), L"KP5") == 0, "keycode 5D=KP5（HID-1.11 例证）");
    check(keycode_name(0x63) && wcscmp(keycode_name(0x63), L"KP.") == 0, "keycode 63=KP.");
    check(keycode_name(0x65) && wcscmp(keycode_name(0x65), L"App") == 0, "keycode 65=App");
    check(keycode_name(0x66) == nullptr, "keycode 66 未收录 → nullptr");
    check(keycode_name(0xE0) == nullptr, "keycode E0 不在数组（修饰键走位图）");

    // —— 修饰键位图（HID-1.11：bit0 LCtrl/1 LShift/2 LAlt/3 LGui/4~7 右侧） ——
    expect(modifier_names(0x02), L"LShift", "mods 02=LShift");
    expect(modifier_names(0x22), L"LShift+RShift", "mods 22=LShift+RShift");
    expect(modifier_names(0x85), L"LCtrl+LAlt+RGui", "mods 85=LCtrl+LAlt+RGui");
    expect(modifier_names(0x00), L"", "mods 00=空");

    // —— Boot 键盘报告行格式（8 字节 = 修饰+保留+6 键码） ——
    expect(kb({0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00}), L"LShift+A↓",
           "02 00 04 → LShift+A↓（设计 §2 解析面板样例同构）");
    expect(kb({0x00, 0x00, 0x04, 0x05, 0x3A, 0x00, 0x00, 0x00}), L"A+B+F1↓",
           "多键数组 A+B+F1↓");
    expect(kb({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}), L"（无按键）",
           "全零报告 → （无按键）");
    expect(kb({0x01, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01}), L"ErrorRollOver（超 6 键）",
           "键码数组 01 → ErrorRollOver");
    expect(kb({0x02, 0x00}), L"LShift↓", "长度 2 仅解修饰键");
    expect(kb({0x80, 0x00, 0x00, 0x00}), L"RGui↓", "RGui↓");
    expect(kb({0x00, 0x00, 0x66, 0x00}), L"66?↓", "未知键码回退十六进制");
    expect(kb({}), L"（空报告）", "空键盘报告");

    // —— 鼠标：按钮位图 + X/Y 相对位移 + 滚轮/宽轴 ——
    expect(ms({0x01, 0x02, 0x00}), L"左键 X+2 Y+0", "boot 3 字节：左键+X+2");
    expect(ms({0x00, 0xFB, 0x01}), L"（无按键） X-5 Y+1", "负位移 FB→-5");
    expect(ms({0x05, 0x00, 0x00}), L"左键 中键 X+0 Y+0", "按钮位图 05=左+中");
    expect(ms({0x00, 0x00, 0x00, 0xFF}, {1, 1, true}), L"（无按键） X+0 Y+0 滚轮-1",
           "4 字节含滚轮 FF→-1");
    expect(ms({0x00, 0x00, 0x01, 0x00, 0x00}, {2, 2, false}), L"（无按键） X+256 Y+0",
           "宽轴 2 字节 LE：00 01→+256");
    expect(ms({0x00, 0xFF, 0xFF, 0x00, 0x00, 0x01}, {2, 2, true}),
           L"（无按键） X-1 Y+0 滚轮+1", "宽轴负值 FF FF→-1");
    expect(ms({}), L"（空报告）", "空鼠标报告");
    check(guess_mouse_format(3).wheel == false && guess_mouse_format(3).x_bytes == 1,
          "guess: 3 字节=boot 无滚轮");
    check(guess_mouse_format(4).wheel == true && guess_mouse_format(4).x_bytes == 1,
          "guess: 4 字节=boot+滚轮");
    check(guess_mouse_format(6).wheel == true && guess_mouse_format(6).x_bytes == 2,
          "guess: ≥6 字节=宽轴+滚轮");
    check(guess_mouse_format(5).x_bytes == 1 && !guess_mouse_format(5).wheel,
          "guess: 5 字节按 boot 兜底");

    // —— 消费页（HUT-1.3 §15.7/§15.9） ——
    expect(cn({0xE9}), L"Vol+", "E9=Vol+");
    expect(cn({0xEA}), L"Vol−", "EA=Vol−");
    expect(cn({0xE2}), L"Mute", "E2=Mute");
    expect(cn({0xB5}), L"下一曲", "B5=下一曲");
    expect(cn({0xB6}), L"上一曲", "B6=上一曲");
    expect(cn({0xB7}), L"Stop", "B7=Stop");
    expect(cn({0xB0}), L"Play", "B0=Play");
    expect(cn({0xCD, 0x00}), L"Play/Pause", "双字节 LE CD 00=Play/Pause");
    expect(cn({0x00}), L"0x0000?", "0x00 未收录回退");
    expect(cn({0x77}), L"0x0077?", "未知用量回退十六进制");
    expect(cn({}), L"（空报告）", "空消费报告");

    // —— 会话帧统一入口：Report ID 剥离 + 各类分发 ——
    expect(parse_hid_report({0x02, 0x02, 0x00, 0x04, 0x00}, hid_parser::HidKind::keyboard,
                            true),
           L"LShift+A↓", "Report ID=02 剥离后按键盘解");
    expect(parse_hid_report({0x01, 0x00, 0x03}, hid_parser::HidKind::mouse, false),
           L"左键 X+0 Y+3", "入口分发 mouse（首字节按钮位图）");
    expect(parse_hid_report({0x03, 0xE9, 0x00}, hid_parser::HidKind::consumer, true),
           L"Vol+", "入口分发 consumer+Report ID 剥离");
    expect(parse_hid_report({0x05}, hid_parser::HidKind::mouse, true),
           L"（空报告）", "仅 Report ID 一字节 → 空报告");

    // —— AsciiParser 委托（与接收区 ASCII 视图同口径） ——
    expect(format_ascii({0x41, 0x42}), L"AB", "ASCII 委托：可打印直出");
    expect(format_ascii({0xE4, 0xBD, 0xA0}), L"你", "ASCII 委托：UTF-8 保真");
    expect(format_ascii({0x01}), L".", "ASCII 委托：控制符 → .");

    printf("parser_selftest: %d 例全绿\n", g_pass);
    return 0;
}
