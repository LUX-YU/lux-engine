#include <algorithm>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/render/features/mesh/MeshFeature.hpp>

namespace lux::render
{
    namespace
    {
        template <bool Depth> constexpr FeatureName type() noexcept
        {
            if constexpr (Depth)
            {
                return DepthOnlyFeature::name;
            }
            else
            {
                return BasicMeshFeature::name;
            }
        }

        RenderResult<std::uint64_t> meshBytes(const MeshDescription& mesh) noexcept
        {
            const auto vertices = cxx::checkedMul(std::uint64_t(mesh.vertex_stride), std::uint64_t(mesh.vertex_count));
            const auto indices = cxx::checkedMul(std::uint64_t(mesh.index_stride), std::uint64_t(mesh.index_count));
            if (!vertices || !indices)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            const auto end = cxx::checkedAdd(mesh.index_offset, *indices);
            if (!end)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            return std::max(*vertices, *end);
        }

        template <bool Depth>
        RenderResult<void> describe(const void* value, std::string_view instance, ResourceClaims& claims) noexcept
        {
            const auto& config = *static_cast<const MeshFeatureConfig*>(value);
            if (auto valid = config.target.validate(); !valid)
            {
                return valid;
            }
            const auto pixels =
                cxx::checkedMul(std::uint64_t(config.target.width) * config.target.height, std::uint64_t(4));
            if (!pixels)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            const bool is_supported_target = config.target.format == rdesc::ETextureFormat::RGBA8_UNORM ||
                                             config.target.format == rdesc::ETextureFormat::RGBA8_SRGB;
            if (!is_supported_target)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            const auto& mesh = config.mesh.description;
            const auto* program = &config.material.program;
            const bool is_invalid_group = config.group.mesh != config.mesh.mesh ||
                                          config.group.material != config.material.material ||
                                          config.group.submesh >= mesh.submeshes.size();
            const bool is_invalid_storage =
                config.mesh_storage.empty() || config.material_storage.empty() || config.instance_storage.empty();
            const auto* material_layout = program->parameterLayout();
            const bool is_invalid_material =
                !Depth &&
                (!material_layout || material_layout->fields.empty() ||
                 (config.material.texture_slot != UINT32_MAX || config.textures.size() != program->texture_count));
            const bool is_invalid_config = is_invalid_group || is_invalid_storage || is_invalid_material;
            if (is_invalid_config)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            const auto attribute = [&](unsigned location, rdesc::ETextureFormat format)
            {
                const auto found = std::ranges::find(mesh.attributes, location, &MeshAttribute::location);
                return found != mesh.attributes.end() && found->format == format;
            };
            const bool is_invalid_vertex = !attribute(0, rdesc::ETextureFormat::RGB32_SFLOAT) ||
                                           (!Depth && !attribute(1, rdesc::ETextureFormat::RG32_SFLOAT));
            if (is_invalid_vertex)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            auto bytes = meshBytes(mesh);
            if (!bytes)
            {
                return cxx::unexpected(bytes.error());
            }
            const auto key = [&](std::string_view local) { return featureKey(type<Depth>(), instance, local); };
            const auto buffer = [&](auto& group,
                                    std::string_view local,
                                    std::uint64_t size,
                                    StorageNameView storage,
                                    const std::string& owner_key)
            {
                group.push_back(
                    {key(local),
                     EResourceClaim::PROVIDE,
                     GraphBufferDesc{size},
                     EGraphOrigin::IMPORTED,
                     {GraphBufferRange{0, size}},
                     StorageRequirement{StorageName(storage.name()), owner_key}}
                );
            };
            buffer(
                claims.storage.global,
                "vertices",
                std::uint64_t(mesh.vertex_count) * mesh.vertex_stride,
                kMeshStorageType,
                config.mesh_storage
            );
            buffer(
                claims.storage.global,
                "indices",
                std::uint64_t(mesh.index_count) * mesh.index_stride,
                kMeshStorageType,
                config.mesh_storage
            );
            buffer(
                claims.storage.scene,
                "matrices",
                sizeof(TransformMatrix),
                kMeshInstanceStorageType,
                config.instance_storage
            );
            buffer(
                claims.storage.scene,
                "properties",
                sizeof(MeshInstanceGpuProperty),
                kMeshInstanceStorageType,
                config.instance_storage
            );
            buffer(
                claims.storage.scene,
                "alive",
                sizeof(std::uint64_t), // Covers a flat value or one paged root address; Attach binds actual capacity.
                kMeshInstanceStorageType,
                config.instance_storage
            );
            // Feature-derived indirect membership, prepared once per Attachment. It is not a Storage.
            claims.resources.push_back(
                {key("members"),
                 EResourceClaim::PROVIDE,
                 GraphBufferDesc{24},
                 EGraphOrigin::IMPORTED,
                 {GraphBufferRange{0, 24}}}
            );
            const GraphImageDesc color{config.target.width, config.target.height, 1, 1, config.target.format};
            const GraphImageDesc
                depth{config.target.width, config.target.height, 1, 1, rdesc::ETextureFormat::D32_SFLOAT};
            claims.resources.push_back({key("color"), EResourceClaim::PROVIDE, color});
            claims.resources.push_back({key("depth"), EResourceClaim::PROVIDE, depth});
            if constexpr (!Depth)
            {
                buffer(
                    claims.storage.global,
                    "material",
                    material_layout->size,
                    kMaterialStorageType,
                    config.material_storage
                );
                for (std::size_t i = 0; i < config.textures.size(); ++i)
                {
                    const auto& image = config.textures[i];
                    claims.storage.global.push_back(
                        {key("texture" + std::to_string(i)),
                         EResourceClaim::PROVIDE,
                         image,
                         EGraphOrigin::IMPORTED,
                         {GraphImageRange{1, 0, image.mips, 0, image.layers}},
                         StorageRequirement{StorageName(kMaterialStorageType.name()), config.material_storage}}
                    );
                }
            }
            return {};
        }

        template <bool Depth>
        RenderResult<void> build(
            const void* value,
            std::string_view instance,
            std::span<const ResolvedResource> resources,
            RenderGraphBuilder& builder
        ) noexcept
        {
            const auto& config = *static_cast<const MeshFeatureConfig*>(value);
            const auto key = [&](std::string_view local) { return featureKey(type<Depth>(), instance, local); };
            const auto resource = [&](std::string_view local) { return findResource(resources, key(local))->version; };
            GraphPass draw{
                key("draw"),
                EGraphPassKind::GRAPHICS,
                {{resource("vertices"), {}, EGraphAccess::READ, 0, {}, EGraphUse::VERTEX},
                 {resource("indices"), {}, EGraphAccess::READ, 0, {}, EGraphUse::INDEX},
                 {resource("matrices"), {}, EGraphAccess::READ, 0, {}, EGraphUse::STORAGE_READ},
                 {resource("properties"), {}, EGraphAccess::READ, 0, {}, EGraphUse::STORAGE_READ},
                 {resource("alive"), {}, EGraphAccess::READ, 0, {}, EGraphUse::STORAGE_READ},
                 {resource("members"), {}, EGraphAccess::READ, 0, {}, EGraphUse::STORAGE_READ},
                 {resource("members"), {}, EGraphAccess::READ, 0, {}, EGraphUse::INDIRECT},
                 {resource("depth"), {}, EGraphAccess::WRITE, 0, {}, EGraphUse::DEPTH_ATTACHMENT}}
            };
            if constexpr (!Depth)
            {
                draw.uses.push_back({resource("color"), {}, EGraphAccess::WRITE, 0, {}, EGraphUse::COLOR_ATTACHMENT});
                draw.uses.push_back({resource("material"), {}, EGraphAccess::READ, 0, {}, EGraphUse::STORAGE_READ});
                for (std::size_t i = 0; i < config.textures.size(); ++i)
                {
                    draw.uses.push_back(
                        {resource("texture" + std::to_string(i)), {}, EGraphAccess::READ, 0, {}, EGraphUse::SAMPLED}
                    );
                }
            }
            if (auto added = builder.addPass(std::move(draw)); !added)
            {
                return cxx::unexpected(added.error());
            }
            if constexpr (Depth)
            {
                if (auto cleared = builder.addPass(
                        {key("clear"),
                         EGraphPassKind::TRANSFER,
                         {{resource("color"), {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}
                    );
                    !cleared)
                {
                    return cxx::unexpected(cleared.error());
                }
            }
            if (auto output = builder.exportOutput({resource("color"), {}, config.target}); !output)
            {
                return output;
            }
            if (auto output = builder.exportOutput({resource("depth")}); !output)
            {
                return output;
            }
            return {};
        }
    } // namespace

    const FeatureDescriptor BasicMeshFeature::descriptor{name, cppType<Config>()};
    const FeatureGraphOps BasicMeshFeature::graph{describe<false>, build<false>};
    const FeatureDescriptor DepthOnlyFeature::descriptor{name, cppType<Config>()};
    const FeatureGraphOps DepthOnlyFeature::graph{describe<true>, build<true>};

    RenderResult<std::size_t> ConfigValueTraits<MeshFeatureConfig>::retainedBytes(const MeshFeatureConfig& value
    ) noexcept
    {
        auto mesh = ConfigValueTraits<MeshBinding>::retainedBytes(value.mesh);
        if (!mesh)
        {
            return cxx::unexpected(mesh.error());
        }
        const auto textures = cxx::checkedMul(value.textures.capacity(), sizeof(GraphImageDesc));
        if (!textures)
        {
            return cxx::unexpected(RenderError{kInvalidConfig});
        }
        auto program = ConfigValueTraits<MaterialProgram>::retainedBytes(value.material.program);
        if (!program)
        {
            return cxx::unexpected(program.error());
        }
        std::size_t total = sizeof(value);
        for (auto bytes :
             {*mesh - sizeof(MeshBinding),
              *program - sizeof(MaterialProgram),
              *textures,
              value.mesh_storage.capacity(),
              value.material_storage.capacity(),
              value.instance_storage.capacity(),
              std::size_t(3)})
        {
            auto sum = cxx::checkedAdd(total, bytes);
            if (!sum)
            {
                return cxx::unexpected(RenderError{kInvalidConfig});
            }
            total = *sum;
        }
        return total;
    }
} // namespace lux::render
