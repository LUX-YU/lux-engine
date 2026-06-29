#pragma once
// ============================================================================
//  FuzzySearchIndex — high-performance fuzzy (subsequence) search over a large
//  set of names, for PER-KEYSTROKE interactive filtering of massive resource
//  collections (assets, node palettes, command + reflection lists, ...).
//
//  Design — NOT a fancy index (a trie/inverted index can't express "subsequence"
//  fuzziness): a very fast LINEAR SCAN with strong pruning + incremental
//  refinement, the fzf / VS-Code approach:
//    * SoA + ONE contiguous name arena (cache-friendly; no per-name allocation).
//    * Per-item 64-bit CHAR BITMASK pre-filter: a single AND rejects most items
//      (a NECESSARY-not-sufficient subsequence condition -> zero false negatives).
//    * fzf-style subsequence scoring (start / word-boundary / camelCase /
//      consecutive bonuses, gap penalty) only on the survivors.
//    * Incremental: a longer query's matches are a SUBSET of the shorter query's
//      matches, so a growing query only re-scans the previous survivor set.
//    * Top-K partial sort when only a few results are visible.
//
//  Dependency-free pure algorithm (std only) — lives in the UI module so editor +
//  any consumer can reuse it (AssetBrowser filter, SearchPopupElement, ...).
// ============================================================================

#include <algorithm>
#include <climits>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lux::ui
{
    class FuzzySearchIndex
    {
    public:
        struct Match
        {
            std::uint32_t index;   ///< caller-side item index (== build() order)
            int           score;   ///< higher = better; 0 for the empty query
        };

        /// (Re)build the corpus from display names. Item i keeps caller index i.
        /// Amortizes ALL preprocessing here (lowercased char-mask + interned name)
        /// so the per-keystroke query() path stays cheap.
        void build(const std::vector<std::string>& names)
        {
            const std::size_t n = names.size();
            mask_.assign(n, 0);
            off_.resize(n);
            len_.resize(n);
            arena_.clear();
            std::size_t total = 0;
            for (const auto& s : names) total += s.size();
            arena_.reserve(total);

            for (std::size_t i = 0; i < n; ++i)
            {
                const std::string& s = names[i];
                const std::uint16_t L =
                    static_cast<std::uint16_t>(std::min<std::size_t>(s.size(), 0xFFFFu));
                off_[i] = static_cast<std::uint32_t>(arena_.size());
                len_[i] = L;
                arena_.append(s.data(), L);            // ORIGINAL case (camelCase + display)
                std::uint64_t m = 0;
                for (std::uint16_t k = 0; k < L; ++k) m |= charBit(toLower(s[k]));
                mask_[i] = m;
            }
            // corpus changed -> drop the incremental/result cache
            prev_query_.clear();
            candidates_.clear();
            have_results_ = false;
        }

        std::size_t size() const noexcept { return mask_.size(); }

        /// Query. Empty query -> ALL items in original order (score 0). topK == 0
        /// returns every match fully sorted; topK > 0 returns only the best K.
        /// Re-calling with the SAME query returns the cached result (free).
        const std::vector<Match>& query(std::string_view q, std::uint32_t topK = 0)
        {
            ql_.clear();
            ql_.reserve(q.size());
            for (char c : q) ql_.push_back(static_cast<char>(toLower(c)));

            if (have_results_ && topK == prev_topk_ && ql_ == prev_query_)
                return results_;                       // unchanged since last call

            // A longer query refines the previous survivor set (its matches are a
            // subset). Anything else (shrunk / edited / first call) -> full scan.
            const bool incremental =
                !prev_query_.empty() && ql_.size() > prev_query_.size()
                && ql_.compare(0, prev_query_.size(), prev_query_) == 0;

            results_.clear();

            if (ql_.empty())
            {
                results_.reserve(mask_.size());
                for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(mask_.size()); ++i)
                    results_.push_back({ i, 0 });
                candidates_.clear();
            }
            else
            {
                const std::uint64_t qmask = maskOf(ql_);
                std::vector<std::uint32_t> next;
                auto consider = [&](std::uint32_t i)
                {
                    if ((mask_[i] & qmask) != qmask) return;     // bitmask pre-filter
                    const std::string_view name(arena_.data() + off_[i], len_[i]);
                    const int s = score(ql_, name);
                    if (s == INT_MIN) return;
                    next.push_back(i);
                    results_.push_back({ i, s });
                };

                if (incremental)
                {
                    next.reserve(candidates_.size());
                    for (std::uint32_t i : candidates_) consider(i);
                }
                else
                {
                    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(mask_.size()); ++i)
                        consider(i);
                }
                candidates_ = std::move(next);
                sortResults(topK);
            }

            prev_query_   = ql_;
            prev_topk_    = topK;
            have_results_ = true;
            return results_;
        }

    private:
        static int  toLower(char c) { return (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : static_cast<unsigned char>(c); }
        static bool isSep(char c)   { return c == '_' || c == '-' || c == '.' || c == '/' || c == '\\' || c == ' '; }
        static bool isUpper(char c) { return c >= 'A' && c <= 'Z'; }

        // Distinct-character bucket -> bit. a-z (0..25), 0-9 (26..35), common
        // separators (36), everything else (37). 64 bits is ample headroom.
        static std::uint64_t charBit(int lc)
        {
            if (lc >= 'a' && lc <= 'z') return 1ull << (lc - 'a');
            if (lc >= '0' && lc <= '9') return 1ull << (26 + (lc - '0'));
            if (lc == '_' || lc == '-' || lc == '.' || lc == '/' || lc == '\\' || lc == ' ')
                return 1ull << 36;
            return 1ull << 37;
        }
        static std::uint64_t maskOf(std::string_view lowered)
        {
            std::uint64_t m = 0;
            for (char c : lowered) m |= charBit(static_cast<unsigned char>(c));
            return m;
        }

        // Greedy fzf-style subsequence scorer. `q` is lowercased; `name` keeps
        // original case (so camelCase humps score as word boundaries). Returns
        // INT_MIN when `q` is not a subsequence of `name`.
        static int score(std::string_view q, std::string_view name)
        {
            std::size_t qi = 0;
            int  s = 0;
            bool prev_match = false;
            for (std::size_t ni = 0; ni < name.size() && qi < q.size(); ++ni)
            {
                const char nc = name[ni];
                if (toLower(nc) == q[qi])
                {
                    int bonus = 0;
                    if (ni == 0)                              bonus += 15;   // start of name
                    else if (isSep(name[ni - 1]))            bonus += 12;   // after separator
                    else if (isUpper(nc) && !isUpper(name[ni - 1])) bonus += 10; // camelCase hump
                    if (prev_match)                          bonus += 8;    // consecutive run
                    s += 16 + bonus;
                    prev_match = true;
                    ++qi;
                }
                else
                {
                    s -= 1;                                   // gap penalty
                    prev_match = false;
                }
            }
            if (qi < q.size()) return INT_MIN;                // didn't consume the whole query
            s -= static_cast<int>(name.size()) / 8;           // mild bias toward concise names
            return s;
        }

        void sortResults(std::uint32_t topK)
        {
            const auto cmp = [this](const Match& a, const Match& b)
            {
                if (a.score != b.score)               return a.score > b.score;
                if (len_[a.index] != len_[b.index])   return len_[a.index] < len_[b.index];
                return a.index < b.index;             // stable tie-break
            };
            if (topK && topK < results_.size())
            {
                std::partial_sort(results_.begin(), results_.begin() + topK, results_.end(), cmp);
                results_.resize(topK);
            }
            else
            {
                std::sort(results_.begin(), results_.end(), cmp);
            }
        }

        // SoA corpus (the hot mask_ column is scanned every query).
        std::vector<std::uint64_t> mask_;
        std::vector<std::uint32_t> off_;
        std::vector<std::uint16_t> len_;
        std::string                arena_;

        // Query state (incremental refinement + same-query cache).
        std::string                prev_query_;
        std::string                ql_;
        std::uint32_t              prev_topk_{ 0 };
        bool                       have_results_{ false };
        std::vector<std::uint32_t> candidates_;   // survivors of the previous query
        std::vector<Match>         results_;
    };

} // namespace lux::ui
