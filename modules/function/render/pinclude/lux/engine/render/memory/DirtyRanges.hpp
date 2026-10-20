#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <span>
#include <vector>

namespace lux::render::detail
{
    struct DirtyWordMask final
    {
        std::uint32_t word_index;
        std::uint64_t bits;
    };

    // Logical-record bits only. GPU acceptance acknowledges the exact selected masks;
    // zero-word removal never scans the remaining dirty records.
    class DirtyRanges final
    {
    public:
        void grow(std::uint64_t records) noexcept
        {
            const auto size = static_cast<std::size_t>((records + 63) / 64);
            if (size > words_.size())
            {
                words_.resize(size);
                word_positions_.resize(size, UINT32_MAX);
                nonzero_words_.reserve(size);
            }
        }

        [[nodiscard]] bool empty() const noexcept
        {
            return nonzero_words_.empty();
        }

        [[nodiscard]] std::size_t wordCount() const noexcept
        {
            return nonzero_words_.size();
        }

        [[nodiscard]] std::uint64_t recordCount() const noexcept
        {
            return records_;
        }

        void clear() noexcept
        {
            for (auto word : nonzero_words_)
            {
                words_[word] = 0;
                word_positions_[word] = UINT32_MAX;
            }
            nonzero_words_.clear();
            records_ = 0;
        }

        void mark(std::uint32_t record) noexcept
        {
            mark({record / 64, std::uint64_t{1} << (record % 64)});
        }

        void mark(DirtyWordMask mask) noexcept
        {
            auto& word = words_[mask.word_index];
            records_ += std::popcount(mask.bits & ~word);
            if (mask.bits && !word)
            {
                word_positions_[mask.word_index] = static_cast<std::uint32_t>(nonzero_words_.size());
                nonzero_words_.push_back(mask.word_index);
            }
            word |= mask.bits;
        }

        void markRange(std::uint64_t first, std::uint64_t count) noexcept
        {
            while (count)
            {
                const auto offset = static_cast<unsigned>(first % 64);
                const auto length = static_cast<unsigned>(std::min<std::uint64_t>(count, 64 - offset));
                const auto bits = length == 64 ? UINT64_MAX : ((std::uint64_t{1} << length) - 1) << offset;
                mark({static_cast<std::uint32_t>(first / 64), bits});
                first += length;
                count -= length;
            }
        }

        void select(std::size_t limit, std::vector<DirtyWordMask>& output) const noexcept
        {
            output.clear();
            const auto count = std::min(limit, nonzero_words_.size());
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto word = nonzero_words_[nonzero_words_.size() - 1 - i];
                output.push_back({word, words_[word]});
            }
            std::ranges::sort(output, {}, &DirtyWordMask::word_index);
        }

        // Selected words are sorted. Coalesce across word boundaries, then split only at
        // the caller's physical page boundary. No live-record scan or range allocation.
        template <class Visit>
        static bool visitRanges(std::span<const DirtyWordMask> masks, std::uint32_t page_records, Visit visit) noexcept
        {
            std::uint64_t first = 0, count = 0;
            const auto flush = [&]() noexcept
            {
                while (count)
                {
                    const auto length =
                        static_cast<std::uint32_t>(std::min<std::uint64_t>(count, page_records - first % page_records));
                    if (!visit(static_cast<std::uint32_t>(first), length))
                    {
                        return false;
                    }
                    first += length;
                    count -= length;
                }
                return true;
            };
            for (auto mask : masks)
            {
                auto bits = mask.bits;
                while (bits)
                {
                    const auto offset = static_cast<unsigned>(std::countr_zero(bits));
                    const auto length = static_cast<unsigned>(std::countr_one(bits >> offset));
                    const auto next = std::uint64_t(mask.word_index) * 64 + offset;
                    if (count && first + count != next && !flush())
                    {
                        return false;
                    }
                    if (count == 0)
                    {
                        first = next;
                    }
                    count += length;
                    const auto selected = length == 64 ? UINT64_MAX : ((std::uint64_t{1} << length) - 1) << offset;
                    bits &= ~selected;
                }
            }
            return flush();
        }

        static void appendRange(std::vector<DirtyWordMask>& output, std::uint32_t first, std::uint32_t count) noexcept
        {
            while (count)
            {
                const auto offset = first % 64;
                const auto length = std::min(count, 64 - offset);
                const auto bits = length == 64 ? UINT64_MAX : ((std::uint64_t{1} << length) - 1) << offset;
                const auto word = first / 64;
                if (!output.empty() && output.back().word_index == word)
                {
                    output.back().bits |= bits;
                }
                else
                {
                    output.push_back({word, bits});
                }
                first += length;
                count -= length;
            }
        }

        void acknowledge(DirtyWordMask mask) noexcept
        {
            auto& word = words_[mask.word_index];
            if (!word)
            {
                return;
            }
            records_ -= std::popcount(word & mask.bits);
            word &= ~mask.bits;
            if (!word)
            {
                const auto position = word_positions_[mask.word_index];
                const auto last = nonzero_words_.back();
                nonzero_words_[position] = last;
                word_positions_[last] = position;
                nonzero_words_.pop_back();
                word_positions_[mask.word_index] = UINT32_MAX;
            }
        }

        void acknowledge(std::span<const DirtyWordMask> masks) noexcept
        {
            for (auto mask : masks)
            {
                acknowledge(mask);
            }
        }

        void acknowledgeRange(std::uint64_t first, std::uint64_t count) noexcept
        {
            while (count)
            {
                const auto offset = static_cast<unsigned>(first % 64);
                const auto length = static_cast<unsigned>(std::min<std::uint64_t>(count, 64 - offset));
                const auto bits = length == 64 ? UINT64_MAX : ((std::uint64_t{1} << length) - 1) << offset;
                acknowledge({static_cast<std::uint32_t>(first / 64), bits});
                first += length;
                count -= length;
            }
        }

    private:
        std::vector<std::uint64_t> words_;
        std::vector<std::uint32_t> nonzero_words_;
        std::vector<std::uint32_t> word_positions_;
        std::uint64_t records_ = 0;
    };
} // namespace lux::render::detail
