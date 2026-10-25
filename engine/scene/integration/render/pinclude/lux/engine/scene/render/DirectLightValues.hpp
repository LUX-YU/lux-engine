#pragma once

#include <Eigen/Geometry>
#include <lux/engine/description/LightDescription.hpp>
#include <lux/engine/render/inputs/DirectLightInputs.hpp>
#include <lux/engine/scene/render/LightRenderResourceComponent.hpp>

namespace lux::scene::detail
{
    [[nodiscard]] LightShadowInputs lightShadowInputs(const rdesc::LightDescription&) noexcept;
    [[nodiscard]] rdesc::LightDescription compiledLightDescription(const LightRenderResourceComponent&) noexcept;

    [[nodiscard]] render::RenderResult<render::VDirectLight>
    lightValue(const rdesc::LightDescription&, const Eigen::Affine3d&) noexcept;
} // namespace lux::scene::detail
