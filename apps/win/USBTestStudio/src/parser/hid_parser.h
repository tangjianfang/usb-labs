// hid_parser.h — EP-4 S4 解析面板·HID 报告解码（设计 §3 IParser/§4.7 解析面板：
// 按设备协议自动解码，HID 报告→修饰键/键码/坐标/消费码，原始视图与解析视图并列）。
// 码表口径（全部核对自 80-参考资料 缓存规范，L1：不凭记忆落表）：
//  · 修饰键位图（HID-1.11 §8.3 修饰字节）：bit0 LCtrl/1 LShift/2 LAlt/3 LGui/
//    4 RCtrl/5 RShift/6 RAlt/7 RGui——E0~E7 用作位图不进键码数组；
//  · Boot 键盘报告（HID-1.11 附录 B）：8 字节 = 修饰 + 保留 + 6 键码；
//    键码 0x01 ErrorRollOver/0x02 POSTFail/0x03 ErrorUndefined；
//  · 键盘页键码（HUT-1.3 §10）：0x04~0x1D a..z、0x1E~0x27 1..0、0x28 起控制键、
//    0x3A~0x45 F1..F12、0x53 起小键盘、0x64 Non-US \、0x65 Application；
//  · 鼠标（HID-1.11 附录 B.2 Boot Mouse）：字节 0 按钮位图（bit0/1/2 按惯例
//    左/右/中），其后 X/Y 相对位移（boot 每轴 1 字节，宽描述符 2 字节 LE），
//    可选滚轮 1 字节——报告长度不足以判别时由 UI 侧指定（MouseFormat）；
//  · 消费页（HUT-1.3 §15.7/§15.9，-table 模式对齐提取——-layout 此表行漂移 1 行）：
//    B0 Play/B1 Pause/B2 Record/B3 FF/B4 Rewind/B5 下一曲/B6 上一曲/B7 Stop/
//    B8 Eject/CD Play·Pause/E2 Mute/E9 Vol+/EA Vol−，报告取 1~2 字节 LE 用量值。
// 纯逻辑无 Win32 依赖：S4 后半 UI 解析面板直接复用；Report ID 由 has_report_id
// 剥离（HidChannel 对 Report ID 透传，多报告描述符会话在 UI 侧声明）。
#pragma once

#include "session/session_codec.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hid_parser {

// —— 键盘页键码名（HUT-1.3 §10；返回 nullptr = 未收录/保留，调用方回退十六进制） ——
inline const wchar_t* keycode_name(uint8_t k) {
    static const wchar_t* const names[] = {
        // 0x00 无键（表中不出现，返回 nullptr 由调用方跳过）
        nullptr,
        L"ErrorRollOver", L"POSTFail", L"ErrorUndefined",   // 0x01~0x03
        L"A", L"B", L"C", L"D", L"E", L"F", L"G", L"H", L"I", L"J", L"K", L"L", L"M",
        L"N", L"O", L"P", L"Q", L"R", L"S", L"T", L"U", L"V", L"W", L"X", L"Y", L"Z",
        L"1", L"2", L"3", L"4", L"5", L"6", L"7", L"8", L"9", L"0",   // 0x1E~0x27
        L"Enter", L"Esc", L"Bksp", L"Tab", L"Space",                   // 0x28~0x2C
        L"-", L"=", L"[", L"]", L"\\", L"NonUS#", L";", L"'", L"`",     // 0x2D~0x35
        L",", L".", L"/", L"CapsLk",                                    // 0x36~0x39
        L"F1", L"F2", L"F3", L"F4", L"F5", L"F6",                       // 0x3A~0x3F
        L"F7", L"F8", L"F9", L"F10", L"F11", L"F12",                    // 0x40~0x45
        L"PrtSc", L"ScrLk", L"Pause",                                   // 0x46~0x48
        L"Ins", L"Home", L"PgUp", L"Del", L"End", L"PgDn",              // 0x49~0x4E
        L"Right", L"Left", L"Down", L"Up",                              // 0x4F~0x52
        L"NumLk", L"KP/", L"KP*", L"KP-", L"KP+", L"KPEnter",           // 0x53~0x58
        L"KP1", L"KP2", L"KP3", L"KP4", L"KP5", L"KP6",                 // 0x59~0x5E
        L"KP7", L"KP8", L"KP9", L"KP0", L"KP.", L"NonUS\\", L"App",     // 0x5F~0x65
    };
    if (k < 0x66) return names[k];   // names[0]=0x00 占位（nullptr），names[k] 即键码 k
    return nullptr;
}

// —— 修饰键位图（HID-1.11：bit0 LCtrl … bit7 RGui；E0~E7 不进键码数组） ——
inline std::wstring modifier_names(uint8_t mods) {
    static const struct { uint8_t bit; const wchar_t* name; } k_mods[] = {
        {0x01, L"LCtrl"}, {0x02, L"LShift"}, {0x04, L"LAlt"}, {0x08, L"LGui"},
        {0x10, L"RCtrl"}, {0x20, L"RShift"}, {0x40, L"RAlt"}, {0x80, L"RGui"},
    };
    std::wstring out;
    for (const auto& m : k_mods)
        if (mods & m.bit) {
            if (!out.empty()) out += L'+';
            out += m.name;
        }
    return out;
}

// Boot 键盘报告 → 解析行（8 字节 = 修饰+保留+键码数组；不足 2 字节仅解修饰）：
// 样例 02 00 04 00 … → "LShift+A↓"；全零 → "（无按键）"；键码数组含 0x01 →
// "ErrorRollOver"。返回宽串供解析面板与产测报告共用。
inline std::wstring format_keyboard(const uint8_t* data, size_t len) {
    if (len == 0) return L"（空报告）";
    const uint8_t mods = data[0];
    std::wstring out = modifier_names(mods);
    if (len >= 3) {
        for (size_t i = 2; i < len; ++i) {
            const uint8_t k = data[i];
            if (k == 0x00) continue;
            if (k == 0x01) return L"ErrorRollOver（超 6 键）";
            if (k == 0x02) return L"POSTFail";
            if (k == 0x03) return L"ErrorUndefined";
            const wchar_t* n = keycode_name(k);
            if (n) {
                if (!out.empty()) out += L'+';
                out += n;
            } else {
                wchar_t buf[8];
                swprintf(buf, 8, L"%02X?", k);
                if (!out.empty()) out += L'+';
                out += buf;
            }
        }
    }
    if (out.empty()) return L"（无按键）";
    return out + L"↓";
}

// —— 鼠标报告（按钮位图 + X/Y 相对位移[+滚轮]；宽描述符每轴 2 字节 LE） ——
struct MouseFormat {
    unsigned x_bytes = 1;   // 1（boot）/ 2（宽描述符）
    unsigned y_bytes = 1;
    bool wheel = false;     // 末字节滚轮（HUT-1.3 §16.3 Wheel，一档一计数）
};

// 按报告长度猜格式（S4 后半 UI 可显式指定覆盖）：3=boot、4=boot+滚轮、
// ≥6=宽轴+滚轮；其余按 boot 兜底。
inline MouseFormat guess_mouse_format(size_t len) {
    if (len == 4) return {1, 1, true};
    if (len >= 6) return {2, 2, true};
    return {1, 1, false};
}

inline int mouse_axis(const uint8_t* p, unsigned bytes) {   // LE 有符号扩展
    int v = 0;
    for (unsigned i = 0; i < bytes && i < 4; ++i) v |= int(p[i]) << (8 * i);
    if (bytes == 1) return int(int8_t(v));
    if (bytes == 2) return int(int16_t(v));
    return v;
}

inline std::wstring format_mouse(const uint8_t* data, size_t len, MouseFormat fmt) {
    if (len == 0) return L"（空报告）";
    std::wstring out;
    const uint8_t buttons = data[0];
    static const wchar_t* const k_btn[] = {L"左键", L"右键", L"中键"};
    for (int b = 0; b < 8; ++b)
        if (buttons & (1u << b)) {
            if (!out.empty()) out += L' ';
            out += (b < 3) ? k_btn[b] : L"键4+";
        }
    if (buttons == 0) out = L"（无按键）";
    size_t off = 1;
    wchar_t buf[32];
    if (off + fmt.x_bytes <= len) {
        swprintf(buf, 32, L" X%+d", mouse_axis(data + off, fmt.x_bytes));
        out += buf;
        off += fmt.x_bytes;
    }
    if (off + fmt.y_bytes <= len) {
        swprintf(buf, 32, L" Y%+d", mouse_axis(data + off, fmt.y_bytes));
        out += buf;
        off += fmt.y_bytes;
    }
    if (fmt.wheel && off < len) {
        swprintf(buf, 32, L" 滚轮%+d", mouse_axis(data + off, 1));
        out += buf;
    }
    return out;
}

// —— 消费页用量名（HUT-1.3 §15.7/§15.9，报告载 1~2 字节 LE 用量值） ——
inline const wchar_t* consumer_usage_name(uint16_t u) {
    switch (u) {
        case 0xB0: return L"Play";
        case 0xB1: return L"Pause";
        case 0xB2: return L"Record";
        case 0xB3: return L"FastForward";
        case 0xB4: return L"Rewind";
        case 0xB5: return L"下一曲";
        case 0xB6: return L"上一曲";
        case 0xB7: return L"Stop";
        case 0xB8: return L"Eject";
        case 0xCD: return L"Play/Pause";
        case 0xE2: return L"Mute";
        case 0xE9: return L"Vol+";
        case 0xEA: return L"Vol−";
        default:   return nullptr;
    }
}

inline std::wstring format_consumer(const uint8_t* data, size_t len) {
    if (len == 0) return L"（空报告）";
    const uint16_t u = (len >= 2) ? uint16_t(data[0] | (data[1] << 8)) : data[0];
    if (const wchar_t* n = consumer_usage_name(u)) return n;
    wchar_t buf[16];
    swprintf(buf, 16, L"0x%04X?", unsigned(u));
    return buf;
}

// —— 会话帧统一入口（S4 后半 UI 接线点；报告描述符带 Report ID 时先剥离） ——
enum class HidKind { keyboard, mouse, consumer };

inline std::wstring parse_hid_report(const uint8_t* data, size_t len, HidKind kind,
                                     bool has_report_id, MouseFormat fmt = {}) {
    if (has_report_id && len > 0) { ++data; --len; }   // Report ID 透传剥离
    switch (kind) {
        case HidKind::keyboard: return format_keyboard(data, len);
        case HidKind::mouse:    return format_mouse(data, len, fmt);
        case HidKind::consumer: return format_consumer(data, len);
    }
    return L"";
}
inline std::wstring parse_hid_report(const std::vector<uint8_t>& v, HidKind kind,
                                     bool has_report_id, MouseFormat fmt = {}) {
    return parse_hid_report(v.data(), v.size(), kind, has_report_id, fmt);
}

// AsciiParser（设计 §3：默认文本视图）= session_codec::to_ascii_view 委托，
// 会话台接收区既有 ASCII 视图与解析面板共用同一实现（口径不二）。
inline std::wstring format_ascii(const std::vector<uint8_t>& v) {
    return session_codec::to_ascii_view(v);
}

}  // namespace hid_parser
