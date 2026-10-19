#include "ServerImpl.hpp"
#include <exec/variant_sender.hpp>

namespace lux::render
{
    static RenderResult<RenderEffect> compileEffect(
        vulkan::DeviceSession& device,
        const CreateEffectRequest& request,
        std::span<const FeatureEntry* const> registrations,
        std::size_t max_config_bytes
    ) noexcept
    {
        const auto* input = &request;
        const auto count = input->features.size();
        std::vector<FeatureInstanceView> selected;
        std::vector<PassProgram> programs;
        std::vector<StorageArgument> arguments;
        std::vector<vulkan::ContextContribution> contexts;
        selected.reserve(count);
        for (std::size_t i = 0; i < input->features.size(); ++i)
        {
            const auto& selection = input->features[i];
            const auto* registration = registrations[i];
            selected.push_back(
                {&registration->descriptor,
                 &registration->graph,
                 selection.config ? &selection.config : nullptr,
                 selection.instance}
            );
        }
        auto assembly = assembleFeatures(selected, max_config_bytes);
        if (!assembly)
        {
            return cxx::unexpected(assembly.error());
        }
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
                selection.instance_key,
                assembly->definition,
                device.enabledFeatures()
            );
            if (!values)
            {
                return cxx::unexpected(values.error());
            }
            const auto passes = assembly->definition.passes().subspan(contribution.first_pass, contribution.pass_count);
            for (std::size_t p = 0; p < values->programs.size(); ++p)
            {
                const auto& value = values->programs[p];
                if (std::ranges::none_of(passes, [&](const auto& pass) { return pass.key == value.pass_key; }))
                {
                    return cxx::unexpected(RenderError{kFeatureOwnership, {index, p}});
                }
                for (std::size_t previous = 0; previous < p; ++previous)
                {
                    if (values->programs[previous].pass_key == value.pass_key)
                    {
                        return cxx::unexpected(RenderError{kFeatureDuplicate, {index, p, previous}});
                    }
                }
            }
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
            }
            for (auto& argument : values->arguments)
            {
                const auto resource =
                    std::ranges::find(assembly->definition.resources(), argument.resource_key, &GraphResource::key);
                if (resource == assembly->definition.resources().end())
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
                const auto resource_index =
                    static_cast<std::uint32_t>(resource - assembly->definition.resources().begin());
                const auto declared = [&](const auto& group)
                {
                    return std::ranges::any_of(
                        group,
                        [&](const auto& binding) { return binding.resource.index == resource_index; }
                    );
                };
                const auto& bindings = assembly->external_bindings;
                const bool has_declaration = declared(bindings.global) || declared(bindings.scene) ||
                                             declared(bindings.view) || declared(bindings.attachment);
                if (!has_declaration)
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
                const bool is_live = std::ranges::any_of(
                    assembly->analysis.lifetimes,
                    [&](const auto& lifetime) { return lifetime.version.resource.index == resource_index; }
                );
                if (!is_live)
                {
                    continue;
                }
                const auto existing =
                    std::ranges::find(arguments, argument.resource_key, &StorageArgument::resource_key);
                if (existing == arguments.end())
                {
                    arguments.push_back(std::move(argument));
                }
                else if (*existing != argument)
                {
                    return cxx::unexpected(RenderError{kStorageMismatch});
                }
            }
        }
        for (const auto pass : assembly->analysis.execution_order)
        {
            if (std::ranges::find(programs, assembly->definition.passes()[pass.index].key, &PassProgram::pass_key) ==
                programs.end())
            {
                return cxx::unexpected(RenderError{kFeatureContract, {pass.index}});
            }
        }
        auto compiled = vulkan::compileGraph(
            device,
            (*assembly).definition,
            (*assembly).analysis,
            std::move(assembly->external_bindings),
            programs,
            input->description,
            std::move(contexts)
        );
        if (!compiled)
        {
            return cxx::unexpected(compiled.error());
        }
        return RenderEffect{std::make_unique<vulkan::CompiledGraph>(std::move(*compiled)), std::move(arguments)};
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

        // Workers receive only immutable feature entries, the request and the actual device borrow.
        // The final continuation is the sole place that adopts a candidate into the owner table.
        auto compiled =
            stdexec::schedule(scheduler) |
            stdexec::then([native = device.get(),
                           request = std::move(input),
                           entries = std::move(registrations),
                           limit = config.max_config_bytes]() noexcept
                          { return compileEffect(*native, request, entries, limit); }) |
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
                                       return cxx::unexpected(candidate.error());
                                   }
                                   if (stop.stop_requested())
                                   {
                                       return cxx::unexpected(RenderError{kCommandCancelled});
                                   }
                                   const auto effect = server->effects.emplace(std::move(*candidate));
                                   ++server->compiled_graphs;
                                   return EffectCreated{effect};
                               }
                           );
                }
            );
        auto invalid = stdexec::just(CreateEffect::Response{cxx::unexpected(initial_error)});
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
            output_response = cxx::unexpected(RenderError{kRuntimeIdentity});
            return;
        }
        while (!effect->attachments.empty())
        {
            if (auto result = detachAttachment(effect->attachments.back()); !result)
            {
                output_response = cxx::unexpected(result.error());
                return;
            }
        }
        auto retirement = device->retirement().reserve();
        if (auto retired = device->retirement().retire(retirement, effect->code); !retired)
        {
            output_response = cxx::unexpected(retired.error());
            return;
        }
        effects.erase(input->effect);
        output_response = Ack{};
    }
} // namespace lux::render
