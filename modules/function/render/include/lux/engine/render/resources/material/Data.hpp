#pragma once

#include <string_view>
#include <lux/engine/render/core/ResourceId.hpp>

#include <lux/engine/render/visibility.h>

#include <algorithm>
#include <array>
#include <vector>
#include <lux/engine/description/MaterialProgramSet.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/resources/texture/Data.hpp>

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

    template <> struct ConfigValueTraits<rdesc::MaterialProgramSet> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t>
        retainedBytes(const rdesc::MaterialProgramSet&) noexcept;
    };

} // namespace lux::render
