// msc_scsi.h — MSC 只读校验：IOCTL_SCSI_PASS_THROUGH_DIRECT（ntddscsi.h）直发
// INQUIRY(0x12) / READ_CAPACITY(0x25) / READ10(0x28)，带 sense 缓冲解析。
// 默认只读；写校验（DESTRUCTIVE）本版不实现。
// 未真机编译，按 MSDN 口径编写。
#pragma once

#include "framework/win32_rai.h"

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
