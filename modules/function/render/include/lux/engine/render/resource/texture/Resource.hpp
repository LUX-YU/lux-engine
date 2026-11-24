#pragma once

#include <lux/engine/render/resource/texture/Data.hpp>
#include <lux/engine/render/resource/ResourceDefinition.hpp>
#include <lux/engine/render/resource/texture/Binding.hpp>
#include <lux/engine/render/graph/DrawResource.hpp>

namespace lux::render
{
    inline constexpr ResourceNameView kTextureResourceName{"lux.render.resource.texture"};

    namespace detail { class SampledTexture2DStorage; struct CpuDrawStorage; }

    // Borrowed by an installed workspace for its synchronous draw selections.
    class LUX_RENDER_PUBLIC TextureDrawBinding final
    {
    public:
        [[nodiscard]] static RenderResult<TextureDrawBinding> bind(const InstalledDrawSources&, RasterResourceSlot) noexcept;
        [[nodiscard]] RenderResult<TextureGpuRef> sample(TextureId) const noexcept;

    private:
        TextureDrawBinding(const detail::SampledTexture2DStorage& storage, detail::CpuDrawStorage& output) noexcept
            : storage_(storage), output_(output) {}

        const detail::SampledTexture2DStorage& storage_;
        detail::CpuDrawStorage& output_;
    };

    class TextureResource final
    {
    public:
        using PerFrameData = std::tuple<>;
        using Config = void;
        static inline const ResourceDefinition descriptor{
            kTextureResourceName,
            EGpuResourceFeature::SAMPLED_IMAGE_ARRAY,
            EGpuResourceFeature::BLOCK_COMPRESSION_BC,
            cxx::make_basic_type_info<TextureBinding>(),
            "Single-layer sampled 2D images with mip, R/RG/RGBA and BC1/3/5/7 uploads, regions and a white default.",
            EResourceDomain::DEVICE
        };
    };
} // namespace lux::render
