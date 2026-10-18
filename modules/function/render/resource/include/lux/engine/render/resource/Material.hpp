#pragma once

#include <algorithm>
#include <array>
#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/data/Shader.hpp>
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
        SpirvStage fragment;
        MaterialParameters defaults{};
        std::uint32_t texture_count = 0;

        [[nodiscard]] std::uint32_t parameterCount() const noexcept
        {
            return static_cast<std::uint32_t>(defaults.size());
        }

        [[nodiscard]] std::uint32_t recordSize() const noexcept
        {
            return 16 * std::max(1u, parameterCount()) + 16 * std::max(1u, texture_count);
        }

        bool operator==(const MaterialProgram&) const noexcept = default;
    };

    [[nodiscard]] ShaderBlock materialParameterBlock(const MaterialProgram&) noexcept;

    struct MaterialPublished final
    {
        MaterialId material;
        std::uint64_t revision = 0;
    };

    struct MaterialBinding final
    {
        MaterialId material;
        ConfigValue program;                     // Same complete cooked contract used by the cold Graph compiler.
        std::uint32_t texture_slot = UINT32_MAX; // UINT32_MAX selects the parameter Buffer; otherwise one Image.
        bool operator==(const MaterialBinding&) const noexcept = default;
    };

    struct MaterialWrite final
    {
        MaterialId material;
        std::uint64_t expected_revision = 0;
        ConfigValue program;
        MaterialParameters parameters{};
        // Logical references resolve to the current image; stale or missing references select white.
        std::vector<TextureId> textures;
    };

    struct MaterialInformation final
    {
        MaterialPublished published;
        ConfigValue program;
        MaterialParameters parameters{};
        // Logical references resolve to the current image; stale or missing references select white.
        std::vector<TextureId> textures;
    };

    struct SetMaterial final
    {
        static constexpr std::string_view name = "lux.render.SetMaterial";
        using Request = MaterialWrite;
        using Response = RenderResult<MaterialPublished>;
    };

    struct QueryMaterial final
    {
        static constexpr std::string_view name = "lux.render.QueryMaterial";
        using Request = MaterialId;
        using Response = RenderResult<MaterialInformation>;
    };

    struct ReleaseMaterial final
    {
        static constexpr std::string_view name = "lux.render.ReleaseMaterial";
        using Request = MaterialPublished;
        using Response = RenderResult<Ack>;
    };

    template <> struct ConfigValueTraits<MaterialProgram> final
    {
        [[nodiscard]] static RenderResult<std::size_t> retainedBytes(const MaterialProgram&) noexcept;
    };

    template <> struct ConfigValueTraits<MaterialBinding> final
    {
        [[nodiscard]] static RenderResult<std::size_t> retainedBytes(const MaterialBinding&) noexcept;
    };

    struct MaterialStorageConfig final
    {
        std::uint64_t resident_byte_budget = 0;
        bool operator==(const MaterialStorageConfig&) const noexcept = default;
    };

    class MaterialStorage final
    {
    public:
        using Config = MaterialStorageConfig;
        static inline const StorageDescriptor description{kMaterialStorageType, 0, 0, cppType<MaterialBinding>()};

        struct Ops final
        {
            RenderResult<MaterialInformation> (*query)(const void*, MaterialId) noexcept;
            RenderResult<void> (*release)(void*, MaterialId, std::uint64_t) noexcept;
        };

        class Access final
        {
        public:
            [[nodiscard]] RenderResult<MaterialInformation> query(MaterialId id) const noexcept
            {
                return ops_->query(native_, id);
            }

            [[nodiscard]] RenderResult<void> release(MaterialId id, std::uint64_t revision) const noexcept
            {
                if (!writable_)
                {
                    return cxx::unexpected(RenderError{kStorageIdentity});
                }
                return ops_->release(native_, id, revision);
            }

        private:
            friend class ResourceRegistry;

            Access(void* native, const Ops& ops, bool writable) noexcept
                : native_(native), ops_(&ops), writable_(writable)
            {
            }

            void* native_;
            const Ops* ops_;
            bool writable_;
        };
    };
} // namespace lux::render
