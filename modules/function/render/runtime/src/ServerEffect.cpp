#include "ServerImpl.hpp"
#include <lux/engine/render/data/Camera.hpp>

namespace lux::render
{
    static RenderResult<RenderEffect> compileEffect(
        vulkan::DeviceSession& device, const CreateEffectRequest& request,
        std::span<const FeatureEntry* const> registrations, std::size_t max_config_bytes
    ) noexcept
    {
        const auto* input = &request;
        const auto count = input->features.size();
        std::vector<FeatureInstanceView> selected;
        std::vector<PassProgram> programs;
        std::vector<StorageArgument> arguments;
        selected.reserve(count);
        for (std::size_t i = 0; i < input->features.size(); ++i)
        {
            const auto& selection = input->features[i];
            const auto* registration = registrations[i];
            selected.push_back({&registration->descriptor, &registration->graph,
                selection.config ? &selection.config : nullptr, selection.instance});
        }
        auto assembly = assembleFeatures(selected, max_config_bytes);
        if (!assembly) return cxx::unexpected(assembly.error());
        for (const auto& contribution : assembly->contributions)
        {
            const auto index = contribution.input_index;
            if (!registrations[index]->programs)
            {
                const bool has_live_pass = std::ranges::any_of(assembly->analysis.execution_order, [&](auto pass) {
                    return pass.index >= contribution.first_pass &&
                        pass.index - contribution.first_pass < contribution.pass_count;
                });
                if (has_live_pass) return cxx::unexpected(RenderError{kFeatureContract});
                continue;
            }
            const auto& selection = selected[index];
            auto values = registrations[index]->programs(
                selection.config ? selection.config->data() : nullptr, selection.instance_key
            );
            if (!values) return cxx::unexpected(values.error());
            const auto passes = assembly->definition.passes().subspan(contribution.first_pass, contribution.pass_count);
            for (std::size_t p = 0; p < values->programs.size(); ++p)
            {
                const auto& value = values->programs[p];
                if (std::ranges::none_of(passes, [&](const auto& pass) { return pass.key == value.pass_key; }))
                    return cxx::unexpected(RenderError{kFeatureOwnership, {index, p}});
                for (std::size_t previous = 0; previous < p; ++previous)
                    if (values->programs[previous].pass_key == value.pass_key)
                        return cxx::unexpected(RenderError{kFeatureDuplicate, {index, p, previous}});
            }
            for (auto& value : values->programs)
            {
                const bool live = std::ranges::any_of(assembly->analysis.execution_order, [&](auto id) {
                    return assembly->definition.passes()[id.index].key == value.pass_key;
                });
                if (live && value.context_parameters.size && value.context_parameters != cameraParameterBlock())
                    return cxx::unexpected(RenderError{kFeatureContract});
                programs.push_back(std::move(value));
            }
            for (auto& argument : values->arguments)
            {
                const auto resource = std::ranges::find(assembly->definition.resources(),
                    argument.resource_key, &GraphResource::key);
                if (resource == assembly->definition.resources().end())
                    return cxx::unexpected(RenderError{kStorageContract});
                const auto resource_index = static_cast<std::uint32_t>(resource - assembly->definition.resources().begin());
                const auto declared = [&](const auto& group) {
                    return std::ranges::any_of(group, [&](const auto& binding) {
                        return binding.resource.index == resource_index;
                    });
                };
                const auto& bindings = assembly->external_bindings;
                const bool has_declaration = declared(bindings.global) || declared(bindings.scene) ||
                    declared(bindings.view) || declared(bindings.attachment);
                if (!has_declaration) return cxx::unexpected(RenderError{kStorageContract});
                const bool is_live = std::ranges::any_of(assembly->analysis.lifetimes, [&](const auto& lifetime) {
                    return lifetime.version.resource.index == resource_index;
                });
                if (!is_live) continue;
                const auto existing =
                    std::ranges::find(arguments, argument.resource_key, &StorageArgument::resource_key);
                if (existing == arguments.end())
                    arguments.push_back(std::move(argument));
                else if (existing->value != argument.value)
                    return cxx::unexpected(RenderError{kStorageMismatch});
            }
        }
        for (const auto pass : assembly->analysis.execution_order)
            if (std::ranges::find(programs, assembly->definition.passes()[pass.index].key, &PassProgram::pass_key) ==
                programs.end())
                return cxx::unexpected(RenderError{kFeatureContract, {pass.index}});
        auto compiled = vulkan::compileGraph(
            device,
            (*assembly).definition,
            (*assembly).analysis,
            (*assembly).external_bindings,
            programs,
            input->description
        );
        if (!compiled) return cxx::unexpected(compiled.error());
        return RenderEffect{std::make_shared<const vulkan::CompiledGraph>(std::move(*compiled)), std::move(arguments)};
    }

    struct RenderServer::Impl::CompileState final
    {
        struct Receiver final
        {
            using receiver_concept = stdexec::receiver_t;
            DeferredCompletion* slot;
            struct Env final
            {
                stdexec::inplace_stop_token token;
                stdexec::inplace_stop_token query(stdexec::get_stop_token_t) const noexcept { return token; }
            };
            Env get_env() const noexcept { return {slot->stop.get_token()}; }
            void set_value() && noexcept { slot->post()(nullptr, nullptr, {}); }
            void set_error(RenderError error) && noexcept { slot->post()(nullptr, nullptr, cxx::unexpected(error)); }
            void set_stopped() && noexcept
            {
                slot->post()(nullptr, nullptr, cxx::unexpected(RenderError{kRuntimeState}));
            }
        };

        vulkan::DeviceSession& device;
        const CreateEffectRequest& request;
        std::vector<const FeatureEntry*> features;
        std::size_t max_config_bytes;
        DeferredCompletion* slot;
        RenderResult<RenderEffect> candidate = cxx::unexpected(RenderError{kRuntimeState});
        stdexec::connect_result_t<CpuSender, Receiver> operation;

        CompileState(vulkan::DeviceSession& native, CpuSchedule& cpu, DeferredCompletion& pending,
            std::vector<const FeatureEntry*> selected, std::size_t config_bytes,
            const std::shared_ptr<const void>& implementation_pin)
            : device(native), request(pending.request->get<CreateEffectRequest>()), features(std::move(selected)),
              max_config_bytes(config_bytes), slot(&pending),
              operation(stdexec::connect(
                  cpu.schedule(
                      [](void* value) noexcept
                      {
                          auto& state = *static_cast<CompileState*>(value);
                          state.candidate = compileEffect(
                              state.device, state.request, state.features, state.max_config_bytes
                          );
                      },
                      this,
                      implementation_pin,
                      {}
                  ),
                  Receiver{&pending}
              ))
        {
        }

        static RenderResult<void> adopt(void* value, void* owner, RenderRequest& request) noexcept
        {
            auto& state = *static_cast<CompileState*>(value);
            auto& server = *static_cast<Impl*>(owner);
            if (server.stop_requested.load()) return request.cancel();
            if (!state.slot->continuation_result) return request.fail(state.slot->continuation_result.error());
            if (!state.candidate) return request.fail(state.candidate.error());
            const auto effect = server.effects.emplace(std::move(*state.candidate));
            auto result = request.reply(EffectCreated{effect});
            if (result)
                ++server.compiled_graphs;
            else
                server.effects.erase(effect);
            return result;
        }
    };

    RenderResult<void> RenderServer::Impl::createEffect(RenderRequest& request) noexcept
    {
        if (!config.cpu) return cxx::unexpected(RenderError{kRuntimeUnsupported});
        const auto& input = request.get<CreateEffectRequest>();
        if (input.features.empty())
            return cxx::unexpected(RenderError{kFeatureContract});
        std::vector<const FeatureEntry*> selected;
        selected.reserve(input.features.size());
        for (const auto& selection : input.features)
        {
            const auto* found = features.find(selection.feature);
            if (!found) return cxx::unexpected(RenderError{kRuntimeIdentity});
            selected.push_back(found);
        }
        // Resolve the directory once on its owner thread. Stable entries outlive the accepted compile;
        // neither the worker nor its operation state borrows the mutable Impl or its directory.
        auto* slot = reserveCompletion();
        if (!slot) return cxx::unexpected(RenderError{kRuntimeCapacity});
        slot->request.emplace(std::move(request));
        auto& cpu = slot->operation;
        cpu.destroy = [](void* state) noexcept { delete static_cast<CompileState*>(state); };
        cpu.adopt = CompileState::adopt;
        cpu.operation = new CompileState(
            *device, config.cpu, *slot, std::move(selected), config.max_config_bytes, config.implementation_code_pin
        );
        stdexec::start(static_cast<CompileState*>(cpu.operation)->operation);
        return {};
    }

    RenderResult<void> RenderServer::Impl::removeEffect(RenderRequest& request) noexcept
    {
        const auto* input = &request.get<EffectRequest>();
        auto* effect = effects.find(input->effect);
        if (!effect) return cxx::unexpected(RenderError{kRuntimeIdentity});
        while (!effect->groups.empty())
            if (auto result = withdraw(*effect, effect->groups.size() - 1); !result) return result;
        effects.erase(input->effect);
        return request.reply();
    }
} // namespace lux::render
