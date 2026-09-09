// log.h — UTS 统一日志规范（spdlog 门面，T12 工程化的日志地基）
//
// 规范（所有核心模块一律遵守）：
//   1. 取日志器：`auto log = ustlog::logger("channel.serial");`——模块名小写点分
//      （app / ui.console / discovery / channel.serial|hid|usb|msc / session.core /
//       session.codec / session.view / parser.hid / parser.pd / engine / framework）。
//   2. 输出格式统一：`[%Y-%m-%d %H:%M:%S.%e][级别][模块][线程] 消息`，UTF-8。
//   3. 级别语义：trace=逐字节/逐包（配 spdlog::to_hex）· debug=逐操作（参数+结果+
//      耗时）· info=生命周期（启动/打开/关闭/扫描）· warn=可恢复异常（重试/回退/
//      超时留痕）· err=失败（必带 GetLastError()/HRESULT 十六进制）。
//   4. sink：滚动文件 %LOCALAPPDATA%\USBTestStudio\logs\<base>.log（5MB×3）+
//      OutputDebugString（GUI 无控制台也可见）；控制台靶（selftest）另接 stdout。
//   5. 级别开关：环境变量 USBTS_LOG_LEVEL=trace|debug|info|warn|err|off，默认 info；
//      warn 及以上即刷盘（崩溃前最后几条不丢）。
#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/fmt/bin_to_hex.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <windows.h>

#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace ustlog {

// 宽字符 → UTF-8（Win32 API 返回的路径/描述进日志前统一转码；失败按空串处理，
// 日志路径不允许再抛异常）
inline std::string w2u(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                        static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    if (::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                              out.data(), n, nullptr, nullptr) <= 0) return {};
    return out;
}

namespace detail {

inline std::string env_level() {
    char buf[16] = {};
    const DWORD n = ::GetEnvironmentVariableA("USBTS_LOG_LEVEL", buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return {};
    for (char* p = buf; *p; ++p) *p = static_cast<char>(::tolower(*p));
    return buf;
}

inline spdlog::level::level_enum parse_level(const std::string& s,
                                             spdlog::level::level_enum dflt) {
    if (s == "trace") return spdlog::level::trace;
    if (s == "debug") return spdlog::level::debug;
    if (s == "info") return spdlog::level::info;
    if (s == "warn" || s == "warning") return spdlog::level::warn;
    if (s == "err" || s == "error") return spdlog::level::err;
    if (s == "critical") return spdlog::level::critical;
    if (s == "off") return spdlog::level::off;
    return dflt;
}

inline std::wstring log_dir() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring base = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : L".";
    return base + L"\\USBTestStudio\\logs";
}

} // namespace detail

// 进程级初始化（幂等；首次调用生效）。also_stdout=true 供控制台靶（selftest）
// 同步输出到终端。任何 sink 失败都降级为仅已成功的 sink——日志不可拖死业务。
inline void init(bool also_stdout = false, const wchar_t* file_base = L"usts") {
    static std::once_flag once;
    std::call_once(once, [also_stdout, file_base] {
        std::vector<spdlog::sink_ptr> sinks;
        // 1) 滚动文件（T12：崩溃后可回放的现场）
        try {
            const std::wstring dir = detail::log_dir();
            ::CreateDirectoryW(dir.c_str(), nullptr);  // 已存在则 ERROR_ALREADY_EXISTS，忽略
            sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                dir + L"\\" + file_base + L".log", 5 * 1024 * 1024, 3));
        } catch (const spdlog::spdlog_ex&) {
            // 目录不可写等：放弃文件 sink，仅余调试器/控制台输出
        }
        // 2) OutputDebugString（GUI 进程在 DbgView/VS 输出窗可见）
        sinks.push_back(std::make_shared<spdlog::sinks::msvc_sink_mt>());
        // 3) stdout（仅控制台靶）
        if (also_stdout) {
            sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        }

        const auto level = detail::parse_level(detail::env_level(), spdlog::level::info);
        for (auto& s : sinks) s->set_level(level);

        auto root = std::make_shared<spdlog::logger>("app", sinks.begin(), sinks.end());
        root->set_level(level);
        // 规范格式：[毫秒时间][级别][模块][线程] 消息
        root->set_pattern("[%Y-%m-%d %H:%M:%S.%e][%^%l%$][%n][T%t] %v");
        root->flush_on(spdlog::level::warn);
        spdlog::register_logger(root);
        spdlog::set_default_logger(root);

        // 静态初始化时序修补：.cpp 匿名空间的模块 logger 在本 init 之前创建，
        // 只捕获到默认 stdout sink——统一补挂文件/调试 sink 与级别格式，
        // 保证任何取 logger 的时序（静态或运行期）最终都进同一套输出。
        spdlog::apply_all([&](const std::shared_ptr<spdlog::logger>& lg) {
            if (!lg || lg->name() == root->name()) return;
            lg->sinks() = sinks;
            lg->set_level(level);
            lg->set_pattern("[%Y-%m-%d %H:%M:%S.%e][%^%l%$][%n][T%t] %v");
            lg->flush_on(spdlog::level::warn);
        });
    });
}

// 模块日志器（同名复用；未 init 也安全——落到注册表默认 stdout logger，
// 各靶 main/selftest 入口必须先 ustlog::init(...)）
inline std::shared_ptr<spdlog::logger> logger(const std::string& name) {
    if (auto existing = spdlog::get(name)) return existing;
    try {
        // 克隆默认 logger 的 sink 与格式，仅换模块名（%n）
        std::vector<spdlog::sink_ptr> sinks = spdlog::default_logger()->sinks();
        auto lg = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
        lg->set_level(spdlog::default_logger()->level());
        lg->set_pattern("[%Y-%m-%d %H:%M:%S.%e][%^%l%$][%n][T%t] %v");
        lg->flush_on(spdlog::level::warn);
        spdlog::register_logger(lg);
        return lg;
    } catch (const spdlog::spdlog_ex&) {
        // 并发竞态：对手线程刚注册同名——返回已注册实例
        return spdlog::get(name);
    }
}

} // namespace ustlog
