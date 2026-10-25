#pragma once

#include <lux/engine/render/extensions/storage/StorageDescriptor.hpp>
#include <lux/engine/render/inputs/SampledTextureInputs.hpp>

namespace lux::render
{
    inline constexpr StorageNameView kTextureStorageType("lux.render.storage.SampledTexture2D");

    struct TextureBinding final
    {
        TextureId texture;
        bool operator==(const TextureBinding&) const noexcept = default;
    };

    class SampledTexture2DStorageDescriptor final
    {
    public:
        using Config = void;
        static inline constexpr StorageDescriptor descriptor{
            kTextureStorageType,
            static_cast<std::uint32_t>(EStorageCapability::SAMPLED_IMAGE_ARRAY),
            static_cast<std::uint32_t>(EStorageCapability::BLOCK_COMPRESSION_BC),
            cxx::typeToken<TextureBinding>(),
            "Single-layer sampled 2D images with mip, R/RG/RGBA and BC1/3/5/7 uploads, regions and a white default."
        };

        using Ops = void;
    };
} // namespace lux::render
