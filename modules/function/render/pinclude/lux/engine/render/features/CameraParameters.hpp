#pragma once

#include <lux/engine/description/ShaderBlock.hpp>
#include <lux/engine/render/inputs/CameraInputs.hpp>

namespace lux::render::detail
{
    // Native encoding of one semantic CameraInput. Shared fields are stored once;
    // professional shaders map only the fields they consume. No light/material state.
    struct CameraParameters final
    {
        TransformMatrix view, projection, inverse_view_projection;
        std::array<float, 4> position_near, direction_far, viewport, background;
        float exposure;
        std::array<float, 3> padding{};
    };

    [[nodiscard]] rdesc::ShaderBlock cameraParameterLayout() noexcept;
    [[nodiscard]] RenderResult<void>
    projectCameraParameters(const CameraInput& input, std::span<std::byte> destination) noexcept;

    [[nodiscard]] RenderResult<void>
    projectCameraInput(cxx::type_info type, std::span<const std::byte> source, std::span<std::byte> destination) noexcept;
} // namespace lux::render::detail
