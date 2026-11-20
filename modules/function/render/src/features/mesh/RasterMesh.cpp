#include <lux/engine/render/features/mesh/RasterMesh.hpp>

#include <algorithm>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/description/GBufferLayout.hpp>
#include <lux/engine/render/extensions/graph/GraphError.hpp>

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
                            return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
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
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
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
            return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        }
        if (purpose == rdesc::EMaterialPurpose::GBUFFER)
        {
            const bool invalid_model = program.shading_model != rdesc::ELightingTechnique::UNLIT &&
                                       program.shading_model != rdesc::ELightingTechnique::PBR_METALLIC_ROUGHNESS;
            if (invalid_model || use->output_semantic != rdesc::GBufferLayout::semantic)
            {
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
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
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
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
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
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

    std::vector<RasterPassTemplate::Input> meshRasterInputs()
    {
        return {
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
    }
} // namespace lux::render::detail
