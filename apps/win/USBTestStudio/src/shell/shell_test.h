// shell_test.h — shell 模块自测微框架（无第三方依赖，任意 CWD 可跑，退出码 0/1）
//
// 用法（shell_selftest.cpp）：
//   static void test_xxx() { CHECK(f()); CHECK_EQ(a, b); }
//   int main() { RUN_TEST(test_xxx); return shell_test::run_all("shell_selftest"); }
// 约定：失败立即打印表达式与位置但不中断（跑完全部再汇总），与仓库 *_selftest 口径一致。
#pragma once

#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <functional>
#include <string>
#include <vector>

namespace shell_test {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& cases() {
    static std::vector<Case> c;
    return c;
}
inline int& failed() {
    static int f = 0;
    return f;
}

inline bool _ck(bool ok, const char* expr, const char* file, int line) {
    if (!ok) {
        std::printf("  FAIL %s:%d  %s\n", file, line, expr);
        ++failed();
    }
    return ok;
}

#define CHECK(x) shell_test::_ck((x), #x, __FILE__, __LINE__)
#define CHECK_EQ(a, b) shell_test::_ck((a) == (b), #a " == " #b, __FILE__, __LINE__)

#define RUN_TEST(fn) shell_test::cases().push_back({#fn, fn})

// ---------------------------------------------------------------------------
// 临时目录/文件助手（T6+ 工程模型/模板库/崩溃管理用；Win32，自清理靠调用方）
// ---------------------------------------------------------------------------
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

inline std::wstring make_temp_dir() {
    wchar_t base[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, base);
    std::wstring dir = std::wstring(base) + L"usts-test-";
    for (int i = 0; i < 8; ++i) dir += static_cast<wchar_t>(L'a' + (::rand() % 26));
    ::CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

inline bool write_file(const std::wstring& path, const char* content) {
    const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = ::WriteFile(h, content,
                                static_cast<DWORD>(std::strlen(content)), &written, nullptr);
    ::CloseHandle(h);
    return ok && written == std::strlen(content);
}

inline void _remove_dir_recurse(const std::wstring& dir) {
    std::wstring pattern = dir + L"\\*";
    WIN32_FIND_DATAW fd;
    const HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring full = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) _remove_dir_recurse(full);
        else ::DeleteFileW(full.c_str());
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    ::RemoveDirectoryW(dir.c_str());
}

inline void remove_temp_dir(const std::wstring& dir) { _remove_dir_recurse(dir); }

inline int run_all(const char* title) {
    std::printf("== %s ==\n", title);
    int n = 0;
    for (auto& c : cases()) {
        const int before = failed();
        c.fn();
        ++n;
        std::printf("  %s %s\n", failed() == before ? "ok  " : "FAIL", c.name);
    }
    std::printf("%d 例，%d 失败 %s\n", n, failed(), failed() == 0 ? "OK" : "");
    return failed() ? 1 : 0;
}

} // namespace shell_test
