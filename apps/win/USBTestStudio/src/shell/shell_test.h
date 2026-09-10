// shell_test.h — shell 模块自测微框架（无第三方依赖，任意 CWD 可跑，退出码 0/1）
//
// 用法（shell_selftest.cpp）：
//   static void test_xxx() { CHECK(f()); CHECK_EQ(a, b); }
//   int main() { RUN_TEST(test_xxx); return shell_test::run_all("shell_selftest"); }
// 约定：失败立即打印表达式与位置但不中断（跑完全部再汇总），与仓库 *_selftest 口径一致。
#pragma once

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
