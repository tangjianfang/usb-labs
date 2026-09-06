// winusb_port.h — WinUSB 批量管道访问（EP-4 S5 前半）：
// CreateFileW(设备接口路径, FILE_FLAG_OVERLAPPED) + WinUsb_Initialize 取接口 0
// 句柄，WinUsb_QueryPipe 遍历接口管道并按 select_data_pipes 选型（批量优先、
// 中断回退），WinUsb_ReadPipe/WritePipe 重叠 I/O——读侧超时由
// WinUsb_AbortPipe + GetOverlappedResult 回收（口径同 HidPort::read_overlapped：
// 超时=轮空，非超时错误=设备失效）。多接口设备（IAD/复合）只取接口 0，
// 关联接口（WinUsb_GetAssociatedInterface）留待真机样本再议。
// 未真机运行，按 MSDN 口径编写（不确定点见 README 清单）。
#pragma once

#include "framework/win32_rai.h"

#include <cstdint>
#include <string>
#include <vector>

#include <winusb.h>   // WINUSB_INTERFACE_HANDLE / WinUsb_*（SDK 导入库 winusb.lib）

// WinUsbPort 打开后的能力快照（open 时填充；vid/pid 自接口路径解析）
struct WinUsbCaps {
    unsigned vid = 0, pid = 0;          // 路径含 usb#vid_xxxx&pid_xxxx 即解析，否则 0
    unsigned char in_pipe = 0;           // 数据 IN 管道（0x81..0x8F）；0 = 无
    unsigned char out_pipe = 0;          // 数据 OUT 管道（0x01..0x0F）；0 = 无
    bool in_is_bulk = false;             // false = 中断管道（选型回退结果）
    bool out_is_bulk = false;
    unsigned in_max_packet = 0, out_max_packet = 0;
    unsigned n_pipes = 0;                // 接口管道总数（含控制/等时不入选项）
};

// 接口管道条目（WinUsb_QueryPipe 的 WINUSB_PIPE_INFORMATION 折叠；
// select_data_pipes 的输入——独立于 OS 结构以便离线自测）
struct WinUsbPipeInfo {
    unsigned char id = 0;                // 0x01..0x0F OUT / 0x81..0x8F IN
    bool is_in = false;
    bool is_bulk = false;                // true=Bulk false=Interrupt（控制/等时不入列）
    unsigned max_packet = 0;
};

// WinUSB 接口句柄非内核对象，须以 WinUsb_Free 释放（RAII closer）
struct winusb_iface_closer {
    using H = WINUSB_INTERFACE_HANDLE;
    static H invalid() noexcept { return nullptr; }
    static void close(H h) noexcept;
};

class WinUsbPort {
public:
    // 设备接口路径（\\?\usb#vid_…&pid_…#…#{GUID}）中解析 VID/PID；
    // 大小写不敏感（实际路径为小写），解析失败置 0。纯逻辑，可离线自测。
    static void parse_vid_pid(const std::wstring& path, unsigned* vid, unsigned* pid);

    // 纯逻辑选型：批量优先、中断回退；IN/OUT 各取同型编号最小者（同型多管道
    // 设备的稳定选择）。控制/等时管道不参与。返回是否存在数据 IN 管道
    //（只出不进的设备读线程无从驱动，open 仍可成功但仅 send 可用）。
    static bool select_data_pipes(const std::vector<WinUsbPipeInfo>& pipes,
                                  unsigned char* in_pipe, bool* in_is_bulk,
                                  unsigned char* out_pipe, bool* out_is_bulk);

    bool open(const std::wstring& path, std::wstring* err = nullptr);
    void close() noexcept;
    bool is_open() const noexcept { return m_handle.valid() && m_iface.valid(); }
    const WinUsbCaps& caps() const noexcept { return m_caps; }

    // 读一个数据 IN 管道传输：超时 → *timed_out=true 返回 false（轮空继续）；
    // 其他失败（常见为设备拔出）返回 false 且 *timed_out=false（读线程退出）。
    bool read_pipe(std::vector<uint8_t>& buf, unsigned timeout_ms, bool* timed_out,
                   std::wstring* err = nullptr);

    // 写一个数据 OUT 管道传输（同步语义：等待完成或失败）。
    bool write_pipe(const uint8_t* data, size_t len, std::wstring* err = nullptr);

private:
    bool query_pipes(std::wstring* err);   // WinUsb_QueryPipe 遍历 + select_data_pipes

    wraii::uhandle<wraii::handle_closer> m_handle;      // CreateFile 句柄
    wraii::uhandle<winusb_iface_closer> m_iface;        // WinUSB 接口句柄（WinUsb_Free 释放）
    WinUsbCaps m_caps;
};
