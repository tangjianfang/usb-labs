// hid_port.h — HID 设备访问：HidD_GetPreparsedData/HidP_GetCaps 取能力，
// ReadFile（重叠 + QPC 计时）读输入报告；写输出报告 = WriteFile 重叠有界
// 主路（默认 3s，CancelIoEx+GOR 回收）+ HidD_SetOutputReport 控制传输回退；
// hid_polling_rate_measure 以 QPC 直方图量化回报率。
// 未真机编译，按 MSDN 口径编写。
#pragma once

#include "framework/win32_rai.h"

#include <hidsdi.h>
#include <hidpi.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct HidCapsInfo {
    USAGE   usage_page = 0;          // HIDP_CAPS.UsagePage
    USAGE   usage = 0;               // HIDP_CAPS.Usage
    USHORT  input_report_len = 0;    // HIDP_CAPS.InputReportByteLength（含 Report ID 字节）
    USHORT  output_report_len = 0;   // HIDP_CAPS.OutputReportByteLength
    USHORT  feature_report_len = 0;  // HIDP_CAPS.FeatureReportByteLength
    USHORT  vid = 0;                 // HIDD_ATTRIBUTES.VendorID
    USHORT  pid = 0;                 // HIDD_ATTRIBUTES.ProductID
    USHORT  version = 0;             // HIDD_ATTRIBUTES.VersionNumber
    bool    has_report_id = false;   // 任一输入 Value/Button caps ReportID≠0（S4 解析剥离判定）
    bool    write_capable = false;   // 句柄是否以 GENERIC_WRITE 打开成功
    std::wstring product;            // HidD_GetProductString（best-effort）
};

class HidPort {
public:
    // 打开设备并填充 caps。先试 GENERIC_READ|GENERIC_WRITE，失败回退 GENERIC_READ（hidapi 同口径）。
    bool open(const std::wstring& path, std::wstring* err = nullptr);
    void close() noexcept { m_handle.reset(); }
    bool is_open() const noexcept { return m_handle.valid(); }

    const HidCapsInfo& caps() const noexcept { return m_caps; }
    const std::wstring& path() const noexcept { return m_path; }

    // 单次重叠读输入报告。返回 false 时：*timed_out=true 表示超时（已 CancelIoEx 并回收操作），
    // 否则错误详情写入 err。
    bool read_overlapped(std::vector<uint8_t>& report, unsigned timeout_ms, bool* timed_out,
                         std::wstring* err = nullptr);

    // 写输出报告：data[0] 为 Report ID（无报告 ID 设备传 0），长度受 OutputReportByteLength 约束。
    // 主路 = 重叠 WriteFile + 有界等待（默认 3s）：超时 → CancelIoEx 取消并回收，边界
    // 竞态口径同 read_overlapped（GOR 成功即按成功——传输恰在超时判定与取消生效间
    // 完成时，按失败重发会使设备收重复命令）。WriteFile 立即失败（集合无中断 OUT
    // 管道/蓝牙 HID 传输栈等不支持该写路径，MSDN/hidapi 口径）时回退
    // HidD_SetOutputReport 控制传输（同步、不可取消，受主机栈控制传输超时约束），
    // 重叠句柄上再失败则内部回退“临时同步句柄”重试（见 README 不确定点）。
    bool set_output_report(const uint8_t* data, size_t len, std::wstring* err = nullptr,
                           unsigned timeout_ms = 3000);

    // 调大驱动输入报告环形缓冲，减少回报率测量丢包（HidD_SetNumInputBuffers，上限 512）
    bool set_num_input_buffers(ULONG count);

private:
    bool fill_caps(std::wstring* err);
    // set_output_report 的回退路径：HidD_SetOutputReport（同步控制传输），
    // 重叠句柄上失败后以“临时同步句柄”再试一次
    bool set_output_report_sync(std::vector<uint8_t>& buf, std::wstring* err);

    wraii::uhandle<wraii::handle_closer> m_handle;
    std::wstring m_path;
    HidCapsInfo m_caps;
};

struct HidPollResult {
    bool ok = false;
    unsigned samples = 0;            // 收到的输入报告数
    double avg_hz = 0;               // (samples-1) / 总时长
    double min_hz = 0;               // 瞬时频率（1/相邻间隔）最小值
    double max_hz = 0;               // 瞬时频率最大值
    double total_s = 0;              // 测量总时长
    unsigned hist_bucket_us = 100;   // 直方图粒度 100us
    std::vector<unsigned> hist;      // hist[i] = 间隔落在 [i*100us, (i+1)*100us)
    unsigned hist_over = 0;          // 超出量程（>25.6ms）的间隔数
    std::wstring detail;             // 失败/异常说明
};

// 打开 path 并测量 seconds 秒回报率。cancelled 返回 true 时提前结束（仍返回已收数据）。
HidPollResult hid_polling_rate_measure(const std::wstring& path, int seconds,
                                       const std::function<bool()>& cancelled = nullptr);
