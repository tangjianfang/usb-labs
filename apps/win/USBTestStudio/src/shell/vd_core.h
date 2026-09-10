// vd_core.h — C++ 原生虚拟 USB 设备内核（设计 §3 W3 / 计划 MS2-T1）
//
// 范围诚实账：最小可用虚拟设备——USB 状态机 + EP0 标准请求子集 + HID 中断 IN 报告流
// + 断点判定 + 错误注入钩点；描述符应答复用 MS1 desc_build。BOT/PD 全协议与
// Python usbsim 桥接归后续里程碑（设计 §3 注记同步）。
#pragma once

#include "../app/log.h"
#include "desc_build.h"
#include "desc_model.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace usts::shell::vd {

enum class DevState { Attached, Default, Addressed, Configured };
inline const char* state_name(DevState s) {
    switch (s) {
    case DevState::Attached: return "Attached";
    case DevState::Default: return "Default";
    case DevState::Addressed: return "Addressed";
    default: return "Configured";
    }
}

enum class Inject { None, StallNext, NakTimes, DropPower, ToggleErr, Babble };

struct VdEvent {
    uint64_t seq = 0;
    uint32_t ts_ms = 0;
    bool host_to_dev = true;
    std::string summary;
    std::vector<uint8_t> data;
    int mark = 0;   // 0 普通 1 断点 2 注入 3 错误
};

struct Breakpoint {
    enum class Kind { CtrlRequest, EnumStep, EpXfer, State, ErrorCount } kind =
        Kind::CtrlRequest;
    uint8_t bmReqType = 0xFF, bRequest = 0xFF;   // CtrlRequest 匹配（0xFF=通配低优先）
    uint16_t wValue = 0xFFFF;
    int enum_step = 0;                            // EnumStep：第 N 个事务
    uint8_t ep = 0, dir = 0;                      // EpXfer
    DevState state = DevState::Configured;        // State
    int error_count = 0;                          // ErrorCount
    bool enabled = true;
};

class VirtualDevice {
public:
    explicit VirtualDevice(desc::DescModel m) : m_model(std::move(m)) {}

    struct CtrlResult {
        bool stall = false;
        std::vector<uint8_t> data;
        bool breakpoint_hit = false;
    };
    // 主机 EP0 控制传输：8B setup → 设备应答（驱动状态机/事件流/断点/注入全在这）
    CtrlResult host_ctrl(const uint8_t setup[8]);

    void set_report_source(std::function<std::vector<uint8_t>()> gen) { m_gen = std::move(gen); }
    std::vector<uint8_t> poll_in_report(uint8_t ep);   // IN 事务（NAK 注入=空返回）

    void inject(Inject kind, int count);
    void reset();                                       // 掉电/总线复位 → Attached
    DevState state() const { return m_state; }
    uint8_t address() const { return m_addr; }
    const std::vector<VdEvent>& events() const { return m_events; }
    void clear_events() { m_events.clear(); }

    bool hit(const Breakpoint& bp) const;              // 断点判定（基于最近事件/状态）

    struct Stats { uint32_t xfers = 0, nak = 0, stall = 0, errors = 0; };
    const Stats& stats() const { return m_stats; }

private:
    void emit(bool host_to_dev, const std::string& summary, std::vector<uint8_t> data,
              int mark = 0);
    desc::DescModel m_model;
    DevState m_state = DevState::Attached;
    uint8_t m_addr = 0;
    uint8_t m_cfg = 0;
    std::function<std::vector<uint8_t>()> m_gen;
    std::vector<VdEvent> m_events;
    Stats m_stats;
    Inject m_inject = Inject::None;
    int m_inject_count = 0;
    uint32_t m_ts = 0;
    uint64_t m_seq = 0;
};

// ---------------------------------------------------------------------------
// 实现（header-only；模块日志 vd.core）
// ---------------------------------------------------------------------------
inline void VirtualDevice::emit(bool h2d, const std::string& summary,
                                std::vector<uint8_t> data, int mark) {
    VdEvent e;
    e.seq = m_seq++;
    e.ts_ms = ++m_ts;   // 逻辑时序（1ms 节拍教学口径）
    e.host_to_dev = h2d;
    e.summary = summary;
    e.data = std::move(data);
    e.mark = mark;
    m_events.push_back(std::move(e));
}

inline VirtualDevice::CtrlResult VirtualDevice::host_ctrl(const uint8_t s[8]) {
    auto log = ustlog::logger("vd.core");
    CtrlResult r;
    ++m_stats.xfers;
    if (m_state == DevState::Attached) m_state = DevState::Default;   // 首事务出 Attached

    // 注入：STALL 下一次
    if (m_inject == Inject::StallNext) {
        m_inject = Inject::None;
        ++m_stats.stall;
        emit(true, "STALL 注入命中", {}, 2);
        emit(false, "STALL", {}, 3);
        r.stall = true;
        return r;
    }
    const uint8_t bmRT = s[0], bReq = s[1];
    const uint16_t wValue = static_cast<uint16_t>(s[2] | (s[3] << 8));
    const uint16_t wLength = static_cast<uint16_t>(s[6] | (s[7] << 8));
    emit(true, "SETUP bmRT=" + desc::detail::to_hex_str(&bmRT, 1),
         std::vector<uint8_t>(s, s + 8));

    auto clip = [&] (std::vector<uint8_t> v) {
        if (v.size() > wLength) v.resize(wLength);   // 标准口径：wLength 截断
        return v;
    };
    switch (bReq) {
    case 0x00:   // GET_STATUS
        r.data = {0x00, 0x00};
        break;
    case 0x05:   // SET_ADDRESS
        m_addr = static_cast<uint8_t>(wValue);
        if (m_state == DevState::Default) m_state = DevState::Addressed;
        break;
    case 0x08:   // GET_DESCRIPTOR
    case 0x06: {
        const uint8_t type = s[3], idx = s[2];
        if (type == desc::kTypeDevice) {
            r.data = clip(desc::build_device_desc(m_model.device));
        } else if (type == desc::kTypeConfiguration) {
            r.data = clip(m_model.configs.empty()
                              ? std::vector<uint8_t>{}
                              : desc::build_config_blob(m_model.configs[0]));
        } else if (type == desc::kTypeString) {
            auto set = desc::build_string_set(m_model.strings);
            // walk 至第 idx 个字符串描述符起点（跳过前 idx 个）
            size_t off = 0;
            bool ok = true;
            for (int k = 0; k < idx; ++k) {
                if (off >= set.size()) { ok = false; break; }
                off += set[off];
            }
            if (!ok || off >= set.size()) {
                r.stall = true;   // 索引越界（D23 同口径）
            } else {
                r.data = {set.begin() + off, set.begin() + off + set[off]};
                if (r.data.size() > wLength) r.data.resize(wLength);
            }
        } else {
            r.stall = true;   // 未实现类型
        }
        break;
    }
    case 0x09:   // SET_CONFIGURATION
        m_cfg = static_cast<uint8_t>(wValue);
        if (m_cfg != 0 && m_state == DevState::Addressed) m_state = DevState::Configured;
        break;
    case 0x0A:   // GET_INTERFACE
        r.data = {0x00};
        break;
    case 0x0B:   // SET_INTERFACE
        break;
    case 0x21:   // SET_IDLE（HID 类请求）
        break;
    default:
        r.stall = true;
        ++m_stats.stall;
        break;
    }
    if (r.stall) {
        ++m_stats.errors;
        emit(false, "STALL（未知/非法请求）", {}, 3);
    } else {
        emit(false, "DATA " + std::to_string(r.data.size()) + "B", r.data);
    }
    log->debug("host_ctrl: req={:#04x} → {} 字节{}", bReq, r.data.size(),
               r.stall ? " STALL" : "");
    return r;
}

inline std::vector<uint8_t> VirtualDevice::poll_in_report(uint8_t ep) {
    ++m_stats.xfers;
    if (m_inject == Inject::NakTimes) {
        if (--m_inject_count <= 0) m_inject = Inject::None;
        ++m_stats.nak;
        emit(false, "NAK（注入）EP" + std::to_string(ep & 0x0F), {}, 2);
        return {};
    }
    if (m_state != DevState::Configured) {
        emit(false, "NAK（未配置）EP" + std::to_string(ep & 0x0F), {});
        return {};
    }
    auto report = m_gen ? m_gen() : std::vector<uint8_t>{};
    emit(false, "IN 报告 EP" + std::to_string(ep & 0x0F) + " " +
                    std::to_string(report.size()) + "B",
         report);
    return report;
}

inline void VirtualDevice::inject(Inject kind, int count) {
    auto log = ustlog::logger("vd.core");
    m_inject = kind;
    m_inject_count = count;
    if (kind == Inject::DropPower) {
        reset();
        emit(false, "掉电注入 → 状态机复位", {}, 2);
    } else if (kind == Inject::ToggleErr || kind == Inject::Babble) {
        ++m_stats.errors;
        emit(false, kind == Inject::ToggleErr ? "数据翻转错误（注入）" : "babble（注入）",
             {}, 2);
        m_inject = Inject::None;
    } else {
        emit(false, "注入就绪 kind=" + std::to_string(static_cast<int>(kind)), {}, 2);
    }
    log->debug("inject: kind={} count={}", static_cast<int>(kind), count);
}

inline void VirtualDevice::reset() {
    auto log = ustlog::logger("vd.core");
    m_state = DevState::Attached;
    m_addr = 0;
    m_cfg = 0;
    m_inject = Inject::None;
    log->info("设备复位 → Attached");
}

inline bool VirtualDevice::hit(const Breakpoint& bp) const {
    if (!bp.enabled || m_events.empty()) return false;
    switch (bp.kind) {
    case Breakpoint::Kind::EnumStep:
        return m_events.back().seq == static_cast<uint64_t>(bp.enum_step);
    case Breakpoint::Kind::State:
        return m_state == bp.state;
    case Breakpoint::Kind::ErrorCount:
        return static_cast<int>(m_stats.errors) >= bp.error_count;
    case Breakpoint::Kind::CtrlRequest: {
        // 最近一个 SETUP 事件（data=8B）匹配
        for (auto it = m_events.rbegin(); it != m_events.rend(); ++it) {
            if (!it->host_to_dev || it->data.size() != 8 ||
                it->summary.rfind("SETUP", 0) != 0)
                continue;
            const uint8_t rt = it->data[0], rq = it->data[1];
            const uint16_t wv = static_cast<uint16_t>(it->data[2] | (it->data[3] << 8));
            return (bp.bmReqType == 0xFF || rt == bp.bmReqType) &&
                   (bp.bRequest == 0xFF || rq == bp.bRequest) &&
                   (bp.wValue == 0xFFFF || wv == bp.wValue);
        }
        return false;
    }
    case Breakpoint::Kind::EpXfer: {
        const auto& e = m_events.back();
        return e.summary.find("EP" + std::to_string(bp.ep & 0x0F)) != std::string::npos &&
               (bp.dir == 0 || (bp.dir == 1 && !e.host_to_dev));
    }
    default:
        return false;
    }
}

} // namespace usts::shell::vd
