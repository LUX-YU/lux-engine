#pragma once

#include <lux/engine/render/capabilities/Mesh3dCapability.hpp>

#include <lux/engine/render/capabilities/CapabilityCatalog.hpp>

namespace lux::render
{
    inline constexpr std::string_view kRasterSurfaceCapability = cxx::make_basic_type_info<Mesh3dCapability>().name();
    inline constexpr std::string_view kRasterSurfaceConfigurationSchema = "lux.render.config.raster_surface";

    struct RasterSurfaceConfiguration final
    {
        bool visibility = true;
        bool operator==(const RasterSurfaceConfiguration&) const noexcept = default;
    };

    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<SerializedRenderConfiguration>
    encodeRasterSurfaceConfiguration(const RasterSurfaceConfiguration&) noexcept;
    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<RasterSurfaceConfiguration>
        decodeRasterSurfaceConfiguration(std::span<const std::byte>) noexcept;
} // namespace lux::render
