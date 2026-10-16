#include "ServerImpl.hpp"

namespace lux::render
{
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

        Impl* owner;
        DeferredCompletion* slot;
        RenderResult<RenderEffect> candidate = cxx::unexpected(RenderError{kRuntimeState});
        stdexec::connect_result_t<CpuSender, Receiver> operation;

        CompileState(Impl& server, DeferredCompletion& pending)
            : owner(&server), slot(&pending),
              operation(stdexec::connect(
                  server.config.cpu.schedule(
                      [](void* value) noexcept
                      {
                          auto& state = *static_cast<CompileState*>(value);
                          state.candidate = state.owner->compileEffect(*state.slot->request);
                      },
                      this,
                      server.config.implementation_code_pin,
                      pending.request->codePin()
                  ),
                  Receiver{&pending}
              ))
        {
        }

        static RenderResult<void> adopt(void* value, RenderRequest& request) noexcept
        {
            auto& state = *static_cast<CompileState*>(value);
            if (state.owner->stop_requested.load()) return request.cancel();
            if (!state.slot->cpu_result) return request.fail(state.slot->cpu_result.error());
            if (!state.candidate) return request.fail(state.candidate.error());
            const auto effect = state.owner->effects.emplace(std::move(*state.candidate));
            auto result = request.reply(EffectCreated{effect});
            if (result)
                ++state.owner->compiled_graphs;
            else
                state.owner->effects.erase(effect);
            return result;
        }
    };

    RenderResult<RenderEffect> RenderServer::Impl::compileEffect(const RenderRequest& request) noexcept
    {
        const auto* input = &request.get<CreateEffectRequest>();
        const auto count = input->features.size();
        if (count == 0 || count > max_features) return cxx::unexpected(RenderError{kFeatureContract});
        std::vector<RetainableEncodedDataView> configs;
        std::vector<FeatureInstanceView> selected;
        std::vector<const FeatureRegistration*> registrations;
        std::vector<lux::cxx::SharedBytes<>> attachments;
        std::vector<PassProgram> programs;
        std::vector<StorageArgument> arguments;
        configs.reserve(count);
        selected.reserve(count);
        for (const auto& attachment : input->attachments)
            attachments.push_back(attachment.bytes());
        for (const auto& selection : input->features)
        {
            const auto registration = std::find_if(
                features.begin(),
                features.end(),
                [&](const auto& f) { return f.descriptor->type.name() == selection.type; }
            );
            if (registration == features.end() || registration->descriptor->version != selection.version)
                return cxx::unexpected(RenderError{kFeatureContract});
            const auto& definition = *registration->descriptor;
            const RetainableEncodedDataView* wire = nullptr;
            if (definition.config_schema)
            {
                if (!selection.config_schema) return cxx::unexpected(RenderError{kDataSchemaMismatch});
                configs.push_back({*selection.config_schema, selection.config.bytes(), attachments});
                wire = &configs.back(); // assembleFeatures checks the complete author Schema once.
            }
            else if (selection.config_schema || !selection.config.bytes().empty())
                return cxx::unexpected(RenderError{kFeatureContract});
            selected.push_back({&definition, registration->graph, wire, selection.instance});
            registrations.push_back(&*registration);
        }
        auto assembly = assembleFeatures(selected, {config.max_config_bytes, SIZE_MAX});
        if (!assembly) return cxx::unexpected(assembly.error());
        for (const auto& contribution : assembly->contributions)
        {
            if (contribution.pass_count == 0) continue;
            const auto index = contribution.input_index;
            if (!registrations[index]->programs) return cxx::unexpected(RenderError{kFeatureContract});
            const auto& selection = selected[index];
            auto values = registrations[index]->programs(selection.config, selection.instance_key);
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
                programs.push_back(std::move(value));
            for (auto& argument : values->arguments)
            {
                const auto existing =
                    std::ranges::find(arguments, argument.resource_key, &StorageArgument::resource_key);
                if (existing == arguments.end())
                    arguments.push_back(std::move(argument));
                else if (!std::ranges::equal(existing->bytes.view(), argument.bytes.view()))
                    return cxx::unexpected(RenderError{kStorageMismatch});
            }
        }
        for (const auto pass : assembly->analysis.execution_order)
            if (std::ranges::find(programs, assembly->definition.passes()[pass.index].key, &PassProgram::pass_key) ==
                programs.end())
                return cxx::unexpected(RenderError{kFeatureContract, {pass.index}});
        auto compiled = vulkan::compileGraph(
            *device,
            (*assembly).definition,
            (*assembly).analysis,
            (*assembly).external_bindings,
            programs,
            input->description
        );
        if (!compiled) return cxx::unexpected(compiled.error());
        return RenderEffect{std::make_shared<const vulkan::CompiledGraph>(std::move(*compiled)), std::move(arguments)};
    }

    RenderResult<void> RenderServer::Impl::createEffect(RenderRequest& request) noexcept
    {
        if (!config.cpu) return cxx::unexpected(RenderError{kRuntimeUnsupported});
        auto* slot = reserveCompletion(true);
        if (!slot) return cxx::unexpected(RenderError{kRuntimeCapacity});
        slot->request.emplace(std::move(request));
        auto& cpu = slot->condition.emplace<CpuCompletion>();
        cpu.destroy = [](void* state) noexcept { delete static_cast<CompileState*>(state); };
        cpu.compiling = true;
        cpu.adopt = CompileState::adopt;
        cpu.operation = new CompileState(*this, *slot);
        ++compiling;
        stdexec::start(static_cast<CompileState*>(cpu.operation)->operation);
        return {};
    }

    RenderResult<void> RenderServer::Impl::removeEffect(RenderRequest& request) noexcept
    {
        const auto* input = &request.get<EffectRequest>();
        auto* effect = effects.find(input->effect);
        if (!effect) return cxx::unexpected(RenderError{kRuntimeIdentity});
        if (auto admitted = admitWithdraw([&](const auto& owner, const auto&) { return &owner == effect; }); !admitted)
            return admitted;
        while (!effect->groups.empty())
            if (auto result = withdraw(*effect, effect->groups.size() - 1); !result) return result;
        effects.erase(input->effect);
        return request.reply();
    }
} // namespace lux::render
