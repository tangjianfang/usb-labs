// crashdump.h — 崩溃 minidump 管理（设计 §8 崩溃恢复 / 计划 MS0-T9）
//
// 契约：
//   1. dump 目录 %LOCALAPPDATA%\USBDevStudio\minidump，文件 crashdump-<yyyyMMdd-HHmmss>.dmp；
//   2. install_handler()：SetUnhandledExceptionFilter → MiniDumpWriteDump（进程一次）；
//      安装本身不做单测（崩溃注入属烟测域），见 README 不确定清单口径；
//   3. scan/mark_seen/is_seen/purge 提供 *dir 注入重载——纯逻辑可测；
//   4. seen 账本 seen.txt（一行一路径）与 dump 同目录；purge 同步清对应账本行。
#pragma once

#include "../app/log.h"
#include "../framework/win32_rai.h"

#include <string>
#include <vector>

namespace usts::shell {

class CrashDumpMgr {
public:
    // —— 真实路径（不可注入）——
    static std::wstring dir();                 // %LOCALAPPDATA%\USBDevStudio\minudump → minidump
    static bool ensure_dir(std::string& err);
    static void install_handler();             // 进程级一次（见契约 2）

    // —— 可注入重载（测试用；dir 必须已存在或为空则返回空列表）——
    static std::vector<std::wstring> scan_dir(const std::wstring& dir);
    static bool mark_seen(const std::wstring& dir, const std::wstring& dump_path);
    static bool is_seen(const std::wstring& dir, const std::wstring& dump_path);
    static size_t purge_older_than_days(const std::wstring& dir, int days);

    // —— 便捷封装（用 dir()）——
    static std::vector<std::wstring> scan() { return scan_dir(dir()); }
};

// ---------------------------------------------------------------------------
// 实现（header-only；模块日志 shell.crash）
// ---------------------------------------------------------------------------
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <windows.h>
#include <minidumpapiset.h>
#include <dbghelp.h>

namespace detail {

inline std::wstring crash_seen_path(const std::wstring& dir) { return dir + L"\\seen.txt"; }

// 文件 mtime（FILETIME → Unix 秒）
inline int64_t file_mtime_unix(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return -1;
    ULARGE_INTEGER u{};
    u.LowPart = fa.ftLastWriteTime.dwLowDateTime;
    u.HighPart = fa.ftLastWriteTime.dwHighDateTime;
    return static_cast<int64_t>((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

inline LONG WINAPI crash_write_dump(EXCEPTION_POINTERS* ep) {
    wchar_t dir[MAX_PATH] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::wstring d = std::wstring(dir) + L"\\USBDevStudio\\minidump";
        ::CreateDirectoryW(d.c_str(), nullptr);
        SYSTEMTIME st = {};
        ::GetLocalTime(&st);
        wchar_t name[64] = {};
        swprintf(name, 64, L"crashdump-%04u%02u%02u-%02u%02u%02u.dmp",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        const std::wstring path = d + L"\\" + name;
        const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            // 动态取 MiniDumpWriteDump（崩溃处理器标准模式：避免崩溃进程依赖
            // dbghelp 静态绑定与加载顺序问题；typedef 沿 minidumpapiset.h 口径）
            using FnMiniDumpWriteDump = BOOL (WINAPI*)(
                HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
            const HMODULE dbghelp = ::GetModuleHandleW(L"dbghelp.dll");
            if (dbghelp) {
                const auto write_dump = reinterpret_cast<FnMiniDumpWriteDump>(
                    ::GetProcAddress(dbghelp, "MiniDumpWriteDump"));
                if (write_dump) {
                    MINIDUMP_EXCEPTION_INFORMATION mei{::GetCurrentThreadId(), ep, FALSE};
                    write_dump(::GetCurrentProcess(), ::GetCurrentProcessId(), h,
                               MiniDumpNormal, &mei, nullptr, nullptr);
                }
            }
            ::CloseHandle(h);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;   // 交回系统默认流程（WER 弹窗）
}

} // namespace detail

inline std::wstring CrashDumpMgr::dir() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    const std::wstring base = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : L".";
    return base + L"\\USBDevStudio\\minidump";
}

inline bool CrashDumpMgr::ensure_dir(std::string& err) {
    // 逐级创建（CreateDirectoryW 不递归）：…\AppData\Local\USBDevStudio\minidump
    const std::wstring d = dir();
    const std::wstring parent = d.substr(0, d.find_last_of(L'\\'));
    ::CreateDirectoryW(parent.c_str(), nullptr);   // 已存在=ALLOWED，忽略结果
    if (!::CreateDirectoryW(d.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) {
        err = "创建 minidump 目录失败 GLE=" + std::to_string(::GetLastError());
        return false;
    }
    return true;
}

inline void CrashDumpMgr::install_handler() {
    auto log = ustlog::logger("shell.crash");
    ::SetUnhandledExceptionFilter(detail::crash_write_dump);
    log->info("minidump 处理器已安装（目录={}）", wraii::wide_to_utf8(dir()));
}

inline std::vector<std::wstring> CrashDumpMgr::scan_dir(const std::wstring& dir) {
    auto log = ustlog::logger("shell.crash");
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    const HANDLE h = ::FindFirstFileW((dir + L"\\*.dmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;   // 目录不存在/无 dmp=空
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        out.push_back(dir + L"\\" + fd.cFileName);
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    // 按文件名排序=按时间排序（文件名含时间戳）——stable 简化
    std::sort(out.begin(), out.end());
    log->debug("scan_dir: {} 个 dump（{}）", out.size(), wraii::wide_to_utf8(dir));
    return out;
}

inline bool CrashDumpMgr::mark_seen(const std::wstring& dir, const std::wstring& dump_path) {
    auto log = ustlog::logger("shell.crash");
    if (is_seen(dir, dump_path)) return true;
    const HANDLE h = ::CreateFileW(detail::crash_seen_path(dir).c_str(), FILE_APPEND_DATA,
                                   0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        log->warn("mark_seen: 打开账本失败 GLE={}", ::GetLastError());
        return false;
    }
    const std::string line = wraii::wide_to_utf8(dump_path) + "\n";
    DWORD written = 0;
    ::WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    ::CloseHandle(h);
    log->debug("mark_seen: {}", wraii::wide_to_utf8(dump_path));
    return true;
}

inline bool CrashDumpMgr::is_seen(const std::wstring& dir, const std::wstring& dump_path) {
    std::wstring text, err;
    if (!wraii::read_text_file_utf8(detail::crash_seen_path(dir), text, &err)) return false;
    return text.find(dump_path) != std::wstring::npos;
}

inline size_t CrashDumpMgr::purge_older_than_days(const std::wstring& dir, int days) {
    auto log = ustlog::logger("shell.crash");
    FILETIME ft_now = {};
    ::GetSystemTimeAsFileTime(&ft_now);
    ULARGE_INTEGER unow{};
    unow.LowPart = ft_now.dwLowDateTime;
    unow.HighPart = ft_now.dwHighDateTime;
    const int64_t now_unix = static_cast<int64_t>((unow.QuadPart - 116444736000000000ULL) / 10000000ULL);
    size_t purged = 0;
    for (const auto& p : scan_dir(dir)) {
        const int64_t mt = detail::file_mtime_unix(p);
        if (mt > 0 && (now_unix - mt) > static_cast<int64_t>(days) * 86400) {
            if (::DeleteFileW(p.c_str())) {
                ++purged;
                log->debug("purge: {}", wraii::wide_to_utf8(p));
            }
        }
    }
    if (purged) log->info("purge_older_than_days({}): 清理 {} 个", days, purged);
    return purged;
}

} // namespace usts::shell
