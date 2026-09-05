// win32_rai.h — Win32 句柄 RAII 封装、错误消息、编码/文件小工具、最小 JSON writer
// 未真机编译，按 MSDN 口径编写（不确定处见 README“不确定 API 清单”与代码内注释）。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wraii {

// ---------------------------------------------------------------------------
// 通用句柄 RAII：CloserT 需提供  using H;  static H invalid();  static void close(H);
// ---------------------------------------------------------------------------
template <typename CloserT>
class unique_handle {
public:
    using H = typename CloserT::H;

    unique_handle() = default;
    explicit unique_handle(H h) noexcept : m_h(h) {}
    unique_handle(unique_handle&& o) noexcept : m_h(o.release()) {}
    unique_handle& operator=(unique_handle&& o) noexcept {
        if (this != &o) reset(o.release());
        return *this;
    }
    ~unique_handle() { reset(); }

    H    get() const noexcept { return m_h; }
    bool valid() const noexcept { return m_h != CloserT::invalid(); }
    explicit operator bool() const noexcept { return valid(); }
    H    release() noexcept { H t = m_h; m_h = CloserT::invalid(); return t; }
    void reset(H h = CloserT::invalid()) noexcept {
        if (m_h != CloserT::invalid()) CloserT::close(m_h);
        m_h = h;
    }

    unique_handle(const unique_handle&) = delete;
    unique_handle& operator=(const unique_handle&) = delete;

private:
    H m_h = CloserT::invalid();
};

// CreateFileW / CreateEventW / LoadLibraryW 等标准句柄（失败返回 nullptr）
struct handle_closer {
    using H = HANDLE;
    static H invalid() noexcept { return nullptr; }
    static void close(H h) noexcept { if (h) CloseHandle(h); }
};

// SetupDiGetClassDevsW（失败返回 INVALID_HANDLE_VALUE，而非 nullptr）
struct hdevinfo_closer {
    using H = HDEVINFO;
    static H invalid() noexcept { return INVALID_HANDLE_VALUE; }
    static void close(H h) noexcept { SetupDiDestroyDeviceInfoList(h); }
};

template <typename CloserT>
using uhandle = unique_handle<CloserT>;

// ---------------------------------------------------------------------------
// 日志级别（引擎与 UI 共用）
// ---------------------------------------------------------------------------
enum class LogLevel : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };
inline const wchar_t* log_level_name(LogLevel lv) noexcept {
    switch (lv) {
        case LogLevel::Debug: return L"DBG ";
        case LogLevel::Info:  return L"INFO";
        case LogLevel::Warn:  return L"WARN";
        default:              return L"ERR ";
    }
}

// ---------------------------------------------------------------------------
// 错误消息
// ---------------------------------------------------------------------------
std::wstring last_error_message(DWORD err);
inline std::wstring win_err(const wchar_t* ctx, DWORD err) {
    return std::wstring(ctx) + L" 失败: " + last_error_message(err);
}

// ---------------------------------------------------------------------------
// 格式化（swprintf 封装，容量 512 字符；避免依赖 <format> 的实现差异）
// ---------------------------------------------------------------------------
std::wstring fmt_v(const wchar_t* fmt, ...);

// ---------------------------------------------------------------------------
// 编码 / 文件
// ---------------------------------------------------------------------------
std::wstring utf8_to_wide(std::string_view s);
std::string  wide_to_utf8(std::wstring_view s);
std::wstring ascii_to_wide(std::string_view s);   // INQUIRY 等纯 ASCII 场景

bool read_file_bytes(const std::wstring& path, std::vector<uint8_t>& out, std::wstring* err);
bool read_text_file_utf8(const std::wstring& path, std::wstring& out, std::wstring* err);
bool write_file_bytes(const std::wstring& path, const void* data, size_t len, std::wstring* err);

// ---------------------------------------------------------------------------
// 路径 / 时间 / 命名
// ---------------------------------------------------------------------------
std::wstring exe_dir();
std::wstring now_timestamp_iso();      // 2026-09-06T12:34:56（与 tools/usbtest started 同构）
std::wstring now_stamp_compact();      // 20260906_123456（报告文件名）
std::wstring sanitize_filename(std::wstring_view s);

// ---------------------------------------------------------------------------
// 最小 JSON writer（字符串转义 / 数字 / 布尔 / null；输出与 tools/usbtest 报告同构）
// ---------------------------------------------------------------------------
std::wstring json_escape(std::wstring_view s);

class json_writer {
public:
    explicit json_writer(bool pretty = true) noexcept : m_pretty(pretty) {}

    json_writer& begin_object();
    json_writer& end_object();
    json_writer& begin_array();
    json_writer& end_array();
    json_writer& key(std::wstring_view k);
    json_writer& string_value(std::wstring_view v);
    json_writer& int_value(long long v);
    json_writer& double_value(double v);
    json_writer& bool_value(bool v);
    json_writer& null_value();

    std::wstring result() const;   // 取全文（尾部一个换行）

private:
    struct Frame { bool is_object; bool need_comma = false; bool after_key = false; };
    void pre();                    // 处理逗号 / 换行 / 缩进

    std::wstring m_out;
    std::vector<Frame> m_stack;
    bool m_pretty;
};

} // namespace wraii
