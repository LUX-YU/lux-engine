#pragma once

#include <string_view>
#include <lux/engine/render/resource/ResourceId.hpp>

#include <lux/engine/render/visibility.h>

#include <algorithm>
#include <array>
#include <vector>
#include <lux/engine/description/MaterialProgramSet.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/resource/texture/Data.hpp>

namespace lux::render
{
    inline constexpr std::string_view kRasterMaterialInput = "lux.render.input.raster_material";

    struct MaterialTag;
    using MaterialId = ResourceId<MaterialTag>;
    struct MaterialProgramTag;
    using MaterialProgramId = ResourceId<MaterialProgramTag>;
    // Client-supplied finite values in the target program's parameter order.
    using MaterialParameters = std::vector<std::array<float, 4>>;

    struct MaterialUpdate final
    {
        MaterialId material;
        MaterialParameters parameters;
        std::vector<TextureId> textures;
    };

    struct MaterialColorUpdate final
    {
        MaterialId material;
        // Index in the selected program's reflected float4 parameter projection.
        std::uint32_t parameter = 0;
        std::array<float, 4> linear_rgba{};
    };

    template <> struct ConfigValueTraits<rdesc::MaterialProgramSet> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t>
        retainedBytes(const rdesc::MaterialProgramSet&) noexcept;
    };

} // namespace lux::render
