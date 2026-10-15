#pragma once

// KVMem LRFU 跨轮 keep-score（诚实移植上游 lrfu.hpp 的 LrfuHistory，修 P0-3/P0-4）。
//
// 【上游是什么】`~/projects/kvmem-llamacpp/kvmem/include/kvmem/lrfu.hpp` 的 LrfuHistory：
//   begin_round() → preview(scores) → commit() → score(block)，λ=0.5、top_fraction=0.9。
//   语义：preview 把 committed 历史按 λ 衰减、只累加本轮 top 分数（≥阈值）的块，得到
//   candidate；commit 把 candidate 变成新的 committed；score 读 candidate 优先、否则
//   committed，缺省 0。Repeated probes derive from committed, never another probe。
//
// 【为什么不能直接用上游】上游的 round 由调用方显式 begin_round/commit 驱动，而本地的
//   "轮"是 prefill chunk（finish()），且有两个上游没有的坑（审查报告 P0-3/P0-4）：
//     P0-3 同轮第二次评分把 attn_score 停留在一次性 cosine —— 修法（D1）：KS 独立字段，
//           selector 只在 LRFU 模式读它；set_retrieval_scores 信号永不进 KS。
//     P0-4 轮次推导（epoch|rows）不可靠：丢弃的 preview 被计一轮、未变化的轮被丢更新 ——
//           修法（D3）：轮次只在提交时推进；finish() 内先 commit 上轮、再 preview 本轮；
//           没打分的 chunk 直接丢弃（discard）、永不 commit。
//
// 【本地接缝】
//   * KvMemScoreProbe 加 ks / ks_candidate / ks_round（D1，独立字段，绝不复用 score_host）。
//   * finish() 内：先 commit 上轮（若有 candidate），再用本轮 D2H 分数 preview；
//     decode 不推进（begin 不碰 ks）；本 chunk 没打分 ⇒ 丢弃、不 commit（D3）。
//   * select_retrieval_pages 内容路：LRFU 开时按 lrfu_score 重排 kept（kept 仍由当轮
//     kvmem_select_blocks 产生，ks 绝不写回 set_retrieval_scores 信号 —— D1 的另一半）。
//
// 【开关】NINFER_TERNARY_KVMEM_LRFU=1（默认关；env_flag 读法）
//        NINFER_TERNARY_KVMEM_LRFU_LAMBDA（默认 0.5，env 实数读法，范围 [0,1]）
//        NINFER_TERNARY_KVMEM_LRFU_TOP_FRAC（默认 0.9，范围 (0,1]）
//   三个开关的唯一读取点都在这个头文件里（与 kvmem_score.h / kvmem_shadow.h 同一条纪律）。
//
// 【数值】float 存（score_host 就是 float）；λ/阈值 double 算。阈值口径照抄上游：
//   rank = min(n-1, (1-frac)*n)，nth_element 取第 rank 小，score >= threshold 才累加。
//   frac=1 ⇒ threshold=-inf ⇒ 全累加（与上游 `fraction_ < 1` 守卫同形）。
//   非负下限 max(0,·)：本地分数是 softmax 质量恒 ≥0，上游 cosine 可为负（P0-6）；
//   这里不断言符号，只保证 KS 永不因"没打分"变负 —— 没打分的块直接保持旧 KS。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace ninfer::ops::detail {

// ---- 开关（唯一读取点）-----------------------------------------------------------
inline bool kvmem_lrfu_enabled() noexcept {
    const char* v = std::getenv("NINFER_TERNARY_KVMEM_LRFU");
    if (v == nullptr || v[0] == '\0') { return false; }
    return std::strtol(v, nullptr, 10) != 0;
}

// 实数旋钮：未设/空/非法 ⇒ fallback。范围：lambda [0,1]（0=纯 recency，上游允许），
// top_frac (0,1]（0 无意义，上游拒绝）。非法值（NaN、越界、尾随垃圾）一律回落 fallback
// 并只喊一次 —— 静默回落比崩进程对，但必须留一行证据（B25 教训：负控必须可观测）。
inline double kvmem_lrfu_env_real(const char* name, double fallback, bool allow_zero) noexcept {
    const char* v = std::getenv(name);
    if (v == nullptr || v[0] == '\0') { return fallback; }
    char* end = nullptr;
    const double parsed = std::strtod(v, &end);
    if (end == v || *end != '\0' || !std::isfinite(parsed) || parsed > 1.0 ||
        (allow_zero ? parsed < 0.0 : parsed <= 0.0)) {
        static bool logged = false;  // 所有旋钮共享：喊一次就够（值本身打出来了）
        if (!logged) {
            logged = true;
            std::fprintf(stderr, "kvmem_lrfu: invalid %s=%s, falling back to %.4f\n", name, v,
                         fallback);
        }
        return fallback;
    }
    return parsed;
}

inline double kvmem_lrfu_lambda() noexcept {
    return kvmem_lrfu_env_real("NINFER_TERNARY_KVMEM_LRFU_LAMBDA", 0.5, true);
}

inline double kvmem_lrfu_top_frac() noexcept {
    return kvmem_lrfu_env_real("NINFER_TERNARY_KVMEM_LRFU_TOP_FRAC", 0.9, false);
}

// ---- LrfuHistory 的诚实移植（float 存、double 算，与上游同语义）------------------
//
//   上游状态：state_（committed scores + round + committed flag）+ candidate_。
//   本地状态：ks（committed）+ ks_candidate（preview）+ ks_round + ks_has_candidate。
//   对应关系：
//     begin_round()      ⇔  新的一轮开始（本地：finish 内 commit 上轮后自动开新轮，无显式调用）
//     preview(scores)    ⇔  kvmem_lrfu_preview（从 committed 派生 candidate，绝不链式派生）
//     commit()           ⇔  kvmem_lrfu_commit（candidate → committed，返回是否真提交了）
//     score(block)       ⇔  kvmem_lrfu_score（candidate 优先，否则 committed，缺省 0）
//     discard_candidate  ⇔  kvmem_lrfu_discard（没打分的 chunk 直接丢弃，D3）
//
//   与上游的刻意差异（有据，非擅改）：
//   1. 无 round 计数器：本地的"轮"就是 finish() 调用本身（commit 上轮 + preview 本轮即一轮），
//      不需要 begin_round 的显式轮号 —— 这正是 P0-4 修掉的"推导轮次"（D3：轮次只在提交时推进）。
//   2. preview 空输入 ⇒ 丢弃 candidate（与上游 `scores.empty() → reset` 同形）。
//   3. 非法分数（NaN/Inf）⇒ 丢弃整轮 preview（上游 throw；本地 noexcept 路径不能抛，丢弃是最
//      接近"不污染 KS"的行为，且留一行证据）。
//   4. preview 只在 committed 长度内衰减：第 i 块的旧 KS 取 i < ks.size() ? ks[i] : 0
//      （与上游 `i < state_.scores->size() ? ... : 0` 同形 —— 历史比本轮短时新块从 0 起）。
struct KvMemLrfuState {
    std::vector<float> ks;            // committed（D1：独立字段，绝不复用 score_host）
    std::vector<float> ks_candidate;  // preview（本轮候选，未 commit 前不影响 score）
    std::uint64_t      ks_round = 0;  // 已提交的轮数（D3：只在 commit 成功时 +1）
    bool has_candidate = false;
};

// preview：从 committed 派生 candidate。重复 preview 覆盖（上游语义：candidate_ = move(next)）。
// lengths: scores 长度即本轮块数；ks 按需增长（新块从 0 起，与上游同形）。
inline void kvmem_lrfu_preview(KvMemLrfuState& st, const float* scores, std::size_t n, double lambda,
                               double top_frac) noexcept {
    if (scores == nullptr || n == 0) {
        st.ks_candidate.clear();
        st.has_candidate = false;
        return;
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(static_cast<double>(scores[i]))) {
            std::fprintf(stderr, "kvmem_lrfu: nonfinite score at block %zu, discarding preview\n", i);
            st.ks_candidate.clear();
            st.has_candidate = false;
            return;
        }
    }
    // 阈值：top_frac < 1 时取第 rank 小（rank = min(n-1, (1-frac)*n)）；frac=1 ⇒ -inf ⇒ 全累加。
    double threshold = -INFINITY;
    if (top_frac < 1.0 && n > 0) {
        std::vector<float> ranked(scores, scores + n);
        const std::size_t rank =
            std::min(n - 1, static_cast<std::size_t>((1.0 - top_frac) * static_cast<double>(n)));
        std::nth_element(ranked.begin(), ranked.begin() + static_cast<std::ptrdiff_t>(rank),
                         ranked.end());
        threshold = static_cast<double>(ranked[rank]);
    }
    if (st.ks.size() < n) { st.ks.resize(n, 0.0f); }
    st.ks_candidate.assign(n, 0.0f);
    for (std::size_t i = 0; i < n; ++i) {
        const double aged = lambda * static_cast<double>(st.ks[i]);
        const double hit  = (static_cast<double>(scores[i]) >= threshold)
                                ? static_cast<double>(scores[i])
                                : 0.0;
        st.ks_candidate[i] = static_cast<float>(aged + hit);
    }
    st.has_candidate = true;
}

// commit：candidate → committed。无 candidate ⇒ 返回 false（上游同形：committed 或 !candidate
// 时返回 false）。成功时 round +1（D3：轮次只在提交时推进）。
inline bool kvmem_lrfu_commit(KvMemLrfuState& st) noexcept {
    if (!st.has_candidate || st.ks_candidate.empty()) { return false; }
    st.ks = st.ks_candidate;
    st.ks_candidate.clear();
    st.has_candidate = false;
    ++st.ks_round;
    return true;
}

// discard：丢弃 preview，committed 不动（D3：没打分的 chunk 直接丢弃，永不 commit）。
inline void kvmem_lrfu_discard(KvMemLrfuState& st) noexcept {
    st.ks_candidate.clear();
    st.has_candidate = false;
}

// score：candidate 优先，否则 committed，缺省 0（上游 score() 同形）。
inline float kvmem_lrfu_score(const KvMemLrfuState& st, std::size_t block) noexcept {
    if (st.has_candidate && !st.ks_candidate.empty()) {
        return block < st.ks_candidate.size() ? st.ks_candidate[block] : 0.0f;
    }
    return block < st.ks.size() ? st.ks[block] : 0.0f;
}

}  // namespace ninfer::ops::detail
