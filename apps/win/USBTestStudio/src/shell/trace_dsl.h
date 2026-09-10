// trace_dsl.h — 过滤 DSL（W4 / 计划 MS4-T7）
// 文法子集：expr := term (('&&'|'||') term)* ; term := '!'? factor ;
//           factor := '(' expr ')' | comparison ;
//           comparison := field op value ; op ∈ {==, !=, >, <, ~}
// 字段域：dir（in|out） ep（数字，取自 summary EPn） len（hex/2 字节数） src mark
//         summary（~ 为包含匹配，值不带引号取到运算符/括号边界）
#pragma once

#include "trace_model.h"

#include <memory>
#include <string>
#include <vector>

namespace usts::shell::trace {

struct DslError {
    size_t pos = 0;
    std::string message;
};

class DslExpr {
public:
    // 解析失败→false 且 err 填位置与原因；成功→可重复 matches
    static bool parse(const std::string& text, DslExpr& out, DslError& err);
    bool matches(const TraceRow& r) const;
    const std::string& text() const { return m_text; }

private:
    struct Node {
        enum class Kind { Cmp, And, Or, Not } kind = Kind::Cmp;
        // Cmp
        std::string field;
        int op = 0;            // 0 == 1 != 2 > 3 < 4 contains
        std::string value;
        // And/Or/Not
        std::vector<std::unique_ptr<Node>> kids;
    };
    std::string m_text;
    std::unique_ptr<Node> m_root;

    struct Parser;   // 前向（实现于下方）
    static bool eval(const Node& n, const TraceRow& r);
};

// ---------------------------------------------------------------------------
// 实现（header-only；模块日志 trace.dsl）
// ---------------------------------------------------------------------------
struct DslExpr::Parser {
    const std::string& s;
    size_t i = 0;
    DslError err;
    explicit Parser(const std::string& str) : s(str) {}
    bool fail(const std::string& msg) {
        err.pos = i;
        err.message = msg;
        return false;
    }
    void skip_ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    }
    bool at_end() {
        skip_ws();
        return i >= s.size();
    }
    std::unique_ptr<DslExpr::Node> parse_expr();    // || 层
    std::unique_ptr<DslExpr::Node> parse_and();     // && 层（优先于 ||）
    std::unique_ptr<DslExpr::Node> parse_term();
    std::unique_ptr<DslExpr::Node> parse_factor();
    std::unique_ptr<DslExpr::Node> parse_cmp();
};

inline std::unique_ptr<DslExpr::Node> DslExpr::Parser::parse_and() {
    auto lhs = parse_term();
    if (!lhs) return nullptr;
    while (true) {
        skip_ws();
        if (s.compare(i, 2, "&&") != 0) return lhs;
        i += 2;
        auto rhs = parse_term();
        if (!rhs) return nullptr;
        auto n = std::make_unique<DslExpr::Node>();
        n->kind = DslExpr::Node::Kind::And;
        n->kids.push_back(std::move(lhs));
        n->kids.push_back(std::move(rhs));
        lhs = std::move(n);
    }
}

inline std::unique_ptr<DslExpr::Node> DslExpr::Parser::parse_expr() {
    auto lhs = parse_and();
    if (!lhs) return nullptr;
    for (;;) {
        skip_ws();
        if (s.compare(i, 2, "||") == 0) {
            i += 2;
            auto rhs = parse_and();
            if (!rhs) return nullptr;
            auto n = std::make_unique<DslExpr::Node>();
            n->kind = DslExpr::Node::Kind::Or;
            n->kids.push_back(std::move(lhs));
            n->kids.push_back(std::move(rhs));
            lhs = std::move(n);
        } else {
            return lhs;
        }
    }
}

inline std::unique_ptr<DslExpr::Node> DslExpr::Parser::parse_term() {
    skip_ws();
    if (i < s.size() && s[i] == '!') {
        ++i;
        auto kid = parse_factor();
        if (!kid) return nullptr;
        auto n = std::make_unique<DslExpr::Node>();
        n->kind = DslExpr::Node::Kind::Not;
        n->kids.push_back(std::move(kid));
        return n;
    }
    return parse_factor();
}

inline std::unique_ptr<DslExpr::Node> DslExpr::Parser::parse_factor() {
    skip_ws();
    if (i < s.size() && s[i] == '(') {
        ++i;
        auto e = parse_expr();
        if (!e) return nullptr;
        skip_ws();
        if (i >= s.size() || s[i] != ')') {
            fail("缺右括号");
            return nullptr;
        }
        ++i;
        return e;
    }
    return parse_cmp();
}

inline std::unique_ptr<DslExpr::Node> DslExpr::Parser::parse_cmp() {
    skip_ws();
    const size_t field_start = i;
    while (i < s.size() && (isalnum(static_cast<unsigned char>(s[i])) || s[i] == '.' ||
                            s[i] == '_'))
        ++i;
    if (i == field_start) {
        fail("缺字段名");
        return nullptr;
    }
    auto n = std::make_unique<DslExpr::Node>();
    n->field = s.substr(field_start, i - field_start);
    skip_ws();
    int op = -1;
    if (s.compare(i, 2, "==") == 0) { op = 0; i += 2; }
    else if (s.compare(i, 2, "!=") == 0) { op = 1; i += 2; }
    else if (s.compare(i, 1, ">") == 0) { op = 2; i += 1; }
    else if (s.compare(i, 1, "<") == 0) { op = 3; i += 1; }
    else if (s.compare(i, 1, "~") == 0) { op = 4; i += 1; }
    if (op < 0) {
        fail("缺比较运算符（== != > < ~）");
        return nullptr;
    }
    n->op = op;
    skip_ws();
    const size_t val_start = i;
    while (i < s.size() && s[i] != ')' && s[i] != '(' && s[i] != '&' && s[i] != '|' &&
           !isspace(static_cast<unsigned char>(s[i])))
        ++i;
    if (i == val_start) {
        fail("缺比较值");
        return nullptr;
    }
    n->value = s.substr(val_start, i - val_start);
    return n;
}

inline bool DslExpr::parse(const std::string& text, DslExpr& out, DslError& err) {
    auto log = ustlog::logger("trace.dsl");
    Parser p(text);
    auto root = p.parse_expr();
    if (!root || !p.at_end()) {
        err = p.err.pos ? p.err : DslError{p.i, "表达式不完整"};
        log->debug("解析失败 @{}: {}", err.pos, err.message);
        return false;
    }
    out.m_text = text;
    out.m_root = std::move(root);
    return true;
}

inline bool DslExpr::eval(const Node& n, const TraceRow& r) {
    switch (n.kind) {
    case Node::Kind::And:
        return eval(*n.kids[0], r) && eval(*n.kids[1], r);
    case Node::Kind::Or:
        return eval(*n.kids[0], r) || eval(*n.kids[1], r);
    case Node::Kind::Not:
        return !eval(*n.kids[0], r);
    default:
        break;
    }
    // Cmp：字段取值
    std::string lhs;
    if (n.field == "dir") lhs = r.host_to_dev ? "out" : "in";
    else if (n.field == "src") lhs = r.source;
    else if (n.field == "summary") lhs = r.summary;
    else if (n.field == "mark") lhs = std::to_string(r.mark);
    else if (n.field == "len") lhs = std::to_string(r.hex.size() / 2);
    else if (n.field == "ep") {   // 从 summary 提取 EPn
        const auto p = r.summary.find("EP");
        if (p == std::string::npos) lhs = "";
        else lhs = r.summary.substr(p + 2, r.summary.find_first_not_of("0123456789", p + 2) - (p + 2));
    } else {
        lhs = "";   // 未知字段=空串（宽松：== 不中，!= 中）
    }
    if (n.op == 4) return lhs.find(n.value) != std::string::npos;
    if (n.op == 0) return lhs == n.value;
    if (n.op == 1) return lhs != n.value;
    // 数值比较（非数字按 0 处理——教学口径）
    const double a = atof(lhs.c_str()), b = atof(n.value.c_str());
    return n.op == 2 ? a > b : a < b;
}

inline bool DslExpr::matches(const TraceRow& r) const {
    return m_root ? eval(*m_root, r) : true;   // 空表达式=全通过
}

} // namespace usts::shell::trace
