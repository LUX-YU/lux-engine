#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace lux::render
{
    enum class EGraphInvalidationReason : std::uint32_t
    {
        NONE = 0u,
        FEATURE_TOPOLOGY = 1u << 0u,
        VIEW_EXTENT = 1u << 1u,
        MDC_STORAGE_GENERATION = 1u << 2u,
        MATERIAL_LAYOUT = 1u << 3u,
        SHADER_REVISION = 1u << 4u,
        SWAPCHAIN = 1u << 5u,
        CLASSIC_MESH_SEGMENT_TOPOLOGY = 1u << 6u,
        UNKNOWN = 1u << 7u
    };

    [[nodiscard]] constexpr EGraphInvalidationReason
    operator|(EGraphInvalidationReason left, EGraphInvalidationReason right) noexcept
    {
        return static_cast<EGraphInvalidationReason>(
            static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right)
        );
    }

    inline constexpr std::size_t kGraphInvalidationReasonCount = 8u;

    struct SceneGraphTelemetry final
    {
        std::array<std::uint64_t, kGraphInvalidationReasonCount> invalidation_counts{};
        std::uint32_t pending_invalidation_bits{0u};
        std::uint64_t compile_attempts{0u};
        std::uint64_t compile_successes{0u};
        std::uint64_t compile_failures{0u};
        std::uint64_t build_nanoseconds{0u};
        std::uint64_t compile_nanoseconds{0u};
        std::uint64_t total_nanoseconds{0u};
        std::uint64_t retired_graph_high_water{0u};
        std::uint64_t retired_view_resource_high_water{0u};
    };

    struct SceneGraphCompileSample final
    {
        std::uint64_t frame_serial{0u};
        std::uint32_t invalidation_bits{0u};
        std::uint64_t build_nanoseconds{0u};
        std::uint64_t compile_nanoseconds{0u};
        std::uint64_t total_nanoseconds{0u};
        bool succeeded{false};
    };
}
