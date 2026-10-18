#include "ServerImpl.hpp"
#include <lux/engine/render/data/Camera.hpp>
#include <exec/variant_sender.hpp>

#ifdef LUX_RUNTIME_EFFECT_TEST_SEAM
namespace lux::render::runtime_cpu_test { void effectAdopted() noexcept; }
#endif

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

    namespace
    {
        // A borrowed RenderThread scheduling target using the Command's existing completion landing place.
        // It neither owns a queue nor extends the lifetime of Impl. Scope/Host shutdown owns that guarantee.
        struct RenderScheduler final
        {
            CompletionPost post;

            bool operator==(const RenderScheduler& other) const noexcept
            {
                return post.owner == other.post.owner && post.publish == other.post.publish;
            }

            template <class Tag> RenderScheduler query(stdexec::get_completion_scheduler_t<Tag>) const noexcept
            {
                return *this;
            }

            struct Sender final
            {
                using sender_concept = stdexec::sender_t;
                using completion_signatures = stdexec::completion_signatures<
                    stdexec::set_value_t(), stdexec::set_stopped_t()
                >;
                CompletionPost post;

                RenderScheduler get_env() const noexcept { return {post}; }

                template <class Receiver> struct Operation final
                {
                    using operation_state_concept = stdexec::operation_state_t;
                    CompletionPost post;
                    Receiver receiver;

                    void start() & noexcept
                    {
                        // Copy the borrowed publisher before publication; the consumer may destroy this state.
                        const auto publish = post;
                        publish(this, [](void* value, RenderResult<void>) noexcept {
                            auto& self = *static_cast<Operation*>(value);
                            if (stdexec::get_stop_token(stdexec::get_env(self.receiver)).stop_requested())
                                stdexec::set_stopped(std::move(self.receiver));
                            else
                                stdexec::set_value(std::move(self.receiver));
                            // Terminal delivery may destroy the connected state. No access to self afterwards.
                        }, {});
                    }
                };

                template <class Receiver> auto connect(Receiver&& receiver) const noexcept
                {
                    return Operation<std::decay_t<Receiver>>{post, std::forward<Receiver>(receiver)};
                }
            };

            Sender schedule() const noexcept { return {post}; }
        };
        static_assert(stdexec::scheduler<RenderScheduler>);

    } // namespace

    struct RenderServer::Impl::CreateEffectSender final
    {
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(CreateEffect::Response), stdexec::set_stopped_t()
        >;
        Impl* owner;
        CreateEffect::Request request;
        std::vector<const FeatureEntry*> features;
        RenderError initial_error;

        template <class Receiver> auto connect(Receiver&& receiver) && noexcept
        {
            // Only this CPU closure goes to the Worker; no mutable Impl or registry is borrowed there.
            auto compiled = stdexec::schedule(owner->scheduler)
                | stdexec::then([
                    device = owner->device.get(), input = std::move(request), registrations = std::move(features),
                    limit = owner->config.max_config_bytes
                ]() noexcept { return compileEffect(*device, input, registrations, limit); })
                | stdexec::upon_error([](execution::EExecutionError error) noexcept -> RenderResult<RenderEffect> {
                    return cxx::unexpected(RenderError{kCpuScheduleFailure, {static_cast<std::uint64_t>(error)}});
                })
                | stdexec::continues_on(RenderScheduler{execution::getCompletionPost(stdexec::get_env(receiver))})
                | stdexec::then(
                    [server = owner](RenderResult<RenderEffect> candidate) noexcept -> CreateEffect::Response {
                        if (!candidate) return cxx::unexpected(candidate.error());
#ifdef LUX_RUNTIME_EFFECT_TEST_SEAM
                        runtime_cpu_test::effectAdopted();
#endif
                        const auto effect = server->effects.emplace(std::move(*candidate));
                        ++server->compiled_graphs;
                        return EffectCreated{effect};
                    }
                );
            auto invalid = stdexec::just(CreateEffect::Response{cxx::unexpected(initial_error)});
            using VSource = exec::variant_sender<decltype(invalid), decltype(compiled)>;
            auto source = initial_error.type != error::ErrorId{} ? VSource{std::move(invalid)} :
                VSource{std::move(compiled)};
            return stdexec::connect(std::move(source), std::forward<Receiver>(receiver));
        }
    };

    RenderServer::Impl::CreateEffectSender RenderServer::Impl::createEffect(CreateEffect::Request&& input) noexcept
    {
        CreateEffectSender sender{this, std::move(input)};
        if (sender.request.features.empty())
        {
            sender.initial_error = {kFeatureContract};
        }
        sender.features.reserve(sender.request.features.size());
        for (const auto& selection : sender.request.features)
        {
            const auto* found = features.find(selection.feature);
            if (!found)
            {
                sender.initial_error = {kRuntimeIdentity};
                break;
            }
            sender.features.push_back(found);
        }
        return sender;
    }

    void RenderServer::Impl::installAsyncBuiltins() noexcept
    {
        addBuiltin<CreateEffect, &Impl::createEffect>();
        installReadbackCommand();
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
        effects.erase(input->effect);
        output_response = Ack{};
    }
} // namespace lux::render
