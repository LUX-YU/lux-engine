#include <algorithm>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <lux/engine/render/runtime/EffectBuild.hpp>
#include <ranges>

namespace lux::render::runtime_detail
{
    RenderResult<RenderEffect> buildEffect(
        std::span<const ResourceDeclaration> declarations,
        const CreateEffectRequest& request,
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
                 selection.parameter_group, selection.parameter_source}
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
        std::vector<bool> live_passes(assembly->definition.passes().size(), false);
        for (auto pass : assembly->analysis.execution_order)
        {
            live_passes[pass.index] = true;
        }
        // Keep preparation-only inputs of live outputs or passes, just as Attach does.
        // Dead contributions cannot introduce allocations or conflicts into the dependency set.
        for (const auto& binding : assembly->definition.instanceData() | std::views::reverse)
        {
            const bool is_live = std::ranges::any_of(binding.passes, [&](auto id) { return live_passes[id.index]; }) ||
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
        const auto resolve = [&](auto& group, EResourceDomain scope) noexcept -> RenderResult<void>
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
                const bool has_binding = factory != declarations.end() && factory->name.name() == name &&
                    factory->binds_graph;
                if (!has_binding)
                {
                    return cxx::unexpected(RenderError{kStorageFactory});
                }
                if (scope != factory->domain)
                    return cxx::unexpected(RenderError{kStorageMismatch});
                auto dependency = std::ranges::find(
                    resource_dependencies, factory->id, &RenderEffect::ResourceDependency::declaration
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
            if (auto resolved = resolve(*groups[i], static_cast<EResourceDomain>(i)); !resolved)
            {
                return cxx::unexpected(resolved.error());
            }
        }

        std::vector<FrameChannel> channels;
        std::vector<std::span<const FrameChannel>> dependency_channels;
        dependency_channels.reserve(resource_dependencies.size());
        for (const auto& dependency : resource_dependencies)
        {
            const auto declaration = std::ranges::find(declarations, dependency.declaration, &ResourceDeclaration::id);
            if (declaration == declarations.end())
            {
                return cxx::unexpected(RenderError{kStorageIdentity});
            }
            dependency_channels.push_back(declaration->channels);
            channels.insert(channels.end(), declaration->channels.begin(), declaration->channels.end());
        }
        auto schema = FrameLayout::create(channels, 1);
        if (!schema)
        {
            return cxx::unexpected(schema.error());
        }
        if (schema->channels().size() > UINT32_MAX)
        {
            return cxx::unexpected(RenderError{kRuntimeCapacity});
        }
        for (std::size_t i = 0; i < resource_dependencies.size(); ++i)
        {
            auto& columns = resource_dependencies[i].columns;
            columns.reserve(dependency_channels[i].size());
            for (const auto& channel : dependency_channels[i])
            {
                columns.push_back(static_cast<std::uint32_t>(schema->find(channel.declaration, channel.element)));
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
        RenderEffect result(request.parameters);
        const auto graph_resources = assembly->definition.resources();
        for (const auto& external : input->definition.inputs)
        {
            if (!std::holds_alternative<GraphImageDesc>(external.resource.description)) continue;
            const auto found = std::ranges::find(graph_resources, external.resource.key, &GraphResource::key);
            if (found == graph_resources.end()) std::terminate();
            const GraphResourceId id{static_cast<std::uint32_t>(found - graph_resources.begin())};
            result.external_images.push_back({external.destination, id});
        }
        result.graph = std::move(assembly->definition);
        result.analysis = std::move(assembly->analysis);
        result.output = input->output;
        result.parameter_groups = std::move(parameter_groups);
        result.resource_dependencies = std::move(resource_dependencies);
        result.input_schema = std::move(*schema);
        return result;
    }

} // namespace lux::render::runtime_detail
