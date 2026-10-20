#include <lux/engine/render/features/RasterMesh.hpp>

#include <algorithm>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/render/features/FeatureAssembly.hpp>
#include <lux/engine/render/features/MaterialMeshRaster.hpp>
#include <lux/engine/render/features/MeshDepthRaster.hpp>

namespace lux::render::detail
{
    template <bool Depth> constexpr FeatureName type() noexcept
    {
        if constexpr (Depth)
        {
            return MeshDepthRasterDescriptor::name;
        }
        else
        {
            return MaterialMeshRasterDescriptor::name;
        }
    }

    inline RenderResult<std::uint64_t> meshBytes(const MeshDescription& mesh) noexcept
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
    RenderResult<void> describe(const void* value, std::string_view instance, FeaturePorts& claims) noexcept
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
        const auto& mesh = config.geometry;
        const auto* program = &config.program;
        const bool is_invalid_group = config.group.mesh.isNull() || config.group.material.isNull() ||
                                      config.group.submesh >= mesh.submeshes.size();
        const bool is_invalid_storage =
            config.mesh_storage.empty() || config.material_storage.empty() || config.instance_storage.empty();
        const auto* material_layout = program->parameterLayout();
        const bool is_invalid_material = !Depth && (!material_layout || material_layout->fields.empty() ||
                                                    config.textures.size() != program->texture_count);
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
        const auto key = [](std::string_view local) { return std::string(local); };
        const auto buffer = [&](auto& group,
                                std::string_view local,
                                std::uint64_t size,
                                StorageNameView storage,
                                const std::string& storage_key)
        {
            group.push_back(
                {key(local),
                 EFeaturePort::OUTPUT,
                 GraphBufferDesc{size},
                 EGraphOrigin::IMPORTED,
                 {GraphBufferRange{0, size}},
                 StorageRequirement{StorageName(storage.name()), storage_key}}
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
             EFeaturePort::OUTPUT,
             GraphBufferDesc{24},
             EGraphOrigin::IMPORTED,
             {GraphBufferRange{0, 24}}}
        );
        GraphImageDesc color{config.target.width, config.target.height, 1, 1, config.target.format};
        GraphImageDesc depth{config.target.width, config.target.height, 1, 1, rdesc::ETextureFormat::D32_SFLOAT};
        color.extent.target_relative = true;
        depth.extent.target_relative = true;
        claims.resources.push_back({key("color"), EFeaturePort::OUTPUT, color});
        claims.resources.push_back({key("depth"), EFeaturePort::OUTPUT, depth});
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
                     EFeaturePort::OUTPUT,
                     image,
                     EGraphOrigin::IMPORTED,
                     {GraphImageRange{1, 0, image.mips, 0, image.layers}},
                     StorageRequirement{StorageName(kMaterialStorageType.name()), config.material_storage}}
                );
            }
        }
        return {};
    }

} // namespace lux::render::detail

#include <algorithm>
#include <cstdint>
#include <lux/engine/render/features/RasterMeshDraws.hpp>

// glslang's --vn output uses uint32_t without including its declaration.
#include "BasicMeshPagedVertex.hpp"
#include "BasicMeshVertex.hpp"
#include "DepthOnlyPagedVertex.hpp"
#include "DepthOnlyVertex.hpp"
#include <lux/engine/render/features/FeatureDefinition.hpp>
#include <lux/engine/render/features/RasterMeshParameters.hpp>

namespace lux::render::detail
{
    template <bool Depth, bool Paged>
    RenderResult<void> build(const void* value, std::string_view instance, FeatureBuildContext& context) noexcept
    {
        const auto& config = *static_cast<const MeshFeatureConfig*>(value);
        const auto type = Depth ? MeshDepthRasterDescriptor::name : MaterialMeshRasterDescriptor::name;
        const auto key = [&](std::string_view local) { return featureKey(type, instance, local); };
        const auto resource = [&](std::string_view local) { return context.resource(local); };
        RasterWork draw;
        rdesc::SpirvArtifact shader;
        const auto vertex = Paged ? (Depth ? std::span<const std::uint32_t>(kDepthOnlyPagedVertex)
                                           : std::span<const std::uint32_t>(kBasicMeshPagedVertex))
                                  : (Depth ? std::span<const std::uint32_t>(kDepthOnlyVertex)
                                           : std::span<const std::uint32_t>(kBasicMeshVertex));
        shader.stages.push_back({rdesc::EShaderType::VERTEX, "main", {vertex.begin(), vertex.end()}});
        const auto reflection = Paged ? (Depth ? std::span<const std::byte>(kDepthOnlyPagedVertexReflection)
                                               : std::span<const std::byte>(kBasicMeshPagedVertexReflection))
                                      : (Depth ? std::span<const std::byte>(kDepthOnlyVertexReflection)
                                               : std::span<const std::byte>(kBasicMeshVertexReflection));
        if (!rdesc::ShaderInfo::deserialize(reflection, shader.interface))
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        draw.interface.parameters.instance_layout = meshViewParameterBlock();
        draw.interface.parameters.values.resize(128);
        draw.interface.parameters.fields = {{"view", {}, "view"}, {"projection", {}, "projection"}};
        draw.interface.resources = {
            {"transforms", {resource("matrices"), {}, EGraphAccess::READ}},
            {"properties", {resource("properties"), {}, EGraphAccess::READ}},
            {"alive", {resource("alive"), {}, EGraphAccess::READ}},
            {"members", {resource("members"), {}, EGraphAccess::READ}}
        };
        draw.graphics.vertex_bindings = {{0, config.geometry.vertex_stride}};
        for (const auto& attribute : config.geometry.attributes)
        {
            if (attribute.location == 0 || (!Depth && attribute.location == 1))
            {
                draw.graphics.vertex_attributes.push_back({attribute.location, 0, attribute.offset, attribute.format});
            }
        }
        draw.graphics.depth_test = draw.graphics.depth_write = true;
        draw.vertex_buffers = {{resource("vertices"), 0, 0}};
        draw.draw = DrawIndexedIndirect{
            resource("indices"),
            0,
            config.geometry.index_stride == 2 ? EIndexType::UINT16 : EIndexType::UINT32,
            resource("members")
        };
        const auto argument = [&](std::string_view local, auto binding)
        { context.arguments.push_back(StorageArgument::make(resource(local).resource, std::move(binding))); };
        argument(
            "vertices",
            MeshBinding{
                config.group.mesh,
                EMeshBuffer::VERTICES,
                config.geometry.vertex_stride,
                config.geometry.attributes
            }
        );
        argument("indices", MeshBinding{config.group.mesh, EMeshBuffer::INDICES, config.geometry.index_stride});
        context.arguments.push_back(
            StorageArgument::make(resource("matrices").resource, MeshInstanceBinding{EMeshInstanceBinding::TRANSFORMS})
        );
        context.arguments.push_back(StorageArgument::make(
            resource("properties").resource,
            MeshInstanceBinding{EMeshInstanceBinding::PROPERTIES}
        ));
        context.arguments.push_back(
            StorageArgument::make(resource("alive").resource, MeshInstanceBinding{EMeshInstanceBinding::ALIVE})
        );
        const auto matrices = resource("matrices").resource;
        const auto members = resource("members").resource;
        auto configuration = ConfigValue::make(MeshDrawConfiguration{matrices, config.group});
        if (!configuration)
        {
            return cxx::unexpected(configuration.error());
        }
        context.bindings.push_back({members, std::move(*configuration), &RasterMeshDraws::create});
        if constexpr (!Depth)
        {
            // describe already qualified the typed cooked contract. Native reflection checks the actual code ABI.
            const auto& material = config.program;
            shader.stages.push_back(material.fragment);
            shader.interface.entry_points.insert(
                shader.interface.entry_points.end(),
                material.interface.entry_points.begin(),
                material.interface.entry_points.end()
            );
            // Project the actual cooked descriptor facts; PipelineLayoutPlan still assigns final locations.
            for (const auto& set : material.interface.sets)
            {
                for (const auto& binding : set.bindings)
                {
                    const bool is_table = binding.type == rdesc::EDescriptorType::STORAGE_BUFFER &&
                                          binding.name == rdesc::kMaterialTableName;
                    const bool is_images = binding.type == rdesc::EDescriptorType::SAMPLED_IMAGE ||
                                           binding.type == rdesc::EDescriptorType::SAMPLER;
                    if (!is_table && !is_images)
                    {
                        return cxx::unexpected(RenderError{kFeatureContract});
                    }
                    auto destination =
                        std::ranges::find(shader.interface.sets, binding.set, &rdesc::DescriptorSetLayoutInfo::set);
                    if (destination == shader.interface.sets.end())
                    {
                        shader.interface.sets.emplace_back(binding.set);
                        destination = std::prev(shader.interface.sets.end());
                    }
                    destination->bindings.push_back(binding);
                    draw.interface.resources.push_back(
                        {binding.name, {resource(is_table ? "material" : "texture0"), {}, EGraphAccess::READ}}
                    );
                }
            }
            argument("material", MaterialBinding{config.group.material, *material.parameterLayout()});
            for (std::uint32_t i = 0; i < config.textures.size(); ++i)
            {
                argument("texture" + std::to_string(i), MaterialBinding{config.group.material, {}, i});
            }
        }
        auto program = context.graph.addProgram(std::move(shader));
        if (!program)
        {
            return cxx::unexpected(program.error());
        }
        draw.interface.program = *program;
        draw.depth = RasterAttachment{{resource("depth")}};
        if constexpr (!Depth)
        {
            draw.colors.push_back({{resource("color")}});
        }
        auto raster = context.graph.addRasterPass(key("draw"), std::move(draw));
        if (!raster)
        {
            return cxx::unexpected(raster.error());
        }
        if (auto published = context.publish("depth", raster->written.back()); !published)
        {
            return published;
        }
        if constexpr (Depth)
        {
            TransferWork clear;
            clear.operations.push_back(ClearImage{{resource("color")}});
            auto cleared = context.graph.addTransferPass(key("clear"), std::move(clear));
            if (!cleared)
            {
                return cxx::unexpected(cleared.error());
            }
            return context.publish("color", cleared->written.front());
        }
        else
        {
            return context.publish("color", raster->written.front());
        }
    }
} // namespace lux::render::detail

namespace lux::render::detail
{
    template RenderResult<void> describe<false>(const void*, std::string_view, FeaturePorts&) noexcept;
    template RenderResult<void> describe<true>(const void*, std::string_view, FeaturePorts&) noexcept;
    template RenderResult<void> build<false, false>(const void*, std::string_view, FeatureBuildContext&) noexcept;
    template RenderResult<void> build<false, true>(const void*, std::string_view, FeatureBuildContext&) noexcept;
    template RenderResult<void> build<true, false>(const void*, std::string_view, FeatureBuildContext&) noexcept;
    template RenderResult<void> build<true, true>(const void*, std::string_view, FeatureBuildContext&) noexcept;
} // namespace lux::render::detail
