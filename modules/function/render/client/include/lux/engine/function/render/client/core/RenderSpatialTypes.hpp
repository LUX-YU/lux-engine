#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace lux::render
{
    struct RenderLargePosition3D final
    {
        std::int32_t page_delta[3]{};
        float local[3]{};
    };
    static_assert(std::is_trivially_copyable_v<RenderLargePosition3D>);
    static_assert(sizeof(RenderLargePosition3D) == 24u);

    /// GPU-compatible 64-byte spatial transform. Each of the first three
    /// vec4-sized columns stores basis.xyz and one local-translation component.
    struct alignas(16) RenderSpatialTransform3D final
    {
        float basis_local[12]{};
        std::int32_t page_delta[3]{};
        std::uint32_t flags{0u};
    };
    static_assert(std::is_trivially_copyable_v<RenderSpatialTransform3D>);
    static_assert(sizeof(RenderSpatialTransform3D) == 64u);

    [[nodiscard]] inline bool
    canRebaseRenderPageDelta(const std::int32_t page_delta[3], const std::int64_t origin_delta[3]) noexcept
    {
        for (std::size_t axis = 0u; axis < 3u; ++axis)
        {
            const auto rebased = static_cast<std::int64_t>(page_delta[axis]) - origin_delta[axis];
            if (rebased < std::numeric_limits<std::int32_t>::min() ||
                rebased > std::numeric_limits<std::int32_t>::max())
            {
                return false;
            }
        }
        return true;
    }

    inline void rebaseRenderPageDelta(std::int32_t page_delta[3], const std::int64_t origin_delta[3]) noexcept
    {
        for (std::size_t axis = 0u; axis < 3u; ++axis)
        {
            page_delta[axis] =
                static_cast<std::int32_t>(static_cast<std::int64_t>(page_delta[axis]) - origin_delta[axis]);
        }
    }

    [[nodiscard]] inline bool
    canRebaseRenderPageDelta2D(const std::int32_t page_delta[2], const std::int64_t origin_delta[3]) noexcept
    {
        for (std::size_t axis = 0u; axis < 2u; ++axis)
        {
            const auto rebased = static_cast<std::int64_t>(page_delta[axis]) - origin_delta[axis];
            if (rebased < std::numeric_limits<std::int32_t>::min() ||
                rebased > std::numeric_limits<std::int32_t>::max())
            {
                return false;
            }
        }
        return true;
    }

    inline void rebaseRenderPageDelta2D(std::int32_t page_delta[2], const std::int64_t origin_delta[3]) noexcept
    {
        for (std::size_t axis = 0u; axis < 2u; ++axis)
        {
            page_delta[axis] =
                static_cast<std::int32_t>(static_cast<std::int64_t>(page_delta[axis]) - origin_delta[axis]);
        }
    }

    /// Explicit-transient-world helper. The matrix is local to page zero;
    /// it is never interpreted as an absolute large-world matrix.
    [[nodiscard]] inline RenderSpatialTransform3D
    makeTransientRenderSpatialTransform3D(const float local_matrix[16]) noexcept
    {
        RenderSpatialTransform3D result{};
        for (std::size_t column = 0; column != 3u; ++column)
        {
            result.basis_local[column * 4u + 0u] = local_matrix[column * 4u + 0u];
            result.basis_local[column * 4u + 1u] = local_matrix[column * 4u + 1u];
            result.basis_local[column * 4u + 2u] = local_matrix[column * 4u + 2u];
            result.basis_local[column * 4u + 3u] = local_matrix[12u + column];
        }
        return result;
    }
} // namespace lux::render
