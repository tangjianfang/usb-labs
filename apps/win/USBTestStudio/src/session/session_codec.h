// session_codec.h — EP-4 S3 会话台·收发编解码（设计 §4.3 发送框智能识别 /
// §4.6 接收区双视图）：发送文本 → 字节（auto 判定：仅 0-9A-Fa-f 与空白 → Hex，
// 奇数个十六进制位整体补前导 0——设计原文"奇数字节"按半字节位口径实现；含
// 其他字符 → ASCII，按 UTF-8 编码故中文可发；可手动锁定 hex/ascii）；字节 →
// 视图文本（Hex 大写空格分组 / ASCII 可打印直出、合法 UTF-8 序列保真、控制
// 字符与非法定符显示 '.'——一行一帧口径，\r\n 等控制符不换行）。时间戳：
// 绝对（一天内毫秒 → HH:MM:SS.mmm）与相对（自会话基准 → 500ms / 1.234s）。
// 纯逻辑无 Win32 依赖：S3 后半的 UI 收发区直接复用，产测报告同口径。
// 日志：session.codec（debug=发送编码判定与逐字节视图解码操作，
// warn=发送解析失败可恢复；to_hex/to_ascii 逐字节循环内不打日志）。
#pragma once

#include "channel/channel.h"
#include "app/log.h"

#include <cstdint>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

namespace session_codec {

// —— 发送框（§4.3） ——
enum class SendEncoding { auto_detect, hex_lock, ascii_lock };

struct SendParseResult {
    std::vector<uint8_t> bytes;   // 解析所得待发字节（空 = 无可发，UI 应禁发）
    bool hex_mode = false;        // 实际采用口径（auto 的判定结果 / 锁定值）
    std::wstring error;           // 非空 = 不可发送（仅 hex 锁定遇非法字符）
};

inline bool sc_is_hex_digit(wchar_t c) noexcept {
    return (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F') || (c >= L'a' && c <= L'f');
}
inline bool sc_is_blank(wchar_t c) noexcept {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}
inline int sc_nibble(wchar_t c) noexcept {
    if (c >= L'0' && c <= L'9') return int(c - L'0');
    if (c >= L'A' && c <= L'F') return int(c - L'A') + 10;
    return int(c - L'a') + 10;
}

// 宽串 → UTF-8 字节（ASCII 模式的发送口径；孤立代理按 U+FFFD 编码保证可逆）
inline void sc_utf8_append(std::vector<uint8_t>& out, std::wstring_view s) {
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned cp = unsigned(s[i]);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() &&
            s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + unsigned(s[i + 1]) - 0xDC00;
            ++i;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }
        if (cp < 0x80) {
            out.push_back(uint8_t(cp));
        } else if (cp < 0x800) {
            out.push_back(uint8_t(0xC0 | (cp >> 6)));
            out.push_back(uint8_t(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(uint8_t(0xE0 | (cp >> 12)));
            out.push_back(uint8_t(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(uint8_t(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(uint8_t(0xF0 | (cp >> 18)));
            out.push_back(uint8_t(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(uint8_t(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(uint8_t(0x80 | (cp & 0x3F)));
        }
    }
}

// 发送文本解析（auto/hex 锁定/ascii 锁定；详见文件头）
inline SendParseResult parse_send_text(std::wstring_view text, SendEncoding mode) {
    SendParseResult r;
    bool hex_only = true;
    size_t digits = 0;
    for (wchar_t c : text) {
        if (sc_is_blank(c)) continue;
        if (sc_is_hex_digit(c)) {
            ++digits;
            continue;
        }
        hex_only = false;
        break;
    }
    const bool as_hex = mode == SendEncoding::hex_lock ||
                        (mode == SendEncoding::auto_detect && hex_only && digits > 0);
    r.hex_mode = as_hex;
    if (as_hex) {
        if (!hex_only) {
            r.error = L"Hex 模式仅允许 0-9A-Fa-f 与空白";
            ustlog::logger("session.codec")->warn(
                "发送解析失败：Hex 锁定含非 hex 字符（输入 {} 字符，放弃发送）", text.size());
            return r;
        }
        std::vector<int> nibbles;
        nibbles.reserve(text.size());
        for (wchar_t c : text)
            if (sc_is_hex_digit(c)) nibbles.push_back(sc_nibble(c));
        if (nibbles.size() % 2) nibbles.insert(nibbles.begin(), 0);   // 奇数位补前导 0
        r.bytes.reserve(nibbles.size() / 2);
        for (size_t i = 0; i < nibbles.size(); i += 2)
            r.bytes.push_back(uint8_t((nibbles[i] << 4) | nibbles[i + 1]));
        ustlog::logger("session.codec")->debug(
            "发送编码判定 HEX：{} 个 hex 位{} → {} 字节", digits,
            digits % 2 ? "（奇数补前导 0）" : "", r.bytes.size());
    } else {
        sc_utf8_append(r.bytes, text);
        ustlog::logger("session.codec")->debug("发送编码判定 ASCII：{} 字符 → UTF-8 {} 字节",
                                               text.size(), r.bytes.size());
    }
    return r;
}

// —— 接收区双视图（§4.6） ——
inline std::wstring to_hex_view(const uint8_t* data, size_t len) {
    std::wstring out;
    out.reserve(len * 3);
    wchar_t buf[3];
    for (size_t i = 0; i < len; ++i) {
        if (i) out += L' ';
        swprintf(buf, 3, L"%02X", unsigned(data[i]));
        out += buf;
    }
    return out;
}
inline std::wstring to_hex_view(const std::vector<uint8_t>& v) {
    return to_hex_view(v.data(), v.size());
}

// UTF-8 解码追加：合法多字节序列还原（>0xFFFF 落代理对）；非法/截断 → '.'
// 并在下一字节重同步。目的为保真显示，不追求严格 UTF-8 校验（过长编码等
// 极端输入按可显示处理）。
inline void sc_utf8_decode_append(std::wstring& out, const uint8_t* data, size_t len) {
    size_t i = 0;
    while (i < len) {
        const uint8_t b = data[i];
        if (b < 0x80) {                                    // ASCII：可打印直出
            out += (b >= 0x20 && b != 0x7F) ? wchar_t(b) : L'.';   // 控制符/DEL → '.'
            ++i;
            continue;
        }
        unsigned need = 0, cp = 0;
        if ((b & 0xE0) == 0xC0)      { need = 1; cp = b & 0x1F; }
        else if ((b & 0xF0) == 0xE0) { need = 2; cp = b & 0x0F; }
        else if ((b & 0xF8) == 0xF0) { need = 3; cp = b & 0x07; }
        bool ok = need > 0;
        for (unsigned k = 1; ok && k <= need; ++k) {
            if (i + k >= len || (data[i + k] & 0xC0) != 0x80) ok = false;
            else cp = (cp << 6) | unsigned(data[i + k] & 0x3F);
        }
        if (!ok) {
            out += L'.';
            ++i;
            continue;
        }
        if (cp > 0xFFFF) {
            cp -= 0x10000;
            out += wchar_t(0xD800 + (cp >> 10));
            out += wchar_t(0xDC00 + (cp & 0x3FF));
        } else {
            out += wchar_t(cp);
        }
        i += need + 1;
    }
}
inline std::wstring to_ascii_view(const std::vector<uint8_t>& v) {
    std::wstring out;
    out.reserve(v.size());
    sc_utf8_decode_append(out, v.data(), v.size());
    return out;
}

// —— 时间戳（§4.6：相对/绝对） ——
inline std::wstring format_time_of_day(unsigned long long ms_of_day) {
    ms_of_day %= 86400000ULL;
    const unsigned h = unsigned(ms_of_day / 3600000);
    const unsigned m = unsigned(ms_of_day / 60000 % 60);
    const unsigned s = unsigned(ms_of_day / 1000 % 60);
    const unsigned ms = unsigned(ms_of_day % 1000);
    wchar_t buf[16];
    swprintf(buf, 16, L"%02u:%02u:%02u.%03u", h, m, s, ms);
    return buf;
}
inline std::wstring format_relative(unsigned long long delta_ms) {
    wchar_t buf[32];
    if (delta_ms < 1000) swprintf(buf, 32, L"%llums", delta_ms);
    else swprintf(buf, 32, L"%llu.%03llus", delta_ms / 1000, delta_ms % 1000);
    return buf;
}

// —— 接收区一行："<时间戳> <IN|OUT> <视图文本>"（设计 §2 接收区样例同构；
// 时间戳标签由会话层按绝对/相对口径预先算好传入，编解码层保持无状态） ——
inline std::wstring format_frame_body_line(const std::wstring& timestamp_label, bool out,
                                           const std::wstring& body) {
    std::wstring line = timestamp_label;
    line += out ? L" OUT " : L" IN  ";
    line += body;
    return line;
}
inline std::wstring format_frame_line(const ChannelFrame& f, bool hex_view,
                                      const std::wstring& timestamp_label) {
    return format_frame_body_line(
        timestamp_label, f.out, hex_view ? to_hex_view(f.bytes) : to_ascii_view(f.bytes));
}

}  // namespace session_codec
