#include <lux/engine/render/features/mesh/RasterMesh.hpp>

#include <algorithm>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/description/GBufferLayout.hpp>
#include <lux/engine/render/extensions/graph/GraphError.hpp>

#include "MeshVertexDxil.hpp"
#include "MeshVertexSpirv.hpp"

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
            std::size_t mask = 0;
            if (use)
            {
                for (const auto& entry : use->shader.interface.entry_points)
                {
                    for (const auto& input : entry.inputs)
                    {
                        const auto location = std::ranges::find(kMeshDxilVaryingLocations, input.semantic_index);
                        if (input.semantic != "TEXCOORD" || location == kMeshDxilVaryingLocations.end())
                        {
                            return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                        }
                        mask |= std::size_t{1} << (location - kMeshDxilVaryingLocations.begin());
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
            auto layout = EMeshVertexLayout::DEPTH_ONLY;
            if (use)
            {
                const bool tangents = readsVarying(use, 5) || readsVarying(use, 6);
                const bool normals = readsVarying(use, 1) || tangents;
                const bool uv = readsVarying(use, 3);
                if (tangents)
                {
                    layout = uv ? EMeshVertexLayout::TANGENT : EMeshVertexLayout::TANGENT_NO_UV;
                }
                else if (normals)
                {
                    layout = uv ? EMeshVertexLayout::SURFACE : EMeshVertexLayout::SURFACE_NO_UV;
                }
                else
                {
                    layout = uv ? EMeshVertexLayout::BASIC : EMeshVertexLayout::BASIC_NO_UV;
                }
            }
            auto shader = meshVertexSpirv({layout, paged, shadow, reflected}).load(target);
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

    RenderResult<MeshRasterLayout>
    meshRasterLayout(
        const MeshDescription& mesh,
        const rdesc::MaterialProgramSet& material,
        const rdesc::ShaderInfo& shader,
        MeshRasterWinding winding
    ) noexcept
    {
        MeshRasterLayout layout;
        auto& state = layout.graphics;
        state.vertex_bindings = {{0, mesh.vertex_stride}};
        for (const auto& input : shader.vertex_inputs)
        {
            // The built-in vertex shader declares TEXCOORD indices matching the mesh attribute slots.
            const auto mesh_location = material.target == rdesc::EShaderTarget::DXIL ? input.semantic_index
                                                                                   : input.location;
            const auto attribute = std::ranges::find(mesh.attributes, mesh_location, &GeometryAttribute::location);
            if (attribute == mesh.attributes.end())
            {
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            }
            layout.geometry_locations.push_back(mesh_location);
            state.vertex_attributes.push_back({input.location, 0, attribute->offset, attribute->format});
        }
        state.depth_test = state.depth_write = true;
        state.cull_mode = material.double_sided ? ECullMode::NONE : ECullMode::BACK;
        // Mesh packets use the bottom-left viewport convention. D3D12's flipped viewport
        // reverses which framebuffer orientation is treated as front-facing.
        const bool flip_viewport_winding = material.target == rdesc::EShaderTarget::DXIL;
        state.front_counter_clockwise = (winding.front_counter_clockwise != winding.reflected) != flip_viewport_winding;
        return layout;
    }

    std::vector<RasterPassTemplate::Input> meshRasterInputs(bool record_table)
    {
        std::vector<RasterPassTemplate::Input> inputs;
        inputs.reserve(kMeshRasterInputNames.size() + 5);
        for (const auto name : kMeshRasterInputNames)
        {
            inputs.push_back({std::string{name}, rdesc::EDescriptorType::STORAGE_BUFFER, EGraphUse::STORAGE_READ, 1});
        }
        if (record_table)
        {
            inputs.push_back({
                .name = std::string{rdesc::kMaterialTableName}, .kind = rdesc::EDescriptorType::STORAGE_BUFFER,
                .usage = EGraphUse::STORAGE_READ, .stages = 2, .shared_records = true
            });
        }
        inputs.push_back({"uTex", rdesc::EDescriptorType::SAMPLED_IMAGE, EGraphUse::SAMPLED, 2});
        inputs.push_back({"uTexSamplers", rdesc::EDescriptorType::SAMPLER, EGraphUse::SAMPLED, 2});
        inputs.push_back({"vertices", {}, EGraphUse::VERTEX});
        inputs.push_back({"indices", {}, EGraphUse::INDEX});
        return inputs;
    }
} // namespace lux::render::detail
