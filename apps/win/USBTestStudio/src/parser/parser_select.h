// parser_select.h — EP-4 S4 解析面板后半·解析器选型与帧解析调度（设计 §3
// IParser 可插拔 / §4.7 "解析面板默认跟随设备协议"）：通道描述 → PaneParser
// （HID 按 HidP_GetCaps 的顶层 usage page/usage 选型——数值核对自缓存
// HUT-1.3：§4 Generic Desktop 页 0x01 内 02=Mouse/06=Keyboard/07=Keypad，
// §15 Consumer 页 0x0C；串口/环回 → AsciiParser 委托 session_codec 口径不二），
// 帧 → 解析文本统一入口（mouse 格式按剥离 Report ID 后的报告长度猜，
// guess_mouse_format）。纯逻辑无 Win32 依赖：session_view 的解析视图分支
// 与 UI 面板共用同一选型，UI 只做开关与重渲染。
// 日志：parser.hid（info=通道解析器选型，debug=逐帧解析派发与鼠标格式猜测）。
#pragma once

#include "channel/channel.h"
#include "parser/hid_parser.h"
#include "parser/pd_telemetry_parser.h"
#include "session/session_codec.h"
#include "app/log.h"

#include <string>

namespace parser_select {

struct PaneParser {
    enum class Kind { none, ascii, hid_keyboard, hid_mouse, hid_consumer, pd_telemetry };
    Kind kind = Kind::none;
    bool has_report_id = false;   // HidCapsInfo：输入报告含 Report ID 前缀字节（透传剥离）

    const wchar_t* name() const noexcept {
        switch (kind) {
            case Kind::hid_keyboard: return L"键盘";
            case Kind::hid_mouse:    return L"鼠标";
            case Kind::hid_consumer: return L"消费页";
            case Kind::ascii:        return L"ASCII";
            case Kind::pd_telemetry: return L"PD 遥测";
            case Kind::none:         return L"无";
        }
        return L"无";
    }
};

// 通道描述 → 解析器选型（§4.7 跟随设备协议）。hid 未收录的 usage 组合 →
// none（UI 解析开关置灰，仅原始视图可用）；非 hid/serial/loopback → none。
inline PaneParser for_channel(const ChannelDesc& d) {
    PaneParser p;
    p.has_report_id = d.hid_report_id;
    if (d.kind == L"hid") {
        if (d.hid_usage_page == 0x01 && (d.hid_usage == 0x06 || d.hid_usage == 0x07))
            p.kind = PaneParser::Kind::hid_keyboard;      // GD 页 Keyboard/Keypad
        else if (d.hid_usage_page == 0x01 && d.hid_usage == 0x02)
            p.kind = PaneParser::Kind::hid_mouse;         // GD 页 Mouse
        else if (d.hid_usage_page == 0x0C)
            p.kind = PaneParser::Kind::hid_consumer;      // Consumer 页（多媒体键）
    } else if (d.kind == L"serial" || d.kind == L"loopback") {
        p.kind = PaneParser::Kind::ascii;                 // AsciiParser（默认文本视图）
    }
    ustlog::logger("parser.hid")->info(
        "解析器选型：通道 kind={} usage_page=0x{:04X} usage=0x{:04X} → {}（report_id={}）",
        ustlog::w2u(d.kind), d.hid_usage_page, d.hid_usage, ustlog::w2u(p.name()),
        p.has_report_id);
    return p;
}

// 帧 → 解析文本（设计 §3 IParser 派发）。none → 空串（调用方回退原始视图）。
inline std::wstring parse_frame(const ChannelFrame& f, const PaneParser& p) {
    ustlog::logger("parser.hid")->log(spdlog::level::debug, "帧解析派发 {} {}B（report_id={}）",
                                      ustlog::w2u(p.name()), f.bytes.size(), p.has_report_id);
    switch (p.kind) {
        case PaneParser::Kind::hid_keyboard:
            return hid_parser::parse_hid_report(f.bytes, hid_parser::HidKind::keyboard,
                                                p.has_report_id);
        case PaneParser::Kind::hid_mouse: {
            // 报告格式按剥离 Report ID 后的长度猜（boot/boot+滚轮/宽轴）
            size_t n = f.bytes.size();
            if (p.has_report_id && n > 0) --n;
            const hid_parser::MouseFormat mf = hid_parser::guess_mouse_format(n);
            ustlog::logger("parser.hid")->debug("鼠标格式猜测 {}B → x{}B/y{}B 滚轮={}", n,
                                                mf.x_bytes, mf.y_bytes, mf.wheel);
            return hid_parser::parse_hid_report(f.bytes, hid_parser::HidKind::mouse,
                                                p.has_report_id, mf);
        }
        case PaneParser::Kind::hid_consumer:
            return hid_parser::parse_hid_report(f.bytes, hid_parser::HidKind::consumer,
                                                p.has_report_id);
        case PaneParser::Kind::ascii:
            return session_codec::to_ascii_view(f.bytes);
        case PaneParser::Kind::pd_telemetry: {
            // S6：key=value 行流 → 状态行渲染（残行缓冲挂在解析器外由面板持有；
            // 此处无状态渲染当前累计值，面板用 pd_telemetry::feed 维护 Sample）
            return {};
        }
        case PaneParser::Kind::none:
            return {};
    }
    return {};
}

// 串口会话 auto 升级：ASCII 选型 + 帧流嗅探到遥测键 → 切 PD 遥测（S6）。
// 返回升级后的选型（未命中原样返回，info 留痕切换沿）。
inline PaneParser upgrade_if_telemetry(const PaneParser& p, const std::vector<uint8_t>& bytes) {
    if (p.kind != PaneParser::Kind::ascii) return p;
    if (!pd_telemetry::sniff(bytes)) return p;
    PaneParser up = p;
    up.kind = PaneParser::Kind::pd_telemetry;
    ustlog::logger("parser.pd")->info("嗅探到遥测键：解析器 {} → PD 遥测",
                                      ustlog::w2u(p.name()));
    return up;
}

}  // namespace parser_select
