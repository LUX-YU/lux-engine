#include "ServerImpl.hpp"
#include <lux/engine/render/data/Camera.hpp>

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

    struct RenderServer::Impl::CreateEffectSender final
    {
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(CreateEffect::Response),
            stdexec::set_error_t(RenderError),
            stdexec::set_stopped_t()>;
        Impl* owner;
        CreateEffect::Request request;
        std::vector<const FeatureEntry*> features;
        RenderError initial_error;

        template <class Receiver> class Operation final
        {
            struct CpuReceiver final
            {
                using receiver_concept = stdexec::receiver_t;
                Operation* operation;

                stdexec::env_of_t<Receiver> get_env() const noexcept
                {
                    return stdexec::get_env(operation->receiver_);
                }

                void set_value() && noexcept
                {
                    operation->completionPost()(operation, adopt, {});
                }

                void set_error(execution::EExecutionError error) && noexcept
                {
                    operation->completionPost()(
                        operation,
                        adopt,
                        cxx::unexpected(RenderError{kCpuScheduleFailure, {static_cast<std::uint64_t>(error)}})
                    );
                }

                void set_stopped() && noexcept
                {
                    operation->completionPost()(operation, adopt, cxx::unexpected(RenderError{kCommandCancelled}));
                }
            };

            struct Compile final
            {
                Operation* operation;
                void operator()() const noexcept
                {
                    auto& self = *operation;
                    self.candidate_ = compileEffect(self.device_, self.request_, self.features_, self.max_config_bytes_);
                }
            };
            using Sender = decltype(stdexec::then(stdexec::schedule(execution::CpuScheduler{}), Compile{}));

        public:
            using operation_state_concept = stdexec::operation_state_t;

            Operation(CreateEffectSender input, Receiver receiver) noexcept
                : owner_(input.owner), device_(*input.owner->device), request_(std::move(input.request)),
                  features_(std::move(input.features)), max_config_bytes_(input.owner->config.max_config_bytes),
                  initial_error_(input.initial_error), receiver_(std::move(receiver)),
                  cpu_operation_(stdexec::connect(
                      stdexec::then(stdexec::schedule(input.owner->scheduler), Compile{this}),
                      CpuReceiver{this}
                  ))
            {
            }

            Operation(const Operation&) = delete;
            Operation(Operation&&) = delete;

            void start() noexcept
            {
                if (initial_error_.type != error::ErrorId{})
                {
                    stdexec::set_error(std::move(receiver_), initial_error_);
                    return;
                }
                stdexec::start(cpu_operation_);
            }

        private:
            CompletionPost completionPost() const noexcept
            {
                return execution::getCompletionPost(stdexec::get_env(receiver_));
            }

            static void adopt(void* value, RenderResult<void> completed) noexcept
            {
                auto& self = *static_cast<Operation*>(value);
                // Only CompletionQueue's owner consumes this continuation. The Worker never touches Impl.
                if (stdexec::get_stop_token(stdexec::get_env(self.receiver_)).stop_requested())
                {
                    stdexec::set_stopped(std::move(self.receiver_));
                    return;
                }
                if (!completed)
                {
                    stdexec::set_error(std::move(self.receiver_), completed.error());
                    return;
                }
                if (!self.candidate_)
                {
                    stdexec::set_error(std::move(self.receiver_), self.candidate_.error());
                    return;
                }
#ifdef LUX_RUNTIME_EFFECT_TEST_SEAM
                runtime_cpu_test::effectAdopted();
#endif
                const auto effect = self.owner_->effects.emplace(std::move(*self.candidate_));
                ++self.owner_->compiled_graphs;
                stdexec::set_value(std::move(self.receiver_), CreateEffect::Response{EffectCreated{effect}});
            }

            Impl* owner_;
            vulkan::DeviceSession& device_;
            CreateEffect::Request request_;
            std::vector<const FeatureEntry*> features_;
            std::size_t max_config_bytes_;
            RenderError initial_error_;
            RenderResult<RenderEffect> candidate_ = cxx::unexpected(RenderError{kRuntimeState});
            Receiver receiver_;
            stdexec::connect_result_t<Sender, CpuReceiver> cpu_operation_;
        };

        template <class Receiver> Operation<std::decay_t<Receiver>> connect(Receiver&& receiver) && noexcept
        {
            return {std::move(*this), std::forward<Receiver>(receiver)};
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
        while (!effect->groups.empty())
        {
            if (auto result = detachExecutionGroup(*effect, effect->groups.size() - 1); !result)
            {
                output_response = cxx::unexpected(result.error());
                return;
            }
        }
        effects.erase(input->effect);
        output_response = Ack{};
    }
} // namespace lux::render
