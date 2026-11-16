#pragma once

#include <lux/engine/render/commands/ImageCommands.hpp>

#include <lux/engine/render/resources/BuiltinResources.hpp>

#include <lux/engine/render/extensions/resource/ResourceDefinition.hpp>
#include <lux/engine/render/inputs/SampledTextureInputs.hpp>

namespace lux::render
{
    struct TextureBinding final
    {
        TextureId texture;
        bool operator==(const TextureBinding&) const noexcept = default;
    };

    class TextureResource final
    {
    public:
        using Commands = std::tuple<ImageCommands::Upload, ImageCommands::CloneRegion, ImageCommands::Release>;
        using Config = void;
        static inline const ResourceDefinition descriptor{
            kTextureResourceName,
            static_cast<std::uint32_t>(EStorageCapability::SAMPLED_IMAGE_ARRAY),
            static_cast<std::uint32_t>(EStorageCapability::BLOCK_COMPRESSION_BC),
            cxx::make_basic_type_info<TextureBinding>(),
            "Single-layer sampled 2D images with mip, R/RG/RGBA and BC1/3/5/7 uploads, regions and a white default.",
            EResourceDomain::DEVICE,
            resourceCommandDescriptors(static_cast<Commands*>(nullptr))
        };

        using Ops = void;
    };
} // namespace lux::render
