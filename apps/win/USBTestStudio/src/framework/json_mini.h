// json_mini.h — 自研轻量 JSON 解析器（仅服务本上位机的测试计划加载）。
// 支持：对象 / 数组 / 字符串（含 \uXXXX 与代理对）/ 数字 / true / false / null。
// 不支持：注释、尾逗号（按标准口径拒绝）。未真机编译，按 MSDN/RFC 8259 口径编写。
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace minijson {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::wstring string;
    std::vector<Value> array;
    std::vector<std::pair<std::wstring, Value>> members;   // 保持文件顺序

    const Value* find(std::wstring_view key) const noexcept {
        if (type != Type::Object) return nullptr;
        for (const auto& kv : members)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }

    bool is_object() const noexcept { return type == Type::Object; }
    bool is_array()  const noexcept { return type == Type::Array; }

    long long as_int(long long dft) const noexcept {
        return type == Type::Number ? static_cast<long long>(number) : dft;
    }
    double as_double(double dft) const noexcept {
        return type == Type::Number ? number : dft;
    }
    std::wstring as_str(const std::wstring& dft) const {
        return type == Type::String ? string : dft;
    }
};

// 解析全文。text 必须是“以 NUL 结尾”的 std::wstring（数值解析用 wcstod 需要终止符）。
// 失败时 err/err_pos 给出首个错误的描述与字符位置。
bool parse(const std::wstring& text, Value& out, std::wstring* err = nullptr,
           size_t* err_pos = nullptr);

// —— 计划字段便捷读取 ——
long long    obj_int(const Value& o, const wchar_t* key, long long dft);
double       obj_double(const Value& o, const wchar_t* key, double dft);
std::wstring obj_str(const Value& o, const wchar_t* key, const std::wstring& dft);
// 兼容 YAML 计划里 0x 十六进制 VID/PID（JSON 中写为字符串 "0x1234" 或十进制数字）
long long    obj_vidpid(const Value& o, const wchar_t* key, long long dft);

} // namespace minijson
