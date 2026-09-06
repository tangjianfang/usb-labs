// msc_channel.h — IChannel 的 MSC 会话实现（EP-4 S5 后半）：
// 适配 MscScsi（IOCTL_SCSI_PASS_THROUGH_DIRECT 只读直通）：发送框即 CDB——
// msc_plan_cdb 按操作码表定数据方向与响应长度；IN 命令回收数据帧，
// CHECK CONDITION（SCSI 状态非 0）回收 18 字节固定格式 SENSE 帧，均经接收
// 回调呈现（SENSE 即设备应答，工程师在接收区直接看到 70 00 05 … 而非报错）。
// MSC 无异步 IN 流 → 不起读线程，回调在 send 调用线程同步触发（SessionPane
// 的回调只 Post 载荷，线程口径兼容）。写命令一律拒收（msc_scsi 本版只读红线，
// 写校验 DESTRUCTIVE 不实现）。模板化 PortT 以便离线自测注入假件。
#pragma once

#include "channel/channel.h"
#include "usb/msc_scsi.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// 数据方向常量（ntddscsi.h 的 SCSI_IOCTL_DATA_* 同数值，本 SDK 头核值：
// OUT=0 / IN=1 / UNSPECIFIED=2；自行命名避免纯逻辑头拉入 ntddscsi）
constexpr int kMscDirOut = 0;
constexpr int kMscDirIn = 1;
constexpr int kMscDirUnspec = 2;
constexpr int kMscDirInvalid = -1;     // CDB 长度非法（SPC：6/10/12/16）

struct MscCdbPlan {
    int dir = kMscDirInvalid;
    uint32_t resp_len = 0;             // IN 命令的响应数据字节数
};

// CDB 操作码 → 数据方向/响应长度（SPC-4/SBC-3 常用命令子集）：
//   IN：INQUIRY/REQUEST_SENSE/MODE_SENSE(6) 长度取 cdb[4]，MODE_SENSE(10) 取
//       大端 [7..8]，READ_CAPACITY(10) 恒 8，READ_CAPACITY(16)（0x9E/SA=0x10）取
//       大端 [10..13]，READ(10/12/16) = 块数×block_size
//       （block_size 由 open 时 READ_CAPACITY 采得；0=未知则长度置 0）
//   OUT：FORMAT UNIT/MODE SELECT(6,10)/SEND DIAGNOSTIC/WRITE(10,12,16)——只读通道拒收
//   未收录操作码按"无数据"直通（状态/SENSE 仍回）——自定义命令可发。
// 响应长度上限 1 MiB（防御病态 CDB 的巨型分配）。纯逻辑，可离线自测。
inline MscCdbPlan msc_plan_cdb(const uint8_t* cdb, size_t len, unsigned block_size) {
    MscCdbPlan p;
    if (len != 6 && len != 10 && len != 12 && len != 16) return p;
    constexpr uint32_t kCap = 1u << 20;
    auto be16 = [&cdb](size_t i) {
        return (uint32_t(cdb[i]) << 8) | uint32_t(cdb[i + 1]);
    };
    auto be32 = [&cdb](size_t i) {
        return (uint32_t(cdb[i]) << 24) | (uint32_t(cdb[i + 1]) << 16) |
               (uint32_t(cdb[i + 2]) << 8) | uint32_t(cdb[i + 3]);
    };
    switch (cdb[0]) {
        case 0x12:                              // INQUIRY
        case 0x03:                              // REQUEST SENSE
        case 0x1A:                              // MODE SENSE(6)
            p.dir = kMscDirIn;
            p.resp_len = cdb[4];
            break;
        case 0x5A:                              // MODE SENSE(10)
            p.dir = kMscDirIn;
            p.resp_len = be16(7);
            break;
        case 0x25:                              // READ CAPACITY(10)
            p.dir = kMscDirIn;
            p.resp_len = 8;
            break;
        case 0x9E:                              // SERVICE ACTION IN：SA=0x10 为 READ
            if (cdb[1] == 0x10) {              // CAPACITY(16)，分配长度大端 [10..13]
                p.dir = kMscDirIn;             //（#75：>2TB 盘经发送框探容量的入口）
                p.resp_len = be32(10);
            } else {
                p.dir = kMscDirUnspec;         // 其余 service action 未收录，按无数据直通
            }
            break;
        case 0x28:                              // READ(10)：块数大端 [7..8]
            p.dir = kMscDirIn;
            p.resp_len = block_size ? be16(7) * uint32_t(block_size) : 0;
            break;
        case 0xA8:                              // READ(12)：块数大端 [6..9]
            p.dir = kMscDirIn;
            p.resp_len = block_size ? be32(6) * uint32_t(block_size) : 0;
            break;
        case 0x88:                              // READ(16)：块数大端 [12..13]
            p.dir = kMscDirIn;                 //（[10..11] 为 GROUP NUMBER，不计——对抗
            p.resp_len = block_size ? be16(12) * uint32_t(block_size) : 0;  // 复核 P6，#75）
            break;
        case 0x04:                              // FORMAT UNIT
        case 0x15:                              // MODE SELECT(6)
        case 0x1D:                              // SEND DIAGNOSTIC
        case 0x2A:                              // WRITE(10)
        case 0x55:                              // MODE SELECT(10)
        case 0x8A:                              // WRITE(16)
        case 0xAA:                              // WRITE(12)
            p.dir = kMscDirOut;
            break;
        default:
            p.dir = kMscDirUnspec;
            break;
    }
    if (p.resp_len > kCap) p.resp_len = kCap;
    return p;
}

// SCSI 身份串（INQUIRY vendor/product 为窄串）→ 宽串（ASCII 直映射）
inline std::wstring msc_wide_ascii(const std::string& s) {
    std::wstring w;
    for (unsigned char c : s)
        if (c) w.push_back(wchar_t(c));
    return w;
}

template <typename PortT = MscScsi>
class MscChannelT : public IChannel {
public:
    explicit MscChannelT(std::wstring path) : m_path(std::move(path)) {}
    ~MscChannelT() override { close(); }

    // "\\.\PhysicalDrive3" / "PhysicalDrive12" / "3" → 3；其余形态 false（纯逻辑）
    static bool parse_drive_index(const std::wstring& path, unsigned* index) {
        auto lower = [](wchar_t c) {              // ASCII 折叠小写（pattern 为小写）
            return (c >= L'A' && c <= L'Z') ? wchar_t(c + (L'a' - L'A')) : c;
        };
        const wchar_t* pat = L"physicaldrive";   // 13 字符
        size_t pos = std::wstring::npos;
        for (size_t i = 0; i + 13 <= path.size(); ++i) {
            bool hit = true;
            for (size_t k = 0; k < 13; ++k)
                if (lower(path[i + k]) != pat[k]) { hit = false; break; }
            if (hit) { pos = i + 13; break; }
        }
        if (pos == std::wstring::npos) {          // 回退：纯数字形态
            if (path.empty()) return false;
            pos = 0;
        }
        unsigned v = 0;
        size_t i = pos, n = 0;
        while (i < path.size() && n < 3 && path[i] >= L'0' && path[i] <= L'9') {
            v = v * 10 + unsigned(path[i] - L'0');
            ++i;
            ++n;
        }
        if (n == 0 || i != path.size()) return false;   // 无数字/尾随杂字符
        *index = v;
        return true;
    }

    bool open(std::wstring* err = nullptr) override {
        if (is_open()) close();
        unsigned idx = 0;
        if (!parse_drive_index(m_path, &idx)) {
            if (err) *err = L"MSC 路径形态须为 \\\\.\\PhysicalDriveN";
            return false;
        }
        if (!m_port.open_physical_drive(idx, /*write_access=*/false, err)) return false;
        m_index = idx;
        // 能力快照（尽力而为，失败不阻断——目录已具名，通道仍可发自定义命令）。
        // open 在 UI 线程同步执行：探测命令同样有界（kProbeTimeoutS），病态盘最多
        // 拖住会话打开数秒而非默认 10s×2（#71 遗留缺陷池）
        unsigned long long sectors = 0;
        unsigned blk = 0;
        m_block = 0;
        if (m_port.read_capacity(&sectors, &blk, nullptr, kProbeTimeoutS)) m_block = blk;
        std::string v8, p16, r4;
        m_port.scsi_inquiry(&v8, &p16, &r4, nullptr, nullptr, kProbeTimeoutS);
        std::wstring id = msc_wide_ascii(v8);
        if (!p16.empty()) {
            if (!id.empty()) id += L' ';
            id += msc_wide_ascii(p16);
        }
        if (id.empty()) id = L"USB Mass Storage";
        m_desc.kind = L"msc";
        m_desc.path = m_path;
        if (blk)
            m_desc.display = wraii::fmt_v(L"MSC PhysicalDrive%u %s %.2fGB·%uB/扇区", idx,
                                          id.c_str(),
                                          double(sectors) * double(blk) / 1e9, blk);
        else
            m_desc.display = wraii::fmt_v(L"MSC PhysicalDrive%u %s", idx, id.c_str());
        return true;
    }
    void close() noexcept override { m_port.close(); }
    bool is_open() const noexcept override { return m_port.is_open(); }

    // 发送一帧 = 发一条 CDB。返回 false 仅当 OS 层失败/未打开/CDB 非法/写命令
    //（err 给原因）；CHECK CONDITION 视为命令完成——SENSE 帧已交付，返回 true。
    bool send(const uint8_t* data, size_t len, std::wstring* err = nullptr) override {
        if (!is_open()) {
            if (err) *err = L"盘未打开";
            return false;
        }
        const MscCdbPlan plan = msc_plan_cdb(data, len, m_block);
        if (plan.dir == kMscDirInvalid) {
            if (err) *err = L"CDB 长度须为 6/10/12/16 字节";
            return false;
        }
        if (plan.dir == kMscDirOut) {
            if (err) *err = wraii::fmt_v(L"MSC 会话只读：写命令（操作码 0x%02X）已拒收", data[0]);
            return false;
        }
        std::vector<uint8_t> resp(plan.resp_len);
        uint8_t sense[32] = {};
        unsigned char status = 0xFF;
        const bool ok = m_port.pass_through(data, uint8_t(len), resp.data(), plan.resp_len,
                                            sense, &status, plan.dir, m_timeout_s, err);
        ReceiveCallback cb;
        {
            std::lock_guard<std::mutex> g(m_mtx);
            m_stats.tx_frames += 1;
            m_stats.tx_bytes += len;
            if (ok && plan.resp_len > 0) {
                m_stats.rx_frames += 1;
                m_stats.rx_bytes += plan.resp_len;
                cb = m_cb;
            } else if (!ok && status != 0xFF) {
                m_stats.rx_frames += 1;          // CHECK CONDITION：SENSE 即应答帧
                m_stats.rx_bytes += 18;
                cb = m_cb;
            }
        }
        if (cb) {
            if (ok) cb(resp);
            else cb(std::vector<uint8_t>(sense, sense + 18));   // 固定格式 SENSE 18 字节
        }
        if (!ok && status == 0xFF) return false;   // OS 层失败（err 已给）
        return true;
    }

    void set_receive_callback(ReceiveCallback cb) override {
        std::lock_guard<std::mutex> g(m_mtx);
        m_cb = std::move(cb);
    }
    // ms → 秒向上取整（MscScsi 直通超时单位为秒）。默认 3s：会话台 send 即 UI
    // 线程同步调用，超时即挂死上界（#71 遗留缺陷池——10s 默认下 NAK 盘每发一条
    // CDB 冻结 UI 10s）。显式 0 同样回落本通道默认 3s（IChannel 契约"0=用实现
    // 默认"；#72 残余埋雷——旧实现 0→端口默认 10s，与其余三通道口径不一致，
    // 调用方按契约传 0 会静默丢掉 UI 线程上界）
    void set_read_timeout(unsigned ms) noexcept override {
        if (ms == 0) {
            m_timeout_s = kDefaultTimeoutS;
            return;
        }
        // 64 位中间量防 unsigned 回绕（对抗复核 1a：ms 近 UINT_MAX 时 ms+999 回绕
        // 为小值 →m_timeout_s=0→端口默认 10s，同一埋雷的残余引信）；大值诚实
        // 放大不截断，结果恒 <UINT_MAX
        const unsigned long long s = (static_cast<unsigned long long>(ms) + 999ULL) / 1000ULL;
        m_timeout_s = static_cast<unsigned>(s);
    }
    const ChannelDesc& desc() const noexcept override { return m_desc; }
    ChannelStats stats() const noexcept override {
        std::lock_guard<std::mutex> g(m_mtx);
        return m_stats;
    }

    // 底层端口直访：产测引擎的 msc_read_verify 等专用流程仍走 Port 原生接口
    PortT& port() noexcept { return m_port; }

private:
    std::wstring m_path;
    unsigned m_index = 0;
    unsigned m_block = 0;          // READ_CAPACITY 块大小（0=未知）
    static constexpr unsigned kDefaultTimeoutS = 3; // 默认有界 3s（UI 线程 send 上界）
    unsigned m_timeout_s = kDefaultTimeoutS;
    // open 时的容量/INQUIRY 探测同界（evolve #74 起别名 msc_scsi.h 单一事实源）
    static constexpr unsigned kProbeTimeoutS = kMscProbeTimeoutS;
    PortT m_port;
    mutable std::mutex m_mtx;      // 保护 m_stats / m_cb
    ChannelStats m_stats;
    ChannelDesc m_desc;
    ReceiveCallback m_cb;
};

using MscChannel = MscChannelT<MscScsi>;
