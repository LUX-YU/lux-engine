#pragma once

#include <lux/engine/render/resource/material/Data.hpp>
#include <lux/engine/render/resource/material/Binding.hpp>
#include <lux/engine/render/resource/ResourceDefinition.hpp>
#include <lux/engine/render/resource/BuiltinResources.hpp>
#include <lux/engine/render/graph/DrawResource.hpp>
#include <lux/engine/render/graph/DrawList.hpp>

namespace lux::render
{
    class CpuCallFrame;
    namespace detail { class RasterMaterialStorage; class SampledTexture2DStorage; }

    struct MaterialDrawSources final
    {
        DrawResourceSlot<detail::RasterMaterialStorage> records;
        std::optional<DrawResourceSlot<detail::SampledTexture2DStorage>> textures;
    };

    class MaterialRecordReader final
    {
    public:
        [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<rdesc::MaterialGpuReference>
        select(CpuCallFrame&, const MaterialDrawSources&, RasterProgramSlot, MaterialId) noexcept;

    private:
        std::vector<RasterSampledImageRead> images_;
    };

    class MaterialResource final
    {
    public:
        using PerFrameData = std::tuple<MaterialColorUpdate>;
        using Config = void;
        static inline const ResourceDefinition descriptor{
            kMaterialResourceName,
            0,
            0,
            cxx::make_basic_type_info<MaterialBinding>(),
            "Reflected raster material pages with stable GPU references and sampled texture dependencies.",
            EResourceDomain::DEVICE,
            resourceFrameTypes(static_cast<PerFrameData*>(nullptr))
        };

        using DrawStorage = detail::RasterMaterialStorage;

        [[nodiscard]] static LUX_RENDER_PUBLIC DrawResourceType drawResourceType() noexcept;
    };
} // namespace lux::render
