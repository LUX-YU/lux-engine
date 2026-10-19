#pragma once

#include <algorithm>
#include <array>
#include <lux/engine/description/MaterialLayout.hpp>
#include <lux/engine/description/SpirvArtifact.hpp>
#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/resource/Texture.hpp>

namespace lux::render
{
    struct MaterialTag;
    using MaterialId = StorageResourceId<MaterialTag>;
    inline constexpr StorageNameView kMaterialStorageType("lux.render.MaterialStorage");
    using MaterialParameters = std::vector<std::array<float, 4>>;

    // Immutable runtime projection of existing cooked MaterialGraph output. No author graph or compiler.
    struct MaterialProgram final
    {
        rdesc::SpirvStage fragment;
        std::uint32_t parameter_count = 0;
        std::uint32_t texture_count = 0;

        rdesc::ShaderInfo interface;

        [[nodiscard]] const rdesc::ShaderBlock* parameterLayout() const noexcept
        {
            for (const auto& set : interface.sets)
            {
                for (const auto& binding : set.bindings)
                {
                    if (binding.name == rdesc::kMaterialTableName)
                    {
                        return &binding.block;
                    }
                }
            }
            return nullptr;
        }

        bool operator==(const MaterialProgram&) const noexcept = default;
    };

    struct MaterialTextureGpuRef final
    {
        std::uint32_t representation_index = 0;
        std::uint32_t resource_index = 0;
        std::uint32_t aux = 0;
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
        MaterialProgram program;
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

    template <> struct ConfigValueTraits<MaterialProgram> final
    {
        [[nodiscard]] static RenderResult<std::size_t> retainedBytes(const MaterialProgram&) noexcept;
    };

    template <> struct ConfigValueTraits<MaterialBinding> final
    {
        [[nodiscard]] static RenderResult<std::size_t> retainedBytes(const MaterialBinding&) noexcept;
    };

    class MaterialStorage final
    {
    public:
        using Config = void;
        static inline const StorageDescriptor description{kMaterialStorageType, 0, 0, cppType<MaterialBinding>()};

        using Ops = void;
    };
} // namespace lux::render
