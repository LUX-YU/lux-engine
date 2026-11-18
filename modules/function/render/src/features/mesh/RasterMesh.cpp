#include <lux/engine/render/features/BuiltinFeatures.hpp>
#include <lux/engine/render/features/mesh/RasterMesh.hpp>
#include <lux/engine/render/features/mesh/RasterMeshParameters.hpp>

#include <algorithm>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/render/extensions/features/FeatureAssembly.hpp>
#include <lux/engine/render/resources/mesh/Binding.hpp>
#include <lux/engine/render/resources/material/Binding.hpp>
#include <lux/engine/render/features/mesh/MeshVisibilityInputs.hpp>
#include <lux/engine/render/features/mesh/RasterMeshDraws.hpp>
#include <lux/engine/render/resource/view/CameraParameters.hpp>

#include "BasicMeshNoUvPagedReflectedVertex.hpp"
#include "BasicMeshNoUvPagedVertex.hpp"
#include "BasicMeshNoUvReflectedVertex.hpp"
#include "BasicMeshNoUvVertex.hpp"
#include "BasicMeshPagedReflectedVertex.hpp"
#include "BasicMeshPagedVertex.hpp"
#include "BasicMeshReflectedVertex.hpp"
#include "MeshVertexDxil.hpp"
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

#include "BasicMeshNoUvPagedShadowReflectedVertex.hpp"
#include "BasicMeshNoUvPagedShadowVertex.hpp"
#include "BasicMeshNoUvShadowReflectedVertex.hpp"
#include "BasicMeshNoUvShadowVertex.hpp"
#include "BasicMeshPagedShadowReflectedVertex.hpp"
#include "BasicMeshPagedShadowVertex.hpp"
#include "BasicMeshShadowReflectedVertex.hpp"
#include "BasicMeshShadowVertex.hpp"
#include "DepthOnlyPagedShadowReflectedVertex.hpp"
#include "DepthOnlyPagedShadowVertex.hpp"
#include "DepthOnlyShadowReflectedVertex.hpp"
#include "DepthOnlyShadowVertex.hpp"
#include "SurfaceMeshNoUvPagedShadowReflectedVertex.hpp"
#include "SurfaceMeshNoUvPagedShadowVertex.hpp"
#include "SurfaceMeshNoUvShadowReflectedVertex.hpp"
#include "SurfaceMeshNoUvShadowVertex.hpp"
#include "SurfaceMeshPagedShadowReflectedVertex.hpp"
#include "SurfaceMeshPagedShadowVertex.hpp"
#include "SurfaceMeshShadowReflectedVertex.hpp"
#include "SurfaceMeshShadowVertex.hpp"
#include "TangentMeshNoUvPagedShadowReflectedVertex.hpp"
#include "TangentMeshNoUvPagedShadowVertex.hpp"
#include "TangentMeshNoUvShadowReflectedVertex.hpp"
#include "TangentMeshNoUvShadowVertex.hpp"
#include "TangentMeshPagedShadowReflectedVertex.hpp"
#include "TangentMeshPagedShadowVertex.hpp"
#include "TangentMeshShadowReflectedVertex.hpp"
#include "TangentMeshShadowVertex.hpp"

namespace lux::render::detail
{
    namespace
    {
        bool readsVarying(const rdesc::MaterialShader* program, std::uint32_t index) noexcept
        {
            if (!program)
            {
                return false;
            }
            for (const auto& entry : program->shader.interface.entry_points)
            {
                if (std::ranges::find(entry.inputs, index, &rdesc::ShaderIoVariable::location) != entry.inputs.end())
                {
                    return true;
                }
            }
            return false;
        }
    } // namespace

    namespace
    {
        RenderResult<rdesc::ShaderProgramArtifact>
        buildDxilMeshVertex(const rdesc::MaterialShader* use, bool shadow, bool reflected) noexcept
        {
            constexpr std::array<std::uint32_t, 6> locations{0, 1, 3, 4, 5, 6};
            std::size_t mask = 0;
            if (use)
            {
                for (const auto& entry : use->shader.interface.entry_points)
                {
                    for (const auto& input : entry.inputs)
                    {
                        const auto location = std::ranges::find(locations, input.semantic_index);
                        if (input.semantic != "TEXCOORD" || location == locations.end())
                        {
                            return cxx::unexpected(RenderError{kFeatureContract});
                        }
                        mask |= std::size_t{1} << (location - locations.begin());
                    }
                }
            }
            const auto variant = mask * 4 + std::size_t(shadow) * 2 + std::size_t(reflected);
            return kMeshVertexDxil[variant].load(rdesc::EShaderTarget::DXIL);
        }

        RenderResult<rdesc::ShaderProgramArtifact> buildMeshVertexShader(
            const rdesc::MaterialShader* use,
            bool paged,
            bool reflected,
            bool shadow,
            rdesc::EShaderTarget target
        ) noexcept
        {
            if (target == rdesc::EShaderTarget::DXIL)
            {
                return buildDxilMeshVertex(use, shadow, reflected);
            }
            const bool tangents = readsVarying(use, 5) || readsVarying(use, 6);
            const bool normals = readsVarying(use, 1) || tangents;
            const bool depth_only = !use;
            const bool uv = readsVarying(use, 3);
            rdesc::ShaderProgramArtifact shader;
            const auto select =
                [&](const auto& normal, const auto& mirrored, const auto& depth, const auto& mirrored_depth) noexcept
            {
                shader = (shadow ? (reflected ? mirrored_depth : depth) : (reflected ? mirrored : normal)).load(target);
            };
            if (paged)
            {
                if (depth_only)
                {
                    select(
                        kDepthOnlyPagedVertex,
                        kDepthOnlyPagedReflectedVertex,
                        kDepthOnlyPagedShadowVertex,
                        kDepthOnlyPagedShadowReflectedVertex
                    );
                }
                else if (!uv && tangents)
                {
                    select(
                        kTangentMeshNoUvPagedVertex,
                        kTangentMeshNoUvPagedReflectedVertex,
                        kTangentMeshNoUvPagedShadowVertex,
                        kTangentMeshNoUvPagedShadowReflectedVertex
                    );
                }
                else if (!uv && normals)
                {
                    select(
                        kSurfaceMeshNoUvPagedVertex,
                        kSurfaceMeshNoUvPagedReflectedVertex,
                        kSurfaceMeshNoUvPagedShadowVertex,
                        kSurfaceMeshNoUvPagedShadowReflectedVertex
                    );
                }
                else if (!uv)
                {
                    select(
                        kBasicMeshNoUvPagedVertex,
                        kBasicMeshNoUvPagedReflectedVertex,
                        kBasicMeshNoUvPagedShadowVertex,
                        kBasicMeshNoUvPagedShadowReflectedVertex
                    );
                }
                else if (tangents)
                {
                    select(
                        kTangentMeshPagedVertex,
                        kTangentMeshPagedReflectedVertex,
                        kTangentMeshPagedShadowVertex,
                        kTangentMeshPagedShadowReflectedVertex
                    );
                }
                else if (normals)
                {
                    select(
                        kSurfaceMeshPagedVertex,
                        kSurfaceMeshPagedReflectedVertex,
                        kSurfaceMeshPagedShadowVertex,
                        kSurfaceMeshPagedShadowReflectedVertex
                    );
                }
                else
                {
                    select(
                        kBasicMeshPagedVertex,
                        kBasicMeshPagedReflectedVertex,
                        kBasicMeshPagedShadowVertex,
                        kBasicMeshPagedShadowReflectedVertex
                    );
                }
            }
            else
            {
                if (depth_only)
                {
                    select(
                        kDepthOnlyVertex,
                        kDepthOnlyReflectedVertex,
                        kDepthOnlyShadowVertex,
                        kDepthOnlyShadowReflectedVertex
                    );
                }
                else if (!uv && tangents)
                {
                    select(
                        kTangentMeshNoUvVertex,
                        kTangentMeshNoUvReflectedVertex,
                        kTangentMeshNoUvShadowVertex,
                        kTangentMeshNoUvShadowReflectedVertex
                    );
                }
                else if (!uv && normals)
                {
                    select(
                        kSurfaceMeshNoUvVertex,
                        kSurfaceMeshNoUvReflectedVertex,
                        kSurfaceMeshNoUvShadowVertex,
                        kSurfaceMeshNoUvShadowReflectedVertex
                    );
                }
                else if (!uv)
                {
                    select(
                        kBasicMeshNoUvVertex,
                        kBasicMeshNoUvReflectedVertex,
                        kBasicMeshNoUvShadowVertex,
                        kBasicMeshNoUvShadowReflectedVertex
                    );
                }
                else if (tangents)
                {
                    select(
                        kTangentMeshVertex,
                        kTangentMeshReflectedVertex,
                        kTangentMeshShadowVertex,
                        kTangentMeshShadowReflectedVertex
                    );
                }
                else if (normals)
                {
                    select(
                        kSurfaceMeshVertex,
                        kSurfaceMeshReflectedVertex,
                        kSurfaceMeshShadowVertex,
                        kSurfaceMeshShadowReflectedVertex
                    );
                }
                else
                {
                    select(
                        kBasicMeshVertex,
                        kBasicMeshReflectedVertex,
                        kBasicMeshShadowVertex,
                        kBasicMeshShadowReflectedVertex
                    );
                }
            }
            if (shader.stages.empty())
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            return shader;
        }
    } // namespace

    RenderResult<rdesc::ShaderProgramArtifact> buildMeshShader(
        const rdesc::MaterialProgramSet& program,
        rdesc::EMaterialPurpose purpose,
        bool paged,
        bool reflected
    ) noexcept
    {
        const auto* use = program.find(purpose);
        if (!use)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        if (purpose == rdesc::EMaterialPurpose::GBUFFER)
        {
            const bool invalid_model = program.shading_model != rdesc::ELightingTechnique::UNLIT &&
                                       program.shading_model != rdesc::ELightingTechnique::PBR_METALLIC_ROUGHNESS;
            if (invalid_model || use->output_semantic != rdesc::GBufferLayout::semantic)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
        }
        auto shader =
            buildMeshVertexShader(use, paged, reflected, purpose == rdesc::EMaterialPurpose::SHADOW, program.target);
        if (!shader)
        {
            return shader;
        }
        shader->stages.insert(shader->stages.end(), use->shader.stages.begin(), use->shader.stages.end());
        shader->interface.entry_points.insert(
            shader->interface.entry_points.end(),
            use->shader.interface.entry_points.begin(),
            use->shader.interface.entry_points.end()
        );
        for (const auto& set : use->shader.interface.sets)
        {
            for (const auto& binding : set.bindings)
            {
                const bool material_table =
                    binding.type == rdesc::EDescriptorType::STORAGE_BUFFER && binding.name == rdesc::kMaterialTableName;
                const bool sampled = binding.type == rdesc::EDescriptorType::SAMPLED_IMAGE ||
                                     binding.type == rdesc::EDescriptorType::SAMPLER;
                if (!material_table && !sampled)
                {
                    return cxx::unexpected(RenderError{kFeatureContract});
                }
                auto destination =
                    std::ranges::find(shader->interface.sets, binding.set, &rdesc::DescriptorSetLayoutInfo::set);
                if (destination == shader->interface.sets.end())
                {
                    shader->interface.sets.emplace_back(binding.set);
                    destination = std::prev(shader->interface.sets.end());
                }
                destination->bindings.push_back(binding);
            }
        }
        return shader;
    }

    RenderResult<GraphicsState> meshGraphicsState(
        std::uint32_t vertex_stride,
        std::span<const MeshAttribute> attributes,
        const rdesc::MaterialProgramSet& material,
        const rdesc::ShaderInfo& shader,
        bool front_counter_clockwise,
        bool reflected
    ) noexcept
    {
        GraphicsState state;
        state.vertex_bindings = {{0, vertex_stride}};
        for (const auto& input : shader.vertex_inputs)
        {
            // The built-in vertex shader declares TEXCOORD indices matching the mesh attribute slots.
            const auto mesh_location = material.target == rdesc::EShaderTarget::DXIL ? input.semantic_index
                                                                                   : input.location;
            const auto attribute = std::ranges::find(attributes, mesh_location, &MeshAttribute::location);
            if (attribute == attributes.end())
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            state.vertex_attributes.push_back({input.location, 0, attribute->offset, attribute->format});
        }
        state.depth_test = state.depth_write = true;
        state.cull_mode = material.double_sided ? ECullMode::NONE : ECullMode::BACK;
        // Mesh packets use the bottom-left viewport convention. D3D12's flipped viewport
        // reverses which framebuffer orientation is treated as front-facing.
        const bool flip_viewport_winding = material.target == rdesc::EShaderTarget::DXIL;
        state.front_counter_clockwise = (front_counter_clockwise != reflected) != flip_viewport_winding;
        return state;
    }

    void describeMeshDrawInputs(FeaturePorts& ports)
    {
        constexpr std::array<std::uint64_t, kMeshDrawInputPorts.size()>
            sizes{64, 64, sizeof(MeshInstanceGpuProperty), 8, 256, 20 * kMeshWindingCount};
        for (std::size_t i = 0; i < kMeshDrawInputPorts.size(); ++i)
        {
            FeaturePort port{kMeshDrawInputPorts[i], EFeaturePort::INPUT, GraphBufferDesc{sizes[i]}};
            if (i == 0)
            {
                port.semantic = kMeshDrawGroups;
            }
            if (i == 4)
            {
                port.semantic = kMeshVisibleMembers;
            }
            ports.resources.push_back(std::move(port));
        }
    }

    RasterWork meshRasterStage(FeatureBuildContext& context, bool camera_view)
    {
        RasterWork result;
        RasterPassTemplate stage;
        stage.purpose = "indexed-mesh";
        stage.inputs = {
            {"transforms", rdesc::EDescriptorType::STORAGE_BUFFER, EGraphUse::STORAGE_READ, 1},
            {"properties", rdesc::EDescriptorType::STORAGE_BUFFER, EGraphUse::STORAGE_READ, 1},
            {"alive", rdesc::EDescriptorType::STORAGE_BUFFER, EGraphUse::STORAGE_READ, 1},
            {"members", rdesc::EDescriptorType::STORAGE_BUFFER, EGraphUse::STORAGE_READ, 1},
            {std::string(rdesc::kMaterialTableName), rdesc::EDescriptorType::STORAGE_BUFFER, EGraphUse::STORAGE_READ, 2
            },
            {"uTex", rdesc::EDescriptorType::SAMPLED_IMAGE, EGraphUse::SAMPLED, 2},
            {"uTexSamplers", rdesc::EDescriptorType::SAMPLER, EGraphUse::SAMPLED, 2},
            {"vertices", {}, EGraphUse::VERTEX},
            {"indices", {}, EGraphUse::INDEX}
        };
        for (const auto name : kMeshDrawInputPorts)
        {
            ResourceAccess access{context.resource(name)};
            access.usage = std::string_view(name) == "commands" ? EGraphUse::INDIRECT : EGraphUse::STORAGE_READ;
            stage.resources.push_back(access);
        }
        stage.parameters.values.resize(sizeof(MeshViewParameters));
        if (camera_view)
        {
            stage.parameters.source = EParameterSource::VIEW;
            stage.parameters.instance_layout = cameraParameterLayout();
            stage.parameters.fields = {{"view", {}, "view"}, {"projection", {}, "projection"}};
        }
        result.program = std::move(stage);
        return result;
    }

    RenderResult<void> bindMeshDraws(
        FeatureBuildContext& context,
        GraphPassId pass,
        rdesc::EMaterialPurpose purpose,
        GraphResourceId views,
        GraphResourceId raster_states,
        bool front_counter_clockwise
    ) noexcept
    {
        MeshDrawConfiguration value;
        value.pass = pass;
        value.purpose = purpose;
        value.front_counter_clockwise = front_counter_clockwise;
        value.views = views;
        value.raster_states = raster_states;
        if (views.index != kGraphInvalidIndex)
        {
            value.depth = context.resource("depth").resource;
        }
        for (std::size_t i = 0; i < kMeshDrawInputPorts.size(); ++i)
        {
            value.inputs[i] = context.resource(kMeshDrawInputPorts[i]).resource;
        }
        auto configuration = ConfigValue::make(value);
        if (!configuration)
        {
            return cxx::unexpected(configuration.error());
        }
        InstanceDataRecipe binding;
        binding.configuration = std::move(*configuration);
        binding.create = &RasterMeshDraws::create;
        binding.passes.push_back(pass);
        binding.inputs.assign(value.inputs.begin(), value.inputs.end());
        if (views.index != kGraphInvalidIndex)
        {
            binding.inputs.push_back(views);
            binding.inputs.push_back(raster_states);
        }
        else
        {
            binding.parameter_bytes = sizeof(CameraParameters);
            binding.parameter_source = EParameterSource::VIEW;
        }
        context.graph.addInstanceData(std::move(binding));
        return {};
    }

    template <bool Depth> RenderResult<void> describe(const void* value, std::string_view, FeaturePorts& ports) noexcept
    {
        const auto& config = *static_cast<const MeshFeatureConfig*>(value);
        if (auto valid = validateEffectOutput(config.output); !valid)
        {
            return valid;
        }
        const bool supported = config.output.format == rdesc::ETextureFormat::RGBA8_UNORM ||
                               config.output.format == rdesc::ETextureFormat::RGBA8_SRGB;
        if (!supported)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        describeMeshDrawInputs(ports);
        GraphImageDesc color{0, 0, 1, 1, config.output.format};
        GraphImageDesc depth{0, 0, 1, 1, rdesc::ETextureFormat::D32_SFLOAT};
        color.extent.source = depth.extent.source = EImageExtentSource::TARGET;
        ports.resources.push_back({"color", EFeaturePort::OUTPUT, color});
        ports.resources.push_back({"depth", EFeaturePort::OUTPUT, depth});
        return {};
    }

    template <bool Depth>
    RenderResult<void>
    build(const void* value, std::string_view key, FeatureBuildContext& context) noexcept
    {
        const auto& config = *static_cast<const MeshFeatureConfig*>(value);
        const auto purpose = Depth ? rdesc::EMaterialPurpose::SHADOW : rdesc::EMaterialPurpose::FORWARD;
        auto depth = context.resource("depth");
        auto color = context.resource("color");
        const auto name = Depth ? detail::MeshDepthRaster::name : detail::MaterialMeshRaster::name;
        auto draw = meshRasterStage(context);
        draw.depth = RasterAttachment{{depth}};
        if constexpr (!Depth)
        {
            draw.colors.push_back({{color}});
        }
        auto pass = context.graph.addRasterPass(featureKey(name, key, "draw"), std::move(draw));
        if (!pass)
        {
            return cxx::unexpected(pass.error());
        }
        if (auto bound = bindMeshDraws(context, pass->pass, purpose, {}, {}, config.front_counter_clockwise);
            !bound)
        {
            return bound;
        }
        depth = pass->written.back();
        if constexpr (!Depth)
        {
            color = pass->written.front();
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
    template RenderResult<void> build<false>(const void*, std::string_view, FeatureBuildContext&) noexcept;
    template RenderResult<void> build<true>(const void*, std::string_view, FeatureBuildContext&) noexcept;
} // namespace lux::render::detail
