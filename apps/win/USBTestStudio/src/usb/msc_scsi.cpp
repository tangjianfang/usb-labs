// msc_scsi.cpp — SCSI PASS-THROUGH DIRECT 实现。已随离线构建编译，未真机运行，
// 运行时 API 行为按 MSDN 口径编写（复核点见 README 不确定清单 #8~#10）。
#include "usb/msc_scsi.h"

#include <winioctl.h>
#include <ntddscsi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// 辅助
// ---------------------------------------------------------------------------
namespace {

std::wstring drive_device_path(unsigned index) {
    wchar_t buf[64];
    ::swprintf(buf, 64, L"\\\\.\\PhysicalDrive%u", index);
    return buf;
}

std::string trim_ascii(const char* s, size_t n) {
    std::string out(s, n);
    while (!out.empty() && (out.back() == ' ' || out.back() == '\0')) out.pop_back();
    size_t first = out.find_first_not_of(' ');
    return first == std::string::npos ? std::string() : out.substr(first);
}

} // namespace

// ---------------------------------------------------------------------------
// MscScsi
// ---------------------------------------------------------------------------
std::wstring MscScsi::device_path(unsigned index) { return drive_device_path(index); }

bool MscScsi::open_physical_drive(unsigned index, bool write_access, std::wstring* err) {
    close();
    DWORD access = write_access ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
    m_handle.reset(::CreateFileW(drive_device_path(index).c_str(), access,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                 nullptr));
    if (!m_handle.valid()) {
        if (err) *err = wraii::win_err(L"CreateFileW(PhysicalDrive)", ::GetLastError());
        return false;
    }
    return true;
}

bool MscScsi::bus_is_usb(bool* is_usb, std::wstring* err) {
    *is_usb = false;
    if (!m_handle.valid()) {
        if (err) *err = L"盘未打开";
        return false;
    }
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    uint8_t buf[1024] = {};
    DWORD returned = 0;
    // STORAGE_DEVICE_DESCRIPTOR 的 BusType 字段（偏移布局以 MSDN 为准）
    if (!::DeviceIoControl(m_handle.get(), IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                           buf, sizeof(buf), &returned, nullptr)) {
        if (err) *err = wraii::win_err(L"IOCTL_STORAGE_QUERY_PROPERTY", ::GetLastError());
        return false;
    }
    auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf);
    *is_usb = desc->BusType == BusTypeUsb;
    return true;
}

int MscScsi::auto_detect_usb_drive() { return msc_auto_detect_impl<MscScsi>(); }

void MscScsi::parse_sense(const unsigned char* sense, unsigned len) {
    m_sense_key = m_sense_asc = m_sense_ascq = 0;
    if (sense == nullptr || len < 4) return;
    unsigned char code = static_cast<unsigned char>(sense[0] & 0x7F);
    if (code == 0x70 || code == 0x71) {          // 固定格式（SPC）
        m_sense_key = static_cast<unsigned char>(sense[2] & 0x0F);
        if (len >= 14) {
            m_sense_asc = sense[12];
            m_sense_ascq = sense[13];
        }
    } else if (code == 0x72 || code == 0x73) {   // 描述符格式（SPC-4：key/ASC/ASCQ 在 1/2/3）
        m_sense_key = static_cast<unsigned char>(sense[1] & 0x0F);
        m_sense_asc = sense[2];
        m_sense_ascq = sense[3];
    }
}

bool MscScsi::pass_through(const uint8_t* cdb, uint8_t cdb_len, void* data, uint32_t data_len,
                           unsigned char* sense, unsigned char* scsi_status, int data_dir,
                           unsigned timeout_s, std::wstring* err) {
    m_sense_key = m_sense_asc = m_sense_ascq = 0;
    if (scsi_status) *scsi_status = 0xFF;
    if (!m_handle.valid()) {
        if (err) *err = L"盘未打开";
        return false;
    }
    if (cdb_len == 0 || cdb_len > 16) {
        if (err) *err = L"CDB 长度非法";
        return false;
    }

    // 单缓冲布局：[SCSI_PASS_THROUGH_DIRECT][8 对齐][sense 32][8 对齐][data]
    // DataBuffer/SenseInfoOffset 必须指向同一缓冲（以 MSDN 为准）。
    constexpr ULONG kSenseLen = 32;
    const ULONG ptd_size = static_cast<ULONG>(sizeof(SCSI_PASS_THROUGH_DIRECT));
    const ULONG sense_off = (ptd_size + 7u) & ~7u;
    const ULONG data_off = (sense_off + kSenseLen + 7u) & ~7u;
    const ULONG total = data_off + (data ? data_len : 0u);

    std::vector<uint8_t> buf(total, 0);
    auto* ptd = reinterpret_cast<SCSI_PASS_THROUGH_DIRECT*>(buf.data());
    ptd->Length = static_cast<USHORT>(ptd_size);
    ptd->PathId = 0;
    ptd->TargetId = 0;
    ptd->Lun = 0;
    ptd->CdbLength = cdb_len;
    ptd->SenseInfoLength = kSenseLen;
    ptd->DataIn = static_cast<BYTE>(data_dir);
    ptd->DataTransferLength = data ? data_len : 0;
    ptd->TimeOutValue = timeout_s == 0 ? 10 : timeout_s;   // 单位秒
    ptd->DataBuffer = data ? buf.data() + data_off : nullptr;
    ptd->SenseInfoOffset = sense_off;
    ::memcpy(ptd->Cdb, cdb, cdb_len);
    if (data && data_dir == SCSI_IOCTL_DATA_OUT) ::memcpy(buf.data() + data_off, data, data_len);

    DWORD returned = 0;
    if (!::DeviceIoControl(m_handle.get(), IOCTL_SCSI_PASS_THROUGH_DIRECT, buf.data(), total,
                           buf.data(), total, &returned, nullptr)) {
        if (err) *err = wraii::win_err(L"IOCTL_SCSI_PASS_THROUGH_DIRECT", ::GetLastError());
        return false;
    }

    if (scsi_status) *scsi_status = ptd->ScsiStatus;
    if (sense) ::memcpy(sense, buf.data() + sense_off, kSenseLen);
    parse_sense(buf.data() + sense_off, kSenseLen);
    if (data && data_dir == SCSI_IOCTL_DATA_IN && data_len > 0)
        ::memcpy(data, buf.data() + data_off, data_len);

    if (ptd->ScsiStatus != 0x00) {
        if (err)
            *err = wraii::fmt_v(L"SCSI 状态 0x%02X（SENSE key=%u ASC=0x%02X ASCQ=0x%02X）",
                                ptd->ScsiStatus, m_sense_key, m_sense_asc, m_sense_ascq);
        return false;
    }
    return true;
}

bool MscScsi::scsi_inquiry(std::string* vendor8, std::string* product16, std::string* rev4,
                           unsigned char* periph_type, std::wstring* err, unsigned timeout_s) {
    // CDB6 INQUIRY：分配长度 36（与 tools/usbtest/msc_test.py 同口径）
    uint8_t cdb[6] = {0x12, 0x00, 0x00, 0x00, 36, 0x00};
    uint8_t data[36] = {};
    uint8_t sense[32] = {};
    unsigned char status = 0;
    if (!pass_through(cdb, 6, data, sizeof(data), sense, &status, SCSI_IOCTL_DATA_IN, timeout_s,
                      err))
        return false;
    if (periph_type) *periph_type = static_cast<unsigned char>(data[0] & 0x1F);
    if (vendor8) *vendor8 = trim_ascii(reinterpret_cast<const char*>(&data[8]), 8);
    if (product16) *product16 = trim_ascii(reinterpret_cast<const char*>(&data[16]), 16);
    if (rev4) *rev4 = trim_ascii(reinterpret_cast<const char*>(&data[32]), 4);
    return true;
}

bool MscScsi::read_capacity(unsigned long long* total_sectors, unsigned* block_size,
                            std::wstring* err, unsigned timeout_s) {
    // 内核见 msc_capacity_probe_impl（READ_CAPACITY(10)→哨兵→(16)，#75）；
    // auto_detect/引擎容量/msc_read_verify/通道 open/msc_enum 五调用点 + read10
    // 经此统一获得 >2TB 支持（对抗复核 P1：委托接线为审读级保证，真机钉在
    // 验收表 D11；假件侧同形态委托可钉内核本身）
    return msc_capacity_probe_impl<MscScsi>(*this, total_sectors, block_size, err, timeout_s);
}

bool MscScsi::read10(unsigned long long lba, unsigned blocks, std::vector<uint8_t>& out,
                     std::wstring* err) {
    // 内核见 msc_read_blocks_impl（容量探测同源 + READ(10)/(16) 选路，#75）
    return msc_read_blocks_impl<MscScsi>(*this, lba, blocks, out, err);
}

// ---------------------------------------------------------------------------
// 只读模式校验
// ---------------------------------------------------------------------------
MscReadVerifyResult msc_read_verify(MscScsi& d, unsigned long long lba_start, unsigned blocks,
                                    unsigned loops,
                                    const std::function<void(int)>& progress,
                                    const std::function<bool()>& cancelled) {
    MscReadVerifyResult r;
    r.lba_start = lba_start;
    r.blocks_per_read = blocks == 0 ? 8 : blocks;
    r.loops = loops == 0 ? 1 : loops;

    std::function<bool()> never = [] { return false; };
    const std::function<bool()>& stop = cancelled ? cancelled : never;

    unsigned long long total = 0;
    unsigned blk = 0;
    std::wstring err;
    if (!d.read_capacity(&total, &blk, &err, kMscProbeTimeoutS)) {
        r.detail = L"READ_CAPACITY 失败: " + err;
        return r;
    }
    r.total_sectors = total;
    r.block_size = blk;
    if (r.blocks_per_read > 128) r.blocks_per_read = 128;   // 单次直通上限 64KB@512B

    ULONGLONG t0 = ::GetTickCount64();
    unsigned long long bytes_ok = 0;
    std::vector<uint8_t> a, b;

    for (unsigned i = 0; i < r.loops; ++i) {
        if (stop()) {
            r.detail = L"被中止";
            return r;
        }
        if (total <= r.blocks_per_read) break;
        unsigned long long lba =
            (lba_start + static_cast<unsigned long long>(i) * r.blocks_per_read) %
            (total - r.blocks_per_read);

        a.clear();
        if (!d.read10(lba, r.blocks_per_read, a, &err)) {
            ++r.errors;
            r.detail = wraii::fmt_v(L"READ10 LBA%llu 失败: %s", lba, err.c_str());
            continue;
        }
        b.clear();
        if (!d.read10(lba, r.blocks_per_read, b, &err)) {
            ++r.errors;
            r.detail = wraii::fmt_v(L"READ10(2) LBA%llu 失败: %s", lba, err.c_str());
            continue;
        }
        if (::memcmp(a.data(), b.data(), a.size()) != 0) {
            ++r.unstable;
            r.detail = wraii::fmt_v(L"LBA%llu 两次读取不一致（介质/链路不稳定）", lba);
            continue;
        }
        // 空/无介质启发：全 0x00 或全 0xFF
        bool all0 = true, allF = true;
        for (uint8_t c : a) {
            if (c != 0x00) all0 = false;
            if (c != 0xFF) allF = false;
            if (!all0 && !allF) break;
        }
        if (all0 || allF) {
            ++r.unstable;
            r.detail = wraii::fmt_v(L"LBA%llu 数据为全 %s（空盘/无介质？）", lba, all0 ? L"00" : L"FF");
            continue;
        }
        bytes_ok += a.size();

        if (progress)
            progress(static_cast<int>((static_cast<unsigned long long>(i + 1)) * 100 / r.loops));
    }

    ULONGLONG dt = ::GetTickCount64() - t0;
    r.mbps = dt > 0 ? static_cast<double>(bytes_ok) / (1024.0 * 1024.0) /
                          (static_cast<double>(dt) / 1000.0)
                    : 0;
    r.ok = r.errors == 0 && r.unstable == 0 && bytes_ok > 0;
    if (r.ok)
        r.detail = wraii::fmt_v(L"%u 轮只读校验一致，%.2f MB/s（盘 %llu 扇区 × %u B）", r.loops,
                                r.mbps, total, blk);
    return r;
}
