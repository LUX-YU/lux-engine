#pragma once

#include <cmath>
#include <cstdlib>
#include <cstdint>

namespace lux::ecs::detail
{
    enum class EContentUploadReplyDisposition : std::uint8_t
    {
        COMMIT,
        FAIL_DOMAIN,
        COMPENSATE_REMOVE
    };

    enum class EContentPreparationDisposition : std::uint8_t
    {
        COMMIT,
        RETRY_LATEST,
        DISCARD_STALE
    };

    [[nodiscard]] constexpr EContentPreparationDisposition
    classifyContentPreparation(
        bool owner_matches,
        bool request_matches,
        bool desired_generation_matches) noexcept
    {
        if (!owner_matches || !request_matches)
            return EContentPreparationDisposition::DISCARD_STALE;
        return desired_generation_matches
            ? EContentPreparationDisposition::COMMIT
            : EContentPreparationDisposition::RETRY_LATEST;
    }

    class ContentRenderRevisionSequence final
    {
    public:
        [[nodiscard]] std::uint64_t next() noexcept
        {
            ++value_;
            if (value_ == 0u)
                ++value_;
            return value_;
        }

        [[nodiscard]] std::uint64_t current() const noexcept
        {
            return value_;
        }

    private:
        std::uint64_t value_{0u};
    };

    /// Identifies one Entry lifetime rather than one component revision. Entry
    /// storage is erased on retirement, so this sequence must live at the
    /// subsystem owner and may never restart at one for a replacement Entry.
    class ContentRenderOwnerSequence final
    {
    public:
        [[nodiscard]] std::uint64_t next() noexcept
        {
            const auto result = next_;
            if (result == 0u)
                std::abort();
            ++next_;
            return result;
        }

    private:
        std::uint64_t next_{1u};
    };

    [[nodiscard]] inline EContentUploadReplyDisposition
    classifyContentUploadReply(
        bool owner_matches,
        bool request_matches,
        bool dispatch_failed,
        std::uint32_t domain_status) noexcept
    {
        if (!owner_matches || !request_matches)
        {
            return !dispatch_failed && domain_status == 0u
                ? EContentUploadReplyDisposition::COMPENSATE_REMOVE
                : EContentUploadReplyDisposition::FAIL_DOMAIN;
        }
        return !dispatch_failed && domain_status == 0u
            ? EContentUploadReplyDisposition::COMMIT
            : EContentUploadReplyDisposition::FAIL_DOMAIN;
    }

    [[nodiscard]] inline bool validVisualLodContract(
        float geometric_error,
        float enter_error_pixels,
        float exit_error_pixels) noexcept
    {
        return std::isfinite(geometric_error) &&
            std::isfinite(enter_error_pixels) &&
            std::isfinite(exit_error_pixels) &&
            geometric_error >= 0.0f &&
            enter_error_pixels > exit_error_pixels &&
            exit_error_pixels >= 0.0f;
    }

    [[nodiscard]] inline bool validTerrainLodContract(
        std::uint8_t level,
        std::uint8_t child_count,
        float geometric_error,
        float enter_error_pixels,
        float exit_error_pixels) noexcept
    {
        return level <= 4u && child_count <= 16u &&
            (level == 0u) == (child_count == 0u) &&
            validVisualLodContract(
                geometric_error,
                enter_error_pixels,
                exit_error_pixels) && exit_error_pixels > 0.0f;
    }

} // namespace lux::ecs::detail
