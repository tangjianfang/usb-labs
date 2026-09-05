// json_mini.cpp — 递归下降 JSON 解析实现。未真机编译，按 MSDN/RFC 8259 口径编写。
#include "framework/json_mini.h"

#include <cstdlib>

namespace minijson {
namespace {

constexpr int kMaxDepth = 32;

class Parser {
public:
    Parser(const std::wstring& s, std::wstring* err, size_t* err_pos)
        : m_s(s), m_err(err), m_err_pos(err_pos) {}

    bool run(Value& out) {
        skip_ws();
        if (!parse_value(out, 0)) return false;
        skip_ws();
        if (m_i != m_s.size()) return fail(L"末尾存在多余内容");
        return true;
    }

private:
    bool fail(const wchar_t* msg) {
        if (m_err && m_err->empty()) *m_err = msg;
        if (m_err_pos) *m_err_pos = m_i;
        return false;
    }

    void skip_ws() {
        while (m_i < m_s.size()) {
            wchar_t c = m_s[m_i];
            if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0xFEFF)
                ++m_i;
            else
                break;
        }
    }

    static bool hex4(const std::wstring& s, size_t pos, unsigned* v) {
        if (pos + 4 > s.size()) return false;
        unsigned val = 0;
        for (int k = 0; k < 4; ++k) {
            wchar_t c = s[pos + static_cast<size_t>(k)];
            unsigned d;
            if (c >= L'0' && c <= L'9')      d = static_cast<unsigned>(c - L'0');
            else if (c >= L'a' && c <= L'f') d = static_cast<unsigned>(c - L'a') + 10u;
            else if (c >= L'A' && c <= L'F') d = static_cast<unsigned>(c - L'A') + 10u;
            else return false;
            val = val * 16u + d;
        }
        *v = val;
        return true;
    }

    void append_utf16(std::wstring& out, unsigned unit) {
        out.push_back(static_cast<wchar_t>(unit));
    }

    bool parse_string(std::wstring& out) {
        out.clear();
        ++m_i; // 跳过开头引号
        while (true) {
            if (m_i >= m_s.size()) return fail(L"字符串未闭合");
            wchar_t c = m_s[m_i++];
            if (c == L'"') return true;
            if (c != L'\\') { out.push_back(c); continue; }
            if (m_i >= m_s.size()) return fail(L"转义符后意外结尾");
            wchar_t e = m_s[m_i++];
            switch (e) {
                case L'"':  out.push_back(L'"');  break;
                case L'\\': out.push_back(L'\\'); break;
                case L'/':  out.push_back(L'/');  break;
                case L'b':  out.push_back(L'\b'); break;
                case L'f':  out.push_back(L'\f'); break;
                case L'n':  out.push_back(L'\n'); break;
                case L'r':  out.push_back(L'\r'); break;
                case L't':  out.push_back(L'\t'); break;
                case L'u': {
                    unsigned hi = 0;
                    if (!hex4(m_s, m_i, &hi)) return fail(L"非法 \\u 转义");
                    m_i += 4;
                    if (hi >= 0xD800u && hi <= 0xDBFFu) {           // 高代理
                        if (m_i + 1 < m_s.size() && m_s[m_i] == L'\\' && m_s[m_i + 1] == L'u') {
                            m_i += 2;
                            unsigned lo = 0;
                            if (!hex4(m_s, m_i, &lo)) return fail(L"非法 \\u 转义");
                            m_i += 4;
                            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                                unsigned cp = 0x10000u + ((hi - 0xD800u) << 10) + (lo - 0xDC00u);
                                // 基本多文种平面之外：拆回 UTF-16 对（wstring 本就是 UTF-16）
                                append_utf16(out, 0xD800u + ((cp - 0x10000u) >> 10));
                                append_utf16(out, 0xDC00u + ((cp - 0x10000u) & 0x3FFu));
                            } else {
                                return fail(L"代理对不配对");
                            }
                        } else {
                            return fail(L"代理对不配对");
                        }
                    } else {
                        append_utf16(out, hi);
                    }
                    break;
                }
                default:
                    return fail(L"非法转义字符");
            }
        }
    }

    bool parse_number(Value& v) {
        wchar_t* end = nullptr;
        double d = ::wcstod(m_s.c_str() + m_i, &end);
        if (end == m_s.c_str() + m_i) return fail(L"非法数值");
        m_i = static_cast<size_t>(end - m_s.c_str());
        v.type = Value::Type::Number;
        v.number = d;
        return true;
    }

    bool parse_value(Value& v, int depth) {
        if (depth > kMaxDepth) return fail(L"嵌套过深");
        skip_ws();
        if (m_i >= m_s.size()) return fail(L"意外结尾");
        wchar_t c = m_s[m_i];
        if (c == L'{') return parse_object(v, depth);
        if (c == L'[') return parse_array(v, depth);
        if (c == L'"') {
            v = Value{};
            v.type = Value::Type::String;
            return parse_string(v.string);
        }
        if (c == L't') {
            if (m_s.compare(m_i, 4, L"true") == 0) {
                m_i += 4;
                v = Value{};
                v.type = Value::Type::Bool;
                v.boolean = true;
                return true;
            }
            return fail(L"非法字面量");
        }
        if (c == L'f') {
            if (m_s.compare(m_i, 5, L"false") == 0) {
                m_i += 5;
                v = Value{};
                v.type = Value::Type::Bool;
                v.boolean = false;
                return true;
            }
            return fail(L"非法字面量");
        }
        if (c == L'n') {
            if (m_s.compare(m_i, 4, L"null") == 0) {
                m_i += 4;
                v = Value{};
                v.type = Value::Type::Null;
                return true;
            }
            return fail(L"非法字面量");
        }
        return parse_number(v);
    }

    bool parse_object(Value& v, int depth) {
        ++m_i; // '{'
        v = Value{};
        v.type = Value::Type::Object;
        skip_ws();
        if (m_i < m_s.size() && m_s[m_i] == L'}') { ++m_i; return true; }
        while (true) {
            skip_ws();
            if (m_i >= m_s.size() || m_s[m_i] != L'"') return fail(L"对象键必须是字符串");
            std::wstring key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (m_i >= m_s.size() || m_s[m_i] != L':') return fail(L"缺少 ':'");
            ++m_i;
            Value item;
            if (!parse_value(item, depth + 1)) return false;
            v.members.emplace_back(std::move(key), std::move(item));
            skip_ws();
            if (m_i >= m_s.size()) return fail(L"对象未闭合");
            if (m_s[m_i] == L',') { ++m_i; continue; }
            if (m_s[m_i] == L'}') { ++m_i; return true; }
            return fail(L"对象内缺少 ',' 或 '}'");
        }
    }

    bool parse_array(Value& v, int depth) {
        ++m_i; // '['
        v = Value{};
        v.type = Value::Type::Array;
        skip_ws();
        if (m_i < m_s.size() && m_s[m_i] == L']') { ++m_i; return true; }
        while (true) {
            Value item;
            if (!parse_value(item, depth + 1)) return false;
            v.array.push_back(std::move(item));
            skip_ws();
            if (m_i >= m_s.size()) return fail(L"数组未闭合");
            if (m_s[m_i] == L',') { ++m_i; continue; }
            if (m_s[m_i] == L']') { ++m_i; return true; }
            return fail(L"数组内缺少 ',' 或 ']'");
        }
    }

    const std::wstring& m_s;
    size_t m_i = 0;
    std::wstring* m_err;
    size_t* m_err_pos;
};

} // namespace

bool parse(const std::wstring& text, Value& out, std::wstring* err, size_t* err_pos) {
    Parser p(text, err, err_pos);
    return p.run(out);
}

long long obj_int(const Value& o, const wchar_t* key, long long dft) {
    const Value* v = o.find(key);
    return v ? v->as_int(dft) : dft;
}

double obj_double(const Value& o, const wchar_t* key, double dft) {
    const Value* v = o.find(key);
    return v ? v->as_double(dft) : dft;
}

std::wstring obj_str(const Value& o, const wchar_t* key, const std::wstring& dft) {
    const Value* v = o.find(key);
    return v ? v->as_str(dft) : dft;
}

long long obj_vidpid(const Value& o, const wchar_t* key, long long dft) {
    const Value* v = o.find(key);
    if (!v) return dft;
    if (v->type == Value::Type::Number) return static_cast<long long>(v->number);
    if (v->type == Value::Type::String)
        return static_cast<long long>(::wcstoul(v->string.c_str(), nullptr, 0)); // "0x1234" → 十六进制
    return dft;
}

} // namespace minijson
