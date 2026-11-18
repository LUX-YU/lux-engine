#pragma once

#include <lux/engine/render/capabilities/Lighting3dCapability.hpp>

#include <array>
#include <lux/engine/render/capabilities/MeshShadows.hpp>
#include <lux/engine/render/capabilities/ClusterGrid.hpp>
#include <lux/engine/render/capabilities/ColorEncoding.hpp>
#include <lux/engine/render/capabilities/CapabilityComposition.hpp>

namespace lux::render
{
    inline constexpr std::string_view kDeferredLightingCapability = cxx::make_basic_type_info<Lighting3dCapability>().name();
    inline constexpr std::string_view kDeferredLightingConfigurationSchema = "lux.render.config.lighting3d";

    struct DeferredLightingConfiguration final
    {
        std::array<float, 4> environment{};
        bool shadows = true;
        bool clustered = true;
        MeshShadowsConfiguration shadow;
        ClusterGrid clusters;
        EColorEncoding encoding = EColorEncoding::SRGB;
        bool operator==(const DeferredLightingConfiguration&) const noexcept = default;
        [[nodiscard]] LUX_RENDER_PUBLIC bool valid() const noexcept;
    };

    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<SerializedRenderConfiguration>
    encodeDeferredLightingConfiguration(const DeferredLightingConfiguration&) noexcept;
    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<
        DeferredLightingConfiguration> decodeDeferredLightingConfiguration(std::span<const std::byte>) noexcept;
} // namespace lux::render
