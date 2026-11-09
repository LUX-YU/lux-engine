#include <algorithm>
#include <lux/engine/render/extensions/storage/StorageBinding.hpp>
#include <lux/engine/render/graph/GraphCompiler.hpp>
#include <lux/engine/render/runtime/EffectCompilation.hpp>
#include <map>
#include <ranges>

namespace lux::render::runtime_detail
{
    RenderResult<std::pair<RenderEffect, RenderGraph>> compileEffect(
        nri::Device& device,
        const nri::CoreInterface& nri_core,
        const detail::EnabledDeviceFeatures& enabled,
        std::array<const detail::QueueProperties*, 3> queues,
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
                 selection.contribution_key,
                 selection.parameter_group}
            );
        }
        auto assembly = composeFeatures(
            enabled,
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
        std::vector<bool> live_passes(assembly->definition.passes().size(), false);
        for (auto pass : assembly->analysis.execution_order)
        {
            live_passes[pass.index] = true;
        }
        for (auto& binding : assembly->bindings)
        {
            std::erase_if(binding.passes, [&](auto pass) { return !live_passes[pass.index]; });
        }
        // Keep preparation-only inputs of live outputs or passes, just as Attach does.
        // Dead contributions cannot introduce allocations or conflicts into the dependency set.
        for (const auto& binding : assembly->bindings | std::views::reverse)
        {
            const bool is_live = !binding.passes.empty() ||
                std::ranges::any_of(binding.outputs, [&](auto id) { return live_resources[id.index]; });
            if (is_live)
            {
                for (auto resource : binding.inputs)
                {
                    live_resources[resource.index] = true;
                }
            }
        }
        std::vector<StorageArgument*> arguments(assembly->definition.resources().size());
        for (auto& argument : assembly->arguments)
        {
            const auto index = argument.resource.index;
            if (index >= arguments.size()) return cxx::unexpected(RenderError{kStorageContract});
            if (!live_resources[index]) continue;
            if (arguments[index] && *arguments[index] != argument)
            {
                return cxx::unexpected(RenderError{kStorageMismatch});
            }
            arguments[index] = &argument;
        }
        const auto public_count = storage_dependencies.size();
        std::vector<bool> used_types(public_count);
        const auto resolve = [&](auto& group, EStorageScope scope) noexcept -> RenderResult<void>
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
                    storage_dependencies.push_back({scope, std::move(requirement), {}, {}});
                    dependency = std::prev(storage_dependencies.end());
                }
                else
                {
                    if (dependency->requirement.config != requirement.config)
                    {
                        return cxx::unexpected(RenderError{kStorageMismatch});
                    }
                }
                StorageArgument argument;
                argument.resource = binding.resource;
                if (auto* source = arguments[binding.resource.index]) argument = std::move(*source);
                dependency->bindings.push_back(std::move(argument));
            }
            return {};
        };
        auto& external = assembly->external_bindings;
        const std::array groups{&external.global, &external.scene, &external.view, &external.attachment};
        for (std::size_t i = 0; i < groups.size(); ++i)
        {
            if (auto resolved = resolve(*groups[i], static_cast<EStorageScope>(i)); !resolved)
            {
                return cxx::unexpected(resolved.error());
            }
        }
        for (std::size_t i = 0; i < public_count; ++i)
        {
            // An upload-only input may legitimately be unused in an empty Effect. If its Storage
            // type is consumed, however, the public destination must be one of those actual bindings.
            if (used_types[i] && storage_dependencies[i].bindings.empty())
            {
                return cxx::unexpected(RenderError{kStorageMismatch});
            }
        }
        std::vector<std::uint32_t> parameter_groups(assembly->definition.passes().size());
        for (const auto& contribution : assembly->contributions)
        {
            std::fill_n(
                parameter_groups.begin() + contribution.first_pass,
                contribution.pass_count,
                contribution.parameter_group
            );
        }
        auto compiled = detail::GraphCompiler::compile(
            device,
            nri_core,
            queues,
            assembly->definition,
            assembly->analysis,
            &input->output,
            detail::ETransientAllocation::ALIAS_MEMORY,
            parameter_groups
        );
        if (!compiled)
        {
            return cxx::unexpected(compiled.error());
        }
        std::vector<bool> required(compiled->resources().size());
        for (std::size_t index = 0; index < required.size(); ++index)
        {
            required[index] = compiled->resources()[index].used;
        }
        for (const auto& binding : assembly->bindings | std::views::reverse)
        {
            const bool live = !binding.passes.empty() ||
                std::ranges::any_of(binding.outputs, [&](auto id) { return required[id.index]; });
            if (live)
            {
                for (auto id : binding.inputs) required[id.index] = true;
            }
        }
        std::erase_if(assembly->bindings, [&](const auto& binding)
        {
            return binding.passes.empty() &&
                std::ranges::none_of(binding.outputs, [&](auto id) { return required[id.index]; });
        });
        for (auto& binding : assembly->bindings)
        {
            const auto [offset, size] = compiled->parameterRegion(binding.parameter_group);
            if (binding.parameter_bytes > size)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            binding.parameter_offset = offset;
            binding.parameter_bytes = size;
        }
        RenderEffect result{
            std::make_unique<detail::RenderPlan>(std::move(*compiled)),
            std::move(storage_dependencies),
            std::move(assembly->bindings),
            std::move(assembly->samplers)
        };
        return std::pair{std::move(result), std::move(assembly->definition)};
    }

} // namespace lux::render::runtime_detail
