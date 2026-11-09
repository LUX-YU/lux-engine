#pragma once

#include <string_view>
#include <lux/engine/render/core/ResourceId.hpp>

#include <lux/engine/render/visibility.h>

#include <algorithm>
#include <array>
#include <vector>
#include <lux/engine/description/MaterialProgramSet.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/inputs/SampledTextureInputs.hpp>

namespace lux::render
{
    inline constexpr std::string_view kRasterMaterialInput = "lux.render.input.raster_material";

    struct MaterialTag;
    using MaterialId = ResourceId<MaterialTag>;
    // Client-supplied finite values in the Cooked parameter order; Render checks the actual layout and count.
    using MaterialParameters = std::vector<std::array<float, 4>>;

    struct MaterialTextureGpuRef final
    {
        std::uint32_t representation_index = 0;
        std::uint32_t resource_index = 0;
        std::uint32_t sampler_index = 0;
        std::uint32_t flags = 0;
    };

    static_assert(sizeof(MaterialTextureGpuRef) == 16);

    // Cooked material value. Asset identifiers have already been resolved by the caller.
    struct MaterialData final
    {
        rdesc::MaterialProgramSet program;
        MaterialParameters parameters{};
        // Client keeps referenced textures valid and explicitly updates these fields before releasing them.
        std::vector<TextureId> textures;
    };

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
