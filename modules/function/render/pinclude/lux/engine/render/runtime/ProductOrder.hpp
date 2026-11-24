#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lux::render::runtime_detail
{
    // Device-local Job ordinals. Callers enumerate one reader at a time in installed order, so
    // each writer sees a stable list regardless of draw order or the number of bindings per product.
    class ProductOrder final
    {
    public:
        void begin(std::size_t count) noexcept
        {
            changed_ = jobs_.size() != count;
            jobs_.resize(count);
            for (auto& job : jobs_) job.used = 0;
        }

        void addRead(std::uint32_t writer, std::uint32_t reader) noexcept
        {
            auto& job = jobs_[writer];
            if (job.used && job.readers[job.used - 1] == reader) return;
            if (job.used == job.readers.size())
            {
                job.readers.push_back(reader);
                changed_ = true;
            }
            else if (job.readers[job.used] != reader)
            {
                job.readers[job.used] = reader;
                changed_ = true;
            }
            ++job.used;
        }

        [[nodiscard]] bool finish() noexcept
        {
            for (auto& job : jobs_)
            {
                if (job.used == job.readers.size()) continue;
                job.readers.resize(job.used);
                changed_ = true;
            }
            if (changed_)
            {
                ordered_.clear();
                ordered_.reserve(jobs_.size());
                for (auto& job : jobs_) job.predecessors = 0;
                for (const auto& job : jobs_)
                    for (auto reader : job.readers) ++jobs_[reader].predecessors;
                for (std::uint32_t i = 0; i < jobs_.size(); ++i)
                    if (!jobs_[i].predecessors) ordered_.push_back(i);
                for (std::size_t i = 0; i < ordered_.size(); ++i)
                    for (auto reader : jobs_[ordered_[i]].readers)
                        if (--jobs_[reader].predecessors == 0) ordered_.push_back(reader);
            }
            return ordered_.size() == jobs_.size();
        }

        [[nodiscard]] std::span<const std::uint32_t> ordered() const & noexcept { return ordered_; }
        std::span<const std::uint32_t> ordered() const && = delete;
        [[nodiscard]] bool blocked(std::uint32_t job) const noexcept { return jobs_[job].predecessors != 0; }

    private:
        struct Job final
        {
            std::vector<std::uint32_t> readers;
            std::uint32_t used = 0, predecessors = 0;
        };
        std::vector<Job> jobs_;
        std::vector<std::uint32_t> ordered_;
        bool changed_ = false;
    };
}
