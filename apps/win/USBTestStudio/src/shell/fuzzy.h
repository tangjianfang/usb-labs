// fuzzy.h — 子序列模糊匹配 + 打分（设计 §8 命令面板 / 计划 MS0-T5）
//
// 算法（UTF-16 逐码元，大小写不敏感）：
//   命中 +10；连续命中（与上一命中相邻）额外 +5；词首命中（前驱非字母数字）额外 +8；
//   首码元命中额外 +6；needle 为空 = matched 且 score 0（列全量）。
//   不满足子序列 = 不匹配。打分只用于排序展示，无业务语义。
#pragma once

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace usts::shell {

struct FuzzyResult {
    bool matched = false;
    int score = 0;
    std::vector<int> hit;   // haystack 命中下标（按 needle 序）
};

inline bool _is_word_char(wchar_t c) { return std::iswalnum(c) != 0; }

inline FuzzyResult fuzzy_match(const std::wstring& needle, const std::wstring& haystack) {
    FuzzyResult r;
    if (needle.empty()) { r.matched = true; return r; }
    size_t hi = 0;
    int prev_hit = -2;   // 连续判定：上一命中下标
    for (size_t ni = 0; ni < needle.size(); ++ni) {
        const wchar_t nc = std::towlower(needle[ni]);
        bool found = false;
        for (; hi < haystack.size(); ++hi) {
            if (std::towlower(haystack[hi]) == nc) {
                r.score += 10;
                if (static_cast<int>(hi) == prev_hit + 1) r.score += 5;      // 连续
                if (hi == 0 || !_is_word_char(haystack[hi - 1])) r.score += 8; // 词首
                if (hi == 0 && ni == 0) r.score += 6;                        // 首字
                r.hit.push_back(static_cast<int>(hi));
                prev_hit = static_cast<int>(hi);
                ++hi;   // 子序列：下一次从其后找
                found = true;
                break;
            }
        }
        if (!found) return r;   // matched=false
    }
    r.matched = true;
    return r;
}

// 按分排序（降序，同分保池序=稳定），剔除不中/零分（零分仅空查询出现，此时全保留）
template <typename T, typename TitleFn>
std::vector<std::pair<T, int>> fuzzy_rank_impl(const std::wstring& q,
                                               const std::vector<T>& pool,
                                               TitleFn title_of) {
    std::vector<std::pair<T, int>> out;
    for (const auto& item : pool) {
        const FuzzyResult r = fuzzy_match(q, title_of(item));
        if (r.matched) out.emplace_back(item, r.score);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    return out;
}

} // namespace usts::shell
