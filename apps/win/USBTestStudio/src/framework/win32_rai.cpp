// win32_rai.cpp — 见头文件说明。未真机编译，按 MSDN 口径编写。
#include "framework/win32_rai.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace wraii {

std::wstring last_error_message(DWORD err) {
    LPWSTR buf = nullptr;
    // FORMAT_MESSAGE_ALLOCATE_BUFFER：lpBuffer 为 LPWSTR*（以 MSDN 为准）
    DWORD n = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                   FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                               reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring out;
    if (n != 0 && buf != nullptr) {
        out.assign(buf, n);
        while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n' || out.back() == L' '))
            out.pop_back();
    }
    if (buf != nullptr) ::LocalFree(buf);
    if (out.empty()) out = fmt_v(L"Win32 错误码 %lu", static_cast<unsigned long>(err));
    return out;
}

std::wstring fmt_v(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    // vswprintf(buf, 容量, 格式, 参数)：MSVC 自 VS2015 起为 C99 一致版本
    ::vswprintf(buf, 512, fmt, ap);
    va_end(ap);
    buf[511] = L'\0';
    return buf;
}

std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) return {};
    int need = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return {};
    std::wstring out(static_cast<size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), need);
    return out;
}

std::string wide_to_utf8(std::wstring_view s) {
    if (s.empty()) return {};
    int need = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                     nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), need,
                          nullptr, nullptr);
    return out;
}

std::wstring ascii_to_wide(std::string_view s) {
    std::wstring out;
    out.reserve(s.size());
    for (char c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        out.push_back(uc < 0x80 ? static_cast<wchar_t>(uc) : L'?');
    }
    return out;
}

bool read_file_bytes(const std::wstring& path, std::vector<uint8_t>& out, std::wstring* err) {
    out.clear();
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = win_err(L"CreateFileW(读)", ::GetLastError());
        return false;
    }
    uhandle<handle_closer> guard(h);
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h, &size)) {
        if (err) *err = win_err(L"GetFileSizeEx", ::GetLastError());
        return false;
    }
    if (size.QuadPart < 0 || size.QuadPart > 64 * 1024 * 1024) {
        if (err) *err = L"文件过大（>64MB），拒绝读入";
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    while (done < out.size()) {
        DWORD want = static_cast<DWORD>(std::min<uint64_t>(out.size() - done, 1u << 20));
        DWORD got = 0;
        if (!::ReadFile(h, out.data() + done, want, &got, nullptr)) {
            if (err) *err = win_err(L"ReadFile", ::GetLastError());
            out.clear();
            return false;
        }
        if (got == 0) break; // EOF
        done += got;
    }
    out.resize(done);
    return true;
}

bool read_text_file_utf8(const std::wstring& path, std::wstring& out, std::wstring* err) {
    std::vector<uint8_t> bytes;
    if (!read_file_bytes(path, bytes, err)) return false;
    // 跳过 UTF-8 BOM
    size_t off = 0;
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) off = 3;
    out = utf8_to_wide(std::string_view(reinterpret_cast<const char*>(bytes.data()) + off,
                                        bytes.size() - off));
    return true;
}

bool write_file_bytes(const std::wstring& path, const void* data, size_t len, std::wstring* err) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = win_err(L"CreateFileW(写)", ::GetLastError());
        return false;
    }
    uhandle<handle_closer> guard(h);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t done = 0;
    while (done < len) {
        DWORD want = static_cast<DWORD>(std::min<uint64_t>(len - done, 1u << 20));
        DWORD wrote = 0;
        if (!::WriteFile(h, p + done, want, &wrote, nullptr)) {
            if (err) *err = win_err(L"WriteFile", ::GetLastError());
            return false;
        }
        done += wrote;
    }
    return true;
}

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    // 以 MSDN 为准：路径可能超过 MAX_PATH，此处按常规部署截断即可
    if (n == 0 || n >= MAX_PATH) return L".";
    std::wstring full(buf, n);
    size_t slash = full.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : full.substr(0, slash);
}

std::wstring now_timestamp_iso() {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    return fmt_v(L"%04u-%02u-%02uT%02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour,
                 st.wMinute, st.wSecond);
}

std::wstring now_stamp_compact() {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    return fmt_v(L"%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour,
                 st.wMinute, st.wSecond);
}

std::wstring sanitize_filename(std::wstring_view s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        bool bad = (c < 0x20) || c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' ||
                   c == L'"' || c == L'<' || c == L'>' || c == L'|';
        out.push_back(bad ? L'_' : c);
        if (out.size() >= 64) break;
    }
    if (out.empty()) out = L"N/A";
    return out;
}

// ---------------------------------------------------------------------------
// json_escape / json_writer
// ---------------------------------------------------------------------------
std::wstring json_escape(std::wstring_view s) {
    std::wstring out;
    out.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
            case L'"':  out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\b': out += L"\\b";  break;
            case L'\f': out += L"\\f";  break;
            case L'\n': out += L"\\n";  break;
            case L'\r': out += L"\\r";  break;
            case L'\t': out += L"\\t";  break;
            default:
                if (c < 0x20) {
                    wchar_t buf[8];
                    ::swprintf(buf, 8, L"\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

void json_writer::pre() {
    if (m_stack.empty()) return;
    Frame& f = m_stack.back();
    if (f.after_key) {           // key 之后的首个值不再加逗号
        f.after_key = false;
        return;
    }
    if (f.need_comma) m_out += L',';
    f.need_comma = true;
    if (m_pretty) {
        m_out += L'\n';
        m_out.append(m_stack.size() * 2, L' ');
    }
}

json_writer& json_writer::begin_object() {
    pre();
    m_out += L'{';
    m_stack.push_back(Frame{true, false, false});
    return *this;
}

json_writer& json_writer::end_object() {
    Frame f = m_stack.back();
    m_stack.pop_back();
    if (m_pretty && f.need_comma) {
        m_out += L'\n';
        m_out.append(m_stack.size() * 2, L' ');
    }
    m_out += L'}';
    return *this;
}

json_writer& json_writer::begin_array() {
    pre();
    m_out += L'[';
    m_stack.push_back(Frame{false, false, false});
    return *this;
}

json_writer& json_writer::end_array() {
    Frame f = m_stack.back();
    m_stack.pop_back();
    if (m_pretty && f.need_comma) {
        m_out += L'\n';
        m_out.append(m_stack.size() * 2, L' ');
    }
    m_out += L']';
    return *this;
}

json_writer& json_writer::key(std::wstring_view k) {
    pre();
    m_out += L'"';
    m_out += json_escape(k);
    m_out += m_pretty ? L"\": " : L"\":";
    m_stack.back().after_key = true;
    return *this;
}

json_writer& json_writer::string_value(std::wstring_view v) {
    pre();
    m_out += L'"';
    m_out += json_escape(v);
    m_out += L'"';
    return *this;
}

json_writer& json_writer::int_value(long long v) {
    pre();
    wchar_t buf[32];
    ::swprintf(buf, 32, L"%lld", v);
    m_out += buf;
    return *this;
}

json_writer& json_writer::double_value(double v) {
    pre();
    wchar_t buf[64];
    if (std::isnan(v) || std::isinf(v)) {   // NaN/Inf 不是合法 JSON
        m_out += L"null";
        return *this;
    }
    ::swprintf(buf, 64, L"%.6g", v);
    m_out += buf;
    return *this;
}

json_writer& json_writer::bool_value(bool v) {
    pre();
    m_out += v ? L"true" : L"false";
    return *this;
}

json_writer& json_writer::null_value() {
    pre();
    m_out += L"null";
    return *this;
}

std::wstring json_writer::result() const {
    return m_out + L"\n";
}

} // namespace wraii
