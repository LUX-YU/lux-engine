#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <vector>

namespace lux::render::detail
{
    // Domain is the stable DeviceSession address; index names its immutable physical queue table.
    struct SubmissionPoint final
    {
        const void* domain = nullptr;
        std::size_t queue = 0;
        std::uint64_t serial = 0;
    };

    class SubmissionFrontiers final
    {
    public:
        // Unbound zero is only an absent contribution. Bind actual owners during cold construction.
        SubmissionFrontiers() noexcept = default;

        SubmissionFrontiers(const void* domain, std::size_t physical_queue_count) noexcept
            : domain_(domain), serials_(physical_queue_count, 0)
        {
            if (!domain || !physical_queue_count)
            {
                std::terminate();
            }
        }

        [[nodiscard]] bool belongsTo(const void* domain, std::size_t count) const noexcept
        {
            return domain_ == domain && serials_.size() == count;
        }

        [[nodiscard]] bool compatible(const SubmissionFrontiers& other) const noexcept
        {
            return belongsTo(other.domain_, other.size());
        }

        // Accept only a confirmed native result. Fence observation never manufactures acceptance.
        void accept(SubmissionPoint point) noexcept
        {
            const bool is_foreign_queue = point.domain != domain_ || point.queue >= serials_.size();
            if (is_foreign_queue)
            {
                std::terminate();
            }
            serials_[point.queue] = std::max(serials_[point.queue], point.serial);
        }

        void merge(const SubmissionFrontiers& other) noexcept
        {
            if (!other.domain_)
            {
                return;
            }
            if (!compatible(other))
            {
                std::terminate();
            }
            for (std::size_t i = 0; i < size(); ++i)
            {
                serials_[i] = std::max(serials_[i], other.serials_[i]);
            }
        }

        [[nodiscard]] bool reachedBy(const SubmissionFrontiers& completed) const noexcept
        {
            if (!compatible(completed))
            {
                return false;
            }
            for (std::size_t i = 0; i < size(); ++i)
            {
                if (serials_[i] > completed.serials_[i])
                {
                    return false;
                }
            }
            return true;
        }

        void clear() noexcept
        {
            std::ranges::fill(serials_, 0);
        }

        [[nodiscard]] const std::vector<std::uint64_t>& serials() const noexcept
        {
            return serials_;
        }

        [[nodiscard]] std::size_t size() const noexcept
        {
            return serials_.size();
        }

        bool operator==(const SubmissionFrontiers&) const noexcept = default;

    private:
        const void* domain_ = nullptr;
        std::vector<std::uint64_t> serials_;
    };
} // namespace lux::render::detail
