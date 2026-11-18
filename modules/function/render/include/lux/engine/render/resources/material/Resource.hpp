#pragma once

#include <lux/engine/render/resources/material/Commands.hpp>
#include <lux/engine/render/resources/material/Binding.hpp>
#include <lux/engine/render/extensions/resource/ResourceDefinition.hpp>
#include <lux/engine/render/resources/BuiltinResources.hpp>

namespace lux::render
{
    class MaterialResource final
    {
    public:
        using Commands = std::tuple<
            MaterialCommands::Create, MaterialCommands::Update, MaterialCommands::Release,
            MaterialCommands::RegisterProgram, MaterialCommands::CreateInstance, MaterialCommands::ReleaseInstance,
            MaterialCommands::UnregisterProgram
        >;
        using PerFrameData = std::tuple<>;
        using Config = void;
        static inline const ResourceDefinition descriptor{
            kMaterialResourceName,
            0,
            0,
            cxx::make_basic_type_info<MaterialBinding>(),
            "Reflected raster material pages with stable GPU references and sampled texture dependencies.",
            EResourceDomain::DEVICE,
            resourceCommandDescriptors(static_cast<Commands*>(nullptr))
        };

        using Ops = void;
    };
} // namespace lux::render
