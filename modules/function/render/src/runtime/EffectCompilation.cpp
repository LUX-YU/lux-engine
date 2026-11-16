#include <algorithm>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
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
        std::span<const ResourceDeclaration> declarations,
        const GraphEffectRequest& request,
        std::span<const FeatureEntry* const> registrations,
        std::size_t max_config_bytes,
        std::vector<RenderEffect::ResourceDependency> resource_dependencies
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
        const auto public_count = resource_dependencies.size();
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
                const auto name = requirement.type.name();
                const auto factory = std::lower_bound(
                    declarations.begin(),
                    declarations.end(),
                    name,
                    [](const ResourceDeclaration& type, std::string_view name) { return type.name.name() < name; }
                );
                if (factory == declarations.end() || factory->name.name() != name)
                {
                    return cxx::unexpected(RenderError{kStorageFactory});
                }
                if (static_cast<unsigned>(scope) != static_cast<unsigned>(factory->domain))
                    return cxx::unexpected(RenderError{kStorageMismatch});
                for (std::size_t i = 0; i < public_count; ++i)
                {
                    used_types[i] = used_types[i] || resource_dependencies[i].declaration == factory->id;
                }
                auto dependency = std::ranges::find_if(
                    resource_dependencies,
                    [&](const auto& value) noexcept
                    {
                        return value.declaration == factory->id;
                    }
                );
                if (dependency == resource_dependencies.end())
                {
                    resource_dependencies.push_back({scope, factory->id, {}, {}});
                    dependency = std::prev(resource_dependencies.end());
                }
                StorageArgument argument;
                argument.resource = binding.resource;
                if (auto* source = arguments[binding.resource.index]) argument = std::move(*source);
                dependency->bindings.push_back(std::move(argument));
            }
            return {};
        };
        auto& external = assembly->external_bindings;
        const std::array groups{&external.global, &external.scene, &external.view};
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
            if (used_types[i] && resource_dependencies[i].bindings.empty())
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
            std::move(resource_dependencies),
            std::move(assembly->bindings),
            std::move(assembly->samplers)
        };
        return std::pair{std::move(result), std::move(assembly->definition)};
    }

} // namespace lux::render::runtime_detail
