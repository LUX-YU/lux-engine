#pragma once

#include <lux/engine/render/resources/texture/Commands.hpp>
#include <lux/engine/render/extensions/resource/ResourceDefinition.hpp>
#include <lux/engine/render/resources/texture/Binding.hpp>
#include <lux/engine/render/resources/BuiltinResources.hpp>

namespace lux::render
{
    class TextureResource final
    {
    public:
        using Commands = std::tuple<TextureCommands::Upload, TextureCommands::CloneRegion, TextureCommands::Release>;
        using PerFrameData = std::tuple<>;
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
