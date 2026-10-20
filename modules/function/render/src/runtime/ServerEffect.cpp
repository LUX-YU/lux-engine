#include <algorithm>
#include <exec/variant_sender.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/runtime/ServerImpl.hpp>
#include <map>

namespace lux::render
{
    static RenderResult<RenderEffect> compileEffect(
        nri::Device& device,
        const nri::CoreInterface& api,
        const detail::EnabledDeviceFeatures& enabled,
        std::span<const nri::FormatSupportBits> formats,
        std::array<std::uint32_t, 2> vertex_limits,
        nri::QueueType queue_type,
        std::uint32_t sampled_image_capacity,
        std::span<const StorageNameView> storage_types,
        const CreateEffectRequest& request,
        std::span<const FeatureEntry* const> registrations,
        std::size_t max_config_bytes
    ) noexcept
    {
        const auto* input = &request;
        const auto count = input->features.size();
        std::vector<FeatureBuildInput> selected;
        std::vector<PassProgram> programs;
        std::vector<std::size_t> program_owners;
        std::vector<StorageArgument> arguments;
        std::vector<detail::FeatureFrameContribution> contexts;
        selected.reserve(count);
        for (std::size_t i = 0; i < input->features.size(); ++i)
        {
            const auto& selection = input->features[i];
            const auto* registration = registrations[i];
            selected.push_back(
                {&registration->descriptor,
                 &registration->graph,
                 selection.config ? &selection.config : nullptr,
                 selection.contribution_key}
            );
        }
        auto assembly = assembleFeatures(selected, max_config_bytes);
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
        const auto qualify = [&](const auto& group) noexcept
        {
            for (const auto& binding : group)
            {
                if (!live_resources[binding.resource.index])
                {
                    continue;
                }
                const auto name = binding.storage.type.name();
                const auto found = std::lower_bound(
                    storage_types.begin(),
                    storage_types.end(),
                    name,
                    [](StorageNameView type, std::string_view name) { return type.name() < name; }
                );
                if (found == storage_types.end() || found->name() != name)
                {
                    return false;
                }
            }
            return true;
        };
        const auto& external = assembly->external_bindings;
        const bool missing_factory = !qualify(external.global) || !qualify(external.scene) || !qualify(external.view) ||
                                     !qualify(external.attachment);
        if (missing_factory)
        {
            return cxx::unexpected(RenderError{kStorageFactory});
        }
        const auto resources = assembly->definition.resources();
        std::map<std::string_view, std::uint32_t, std::less<>> resource_names;
        for (std::uint32_t i = 0; i < resources.size(); ++i)
        {
            resource_names.emplace(resources[i].key, i);
        }
        std::vector<bool> declared_storage(resources.size(), false);
        for (const auto* group : {&external.global, &external.scene, &external.view, &external.attachment})
        {
            for (const auto& binding : *group)
            {
                declared_storage[binding.resource.index] = true;
            }
        }
        std::vector<std::size_t> argument_indices(resources.size(), SIZE_MAX);
        std::vector<std::size_t> pass_owners(assembly->definition.passes().size(), SIZE_MAX);
        for (const auto& contribution : assembly->contributions)
        {
            const auto index = contribution.input_index;
            if (!registrations[index]->programs)
            {
                const bool has_live_pass = std::ranges::any_of(
                    assembly->analysis.execution_order,
                    [&](auto pass)
                    {
                        return pass.index >= contribution.first_pass &&
                               pass.index - contribution.first_pass < contribution.pass_count;
                    }
                );
                if (has_live_pass)
                {
                    return cxx::unexpected(RenderError{kFeatureContract});
                }
                continue;
            }
            const auto& selection = selected[index];
            auto values = registrations[index]->programs(
                selection.config ? selection.config->data() : nullptr,
                selection.contribution_key,
                assembly->definition,
                enabled
            );
            if (!values)
            {
                return cxx::unexpected(values.error());
            }
            const auto passes = assembly->definition.passes().subspan(contribution.first_pass, contribution.pass_count);
            std::fill_n(pass_owners.begin() + contribution.first_pass, contribution.pass_count, index);
            for (auto& value : values->contexts)
            {
                const bool is_declared = std::ranges::any_of(
                    passes,
                    [&](const auto& pass)
                    {
                        return std::ranges::any_of(
                            pass.uses,
                            [&](const auto& use) { return use.version.resource == value.resource; }
                        );
                    }
                );
                if (!is_declared)
                {
                    return cxx::unexpected(RenderError{kFeatureOwnership});
                }
                contexts.push_back(std::move(value));
            }
            for (auto& value : values->programs)
            {
                programs.push_back(std::move(value));
                program_owners.push_back(index);
            }
            for (auto& argument : values->arguments)
            {
                const auto resource = resource_names.find(argument.resource_key);
                if (resource == resource_names.end())
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
                const auto resource_index = resource->second;
                if (!declared_storage[resource_index])
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
                if (!live_resources[resource_index])
                {
                    continue;
                }
                auto& existing = argument_indices[resource_index];
                if (existing == SIZE_MAX)
                {
                    existing = arguments.size();
                    arguments.push_back(std::move(argument));
                }
                else if (arguments[existing] != argument)
                {
                    return cxx::unexpected(RenderError{kStorageMismatch});
                }
            }
        }
        auto indexed = detail::indexPrograms(assembly->definition, programs);
        if (!indexed)
        {
            const auto position = indexed.error().args[0];
            const auto error = indexed.error().args[1] ? kFeatureDuplicate : kFeatureOwnership;
            return cxx::unexpected(RenderError{error, {program_owners[position], position}});
        }
        for (std::size_t pass = 0; pass < indexed->size(); ++pass)
        {
            const auto* program = (*indexed)[pass];
            if (program && program_owners[program - programs.data()] != pass_owners[pass])
            {
                return cxx::unexpected(RenderError{kFeatureOwnership, {pass}});
            }
        }
        for (const auto pass : assembly->analysis.execution_order)
        {
            if (!(*indexed)[pass.index])
            {
                return cxx::unexpected(RenderError{kFeatureContract, {pass.index}});
            }
        }
        auto compiled = detail::compileGraph(
            device,
            api,
            enabled,
            formats,
            vertex_limits,
            queue_type,
            sampled_image_capacity,
            (*assembly).definition,
            (*assembly).analysis,
            std::move(assembly->external_bindings),
            *indexed,
            input->description,
            std::move(contexts)
        );
        if (!compiled)
        {
            return cxx::unexpected(compiled.error());
        }
        return RenderEffect{std::make_unique<detail::CompiledGraph>(std::move(*compiled)), std::move(arguments)};
    }

    auto RenderServer::Impl::createEffect(CreateEffect::Request&& input) noexcept
    {
        RenderError initial_error;
        std::vector<const FeatureEntry*> registrations;
        if (input.features.empty())
        {
            initial_error = {kFeatureContract};
        }
        registrations.reserve(input.features.size());
        for (const auto& selection : input.features)
        {
            const auto* found = features.find(selection.feature);
            if (!found)
            {
                initial_error = {kRuntimeIdentity};
                break;
            }
            registrations.push_back(found);
        }

        std::vector<StorageNameView> storage_types;
        resources->forEachType(
            [&](StorageTypeId id, const StorageDescriptor& type, std::uint32_t) noexcept
            {
                const auto* factory = resources->factory(id);
                if (factory && factory->binding_ops &&
                    static_cast<const detail::StorageBindingOps*>(factory->binding_ops)->resolve)
                {
                    storage_types.push_back(type.type);
                }
            }
        );
        std::ranges::sort(storage_types, {}, [](StorageNameView type) { return type.name(); });
        std::uint32_t sampled_image_capacity = 0;
        if (device->enabledFeatures().texture_descriptors)
        {
            const auto capacity = device->sampledImageCapacity();
            if (capacity)
            {
                sampled_image_capacity = *capacity;
            }
            else
            {
                initial_error = capacity.error();
            }
        }
        // Workers receive only immutable feature entries, the request and actual device facts.
        // The final continuation is the sole place that adopts a candidate into the Effect table.
        auto compiled =
            stdexec::schedule(scheduler) |
            stdexec::then(
                [native = &device->device(),
                 api = &device->nriCore(),
                 enabled = device->enabledFeatures(),
                 formats = device->formatSupport(),
                 vertex_limits = std::array{device->vertexStrideLimit(), device->vertexOffsetLimit()},
                 queue_type = device->queue().properties().type,
                 sampled_image_capacity,
                 storage_types = std::move(storage_types),
                 request = std::move(input),
                 entries = std::move(registrations),
                 limit = config.max_config_bytes]() noexcept
                {
                    return compileEffect(
                        *native,
                        *api,
                        enabled,
                        formats,
                        vertex_limits,
                        queue_type,
                        sampled_image_capacity,
                        storage_types,
                        request,
                        entries,
                        limit
                    );
                }
            ) |
            stdexec::upon_error(
                [](execution::EExecutionError error) noexcept -> RenderResult<RenderEffect>
                { return cxx::unexpected(RenderError{kCpuScheduleFailure, {static_cast<std::uint64_t>(error)}}); }
            ) |
            stdexec::continues_on(render_loop_.scheduler()) |
            stdexec::let_value(
                [server = this](RenderResult<RenderEffect>& candidate) noexcept
                {
                    return stdexec::read_env(stdexec::get_stop_token) |
                           stdexec::then(
                               [server, &candidate](auto stop) noexcept -> CreateEffect::Response
                               {
                                   if (!candidate)
                                   {
                                       return {candidate.error()};
                                   }
                                   if (stop.stop_requested())
                                   {
                                       return {RenderError{kCommandCancelled}};
                                   }
                                   const auto effect = server->effects.emplace(std::move(*candidate));
                                   ++server->compiled_graphs;
                                   return EffectCreated{{}, effect};
                               }
                           );
                }
            );
        auto invalid = stdexec::just(CreateEffect::Response{initial_error});
        using VSource = exec::variant_sender<decltype(invalid), decltype(compiled)>;
        return initial_error.type != error::ErrorId{} ? VSource{std::move(invalid)} : VSource{std::move(compiled)};
    }

    void RenderServer::Impl::installAsyncBuiltins() noexcept
    {
        addBuiltin<CreateEffect, &Impl::createEffect>();
    }

    void RenderServer::Impl::removeEffect(
        const RemoveEffect::Request& _request,
        RemoveEffect::Response& output_response
    ) noexcept
    {
        const auto* input = &_request;
        auto* effect = effects.find(input->effect);
        if (!effect)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        while (!effect->attachments.empty())
        {
            detachAttachment(effect->attachments.back());
        }
        device->deferRelease([code = std::move(effect->code)]() mutable noexcept { code.reset(); });
        effects.erase(input->effect);
        output_response = {};
    }
} // namespace lux::render
