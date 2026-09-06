// parser_selftest.cpp — EP-4 S4 解析面板·离线自测（无真机即可跑）：
// 前半 hid_parser 纯逻辑行为面——键盘页键码表（0x04~0x65 对照 HUT-1.3 §10）、
// 修饰键位图（HID-1.11 bit0 LCtrl…bit7 RGui）、boot 键盘 8 字节报告行格式、
// ErrorRollOver 错误码、鼠标按钮位图/X·Y 有符号位移/滚轮/宽轴 LE、
// 消费页用量名（§15.7/§15.9：B0 Play…EA Vol−）、Report ID 剥离、
// AsciiParser 委托 session_codec 口径一致；
// 后半 parser_select 选型与调度——usage page/usage → 键盘/鼠标/消费页/ASCII/
// 无（数值核对自缓存 HUT-1.3 §4：GD 页 0x01 内 02 Mouse/06 Keyboard/07 Keypad、
// §15 Consumer 0x0C）、帧解析分发、frame_line 解析行装配、RenderCursor
// 原始|解析切换。UI 面板接线随整片真机验收。
#include "parser/hid_parser.h"
#include "parser/parser_select.h"
#include "session/session_core.h"
#include "session/session_view.h"

#include <cstdio>
#include <cwchar>
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

    // —— S4 后半：解析器选型（usage 数值核对自缓存 HUT-1.3 §4/§15） ——
    using PK = parser_select::PaneParser::Kind;
    auto chan = [](const wchar_t* kind, unsigned page, unsigned usage, bool rid) {
        ChannelDesc d;
        d.kind = kind;
        d.hid_usage_page = page;
        d.hid_usage = usage;
        d.hid_report_id = rid;
        return parser_select::for_channel(d);
    };
    check(chan(L"hid", 0x01, 0x06, true).kind == PK::hid_keyboard, "选型: GD+06 → 键盘");
    check(chan(L"hid", 0x01, 0x07, false).kind == PK::hid_keyboard, "选型: GD+07 Keypad → 键盘");
    check(chan(L"hid", 0x01, 0x02, true).kind == PK::hid_mouse, "选型: GD+02 → 鼠标");
    check(chan(L"hid", 0x0C, 0x01, false).kind == PK::hid_consumer, "选型: 消费页 0x0C → 消费");
    check(chan(L"hid", 0x01, 0x04, false).kind == PK::none, "选型: GD+04 Joystick 未收录 → 无");
    check(chan(L"hid", 0x00, 0x00, false).kind == PK::none, "选型: usage 缺失 → 无");
    check(chan(L"serial", 0, 0, false).kind == PK::ascii, "选型: 串口 → ASCII");
    check(chan(L"loopback", 0, 0, false).kind == PK::ascii, "选型: 环回 → ASCII");
    check(chan(L"", 0, 0, false).kind == PK::none, "选型: 未知通道 → 无");
    check(chan(L"hid", 0x01, 0x06, true).has_report_id, "选型: report_id 透传");
    check(wcscmp(chan(L"hid", 0x01, 0x06, true).name(), L"键盘") == 0, "选型: 名称 键盘");

    // —— S4 后半：帧解析调度（parse_frame 统一入口） ——
    auto frx = [](std::initializer_list<uint8_t> b) {
        ChannelFrame f;
        f.bytes.assign(b);
        return f;
    };
    expect(parser_select::parse_frame(
               frx({0x02, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00}),
               {PK::hid_keyboard, true}),
           L"LShift+A↓", "调度: 键盘+Report ID 剥离");
    expect(parser_select::parse_frame(frx({0x01, 0x00, 0x00, 0x01}), {PK::hid_mouse, true}),
           L"（无按键） X+0 Y+1", "调度: 鼠标按剥离后长度猜 boot 格式");
    expect(parser_select::parse_frame(frx({0x00, 0x00, 0x01, 0x00}), {PK::hid_mouse, false}),
           L"（无按键） X+0 Y+1 滚轮+0", "调度: 鼠标无 Report ID（4 字节含滚轮）");
    expect(parser_select::parse_frame(frx({0xE9}), {PK::hid_consumer, false}), L"Vol+",
           "调度: 消费页");
    expect(parser_select::parse_frame(frx({0xE4, 0xBD, 0xA0}), {PK::ascii, false}), L"你",
           "调度: ASCII 委托同口径");
    expect(parser_select::parse_frame(frx({0x01}), {PK::none, false}), L"",
           "调度: 无解析器 → 空串回退原始");

    // —— S4 后半：frame_line 解析行装配（同一行装配，两视图只差正文） ——
    {
        session_core::SessionCore c;
        c.set_time_base(0, 0);
        const std::vector<uint8_t> kb{0x02, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};
        c.record_rx(kb.data(), kb.size(), 0);
        const parser_select::PaneParser p{PK::hid_keyboard, true};
        expect(c.frame_line(c.journal.frames()[0], true, false, &p), L"0ms IN  LShift+A↓",
               "frame_line: 解析行正文=键位");
        expect(c.frame_line(c.journal.frames()[0], true, false),
               L"0ms IN  02 02 00 04 00 00 00 00 00",
               "frame_line: 无 parser 回退原始 Hex（默认参不破坏 S3 口径）");
    }

    // —— S4 后半：RenderCursor 原始|解析切换（poll 增量 / rebuild 全量同口径） ——
    {
        session_core::SessionCore c;
        c.set_time_base(0, 0);
        const std::vector<uint8_t> a{0x02, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};
        const std::vector<uint8_t> b{0x02, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        c.record_rx(a.data(), a.size(), 0);
        c.record_rx(b.data(), b.size(), 10);

        session_view::RenderCursor v;
        v.parser = {PK::hid_keyboard, true};
        const auto raw = v.poll(c);
        check(raw.size() == 2 && raw[0].find(L"02 02 00 04") != std::wstring::npos,
              "游标: 默认原始视图（Hex 正文）");

        v.parsed_view = true;
        const auto parsed = v.rebuild(c);
        check(parsed.size() == 2 && parsed[0] == L"0ms IN  LShift+A↓" && parsed[1] == L"10ms IN  RShift↓",
              "游标: 解析切换 rebuild 全量按新口径");

        const std::vector<uint8_t> d{0x02, 0x02, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00};
        c.record_rx(d.data(), d.size(), 20);
        const auto inc = v.poll(c);
        check(inc.size() == 1 && inc[0] == L"20ms IN  LShift+B↓",
              "游标: 解析态后续 poll 增量仍为解析行");

        session_view::RenderCursor w;
        w.parser = {PK::none, false};
        w.parsed_view = true;   // 无解析器：开关即使为真也回退原始
        const auto fb = w.rebuild(c);
        check(fb.size() == 3 && fb[0].find(L"LShift") == std::wstring::npos,
              "游标: 选型 none 时解析开关回退原始视图");
    }

    printf("parser_selftest: %d 例全绿\n", g_pass);
    return 0;
}
