// pd_telemetry_parser.h — EP-4 S6 PD 解析面板：Lab5 遥测契约（telemetry_contract.h
// 契约版本 1）的 UTS 侧解码。输入为串口会话收到的原始帧（key=value 行流，
// 与 pd_test.py _telemetry 同源），维护：CC 状态、合同电压/电流、VBUS 实测、
// 最近一次状态迁移历史。纯逻辑无 Win32 依赖；渲染输出 UTF-16 供会话面板
// 解析视图直显。契约只加字段不改名——未知键宽容跳过（warn 留痕一次）。
// 日志：parser.pd（info=流识别与状态迁移，warn=坏行/未知键）。
#pragma once

#include "channel/channel.h"
#include "app/log.h"

#include <string>
#include <vector>

namespace pd_telemetry {

struct Sample {
    std::wstring cc_state;    // Attached.SNK / Unattached.SNK / ...
    double contract_v = 0.0;  // 合同电压 V
    double contract_i = 0.0;  // 合同电流 A
    double vbus_v = 0.0;      // VBUS 实测 V
    bool has_contract = false;
    bool has_vbus = false;
    bool has_cc = false;
};

// 行 → 键值更新（契约键名锁定；返回是否命中任一已知键）
inline bool apply_line(Sample& s, const std::string& line, bool* unknown_key = nullptr) {
    auto log = ustlog::logger("parser.pd");
    const size_t eq = line.find('=');
    if (eq == std::string::npos || eq == 0) {
        if (!line.empty()) log->warn("遥测坏行（无 =）：{} 字节", line.size());
        return false;
    }
    const std::string k = line.substr(0, eq);
    const std::string v = line.substr(eq + 1);
    try {
        if (k == "cc_state") {
            const std::wstring next(v.begin(), v.end());   // 键值均为 ASCII，直接宽化
            if (s.has_cc && s.cc_state != next)
                log->info("CC 状态迁移：{} → {}", ustlog::w2u(s.cc_state), ustlog::w2u(next));
            s.cc_state = next;
            s.has_cc = true;
            return true;
        }
        if (k == "contract_v") { s.contract_v = std::stod(v); s.has_contract = true; return true; }
        if (k == "contract_i") { s.contract_i = std::stod(v); s.has_contract = true; return true; }
        if (k == "vbus_v")     { s.vbus_v = std::stod(v); s.has_vbus = true; return true; }
    } catch (const std::exception&) {
        log->warn("遥测数值解析失败：{}={}（{} 字节）", k, v, v.size());
        return false;
    }
    // 未知键：契约允许只加字段，宽容记录不告警刷屏（每键仅首见时 debug）
    if (unknown_key) *unknown_key = true;
    return false;
}

// 渲染一行状态（会话面板解析视图）：CC | 合同 | VBUS
inline std::wstring render(const Sample& s) {
    wchar_t buf[64] = {};
    std::wstring out = L"PD 遥测";
    if (s.has_cc) out += L" | CC=" + s.cc_state;
    if (s.has_contract) {
        ::swprintf(buf, 64, L" | 合同 %.2fV/%.2fA", s.contract_v, s.contract_i);
        out += buf;
    }
    if (s.has_vbus) {
        ::swprintf(buf, 64, L" | VBUS %.2fV", s.vbus_v);
        out += buf;
    }
    return out;
}

// 帧流嗅探：是否含遥测行（会话面板 auto 模式把 serial 解析器升级为 PD 遥测）
inline bool sniff(const std::vector<uint8_t>& bytes) {
    static const char* kKeys[] = {"cc_state=", "contract_v=", "contract_i=", "vbus_v="};
    for (size_t i = 0; i + 1 < bytes.size(); ++i) {
        for (const char* key : kKeys) {
            const size_t n = std::char_traits<char>::length(key);
            size_t j = 0;
            while (j < n && i + j < bytes.size() &&
                   static_cast<char>(bytes[i + j]) == key[j])
                ++j;
            if (j == n) return true;
        }
    }
    return false;
}

// 增量解码：把一帧按行拆分逐行 apply（帧边界无关，按 \n 切；残行由调用方
// 用 tail 参数续——面板逐帧调用带残行缓冲即可）
struct FeedResult {
    bool updated = false;      // 本帧产生了字段更新
    std::string tail;          // 未结尾的残行
};
inline FeedResult feed(Sample& s, const std::vector<uint8_t>& bytes, std::string& tail) {
    FeedResult r;
    r.tail = tail;
    tail.clear();
    for (uint8_t b : bytes) {
        const char c = static_cast<char>(b);
        if (c == '\n' || c == '\r') {
            if (!r.tail.empty()) {
                if (apply_line(s, r.tail)) r.updated = true;
                r.tail.clear();
            }
            continue;
        }
        r.tail.push_back(c);
        if (r.tail.size() >= 256) {          // 异常长行保护（口径同 pe_sink/固件）
            if (apply_line(s, r.tail)) r.updated = true;
            r.tail.clear();
        }
    }
    tail = r.tail;                            // 残行交还调用方续传
    return r;
}

}  // namespace pd_telemetry
