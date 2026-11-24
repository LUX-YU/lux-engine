#pragma once

#include <lux/engine/render/resource/ResourceId.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>

namespace lux::render
{
    struct TransformTag;
    using TransformRef = ResourceId<TransformTag>;

    // Shader-visible matrix row. CPU mutations use TransformRef to check the owner and generation.
    struct TransformId final
    {
        std::uint32_t value;
        bool operator==(const TransformId&) const noexcept = default;
    };

    static_assert(sizeof(TransformId) == sizeof(std::uint32_t));
    static_assert(std::is_standard_layout_v<TransformId> && std::is_trivially_copyable_v<TransformId>);

    using TransformMatrix = std::array<float, 16>;
    static_assert(sizeof(TransformMatrix) == 64 && std::is_trivially_copyable_v<TransformMatrix>);
    inline constexpr TransformMatrix kIdentityTransform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    [[nodiscard]] inline bool isFiniteTransform(const TransformMatrix& value) noexcept
    {
        return std::ranges::all_of(value, [](float element) noexcept { return std::isfinite(element); });
    }

    struct TransformRecordUpdate final
    {
        TransformRef transform;
        TransformMatrix matrix = kIdentityTransform;
    };
} // namespace lux::render
