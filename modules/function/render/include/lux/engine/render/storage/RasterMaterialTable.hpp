#pragma once

#include <lux/engine/render/visibility.h>

#include <algorithm>
#include <array>
#include <lux/engine/description/MaterialProgramSet.hpp>
#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/storage/SampledTexture2D.hpp>

namespace lux::render
{
    struct MaterialTag;
    using MaterialId = StorageResourceId<MaterialTag>;
    inline constexpr StorageNameView kMaterialStorageType("lux.render.storage.RasterMaterialTable");
    // Client-supplied finite values in the Cooked parameter order; Render checks the actual layout and count.
    using MaterialParameters = std::vector<std::array<float, 4>>;

    struct MaterialTextureGpuRef final
    {
        std::uint32_t representation_index = 0;
        std::uint32_t resource_index = 0;
        std::uint32_t sampler_index = 0;
        std::uint32_t flags = 0;
    };

    static_assert(sizeof(MaterialTextureGpuRef) == 16);

    struct MaterialBinding final
    {
        MaterialId material;
        rdesc::ShaderBlock layout;               // Complete reflected record ABI; image slots need no Shader copy.
        std::uint32_t texture_slot = UINT32_MAX; // UINT32_MAX selects the parameter Buffer; otherwise one Image.
        bool operator==(const MaterialBinding&) const noexcept = default;
    };

    struct MaterialWrite final
    {
        StorageId storage;
        rdesc::MaterialProgramSet program;
        MaterialParameters parameters{};
        // Client keeps referenced textures valid and explicitly updates these fields before releasing them.
        std::vector<TextureId> textures;
    };

    struct CreateMaterial final
    {
        static constexpr std::string_view name = "lux.render.CreateMaterial";
        using Request = MaterialWrite;

        struct Response final
        {
            RenderError error{};
            MaterialId material{};
        };
    };

    struct MaterialUpdate final
    {
        MaterialId material;
        MaterialParameters parameters;
        std::vector<TextureId> textures;
    };

    struct UpdateMaterial final
    {
        static constexpr std::string_view name = "lux.render.UpdateMaterial";
        // Related writes and releases are ordered by the Client, including across Client connections.
        // Program and table slot remain fixed. A different program requires a new Material identity.
        using Request = MaterialUpdate;
        using Response = AckResponse;
    };

    struct ReleaseMaterial final
    {
        static constexpr std::string_view name = "lux.render.ReleaseMaterial";
        using Request = MaterialId;
        using Response = AckResponse;
    };

    template <> struct ConfigValueTraits<rdesc::MaterialProgramSet> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t>
        retainedBytes(const rdesc::MaterialProgramSet&) noexcept;
    };

    template <> struct ConfigValueTraits<MaterialBinding> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t> retainedBytes(const MaterialBinding&) noexcept;
    };

    class RasterMaterialTableStorageDescriptor final
    {
    public:
        using Config = void;
        static inline const StorageDescriptor descriptor{
            kMaterialStorageType,
            0,
            0,
            cppType<MaterialBinding>(),
            "Reflected cooked raster material parameter tables with stable slots and sampled texture references."
        };

        using Ops = void;
    };
} // namespace lux::render
