#include "MeshDraws.hpp"
#include <algorithm>
#include <cstdint>

// glslang's --vn output uses uint32_t without including its declaration.
#include "BasicMeshPagedVertex.hpp"
#include "BasicMeshVertex.hpp"
#include "DepthOnlyPagedVertex.hpp"
#include "DepthOnlyVertex.hpp"
#include <lux/engine/render/features/mesh/MeshFeature.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshFeature.hpp>

namespace lux::render::vulkan
{
    namespace
    {
        template <bool Depth>
        RenderResult<ProgramContribution> programs(
            const void* value,
            std::string_view instance,
            const RenderGraphDefinition& graph,
            const EnabledDeviceFeatures& enabled
        ) noexcept
        {
            const auto& config = *static_cast<const MeshFeatureConfig*>(value);
            const auto type = Depth ? DepthOnlyFeature::name : BasicMeshFeature::name;
            const auto key = [&](std::string_view local) { return featureKey(type, instance, local); };
            const auto resource = [&](std::string_view local) { return "shared:" + key(local); };
            ProgramContribution result;
            PassProgram draw{key("draw")};
            const auto vertex = enabled.buffer_device_address
                                    ? (Depth ? std::span<const std::uint32_t>(kDepthOnlyPagedVertex)
                                             : std::span<const std::uint32_t>(kBasicMeshPagedVertex))
                                    : (Depth ? std::span<const std::uint32_t>(kDepthOnlyVertex)
                                             : std::span<const std::uint32_t>(kBasicMeshVertex));
            draw.shader.stages.push_back({rdesc::EShaderType::VERTEX, "main", {vertex.begin(), vertex.end()}});
            const auto reflection = enabled.buffer_device_address
                                        ? (Depth ? std::span<const std::byte>(kDepthOnlyPagedVertexReflection)
                                                 : std::span<const std::byte>(kBasicMeshPagedVertexReflection))
                                        : (Depth ? std::span<const std::byte>(kDepthOnlyVertexReflection)
                                                 : std::span<const std::byte>(kBasicMeshVertexReflection));
            if (!rdesc::ShaderInfo::deserialize(reflection, draw.shader.interface))
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            draw.context_parameters = meshViewParameterBlock();
            draw.parameter_values.resize(128);
            draw.parameters = {
                {"view", "", "view", EParameterSource::CONTEXT},
                {"projection", "", "projection", EParameterSource::CONTEXT}
            };
            draw.resources = {
                {"transforms", resource("matrices")},
                {"properties", resource("properties")},
                {"alive", resource("alive")},
                {"members", resource("members")}
            };
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
            draw.vertex_buffers = {{resource("vertices"), 0, 0}};
            draw.draw = DrawIndexedIndirect{
                resource("indices"),
                0,
                config.mesh.description.index_stride == 2 ? EIndexType::UINT16 : EIndexType::UINT32,
                resource("members")
            };
            const auto argument = [&](std::string_view local, auto binding) -> RenderResult<void>
            {
                result.arguments.push_back(StorageArgument::make(resource(local), std::move(binding)));
                return {};
            };
            if (auto added = argument("vertices", config.mesh); !added)
            {
                return cxx::unexpected(added.error());
            }
            auto indices = config.mesh;
            indices.buffer = EMeshBuffer::INDICES;
            if (auto added = argument("indices", std::move(indices)); !added)
            {
                return cxx::unexpected(added.error());
            }
            result.arguments.push_back(
                StorageArgument::make(resource("matrices"), MeshInstanceBinding{EMeshInstanceBinding::TRANSFORMS})
            );
            result.arguments.push_back(
                StorageArgument::make(resource("properties"), MeshInstanceBinding{EMeshInstanceBinding::PROPERTIES})
            );
            result.arguments.push_back(
                StorageArgument::make(resource("alive"), MeshInstanceBinding{EMeshInstanceBinding::ALIVE})
            );
            const auto find = [&](std::string_view name) noexcept -> GraphResourceId
            {
                const auto found = std::ranges::find(graph.resources(), resource(name), &GraphResource::key);
                return found == graph.resources().end()
                           ? GraphResourceId{}
                           : GraphResourceId{static_cast<std::uint32_t>(found - graph.resources().begin())};
            };
            auto configuration = ConfigValue::make(MeshDrawConfiguration{find("matrices"), config.group});
            if (!configuration)
            {
                return cxx::unexpected(configuration.error());
            }
            result.contexts.push_back({find("members"), std::move(*configuration), &MeshDraws::create});
            if constexpr (!Depth)
            {
                // describe already qualified the typed cooked contract. Native reflection checks the actual code ABI.
                const auto& material = config.material.program;
                draw.shader.stages.push_back(material.fragment);
                draw.shader.interface.entry_points.insert(
                    draw.shader.interface.entry_points.end(),
                    material.interface.entry_points.begin(),
                    material.interface.entry_points.end()
                );
                // Project the actual cooked descriptor facts; LayoutPlan still assigns final locations.
                for (const auto& set : material.interface.sets)
                {
                    for (const auto& binding : set.bindings)
                    {
                        const bool is_table =
                            binding.type == rdesc::EDescriptorType::STORAGE_BUFFER && binding.name == "uMats";
                        const bool is_images = binding.type == rdesc::EDescriptorType::COMBINED_IMAGE_SAMPLER;
                        if (!is_table && !is_images)
                        {
                            return cxx::unexpected(RenderError{kFeatureContract});
                        }
                        auto destination = std::ranges::find(
                            draw.shader.interface.sets,
                            binding.set,
                            &rdesc::DescriptorSetLayoutInfo::set
                        );
                        if (destination == draw.shader.interface.sets.end())
                        {
                            draw.shader.interface.sets.emplace_back(binding.set);
                            destination = std::prev(draw.shader.interface.sets.end());
                        }
                        destination->bindings.push_back(binding);
                        draw.resources.push_back({binding.name, resource(is_table ? "material" : "texture0")});
                    }
                }
                if (auto added = argument("material", config.material); !added)
                {
                    return cxx::unexpected(added.error());
                }
                if (!config.textures.empty())
                {
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
