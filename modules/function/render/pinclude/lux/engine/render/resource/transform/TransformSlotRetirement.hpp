#pragma once

#include <lux/engine/render/extensions/device/SubmissionFrontiers.hpp>
#include <deque>
#include <span>
#include <utility>
#include <vector>

namespace lux::render::detail
{
    class TransformSlotRetirement final
    {
    public:
        void release(std::uint32_t slot) noexcept { released_.push_back(slot); }
        [[nodiscard]] bool empty() const noexcept { return released_.empty() && batches_.empty(); }

        void collect(
            bool round_active,
            std::span<const SubmissionFrontiers> submitted,
            const SubmissionFrontiers& completed,
            std::vector<std::uint32_t>& free
        ) noexcept
        {
            while (!batches_.empty() && batches_.front().frontier.reachedBy(completed))
            {
                auto& slots = batches_.front().slots;
                free.insert(free.end(), slots.begin(), slots.end());
                batches_.pop_front();
            }
            // A live round can still accept work referencing the old property stream.
            if (released_.empty() || round_active) return;
            auto frontier = submitted.front();
            for (const auto& slot : submitted.subspan(1))
            {
                frontier.merge(slot);
            }
            if (frontier.reachedBy(completed))
            {
                free.insert(free.end(), released_.begin(), released_.end());
            }
            else if (!batches_.empty() && batches_.back().frontier == frontier)
            {
                auto& slots = batches_.back().slots;
                slots.insert(slots.end(), released_.begin(), released_.end());
            }
            else
            {
                batches_.push_back({std::move(frontier), std::exchange(released_, {})});
            }
            released_.clear();
        }

    private:
        struct Batch final
        {
            SubmissionFrontiers frontier;
            std::vector<std::uint32_t> slots;
        };

        std::vector<std::uint32_t> released_;
        std::deque<Batch> batches_;
    };
} // namespace lux::render::detail
