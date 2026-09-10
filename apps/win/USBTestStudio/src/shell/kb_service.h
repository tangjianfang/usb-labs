// script_console.h + kb_service.h 合体（计划 MS5-T11/T12）：
// 脚本记录/回放 VM + 知识服务 kb 查询 + MES 上报命令模板 + 角色权限模型。
// REPL 解释器本体归滚动（设计 §8 P2）——MS5 落命令记录/回放（P1 口径）。
#pragma once

#include "../app/log.h"
#include "command_registry.h"
#include "desc_lint.h"

#include <string>
#include <vector>

namespace usts::shell {

// ---------------------------------------------------------------------------
// 脚本：命令记录 → jsonl → 回放（逐条 invoke CommandRegistry）
// ---------------------------------------------------------------------------
class CommandRecorder {
public:
    void record(const std::string& cmd_id) {
        m_ids.push_back(cmd_id);
        ustlog::logger("script")->debug("记录命令: {}", cmd_id);
    }
    std::string to_jsonl() const {
        std::string out;
        for (const auto& id : m_ids) out += "{\"cmd\":\"" + id + "\"}\n";
        return out;
    }
    bool load_jsonl(const std::string& text, std::string& err) {
        m_ids.clear();
        size_t pos = 0;
        while (pos < text.size()) {
            const auto eol = text.find('\n', pos);
            const std::string line =
                text.substr(pos, (eol == std::string::npos ? text.size() : eol) - pos);
            pos = (eol == std::string::npos) ? text.size() : eol + 1;
            if (line.empty()) continue;
            const auto p = line.find("\"cmd\":\"");
            if (p == std::string::npos) { err = "行缺 cmd 字段"; return false; }
            const auto q = line.find('"', p + 7);
            if (q == std::string::npos) { err = "cmd 值未闭合"; return false; }
            m_ids.push_back(line.substr(p + 7, q - (p + 7)));
        }
        return true;
    }
    // 回放：逐条 invoke；未注册命令=跳过并计数（不中断——脚本口径同产测步）
    size_t replay(int* invoked = nullptr) const {
        auto log = ustlog::logger("script");
        size_t ok = 0;
        for (const auto& id : m_ids)
            if (CommandRegistry::instance().invoke(id)) ++ok;
        if (invoked) *invoked = static_cast<int>(ok);
        log->info("回放: {}/{} 命中", ok, m_ids.size());
        return ok;
    }
    const std::vector<std::string>& ids() const { return m_ids; }
    void clear() { m_ids.clear(); }

private:
    std::vector<std::string> m_ids;
};

// ---------------------------------------------------------------------------
// 知识服务：规则 id / 描述符字段 → 条款引用（首批=Linter 25 规数据同源）
// ---------------------------------------------------------------------------
struct KbEntry {
    std::string key;        // "D15" / "bcdUSB" / "wMaxPacketSize"
    std::string clause;     // "USB 2.0 §9.6.6"
    std::string note;       // 修复建议（规则 fix）或字段说明
};
// 命中即返；规则 id 优先，字段名次之（字段表内置核心 8 字段）；未命中=nullptr
const KbEntry* kb_lookup(const std::string& key);
size_t kb_entry_count();

// ---------------------------------------------------------------------------
// MES 上报命令模板（退出码+报告路径 → PowerShell/curl 文本；网络调用归 UI 域）
// ---------------------------------------------------------------------------
inline std::string mes_command(const std::string& report_path, const std::string& station,
                               const std::string& dut_sn, int exit_code) {
    auto log = ustlog::logger("kb");
    const std::string cmd =
        "powershell -NoProfile -Command \"Invoke-RestMethod -Method Post -Uri "
        "'http://mes.local/api/usts/report' -ContentType 'application/json' -Body "
        "'{\\\"station\\\":\\\"" + station + "\\\",\\\"dut_sn\\\":\\\"" + dut_sn +
        "\\\",\\\"verdict\\\":\\\"" + (exit_code == 0 ? "PASS" : "FAIL") +
        "\\\",\\\"report\\\":\\\"" + report_path + "\\\"}'\"";
    log->debug("mes_command: station={} sn={} exit={}", station, dut_sn, exit_code);
    return cmd;
}

// ---------------------------------------------------------------------------
// 角色权限模型（operator/engineer/admin → 命令前缀白名单）
// ---------------------------------------------------------------------------
enum class Role { Operator, Engineer, Admin };
// 判定：admin 全放行；engineer 放行除 file.* 危险写外（教学口径：operator 仅产线域
// run.*/view.perspective.*）；其余拒绝。
bool role_allows(Role role, const std::string& cmd_id);
inline const wchar_t* role_name(Role r) {
    switch (r) {
    case Role::Operator: return L"operator";
    case Role::Engineer: return L"engineer";
    default: return L"admin";
    }
}

// ---------------------------------------------------------------------------
// 实现（模块日志 kb）
// ---------------------------------------------------------------------------
inline const std::vector<KbEntry>& kb_entries() {
    static std::vector<KbEntry> t = [] {
        std::vector<KbEntry> v;
        for (const auto& r : desc::DescLinter::rule_metas())   // 25 规同源桥接
            v.push_back({r.id, r.clause,
                         "severity=" + std::to_string(r.severity) + " 修复: " + r.fix});
        // 核心字段首批 8 条（描述符字段 → 规范条款）
        v.push_back({"bcdUSB", "USB 2.0 §9.6.1", "二进制 BCD：0x0200=USB 2.00"});
        v.push_back({"bMaxPacketSize0", "USB 2.0 §9.6.1", "EP0 最大包：FS 设备 8/16/32/64"});
        v.push_back({"idVendor", "USB-IF", "正式申请的 VID（usb.org 申请）"});
        v.push_back({"wTotalLength", "USB 2.0 §9.6.3", "配置总长=配置头+全部子描述符"});
        v.push_back({"bmAttributes", "USB 2.0 §9.6.3", "bit7 保留必置 1；bit6 自电；bit5 唤醒"});
        v.push_back({"bInterfaceNumber", "USB 2.0 §9.6.5", "接口号 0 起连续"});
        v.push_back({"bEndpointAddress", "USB 2.0 §9.6.6", "bit7=IN；低 4 位端点号 1..15"});
        v.push_back({"wMaxPacketSize", "USB 2.0 §9.6.6", "FS：批量/中断≤64，同步≤1023"});
        return v;
    }();
    return t;
}

inline const KbEntry* kb_lookup(const std::string& key) {
    for (const auto& e : kb_entries())
        if (e.key == key) return &e;
    return nullptr;
}

inline size_t kb_entry_count() { return kb_entries().size(); }

inline bool role_allows(Role role, const std::string& cmd_id) {
    auto log = ustlog::logger("kb");
    bool ok;
    switch (role) {
    case Role::Admin:
        ok = true;
        break;
    case Role::Engineer:
        ok = cmd_id.rfind("file.", 0) != 0;   // 工程写操作外放行（教学口径）
        break;
    default:                                    // operator：仅产线域
        ok = cmd_id.rfind("run.", 0) == 0 ||
             cmd_id.rfind("view.perspective", 0) == 0;
        break;
    }
    log->debug("role_allows: {} {} → {}", wraii::wide_to_utf8(role_name(role)), cmd_id, ok);
    return ok;
}

} // namespace usts::shell
