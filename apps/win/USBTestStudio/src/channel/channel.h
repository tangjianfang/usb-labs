// channel.h — EP-4 工程师通信控制台·统一通道抽象（设计 §3）：
// 会话台(S3)与产测引擎共用同一套通道实现——HID/串口/WinUSB/MSC 四种通道
// 同一 IChannel 契约：open/close、send(bytes)、on_receive(cb)、set_timeout、描述信息。
// 实现方约定：send 为同步语义；接收由后台读线程驱动回调（回调内在读线程上下文，
// UI 层需自行投递消息，勿在回调内重入 open/close）。
// 日志：本文件为纯抽象契约/结构定义，不打日志——级别语义统一归各实现层
//（channel.serial / channel.hid / channel.usb / channel.msc）。
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// 一帧收/发记录（S3 接收区时间戳与统计的原始单位）
struct ChannelFrame {
    bool out = false;                       // true=发送 false=接收
    unsigned long long t_ms = 0;            // GetTickCount64 口径
    std::vector<uint8_t> bytes;
};

struct ChannelStats {
    unsigned long long rx_frames = 0, tx_frames = 0;
    unsigned long long rx_bytes = 0, tx_bytes = 0;
};

struct ChannelDesc {
    std::wstring kind;      // "serial" / "hid" / "usb" / "msc"
    std::wstring display;   // "COM7 @115200 8N1" / "HID 1234:0002 ..."
    std::wstring path;      // 打开用路径（\\.\COM7 / HID 设备接口路径）
    // HID 顶层集合能力（HidChannel open 时自 HidCapsInfo 透传；S4 解析面板
    // 按 usage page/usage 选型，report_id 决定解析前是否剥离前缀字节）
    unsigned hid_usage_page = 0;   // 0x01 Generic Desktop / 0x0C Consumer …
    unsigned hid_usage = 0;        // GD 页 0x02 Mouse / 0x06 Keyboard / 0x07 Keypad …
    bool hid_report_id = false;    // 输入报告带 Report ID 前缀
};

using ReceiveCallback = std::function<void(const std::vector<uint8_t>&)>;

// 通道层 send 的写超时上界（ms，evolve #71/#72）：send 在 UI 线程同步执行，
// 设备 NAK 永续时等待必须有界——各通道实现须把此常量显式透传给端口层写接口
//（HidPort::set_output_report / WinUsbPort::write_pipe 的 timeout_ms 形参），
// 不依赖端口层默认值（防两侧默认漂移；通道→端口契约由 channel_selftest 钉住）
constexpr unsigned kSendTimeoutMs = 3000;

class IChannel {
public:
    virtual ~IChannel() = default;

    // 打开（参数在构造时给定）；失败时 *err 给出可显示原因
    virtual bool open(std::wstring* err = nullptr) = 0;
    virtual void close() noexcept = 0;
    virtual bool is_open() const noexcept = 0;

    // 发送一帧（同步完成或失败）；同时计入 tx 统计
    virtual bool send(const uint8_t* data, size_t len, std::wstring* err = nullptr) = 0;

    // 注册接收回调（读线程上下文调用）；置空即停用。重复注册以后者为准。
    virtual void set_receive_callback(ReceiveCallback cb) = 0;

    // 读超时（读线程轮片上限，ms），0 = 用实现默认
    virtual void set_read_timeout(unsigned ms) noexcept = 0;

    virtual const ChannelDesc& desc() const noexcept = 0;
    virtual ChannelStats stats() const noexcept = 0;
};
