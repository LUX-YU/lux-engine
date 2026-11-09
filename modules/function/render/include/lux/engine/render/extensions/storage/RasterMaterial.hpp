#pragma once

#include <lux/engine/render/core/ConfigValue.hpp>

#include <lux/engine/render/extensions/storage/StorageDescriptor.hpp>
#include <lux/engine/render/inputs/RasterMaterialInputs.hpp>

namespace lux::render
{
    inline constexpr StorageNameView kMaterialStorageType("lux.render.storage.RasterMaterial");

    struct MaterialBinding final
    {
        MaterialId material;
        rdesc::ShaderBlock layout;               // Complete reflected record ABI; image slots need no Shader copy.
        std::uint32_t texture_slot = UINT32_MAX; // UINT32_MAX selects the parameter Buffer; otherwise one Image.
        bool operator==(const MaterialBinding&) const noexcept = default;
    };

    class RasterMaterialStorageDescriptor final
    {
    public:
        using Config = void;
        static inline const StorageDescriptor descriptor{
            kMaterialStorageType,
            static_cast<std::uint32_t>(EStorageCapability::STORAGE_BUFFER_ARRAY),
            0,
            cxx::make_basic_type_info<MaterialBinding>(),
            "Reflected raster material pages with stable GPU references and sampled texture dependencies."
        };

        using Ops = void;
    };

    template <> struct ConfigValueTraits<MaterialBinding> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t> retainedBytes(const MaterialBinding&) noexcept;
    };
} // namespace lux::render
