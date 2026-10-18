#include <cstdint>

// glslang's --vn output uses uint32_t without including its declaration.
#include "BasicMeshVertex.hpp"
#include "DepthOnlyVertex.hpp"
#include <lux/engine/render/features/MeshFeature.hpp>
#include <lux/engine/render/native/vulkan/features/MeshFeature.hpp>

namespace lux::render::vulkan
{
    namespace
    {
        template <bool Depth>
        RenderResult<ProgramContribution> programs(const void* value, std::string_view instance) noexcept
        {
            const auto& config = *static_cast<const MeshFeatureConfig*>(value);
            const auto type = Depth ? DepthOnlyFeature::name : BasicMeshFeature::name;
            const auto key = [&](std::string_view local) { return featureKey(type, instance, local); };
            const auto resource = [&](std::string_view local) { return "shared:" + key(local); };
            ProgramContribution result;
            PassProgram draw{key("draw")};
            const auto vertex = Depth ? std::span<const std::uint32_t>(kDepthOnlyVertex)
                                      : std::span<const std::uint32_t>(kBasicMeshVertex);
            draw.shader.stages.push_back({rdesc::EShaderType::VERTEX, "main", {vertex.begin(), vertex.end()}});
            ShaderBlock transform{64, 16}, word{4, 4};
            transform.runtime_records = word.runtime_records = true;
            transform.fields = {{"model", rdesc::EDataScalar::FLOAT, 0, 4, 4, 0, 16}};
            word.fields = {{"value", rdesc::EDataScalar::UINT, 0, 1, 1, 0, 0}};
            draw.shader.interface.bindings = {
                {3, 7, EDescriptorKind::STORAGE_BUFFER, 1, transform, "transforms", 1},
                {2, 6, EDescriptorKind::STORAGE_BUFFER, 1, word, "members", 1}
            };
            draw.shader.interface.parameters = draw.context_parameters = meshViewParameterBlock();
            draw.shader.interface.parameter_stages = 1;
            draw.parameter_values.resize(128);
            draw.parameters = {
                {"view", "", "view", EParameterSource::CONTEXT},
                {"projection", "", "projection", EParameterSource::CONTEXT}
            };
            draw.resources = {{"transforms", resource("matrices")}, {"members", resource("members")}};
            draw.graphics.vertex_bindings = {{0, config.mesh.description.vertex_stride}};
            for (const auto& attribute : config.mesh.description.attributes)
            {
                if (attribute.location == 0 || (!Depth && attribute.location == 1))
                {
                    draw.graphics.vertex_attributes.push_back(
                        {attribute.location, 0, attribute.offset, attribute.format}
                    );
                }
            }
            draw.graphics.depth_test = draw.graphics.depth_write = true;
            draw.vertex_buffers = {{resource("mesh"), 0, 0}};
            draw.draw = DrawIndexedIndirect{
                resource("mesh"),
                config.mesh.description.index_offset,
                config.mesh.description.index_stride == 2 ? EIndexType::UINT16 : EIndexType::UINT32,
                resource("members")
            };
            const auto argument = [&](std::string_view local, auto binding) -> RenderResult<void>
            {
                auto config_value = ConfigValue::make(std::move(binding));
                if (!config_value)
                {
                    return cxx::unexpected(config_value.error());
                }
                result.arguments.push_back({resource(local), std::move(*config_value)});
                return {};
            };
            if (auto added = argument("mesh", config.mesh); !added)
            {
                return cxx::unexpected(added.error());
            }
            if (auto added = argument("matrices", config.instances); !added)
            {
                return cxx::unexpected(added.error());
            }
            auto members = config.instances;
            members.role = EMeshInstanceBinding::DRAW;
            if (auto added = argument("members", members); !added)
            {
                return cxx::unexpected(added.error());
            }
            if constexpr (!Depth)
            {
                // describe already qualified the typed cooked contract. Native reflection checks the actual code ABI.
                const auto& material = *static_cast<const MaterialProgram*>(config.material.program.data());
                draw.shader.stages.push_back(material.fragment);
                draw.shader.interface.bindings.push_back(
                    {4, 4, EDescriptorKind::STORAGE_BUFFER, 1, materialParameterBlock(material), "uMats", 2}
                );
                draw.resources.push_back({"uMats", resource("material")});
                if (auto added = argument("material", config.material); !added)
                {
                    return cxx::unexpected(added.error());
                }
                if (!config.textures.empty())
                {
                    draw.shader.interface.bindings.push_back(
                        {2, 0, EDescriptorKind::SAMPLED_IMAGE_ARRAY, 0, {}, "uTex", 2}
                    );
                    draw.resources.push_back({"uTex", resource("texture0")});
                    for (std::uint32_t i = 0; i < config.textures.size(); ++i)
                    {
                        auto texture = config.material;
                        texture.texture_slot = i;
                        if (auto added = argument("texture" + std::to_string(i), std::move(texture)); !added)
                        {
                            return cxx::unexpected(added.error());
                        }
                    }
                }
            }
            result.programs.push_back(std::move(draw));
            if constexpr (Depth)
            {
                result.programs.push_back({key("clear")});
            }
            return result;
        }
    } // namespace

} // namespace lux::render::vulkan

namespace lux::render
{
    feature_detail::Implementation FeatureProvider<BasicMeshFeature>::definition() const noexcept
    {
        return {&BasicMeshFeature::descriptor, &BasicMeshFeature::graph, vulkan::programs<false>};
    }

    feature_detail::Implementation FeatureProvider<DepthOnlyFeature>::definition() const noexcept
    {
        return {&DepthOnlyFeature::descriptor, &DepthOnlyFeature::graph, vulkan::programs<true>};
    }
} // namespace lux::render
