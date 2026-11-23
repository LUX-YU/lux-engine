#pragma once

#include <lux/engine/render/resource/texture/Data.hpp>
#include <lux/engine/render/resource/ResourceDefinition.hpp>
#include <lux/engine/render/resource/texture/Binding.hpp>
#include <lux/engine/render/resource/BuiltinResources.hpp>
#include <lux/engine/render/graph/DrawResource.hpp>

namespace lux::render
{
    class CpuCallFrame;
    namespace detail { class SampledTexture2DStorage; }

    class TextureResource final
    {
    public:
        using PerFrameData = std::tuple<>;
        using Config = void;
        static inline const ResourceDefinition descriptor{
            kTextureResourceName,
            static_cast<std::uint32_t>(EStorageCapability::SAMPLED_IMAGE_ARRAY),
            static_cast<std::uint32_t>(EStorageCapability::BLOCK_COMPRESSION_BC),
            cxx::make_basic_type_info<TextureBinding>(),
            "Single-layer sampled 2D images with mip, R/RG/RGBA and BC1/3/5/7 uploads, regions and a white default.",
            EResourceDomain::DEVICE
        };


        using DrawStorage = detail::SampledTexture2DStorage;
        [[nodiscard]] static LUX_RENDER_PUBLIC DrawResourceType drawResourceType() noexcept;
        [[nodiscard]] static LUX_RENDER_PUBLIC RenderResult<TextureGpuRef>
        sample(CpuCallFrame&, DrawResourceSlot<DrawStorage>, TextureId) noexcept;
    };
} // namespace lux::render
