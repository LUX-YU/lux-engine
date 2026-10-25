#pragma once

#include <lux/engine/render/extensions/storage/StorageDescriptor.hpp>
#include <lux/engine/render/inputs/RasterMaterialInputs.hpp>

namespace lux::render
{
    inline constexpr StorageNameView kMaterialStorageType("lux.render.storage.RasterMaterialTable");

    struct MaterialBinding final
    {
        MaterialId material;
        rdesc::ShaderBlock layout;               // Complete reflected record ABI; image slots need no Shader copy.
        std::uint32_t texture_slot = UINT32_MAX; // UINT32_MAX selects the parameter Buffer; otherwise one Image.
        bool operator==(const MaterialBinding&) const noexcept = default;
    };

    class RasterMaterialTableStorageDescriptor final
    {
    public:
        using Config = void;
        static inline const StorageDescriptor descriptor{
            kMaterialStorageType,
            0,
            0,
            cxx::typeToken<MaterialBinding>(),
            "Reflected cooked raster material parameter tables with stable slots and sampled texture references."
        };

        using Ops = void;
    };

    template <> struct ConfigValueTraits<MaterialBinding> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t> retainedBytes(const MaterialBinding&) noexcept;
    };
} // namespace lux::render
