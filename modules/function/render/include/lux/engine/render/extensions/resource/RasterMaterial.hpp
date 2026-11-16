#pragma once

#include <lux/engine/render/commands/MaterialCommands.hpp>

#include <lux/engine/render/resources/BuiltinResources.hpp>

#include <lux/engine/render/core/ConfigValue.hpp>

#include <lux/engine/render/extensions/resource/ResourceDefinition.hpp>
#include <lux/engine/render/inputs/RasterMaterialInputs.hpp>

namespace lux::render
{
    struct MaterialBinding final
    {
        MaterialId material;
        rdesc::ShaderBlock layout;               // Complete reflected record ABI; image slots need no Shader copy.
        std::uint32_t texture_slot = UINT32_MAX; // UINT32_MAX selects the parameter Buffer; otherwise one Image.
        bool operator==(const MaterialBinding&) const noexcept = default;
    };

    class MaterialResource final
    {
    public:
        using Commands = std::tuple<MaterialCommands::Create, MaterialCommands::Update, MaterialCommands::Release>;
        using Config = void;
        static inline const ResourceDefinition descriptor{
            kMaterialResourceName,
            static_cast<std::uint32_t>(EStorageCapability::STORAGE_BUFFER_ARRAY),
            0,
            cxx::make_basic_type_info<MaterialBinding>(),
            "Reflected raster material pages with stable GPU references and sampled texture dependencies.",
            EResourceDomain::DEVICE,
            resourceCommandDescriptors(static_cast<Commands*>(nullptr))
        };

        using Ops = void;
    };

    template <> struct ConfigValueTraits<MaterialBinding> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t> retainedBytes(const MaterialBinding&) noexcept;
    };
} // namespace lux::render
