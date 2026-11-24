#include <lux/engine/render/device/DeviceOptions.hpp>
#include <lux/engine/render/effect/implementations/mesh_common/IndexedDraws.hpp>

#include <algorithm>
#include <utility>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/description/GBufferLayout.hpp>
#include <lux/engine/render/graph/GraphError.hpp>

#include "MeshVertexDxil.hpp"
#include "MeshVertexSpirv.hpp"

namespace lux::render::detail
{
    namespace
    {
        const MeshVertexShaders* meshVertexShadersDxil(const rdesc::MaterialShader& use) noexcept
        {
            std::size_t mask = 0;
            for (const auto& entry : use.shader.interface.entry_points)
            {
                for (const auto& input : entry.inputs)
                {
                    const auto location = std::ranges::find(kMeshDxilVaryingLocations, input.semantic_index);
                    if (input.semantic != "TEXCOORD" || location == kMeshDxilVaryingLocations.end())
                    {
                        return nullptr;
                    }
                    mask |= std::size_t{1} << (location - kMeshDxilVaryingLocations.begin());
                }
            }
            const bool shadow = use.purpose == rdesc::EMaterialPurpose::SHADOW;
            return &kMeshVertexDxil[mask * 2 + std::size_t(shadow)];
        }

        const MeshVertexShaders* meshVertexShadersSpirv(const rdesc::MaterialShader& use, bool paged) noexcept
        {
            bool normals = false, tangents = false, uv = false;
            for (const auto& entry : use.shader.interface.entry_points)
            {
                for (const auto& input : entry.inputs)
                {
                    switch (input.location)
                    {
                    case 1: normals = true; break;
                    case 3: uv = true; break;
                    case 5:
                    case 6: tangents = true; break;
                    }
                }
            }
            auto layout = uv ? EMeshVertexLayout::BASIC : EMeshVertexLayout::BASIC_NO_UV;
            if (tangents) layout = uv ? EMeshVertexLayout::TANGENT : EMeshVertexLayout::TANGENT_NO_UV;
            else if (normals) layout = uv ? EMeshVertexLayout::SURFACE : EMeshVertexLayout::SURFACE_NO_UV;
            const bool shadow = use.purpose == rdesc::EMaterialPurpose::SHADOW;
            return &meshVertexSpirv(layout, paged, shadow);
        }

        bool matchesMeshVaryings(
            const rdesc::EntryPointInfo& vertex, const rdesc::EntryPointInfo& fragment, rdesc::EShaderTarget target
        ) noexcept
        {
            for (const auto& input : fragment.inputs)
            {
                const auto output = std::ranges::find_if(vertex.outputs, [&](const auto& varying)
                {
                    if (target == rdesc::EShaderTarget::SPIRV) return varying.location == input.location;
                    return varying.semantic == input.semantic && varying.semantic_index == input.semantic_index;
                });
                if (output == vertex.outputs.end()) return false;
                const bool compatible = output->base == input.base && output->vec_size == input.vec_size &&
                    output->columns == input.columns && output->array_size == input.array_size;
                if (!compatible) return false;
            }
            return true;
        }
    } // namespace

    std::unique_ptr<MeshMaterialShader> prepareMeshShader(
        const rdesc::MaterialProgramSet& program, rdesc::MaterialShader&& use, bool paged
    ) noexcept
    {
        if (use.purpose == rdesc::EMaterialPurpose::GBUFFER)
        {
            const bool invalid_model = program.shading_model != rdesc::ELightingTechnique::UNLIT &&
                                       program.shading_model != rdesc::ELightingTechnique::PBR_METALLIC_ROUGHNESS;
            if (invalid_model || use.output_semantic != rdesc::GBufferLayout::semantic) return nullptr;
        }
        for (const auto& set : use.shader.interface.sets)
        {
            for (const auto& binding : set.bindings)
            {
                const bool records = binding.type == rdesc::EDescriptorType::STORAGE_BUFFER &&
                    binding.name == rdesc::kMaterialTableName;
                const bool sampled = binding.type == rdesc::EDescriptorType::SAMPLED_IMAGE ||
                    binding.type == rdesc::EDescriptorType::SAMPLER;
                if (!records && !sampled) return nullptr;
            }
        }
        const bool dxil = program.target == rdesc::EShaderTarget::DXIL;
        const auto* vertex = dxil ? meshVertexShadersDxil(use) : meshVertexShadersSpirv(use, paged);
        if (!vertex) return nullptr;

        const auto& normal = dxil ? (*vertex)[0].dxil : (*vertex)[0].spirv;
        const auto& reflected = dxil ? (*vertex)[1].dxil : (*vertex)[1].spirv;
        const bool valid_code = rdesc::validShaderContainer(program.target, normal.words) &&
            rdesc::validShaderContainer(program.target, reflected.words);
        if (!valid_code || !std::ranges::equal(normal.reflection, reflected.reflection)) return nullptr;
        rdesc::ShaderInfo abi;
        if (!rdesc::ShaderInfo::deserialize(normal.reflection, abi)) return nullptr;
        const bool valid_entry = abi.entry_points.size() == 1 &&
            abi.entry_points.front().stage == rdesc::EShaderType::VERTEX && !abi.entry_points.front().name.empty();
        if (!valid_entry) return nullptr;
        auto& fragment_entry = use.shader.interface.entry_points.front();
        if (!matchesMeshVaryings(abi.entry_points.front(), fragment_entry, program.target)) return nullptr;

        abi.entry_points.push_back(std::move(fragment_entry));
        for (auto& set : use.shader.interface.sets)
        {
            for (auto& binding : set.bindings)
            {
                auto destination = std::ranges::find(abi.sets, binding.set, &rdesc::DescriptorSetLayoutInfo::set);
                if (destination == abi.sets.end())
                {
                    abi.sets.emplace_back(binding.set);
                    destination = std::prev(abi.sets.end());
                }
                destination->bindings.push_back(std::move(binding));
            }
        }
        const auto cull = program.double_sided ? ECullMode::NONE : ECullMode::BACK;
        return std::make_unique<MeshMaterialShader>(MeshMaterialShader{
            .target = program.target,
            .fragment = std::move(use.shader.stages.front()),
            .vertex_words = {normal.words, reflected.words},
            .interface = std::move(abi),
            .cull_mode = cull
        });
    }

    rdesc::ShaderProgramArtifact MeshMaterialShader::artifact(bool reflected) const noexcept
    {
        rdesc::ShaderProgramArtifact result;
        result.target = target;
        result.interface = interface;
        const auto words = vertex_words[reflected];
        result.stages.reserve(2);
        result.stages.push_back({
            rdesc::EShaderType::VERTEX, interface.entry_points.front().name, {words.begin(), words.end()}
        });
        result.stages.push_back(fragment);
        return result;
    }

    RenderResult<MeshRasterLayout>
    meshRasterLayout(const MeshVertexLayout& mesh, const MeshMaterialShader& material) noexcept
    {
        MeshRasterLayout layout;
        auto& state = layout.graphics;
        state.vertex_bindings = {{0, mesh.vertex_stride}};
        const bool dxil = material.target == rdesc::EShaderTarget::DXIL;
        for (const auto& input : material.interface.vertex_inputs)
        {
            // The built-in vertex shader declares TEXCOORD indices matching the mesh attribute slots.
            const auto mesh_location = dxil ? input.semantic_index : input.location;
            const auto attribute = std::ranges::find(mesh.attributes, mesh_location, &GeometryAttribute::location);
            if (attribute == mesh.attributes.end())
            {
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            }
            layout.geometry_locations.push_back(mesh_location);
            state.vertex_attributes.push_back({input.location, 0, attribute->offset, attribute->format});
        }
        state.depth_test = state.depth_write = true;
        state.cull_mode = material.cull_mode;
        return layout;
    }

    std::vector<RasterPassTemplate::Input> meshRasterInputs(bool record_table)
    {
        static_assert(DeviceOptions{}.private_descriptor_reserve[2] >= kMeshRasterInputNames.size());
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
