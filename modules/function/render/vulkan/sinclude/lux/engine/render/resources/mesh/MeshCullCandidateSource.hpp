#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lux::render
{
    /// Shared CPU/GPU layout written by the coarse candidate producer.  The
    /// indirect command starts at dispatch_group_count_x and therefore remains
    /// directly consumable by vkCmdDispatchIndirect.
    struct alignas(16) CandidateDispatchState final
    {
        std::uint32_t requested{0u};
        std::uint32_t accepted{0u};
        std::uint32_t overflow{0u};
        std::uint32_t reserved{0u};
        std::uint32_t dispatch_group_count_x{0u};
        std::uint32_t dispatch_group_count_y{0u};
        std::uint32_t dispatch_group_count_z{0u};
        std::uint32_t dispatch_reserved{0u};
    };
    static_assert(sizeof(CandidateDispatchState) == 32u);

    /// Domain-neutral seam between a coarse GPU candidate producer and the
    /// standard mesh cull passes.  The producer owns the RenderGraph resources;
    /// Forward/Deferred consume the canonical candidate array and bounded
    /// capacity without learning which domain (world clusters today, something
    /// else tomorrow) produced them. The dispatch buffer remains producer-owned
    /// telemetry rather than a draw-correctness dependency.
    class MeshCullCandidateSource final
    {
    public:
        static constexpr std::string_view kCandidateSlotsResource = "MeshCullCandidateSlots";
        static constexpr std::string_view kDispatchArgsResource = "MeshCullCandidateDispatchArgs";
        // The graph resources above may be produced by a contribution whose
        // passes are declared after their consumers.  Consumers depend on this
        // domain-neutral pass contract instead of naming RenderCluster.
        static constexpr std::string_view kProducerPass = "RenderClusterCandidateFinalize";
        static constexpr std::uint32_t kDispatchArgsOffset = offsetof(CandidateDispatchState, dispatch_group_count_x);

        void publish(std::uint32_t capacity) noexcept
        {
            capacity_ = capacity;
            active_ = capacity != 0u;
        }

        void clear() noexcept
        {
            active_ = false;
            capacity_ = 0u;
        }

        [[nodiscard]] bool active() const noexcept
        {
            return active_;
        }
        [[nodiscard]] std::uint32_t capacity() const noexcept
        {
            return capacity_;
        }

    private:
        std::uint32_t capacity_{0u};
        bool active_{false};
    };
} // namespace lux::render
