#include <algorithm>
#include <lux/engine/render/extensions/GraphCommands.hpp>
#include <lux/engine/render/extensions/storage/StorageBinding.hpp>
#include <lux/engine/render/graph/GraphCompiler.hpp>
#include <lux/engine/render/runtime/EffectCompilation.hpp>
#include <map>
#include <ranges>

namespace lux::render::runtime_detail
{
    RenderResult<RenderEffect> compileEffect(
        nri::Device& device,
        const nri::CoreInterface& nri_core,
        const detail::EnabledDeviceFeatures& enabled,
        std::span<const nri::FormatSupportBits> formats,
        std::array<std::uint32_t, 2> vertex_limits,
        std::array<const detail::QueueProperties*, 3> queues,
        std::uint32_t sampled_image_capacity,
        std::uint32_t sampler_capacity,
        std::span<const StorageNameView> storage_types,
        const GraphEffectRequest& request,
        std::span<const FeatureEntry* const> registrations,
        std::size_t max_config_bytes,
        std::vector<RenderEffect::StorageDependency> storage_dependencies
    ) noexcept
    {
        const auto* input = &request;
        const auto count = input->definition.features.size();
        std::vector<FeatureBuildInput> selected;
        selected.reserve(count);
        for (std::size_t i = 0; i < input->definition.features.size(); ++i)
        {
            const auto& selection = input->definition.features[i];
            const auto* registration = registrations[i];
            selected.push_back(
                {FeatureName{registration->name},
                 registration->config_type,
                 &registration->graph,
                 selection.config ? &selection.config : nullptr,
                 selection.contribution_key}
            );
        }
        auto assembly = composeFeatures(
            selected,
            max_config_bytes,
            input->definition.connections,
            input->definition.outputs,
            input->definition.inputs
        );
        if (!assembly)
        {
            return cxx::unexpected(assembly.error());
        }
        // Installation owns these immutable names. Only this request's cold input is borrowed;
        // the Worker never searches the mutable Registry or acquires another registry owner.
        std::vector<bool> live_resources(assembly->definition.resources().size(), false);
        for (const auto& lifetime : assembly->analysis.lifetimes)
        {
            live_resources[lifetime.version.resource.index] = true;
        }
        for (auto parameter : assembly->analysis.parameter_inputs)
        {
            live_resources[parameter.index] = true;
        }
        // Keep preparation-only inputs of live outputs, just as Attach does. Dead contributions
        // cannot introduce allocations or conflicts into the adopted dependency set.
        for (const auto& binding : assembly->bindings | std::views::reverse)
        {
            if (std::ranges::any_of(binding.outputs, [&](auto id) { return live_resources[id.index]; }))
            {
                for (auto resource : binding.inputs)
                {
                    live_resources[resource.index] = true;
                }
            }
        }
        const auto public_count = storage_dependencies.size();
        std::vector<bool> used_types(public_count);
        const auto resolve = [&](auto& group, ECapabilityInputScope scope) noexcept -> RenderResult<void>
        {
            for (auto& binding : group)
            {
                if (!live_resources[binding.resource.index])
                {
                    continue;
                }
                auto& requirement = binding.storage;
                if (requirement.key.empty())
                {
                    requirement.key = assembly->definition.resources()[binding.resource.index].key;
                }
                const auto name = requirement.type.name();
                const auto factory = std::lower_bound(
                    storage_types.begin(),
                    storage_types.end(),
                    name,
                    [](StorageNameView type, std::string_view name) { return type.name() < name; }
                );
                if (factory == storage_types.end() || factory->name() != name)
                {
                    return cxx::unexpected(RenderError{kStorageFactory});
                }
                for (std::size_t i = 0; i < public_count; ++i)
                {
                    used_types[i] = used_types[i] || storage_dependencies[i].requirement.type == requirement.type;
                }
                auto dependency = std::ranges::find_if(
                    storage_dependencies,
                    [&](const auto& value) noexcept
                    {
                        return value.scope == scope && value.requirement.type == requirement.type &&
                               value.requirement.key == requirement.key;
                    }
                );
                if (dependency == storage_dependencies.end())
                {
                    storage_dependencies.push_back({scope, std::move(requirement), {binding.resource}, {}});
                }
                else
                {
                    if (dependency->requirement.config != requirement.config)
                    {
                        return cxx::unexpected(RenderError{kStorageMismatch});
                    }
                    dependency->resources.push_back(binding.resource);
                }
            }
            return {};
        };
        auto& external = assembly->external_bindings;
        const std::array groups{&external.global, &external.scene, &external.view, &external.attachment};
        for (std::size_t i = 0; i < groups.size(); ++i)
        {
            if (auto resolved = resolve(*groups[i], static_cast<ECapabilityInputScope>(i)); !resolved)
            {
                return cxx::unexpected(resolved.error());
            }
        }
        for (std::size_t i = 0; i < public_count; ++i)
        {
            // An upload-only input may legitimately be unused in an empty Effect. If its Storage
            // type is consumed, however, the public destination must be one of those actual bindings.
            if (used_types[i] && storage_dependencies[i].resources.empty())
            {
                return cxx::unexpected(RenderError{kStorageMismatch});
            }
        }
        auto compiled = detail::GraphCompiler::compile(
            device,
            nri_core,
            enabled,
            formats,
            vertex_limits,
            queues,
            sampled_image_capacity,
            sampler_capacity,
            assembly->definition,
            assembly->analysis,
            &input->description
        );
        if (!compiled)
        {
            return cxx::unexpected(compiled.error());
        }
        return RenderEffect{
            std::make_unique<detail::RenderPlan>(std::move(*compiled)),
            std::move(assembly->definition),
            std::move(assembly->analysis),
            std::move(storage_dependencies),
            std::move(assembly->arguments),
            std::move(assembly->bindings),
            std::move(assembly->samplers)
        };
    }

} // namespace lux::render::runtime_detail
