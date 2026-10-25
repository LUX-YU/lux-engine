#include <lux/engine/render/features/BuiltinFeatures.hpp>
#include <lux/engine/render/features/RasterMesh.hpp>
#include <lux/engine/render/features/RasterMeshParameters.hpp>

#include <algorithm>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/render/extensions/features/FeatureAssembly.hpp>
#include <lux/engine/render/features/MeshVisibilityInputs.hpp>
#include <lux/engine/render/features/RasterMeshDraws.hpp>

#include "BasicMeshNoUvPagedReflectedVertex.hpp"
#include "BasicMeshNoUvPagedVertex.hpp"
#include "BasicMeshNoUvReflectedVertex.hpp"
#include "BasicMeshNoUvVertex.hpp"
#include "BasicMeshPagedReflectedVertex.hpp"
#include "BasicMeshPagedVertex.hpp"
#include "BasicMeshReflectedVertex.hpp"
#include "BasicMeshVertex.hpp"
#include "DepthOnlyPagedReflectedVertex.hpp"
#include "DepthOnlyPagedVertex.hpp"
#include "DepthOnlyReflectedVertex.hpp"
#include "DepthOnlyVertex.hpp"
#include "SurfaceMeshNoUvPagedReflectedVertex.hpp"
#include "SurfaceMeshNoUvPagedVertex.hpp"
#include "SurfaceMeshNoUvReflectedVertex.hpp"
#include "SurfaceMeshNoUvVertex.hpp"
#include "SurfaceMeshPagedReflectedVertex.hpp"
#include "SurfaceMeshPagedVertex.hpp"
#include "SurfaceMeshReflectedVertex.hpp"
#include "SurfaceMeshVertex.hpp"
#include "TangentMeshNoUvPagedReflectedVertex.hpp"
#include "TangentMeshNoUvPagedVertex.hpp"
#include "TangentMeshNoUvReflectedVertex.hpp"
#include "TangentMeshNoUvVertex.hpp"
#include "TangentMeshPagedReflectedVertex.hpp"
#include "TangentMeshPagedVertex.hpp"
#include "TangentMeshReflectedVertex.hpp"
#include "TangentMeshVertex.hpp"

namespace lux::render::detail
{
    namespace
    {
        bool readsLocation(const rdesc::MaterialShader* program, std::uint32_t location) noexcept
        {
            if (!program)
            {
                return false;
            }
            for (const auto& entry : program->shader.interface.entry_points)
            {
                if (std::ranges::find(entry.inputs, location, &rdesc::ShaderIoVariable::location) != entry.inputs.end())
                {
                    return true;
                }
            }
            return false;
        }
    } // namespace

    RenderResult<void> describeMeshGroup(
        const RasterMeshGroup& config,
        std::string_view prefix,
        rdesc::EMaterialPurpose purpose,
        FeaturePorts& claims,
        std::uint32_t visible_capacity,
        std::uint32_t view_count
    ) noexcept
    {
        const auto& mesh = config.geometry;
        const auto* program = config.program.find(purpose);
        const bool invalid_program =
            !program && (purpose != rdesc::EMaterialPurpose::SHADOW || !config.program.programs.empty());
        const bool invalid_group = config.group.mesh.isNull() || config.group.material_table_source.isNull() ||
                                   config.group.submesh >= mesh.submeshes.size();
        const bool invalid_storage =
            config.mesh_storage.empty() || config.material_storage.empty() || config.instance_storage.empty();
        const bool invalid_textures = config.textures.size() != config.program.texture_count;
        if (invalid_program || invalid_group || invalid_storage || invalid_textures)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        const auto attribute = [&](unsigned location, rdesc::ETextureFormat format)
        {
            const auto found = std::ranges::find(mesh.attributes, location, &MeshAttribute::location);
            return found != mesh.attributes.end() && found->format == format;
        };
        const bool needs_tangent = readsLocation(program, 5) || readsLocation(program, 6);
        const bool needs_normal = readsLocation(program, 1) || needs_tangent;
        const bool invalid_vertex = !attribute(0, rdesc::ETextureFormat::RGB32_SFLOAT) ||
                                    (readsLocation(program, 3) && !attribute(1, rdesc::ETextureFormat::RG32_SFLOAT)) ||
                                    (needs_normal && !attribute(2, rdesc::ETextureFormat::RGB32_SFLOAT)) ||
                                    (needs_tangent && !attribute(3, rdesc::ETextureFormat::RGBA32_SFLOAT));
        if (invalid_vertex)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        const auto key = [&](std::string_view local) { return std::string(prefix) + std::string(local); };
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
        buffer(claims.storage.scene, "alive", sizeof(std::uint64_t), kMeshInstanceStorageType, config.instance_storage);
        if (visible_capacity)
        {
            const bool invalid_capacity = visible_capacity > UINT32_MAX - 68 || !view_count;
            const bool oversized =
                !invalid_capacity &&
                meshVisibleStride(visible_capacity) / 4 > UINT32_MAX / (std::uint64_t(view_count) * kMeshWindingCount);
            if (invalid_capacity || oversized)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            const auto stream_count = std::uint64_t(view_count) * kMeshWindingCount;
            FeaturePort members{
                key("members"),
                EFeaturePort::INPUT,
                GraphBufferDesc{meshVisibleStride(visible_capacity) * stream_count}
            };
            members.semantic = kMeshVisibleMembers;
            claims.resources.push_back(std::move(members));
            claims.resources.push_back({key("commands"), EFeaturePort::INPUT, GraphBufferDesc{stream_count * 20}});
            claims.resources.push_back({key("counts"), EFeaturePort::INPUT, GraphBufferDesc{stream_count * 4}});
        }
        else
        {
            claims.resources.push_back(
                {key("members"),
                 EFeaturePort::OUTPUT,
                 GraphBufferDesc{24},
                 EGraphOrigin::IMPORTED,
                 {GraphBufferRange{0, 24}}}
            );
        }
        if (const auto* layout = config.program.parameterLayout())
        {
            buffer(claims.storage.global, "material", layout->size, kMaterialStorageType, config.material_storage);
        }
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
        return {};
    }

    void bindVisibleMeshDraw(
        RasterWork& work,
        FeatureBuildContext& context,
        std::string_view prefix,
        std::uint32_t capacity,
        std::uint32_t view,
        std::uint32_t winding
    ) noexcept
    {
        const auto stream = std::uint64_t(view) * kMeshWindingCount + winding;
        auto& members = *std::ranges::find(work.interface.resources, "members", &ResourceParameter::shader_name);
        const auto size = meshVisibleStride(capacity);
        members.access.range = GraphBufferRange{size * stream, size};
        auto& draw = std::get<DrawIndexedIndirect>(work.draw);
        draw.commands_resource = context.resource(std::string(prefix) + "commands");
        draw.commands_offset = stream * 20;
        draw.count_resource = context.resource(std::string(prefix) + "counts");
        draw.count_offset = stream * 4;
    }

    namespace
    {
        RenderResult<rdesc::SpirvArtifact> buildMeshVertexShader(
            const rdesc::MaterialShader* use,
            bool paged,
            bool reflected
        ) noexcept
        {
            const bool tangents = readsLocation(use, 5) || readsLocation(use, 6);
            const bool normals = readsLocation(use, 1) || tangents;
            const bool depth_only = !use;
            const bool uv = readsLocation(use, 3);
            rdesc::SpirvArtifact shader;
            std::span<const std::uint32_t> vertex;
            std::span<const std::byte> reflection;
            const auto select = [&](auto& words, auto& info, auto& reflected_words, auto& reflected_info)
            {
                vertex =
                    reflected ? std::span<const std::uint32_t>(reflected_words) : std::span<const std::uint32_t>(words);
                reflection = reflected ? std::span<const std::byte>(reflected_info) : std::span<const std::byte>(info);
            };
            if (paged)
            {
                if (depth_only)
                {
                    select(
                        kDepthOnlyPagedVertex,
                        kDepthOnlyPagedVertexReflection,
                        kDepthOnlyPagedReflectedVertex,
                        kDepthOnlyPagedReflectedVertexReflection
                    );
                }
                else if (!uv && tangents)
                {
                    select(
                        kTangentMeshNoUvPagedVertex,
                        kTangentMeshNoUvPagedVertexReflection,
                        kTangentMeshNoUvPagedReflectedVertex,
                        kTangentMeshNoUvPagedReflectedVertexReflection
                    );
                }
                else if (!uv && normals)
                {
                    select(
                        kSurfaceMeshNoUvPagedVertex,
                        kSurfaceMeshNoUvPagedVertexReflection,
                        kSurfaceMeshNoUvPagedReflectedVertex,
                        kSurfaceMeshNoUvPagedReflectedVertexReflection
                    );
                }
                else if (!uv)
                {
                    select(
                        kBasicMeshNoUvPagedVertex,
                        kBasicMeshNoUvPagedVertexReflection,
                        kBasicMeshNoUvPagedReflectedVertex,
                        kBasicMeshNoUvPagedReflectedVertexReflection
                    );
                }
                else if (tangents)
                {
                    select(
                        kTangentMeshPagedVertex,
                        kTangentMeshPagedVertexReflection,
                        kTangentMeshPagedReflectedVertex,
                        kTangentMeshPagedReflectedVertexReflection
                    );
                }
                else if (normals)
                {
                    select(
                        kSurfaceMeshPagedVertex,
                        kSurfaceMeshPagedVertexReflection,
                        kSurfaceMeshPagedReflectedVertex,
                        kSurfaceMeshPagedReflectedVertexReflection
                    );
                }
                else
                {
                    select(
                        kBasicMeshPagedVertex,
                        kBasicMeshPagedVertexReflection,
                        kBasicMeshPagedReflectedVertex,
                        kBasicMeshPagedReflectedVertexReflection
                    );
                }
            }
            else
            {
                if (depth_only)
                {
                    select(
                        kDepthOnlyVertex,
                        kDepthOnlyVertexReflection,
                        kDepthOnlyReflectedVertex,
                        kDepthOnlyReflectedVertexReflection
                    );
                }
                else if (!uv && tangents)
                {
                    select(
                        kTangentMeshNoUvVertex,
                        kTangentMeshNoUvVertexReflection,
                        kTangentMeshNoUvReflectedVertex,
                        kTangentMeshNoUvReflectedVertexReflection
                    );
                }
                else if (!uv && normals)
                {
                    select(
                        kSurfaceMeshNoUvVertex,
                        kSurfaceMeshNoUvVertexReflection,
                        kSurfaceMeshNoUvReflectedVertex,
                        kSurfaceMeshNoUvReflectedVertexReflection
                    );
                }
                else if (!uv)
                {
                    select(
                        kBasicMeshNoUvVertex,
                        kBasicMeshNoUvVertexReflection,
                        kBasicMeshNoUvReflectedVertex,
                        kBasicMeshNoUvReflectedVertexReflection
                    );
                }
                else if (tangents)
                {
                    select(
                        kTangentMeshVertex,
                        kTangentMeshVertexReflection,
                        kTangentMeshReflectedVertex,
                        kTangentMeshReflectedVertexReflection
                    );
                }
                else if (normals)
                {
                    select(
                        kSurfaceMeshVertex,
                        kSurfaceMeshVertexReflection,
                        kSurfaceMeshReflectedVertex,
                        kSurfaceMeshReflectedVertexReflection
                    );
                }
                else
                {
                    select(
                        kBasicMeshVertex,
                        kBasicMeshVertexReflection,
                        kBasicMeshReflectedVertex,
                        kBasicMeshReflectedVertexReflection
                    );
                }
            }
            shader.stages.push_back({rdesc::EShaderType::VERTEX, "main", {vertex.begin(), vertex.end()}});
            if (!rdesc::ShaderInfo::deserialize(reflection, shader.interface))
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            return shader;
        }
    } // namespace

    RenderResult<RasterWork> buildMeshGroup(
        const RasterMeshGroup& config,
        std::string_view prefix,
        rdesc::EMaterialPurpose purpose,
        bool paged,
        FeatureBuildContext& context,
        bool derive_members,
        bool reflected
    ) noexcept
    {
        const auto resource = [&](std::string_view local)
        { return context.resource(std::string(prefix) + std::string(local)); };
        const auto* use = config.program.find(purpose);
        auto vertex_shader = buildMeshVertexShader(use, paged, reflected);
        if (!vertex_shader)
        {
            return cxx::unexpected(vertex_shader.error());
        }
        auto shader = std::move(*vertex_shader);
        RasterWork draw;
        draw.interface.parameters.instance_layout = meshViewParameterBlock();
        draw.interface.parameters.values.resize(sizeof(MeshViewParameters));
        draw.interface.parameters.fields = {{"view", {}, "view"}, {"projection", {}, "projection"}};
        draw.interface.resources = {
            {"transforms", {resource("matrices")}},
            {"properties", {resource("properties")}},
            {"alive", {resource("alive")}},
            {"members", {resource("members")}}
        };
        draw.graphics.vertex_bindings = {{0, config.geometry.vertex_stride}};
        for (const auto& input : shader.interface.vertex_inputs)
        {
            const auto& attribute =
                *std::ranges::find(config.geometry.attributes, input.location, &MeshAttribute::location);
            draw.graphics.vertex_attributes.push_back({attribute.location, 0, attribute.offset, attribute.format});
        }
        draw.graphics.depth_test = draw.graphics.depth_write = true;
        draw.graphics.cull_mode = config.program.double_sided ? ECullMode::NONE : ECullMode::BACK;
        draw.graphics.front_counter_clockwise = config.front_counter_clockwise != reflected;
        draw.vertex_buffers = {{resource("vertices"), 0, 0}};
        draw.draw = DrawIndexedIndirect{
            resource("indices"),
            0,
            config.geometry.index_stride == 2 ? EIndexType::UINT16 : EIndexType::UINT32,
            resource("members")
        };
        const auto argument = [&](std::string_view local, auto binding)
        {
            if (!reflected)
            {
                context.arguments.push_back(StorageArgument::make(resource(local).resource, std::move(binding)));
            }
        };
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
        argument("matrices", MeshInstanceBinding{EMeshInstanceBinding::TRANSFORMS});
        argument("properties", MeshInstanceBinding{EMeshInstanceBinding::PROPERTIES});
        argument("alive", MeshInstanceBinding{EMeshInstanceBinding::ALIVE});
        if (derive_members && !reflected)
        {
            auto configuration = ConfigValue::make(
                MeshDrawConfiguration{resource("matrices").resource, config.group, resource("members").resource}
            );
            if (!configuration)
            {
                return cxx::unexpected(configuration.error());
            }
            context.bindings.push_back(
                {{resource("members").resource}, std::move(*configuration), &RasterMeshDraws::create}
            );
        }
        if (use)
        {
            shader.stages.insert(shader.stages.end(), use->shader.stages.begin(), use->shader.stages.end());
            shader.interface.entry_points.insert(
                shader.interface.entry_points.end(),
                use->shader.interface.entry_points.begin(),
                use->shader.interface.entry_points.end()
            );
            for (const auto& set : use->shader.interface.sets)
            {
                for (const auto& binding : set.bindings)
                {
                    const bool table = binding.type == rdesc::EDescriptorType::STORAGE_BUFFER &&
                                       binding.name == rdesc::kMaterialTableName;
                    const bool images = binding.type == rdesc::EDescriptorType::SAMPLED_IMAGE ||
                                        binding.type == rdesc::EDescriptorType::SAMPLER;
                    if (!table && !images)
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
                    draw.interface.resources.push_back({binding.name, {resource(table ? "material" : "texture0")}});
                }
            }
            if (const auto* layout = config.program.parameterLayout())
            {
                argument("material", MaterialBinding{config.group.material_table_source, *layout});
            }
            for (std::uint32_t i = 0; i < config.textures.size(); ++i)
            {
                argument("texture" + std::to_string(i), MaterialBinding{config.group.material_table_source, {}, i});
            }
        }
        auto program = context.graph.addProgram(std::move(shader));
        if (!program)
        {
            return cxx::unexpected(program.error());
        }
        draw.interface.program = *program;
        return draw;
    }

    template <bool Depth> RenderResult<void> describe(const void* value, std::string_view, FeaturePorts& ports) noexcept
    {
        const auto& config = *static_cast<const MeshFeatureConfig*>(value);
        if (auto valid = config.target.validate(); !valid)
        {
            return valid;
        }
        const bool supported = config.target.format == rdesc::ETextureFormat::RGBA8_UNORM ||
                               config.target.format == rdesc::ETextureFormat::RGBA8_SRGB;
        if (!supported)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        auto result = describeMeshGroup(
            config.mesh,
            {},
            Depth ? rdesc::EMaterialPurpose::SHADOW : rdesc::EMaterialPurpose::FORWARD,
            ports
        );
        if (!result)
        {
            return result;
        }
        GraphImageDesc color{config.target.width, config.target.height, 1, 1, config.target.format};
        GraphImageDesc depth{config.target.width, config.target.height, 1, 1, rdesc::ETextureFormat::D32_SFLOAT};
        color.extent.source = depth.extent.source = EImageExtentSource::TARGET;
        ports.resources.push_back({"color", EFeaturePort::OUTPUT, color});
        ports.resources.push_back({"depth", EFeaturePort::OUTPUT, depth});
        return {};
    }

    template <bool Depth, bool Paged>
    RenderResult<void> build(const void* value, std::string_view key, FeatureBuildContext& context) noexcept
    {
        const auto& config = *static_cast<const MeshFeatureConfig*>(value);
        const auto purpose = Depth ? rdesc::EMaterialPurpose::SHADOW : rdesc::EMaterialPurpose::FORWARD;
        auto depth = context.resource("depth");
        auto color = context.resource("color");
        const auto name = Depth ? detail::MeshDepthRaster::name : detail::MaterialMeshRaster::name;
        for (unsigned winding = 0; winding < 2; ++winding)
        {
            auto draw = buildMeshGroup(config.mesh, {}, purpose, Paged, context, true, winding != 0);
            if (!draw)
            {
                return cxx::unexpected(draw.error());
            }
            const auto load = winding == 0 ? EAttachmentLoad::CLEAR : EAttachmentLoad::LOAD;
            draw->depth = RasterAttachment{{depth}, load};
            if constexpr (!Depth)
            {
                draw->colors.push_back({{color}, load});
            }
            auto pass =
                context.graph.addRasterPass(featureKey(name, key, "draw" + std::to_string(winding)), std::move(*draw));
            if (!pass)
            {
                return cxx::unexpected(pass.error());
            }
            depth = pass->written.back();
            if constexpr (!Depth)
            {
                color = pass->written.front();
            }
        }
        if (auto result = context.publish("depth", depth); !result)
        {
            return result;
        }
        if constexpr (Depth)
        {
            auto cleared = context.graph.addTransferPass(
                featureKey(name, key, "clear"),
                TransferWork{{ClearImage{{context.resource("color")}}}}
            );
            if (!cleared)
            {
                return cxx::unexpected(cleared.error());
            }
            return context.publish("color", cleared->written.front());
        }
        else
        {
            return context.publish("color", color);
        }
    }

    template RenderResult<void> describe<false>(const void*, std::string_view, FeaturePorts&) noexcept;
    template RenderResult<void> describe<true>(const void*, std::string_view, FeaturePorts&) noexcept;
    template RenderResult<void> build<false, false>(const void*, std::string_view, FeatureBuildContext&) noexcept;
    template RenderResult<void> build<false, true>(const void*, std::string_view, FeatureBuildContext&) noexcept;
    template RenderResult<void> build<true, false>(const void*, std::string_view, FeatureBuildContext&) noexcept;
    template RenderResult<void> build<true, true>(const void*, std::string_view, FeatureBuildContext&) noexcept;
} // namespace lux::render::detail
