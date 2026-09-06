// msc_scsi.h — MSC 只读校验：IOCTL_SCSI_PASS_THROUGH_DIRECT（ntddscsi.h）直发
// INQUIRY(0x12) / READ_CAPACITY(10)(0x25)→(16)(0x9E，>2TB 哨兵回退) /
// READ(10)(0x28)·READ(16)(0x88，高 LBA 选路)，带 sense 缓冲解析。
// 默认只读；写校验（DESTRUCTIVE）本版不实现。
// 已随离线构建编译（五靶，#64 起），未真机运行，运行时 API 行为按 MSDN 口径编写。
#pragma once

#include "framework/win32_rai.h"

#include <ntddscsi.h>   // SCSI_IOCTL_DATA_*（容量探测内核模板用）

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// MSC 探测命令（READ_CAPACITY/INQUIRY）的有界超时秒数（evolve #74）：
// 旧口径下 auto_detect 与产测引擎的孪生探测路径不传超时 → pass_through 回落
// 端口默认 10s（TimeOutValue），病态盘 ×10 轮最坏分钟级且无取消。3s 与
// msc_enum 扫描（kScanTimeoutS）、MscChannel open 探测同界——全仓探测统一
// 上界，单一事实源；数据面 READ10 保持 20s（大块传输自成一界，非探测）。
inline constexpr unsigned kMscProbeTimeoutS = 3;

class MscScsi {
public:
    static std::wstring device_path(unsigned index);        // \\.\PhysicalDriveN

    // write_access 仅在需要写测试时为 true（本版恒用只读）
    bool open_physical_drive(unsigned index, bool write_access, std::wstring* err = nullptr);
    void close() noexcept { m_handle.reset(); }
    bool is_open() const noexcept { return m_handle.valid(); }

    // 自动探测 USB 大容量盘：扫描 PhysicalDrive0..9，取首个“BusTypeUsb 且 READ_CAPACITY 成功”的盘
    //（内核见 msc_auto_detect_impl，可注入假件离线自测）
    static int auto_detect_usb_drive();

    // SCSI 直通。cdb/cdb_len 组成命令；data_dir 取 SCSI_IOCTL_DATA_IN / _OUT / _UNSPECIFIED；
    // sense（至多 32 字节）与 scsi_status 回传。返回 false 时 err 给出 OS 或 SENSE 错误。
    bool pass_through(const uint8_t* cdb, uint8_t cdb_len, void* data, uint32_t data_len,
                      unsigned char* sense /*>=32 或 nullptr*/, unsigned char* scsi_status,
                      int data_dir, unsigned timeout_s, std::wstring* err);

    // timeout_s 缺省 = kMscProbeTimeoutS（对抗复核 R2：旧默认 0 → pass_through 回落
    // 端口默认 10s，未来调用方漏传即静默丢探测上界；#74 起缺省即有界）
    bool scsi_inquiry(std::string* vendor8, std::string* product16, std::string* rev4,
                      unsigned char* periph_type, std::wstring* err,
                      unsigned timeout_s = kMscProbeTimeoutS);
    bool read_capacity(unsigned long long* total_sectors, unsigned* block_size, std::wstring* err,
                       unsigned timeout_s = kMscProbeTimeoutS);
    bool read10(unsigned long long lba, unsigned blocks, std::vector<uint8_t>& out,
                std::wstring* err);

    // IOCTL_STORAGE_QUERY_PROPERTY → STORAGE_DEVICE_DESCRIPTOR.BusType == BusTypeUsb
    bool bus_is_usb(bool* is_usb, std::wstring* err = nullptr);

    // 最近一次 SENSE 解析结果
    unsigned char sense_key() const noexcept { return m_sense_key; }
    unsigned char sense_asc() const noexcept { return m_sense_asc; }
    unsigned char sense_ascq() const noexcept { return m_sense_ascq; }

private:
    void parse_sense(const unsigned char* sense, unsigned len);

    wraii::uhandle<wraii::handle_closer> m_handle;
    unsigned char m_sense_key = 0, m_sense_asc = 0, m_sense_ascq = 0;
};

// ---------------------------------------------------------------------------
// auto_detect 内核（模板化 PortT 以便离线自测注入假件，产线形态即 MscScsi）：
// 扫描 PhysicalDrive0..9，取首个"BusTypeUsb 且 READ_CAPACITY 成功"的盘。
// 探测命令显式走 kMscProbeTimeoutS 有界超时（evolve #74：旧实现不传超时，
// 孪生探测路径回落端口默认 10s）。
// ---------------------------------------------------------------------------
template <typename PortT = MscScsi>
int msc_auto_detect_impl() {
    for (unsigned i = 0; i < 10; ++i) {
        PortT probe;
        if (!probe.open_physical_drive(i, /*write_access=*/false, nullptr)) continue;
        bool usb = false;
        if (!probe.bus_is_usb(&usb, nullptr) || !usb) continue;
        unsigned long long total = 0;
        unsigned blk = 0;
        if (probe.read_capacity(&total, &blk, nullptr, kMscProbeTimeoutS) && blk > 0)
            return static_cast<int>(i);
    }
    return -1;
}

// ---------------------------------------------------------------------------
// READ CDB 选型（纯逻辑，可离线自测）：最高寻址 LBA（lba+blocks-1）在 32 位内
// 用 READ(10)(0x28)（最大兼容）；越过 0xFFFFFFFF 即 READ(16)(0x88，LBA 8 字节
// 大端）。旧行为恒用 READ10 且 CDB 只填低 32 位——>2TB 盘上界检查（64 位）通过
// 而设备收到截断 LBA，静默读错扇区（evolve #75 修复）。
// 返回操作码，cdb_len 回传 10/16。
// ---------------------------------------------------------------------------
inline uint8_t msc_read_cdb(unsigned long long lba, unsigned blocks, uint8_t (&cdb)[16],
                            uint8_t* cdb_len) {
    if (lba + blocks <= 0x100000000ull) {
        cdb[0] = 0x28;   // READ(10)：LBA 大端 [2..5]，传输块数大端 [7..8]
        cdb[1] = 0;
        cdb[2] = static_cast<uint8_t>((lba >> 24) & 0xFF);
        cdb[3] = static_cast<uint8_t>((lba >> 16) & 0xFF);
        cdb[4] = static_cast<uint8_t>((lba >> 8) & 0xFF);
        cdb[5] = static_cast<uint8_t>(lba & 0xFF);
        cdb[6] = 0;
        cdb[7] = static_cast<uint8_t>((blocks >> 8) & 0xFF);
        cdb[8] = static_cast<uint8_t>(blocks & 0xFF);
        cdb[9] = 0;
        *cdb_len = 10;
        return 0x28;
    }
    cdb[0] = 0x88;   // READ(16)：LBA 大端 [2..9]，传输块数大端 [12..13]，CONTROL [15]
    for (unsigned i = 0; i < 8; ++i)
        cdb[2 + i] = static_cast<uint8_t>((lba >> (56 - i * 8)) & 0xFF);
    cdb[10] = cdb[11] = 0;
    cdb[12] = static_cast<uint8_t>((blocks >> 8) & 0xFF);
    cdb[13] = static_cast<uint8_t>(blocks & 0xFF);
    cdb[14] = cdb[15] = 0;
    *cdb_len = 16;
    return 0x88;
}

// ---------------------------------------------------------------------------
// 容量探测内核（模板化 PortT 以便离线自测注入假件，产线形态即 MscScsi）：
// READ_CAPACITY(10)(0x25) 的 last LBA 只有 4 字节，>=2TB 盘按 SBC-3 回
// 0xFFFFFFFF 哨兵，此时升 READ_CAPACITY(16)（opcode 0x9E / service action
// 0x10，分配长度 32，last LBA 为 8 字节大端）。旧行为遇哨兵直接报错
// （README 已知限制 #3，evolve #75 关闭）。超时秒数单一参数透传两条直通。
// ---------------------------------------------------------------------------
namespace msc_detail {
inline unsigned long long be32(const uint8_t* p) {
    return (static_cast<unsigned long long>(p[0]) << 24) |
           (static_cast<unsigned long long>(p[1]) << 16) |
           (static_cast<unsigned long long>(p[2]) << 8) |
           static_cast<unsigned long long>(p[3]);
}
inline unsigned long long be64(const uint8_t* p) {
    return (be32(p) << 32) | be32(p + 4);
}
} // namespace msc_detail

template <typename PortT>
bool msc_capacity_probe_impl(PortT& port, unsigned long long* total_sectors,
                             unsigned* block_size, std::wstring* err, unsigned timeout_s) {
    // READ_CAPACITY(10)：last LBA 大端 [0..3]，块长大端 [4..7]
    uint8_t cdb10[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t data[32] = {};
    uint8_t sense[32] = {};
    unsigned char status = 0;
    if (!port.pass_through(cdb10, sizeof(cdb10), data, 8, sense, &status,
                           SCSI_IOCTL_DATA_IN, timeout_s, err))
        return false;
    unsigned long long last_lba = msc_detail::be32(data);
    unsigned blk = static_cast<unsigned>(msc_detail::be32(data + 4));
    if (last_lba != 0xFFFFFFFFull) {              // ≤2TB：10 字节容量即可表达
        if (blk == 0) {
            if (err) *err = L"READ_CAPACITY(10) 返回块大小 0";
            return false;
        }
        if (total_sectors) *total_sectors = last_lba + 1;
        if (block_size) *block_size = blk;
        return true;
    }
    // 哨兵 = 容量（或精确扇区数）超出 10 字节表达范围 → READ_CAPACITY(16)
    uint8_t cdb16[16] = {0x9E, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0};
    if (!port.pass_through(cdb16, sizeof(cdb16), data, 32, sense, &status,
                           SCSI_IOCTL_DATA_IN, timeout_s, err)) {
        if (err) *err = L"READ_CAPACITY(16)（>2TB）: " + *err;
        return false;
    }
    unsigned long long last16 = msc_detail::be64(data);
    blk = static_cast<unsigned>(msc_detail::be32(data + 8));
    if (blk == 0) {
        if (err) *err = L"READ_CAPACITY(16) 返回块大小 0";
        return false;
    }
    if (total_sectors) *total_sectors = last16 + 1;
    if (block_size) *block_size = blk;
    return true;
}

// ---------------------------------------------------------------------------
// 只读块读内核（模板化 PortT 以便离线自测注入假件，产线形态即 MscScsi）：
// 容量探测（经 msc_capacity_probe_impl，>2TB 同源）→ 上界/块数校验 →
// msc_read_cdb 选路 READ(10)/(16) → 直通。read10 的旧实现只装低 32 位 LBA，
// >2TB 盘高 LBA 静默截断（对抗复核 P2：接线层无测试链路——模板化后可钉）。
// 数据面超时 20s 自成一界（大块传输，非探测，口径自 #70 起未变）。
// ---------------------------------------------------------------------------
template <typename PortT>
bool msc_read_blocks_impl(PortT& port, unsigned long long lba, unsigned blocks,
                          std::vector<uint8_t>& out, std::wstring* err) {
    unsigned blk = 0;
    unsigned long long total = 0;
    if (!port.read_capacity(&total, &blk, err, kMscProbeTimeoutS)) return false;
    if (blocks == 0 || blocks > 1024) {
        if (err) *err = L"READ 块数越界（1..1024）";
        return false;
    }
    if (lba + blocks > total) {
        if (err) *err = wraii::fmt_v(L"LBA %llu+%u 超出容量 %llu", lba, blocks, total);
        return false;
    }
    uint32_t want = static_cast<uint32_t>(blocks) * blk;
    out.assign(want, 0);
    uint8_t cdb[16] = {};
    uint8_t cdb_len = 0;
    msc_read_cdb(lba, blocks, cdb, &cdb_len);
    uint8_t sense[32] = {};
    unsigned char status = 0;
    return port.pass_through(cdb, cdb_len, out.data(), want, sense, &status,
                             SCSI_IOCTL_DATA_IN, 20, err);
}

// ---------------------------------------------------------------------------
// 只读模式校验（msc_read_verify）：同一区段连读两次比对（稳定性），
// 并检查全 0x00 / 全 0xFF（空盘/无介质启发）。READ10 只读，不写盘。
// ---------------------------------------------------------------------------
struct MscReadVerifyResult {
    bool ok = false;
    unsigned long long lba_start = 0;
    unsigned blocks_per_read = 0;
    unsigned loops = 0;
    unsigned errors = 0;        // READ10 命令失败次数
    unsigned unstable = 0;      // 两次读取不一致次数
    unsigned long long total_sectors = 0;
    unsigned block_size = 0;
    double mbps = 0;
    std::wstring detail;
};

MscReadVerifyResult msc_read_verify(MscScsi& d, unsigned long long lba_start, unsigned blocks,
                                    unsigned loops,
                                    const std::function<void(int pct)>& progress = nullptr,
                                    const std::function<bool()>& cancelled = nullptr);
