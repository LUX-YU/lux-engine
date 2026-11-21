#pragma once

#include <lux/engine/render/extensions/device/SubmissionFrontiers.hpp>
#include <exception>
#include <span>
#include <utility>
#include <vector>

namespace lux::render::detail
{
    class TransformSlotRetirement final
    {
    public:
        void release(std::uint32_t slot) noexcept { released_.push_back(slot); }

        [[nodiscard]] bool empty() const noexcept { return released_.empty() && count_ == 0; }

        void collect(
            bool round_active,
            std::span<const SubmissionFrontiers> submitted,
            const SubmissionFrontiers& completed,
            std::vector<std::uint32_t>& free
        ) noexcept
        {
            if (count_ && batches_[head_].frontier.reachedBy(completed))
                reclaim(submitted.size(), completed, free);
            // A live round can still accept work referencing the old property stream.
            if (!released_.empty() && !round_active) seal(submitted, completed, free);
        }

    private:
        struct Batch final
        {
            SubmissionFrontiers frontier;
            std::vector<std::uint32_t> slots;
        };

        void reclaim(
            std::size_t frame_count,
            const SubmissionFrontiers& completed,
            std::vector<std::uint32_t>& free
        ) noexcept
        {
            do
            {
                auto& slots = batches_[head_].slots;
                free.insert(free.end(), slots.begin(), slots.end());
                clearSlots(slots);
                head_ = (head_ + 1) % batches_.size();
                --count_;
            } while (count_ && batches_[head_].frontier.reachedBy(completed));
            const bool past_peak = batches_.size() / 2 > frame_count && count_ <= frame_count;
            if (past_peak) resizeBatches(frame_count);
        }

        void seal(
            std::span<const SubmissionFrontiers> submitted,
            const SubmissionFrontiers& completed,
            std::vector<std::uint32_t>& free
        ) noexcept
        {
            if (submitted.empty()) std::terminate();
            frontier_ = submitted.front();
            for (const auto& slot : submitted.subspan(1)) frontier_.merge(slot);
            if (frontier_.reachedBy(completed))
            {
                free.insert(free.end(), released_.begin(), released_.end());
                clearSlots(released_);
                return;
            }

            const bool append = !count_ || batchAt(count_ - 1).frontier != frontier_;
            if (append)
            {
                if (count_ == batches_.size()) resizeBatches(count_ ? count_ * 2 : submitted.size());
                auto& next = batchAt(count_);
                ++count_;
                next.frontier = frontier_;
                if (released_.size() > next.slots.capacity())
                {
                    next.slots.swap(released_);
                    return;
                }
            }
            auto& slots = batchAt(count_ - 1).slots;
            slots.insert(slots.end(), released_.begin(), released_.end());
            clearSlots(released_);
        }

        // Keep at most 4 KiB of idle slot storage per row after a release burst.
        static void clearSlots(std::vector<std::uint32_t>& slots) noexcept
        {
            constexpr std::size_t retained_slots = 1024;
            if (slots.capacity() > retained_slots) std::vector<std::uint32_t>{}.swap(slots);
            else slots.clear();
        }

        [[nodiscard]] Batch& batchAt(std::size_t offset) noexcept
        {
            return batches_[(head_ + offset) % batches_.size()];
        }

        void resizeBatches(std::size_t size) noexcept
        {
            std::vector<Batch> rows(size);
            for (std::size_t i = 0; i < count_; ++i) rows[i] = std::move(batchAt(i));
            batches_.swap(rows);
            head_ = 0;
        }

        std::vector<std::uint32_t> released_;
        SubmissionFrontiers frontier_;
        std::vector<Batch> batches_;
        std::size_t head_ = 0, count_ = 0;
    };
} // namespace lux::render::detail
