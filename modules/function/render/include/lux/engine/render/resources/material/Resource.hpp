#pragma once

#include <lux/engine/render/resources/material/Commands.hpp>
#include <lux/engine/render/resources/material/Binding.hpp>
#include <lux/engine/render/extensions/resource/ResourceDefinition.hpp>
#include <lux/engine/render/resources/BuiltinResources.hpp>
#include <lux/engine/render/graph/DrawResource.hpp>
#include <lux/engine/render/graph/DrawList.hpp>

namespace lux::render
{
    class FramePreparation;
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
        select(FramePreparation&, const MaterialDrawSources&, RasterProgramSlot, MaterialId) noexcept;

    private:
        std::vector<RasterSampledImageRead> images_;
    };

    class MaterialResource final
    {
    public:
        using Commands = std::tuple<
            MaterialCommands::RegisterProgram, MaterialCommands::CreateInstance, MaterialCommands::Update,
            MaterialCommands::UpdateColor, MaterialCommands::ReleaseInstance, MaterialCommands::UnregisterProgram
        >;
        using PerFrameData = std::tuple<MaterialColorUpdate>;
        using Config = void;
        static inline const ResourceDefinition descriptor{
            kMaterialResourceName,
            0,
            0,
            cxx::make_basic_type_info<MaterialBinding>(),
            "Reflected raster material pages with stable GPU references and sampled texture dependencies.",
            EResourceDomain::DEVICE,
            resourceCommandDescriptors(static_cast<Commands*>(nullptr)),
            resourceFrameTypes(static_cast<PerFrameData*>(nullptr))
        };

        using Ops = void;
        using DrawStorage = detail::RasterMaterialStorage;

        [[nodiscard]] static LUX_RENDER_PUBLIC DrawResourceType drawResourceType() noexcept;
    };
} // namespace lux::render
