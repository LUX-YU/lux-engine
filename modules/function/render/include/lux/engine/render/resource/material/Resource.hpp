#pragma once

#include <lux/engine/render/resource/material/Data.hpp>
#include <lux/engine/render/resource/material/Binding.hpp>
#include <lux/engine/render/resource/ResourceDefinition.hpp>
#include <lux/engine/render/graph/DrawResource.hpp>
#include <lux/engine/render/graph/DrawList.hpp>

namespace lux::render
{
    inline constexpr ResourceNameView kMaterialResourceName{"lux.render.resource.material"};

    namespace detail
    {
        class MaterialStorage;
        class SampledTexture2DStorage;
        struct CpuDrawStorage;
    }

    // Bind once when the Job installs the algorithm. The workspace borrows these owners for that installation.
    class LUX_RENDER_PUBLIC MaterialDrawBinding final
    {
    public:
        [[nodiscard]] static RenderResult<MaterialDrawBinding>
        bind(const InstalledDrawSources&, RasterResourceSlot records, std::optional<RasterResourceSlot> textures) noexcept;
        [[nodiscard]] RenderResult<rdesc::MaterialGpuReference> record(RasterProgramSlot, MaterialId) const noexcept;

    private:
        MaterialDrawBinding(
            const detail::MaterialStorage& materials, const detail::SampledTexture2DStorage* textures,
            detail::CpuDrawStorage& output
        ) noexcept : materials_(materials), textures_(textures), output_(output) {}

        const detail::MaterialStorage& materials_;
        const detail::SampledTexture2DStorage* textures_;
        detail::CpuDrawStorage& output_;
    };

    class MaterialResource final
    {
    public:
        using PerFrameData = std::tuple<MaterialColorUpdate>;
        using Config = void;
        static inline const ResourceDefinition descriptor{
            kMaterialResourceName,
            {},
            {},
            cxx::make_basic_type_info<MaterialBinding>(),
            "Reflected raster material pages with stable GPU references and sampled texture dependencies.",
            EResourceDomain::DEVICE,
            resourceFrameTypes(static_cast<PerFrameData*>(nullptr))
        };
    };
} // namespace lux::render
