#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <type_traits>
#include <lux/engine/render/core/Error.hpp>

namespace lux::render
{
    inline constexpr std::array<float, 16> kIdentityCameraMatrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    // Normalized Target coordinates; unchanged fractions remain meaningful after Target resize.
    struct CameraViewport final
    {
        float x = 0, y = 0, width = 1, height = 1, min_depth = 0, max_depth = 1;
        [[nodiscard]] RenderResult<void> validate() const noexcept;
        bool operator==(const CameraViewport&) const noexcept = default;
    };

    // Column-major matrices, Vulkan clip depth [0,1]. The View owns this value, never an Effect binding.
    struct CameraState final
    {
        std::array<float, 16> view = kIdentityCameraMatrix;
        std::array<float, 16> projection = kIdentityCameraMatrix;
        CameraViewport viewport;
        [[nodiscard]] RenderResult<void> validate() const noexcept;
        bool operator==(const CameraState&) const noexcept = default;
    };

    // Explicit in-process parameter block, not a Wire encoding of the entire C++ object.
    static_assert(std::is_standard_layout_v<CameraState> && offsetof(CameraState, projection) == 64 &&
        offsetof(CameraState, viewport) == 128);
    [[nodiscard]] inline std::span<const std::byte> cameraParameters(const CameraState& camera) noexcept
    {
        return std::as_bytes(std::span(&camera, 1)).first(128);
    }
}
