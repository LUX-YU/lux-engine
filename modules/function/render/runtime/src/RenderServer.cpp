#include "TypedProtocol.hpp"
#include "Deferred.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>

namespace lux::render
{
    namespace ipc = platform::ipc;
    using namespace runtime_detail;

    namespace
    {
        struct View final { std::array<float, 4> color; ResourceDomain domain; };
        struct Scene final { ResourceDomain domain; cxx::SlotMap<View, ViewTag> views; };
        struct Target final { TargetDesc description; std::uint64_t epoch = 1; };
        struct ExecutionGroup final
        {
            SceneId scene;
            ViewId view;
            TargetId target;
            vulkan::ExecutionContext* context;
        };
        struct RenderEffect final
        {
            std::shared_ptr<const vulkan::CompiledGraph> code;
            std::vector<StorageArgument> arguments;
            std::vector<ExecutionGroup> groups;
            std::size_t next_group = 0;
        };
        struct CommandEntry final
        {
            CommandId id;
            CommandHandler binding;
            CommandTransport transport;
            std::shared_ptr<const void> code_pin;
            const StorageDescriptor* storage = nullptr;
            bool available = true;
        };
    }

    struct RenderServer::Impl final
    {
        [[nodiscard]] RenderResult<void> registerFeature(FeatureRegistration feature, std::span<const CommandTransport> transports) noexcept;
        [[nodiscard]] RenderResult<void> registerStorage(StorageRegistration storage, std::span<const CommandTransport> transports) noexcept;
        [[nodiscard]] RenderResult<void> run() noexcept;
        void stop() noexcept { stop_requested.store(true); }
        [[nodiscard]] RenderServerStats stats() const noexcept;

        RenderServerConfig config;
        ipc::IpcBoundType<ValueEnvelope<RenderError>> failure_type;
        ipc::IpcResponder responder;
        SessionIdentity identity;
        std::vector<FeatureRegistration> features;
        std::vector<StorageRegistration> storage_types;
        cxx::SlotMap<CommandEntry, CommandTag> command_table;
        HelloResponse manifest;
        cxx::SlotMap<Scene, SceneTag> scenes;
        cxx::SlotMap<Target, TargetTag> targets;
        cxx::SlotMap<RenderEffect, EffectTag> effects;
        std::optional<vulkan::InstanceContext> instance;
        std::unique_ptr<vulkan::DeviceSession> device;
        std::vector<RenderRequest> undelivered;
        DeferredSlots deferred;
        CompletionQueue completion_events;
        std::atomic<std::size_t> completion_publishers{0};
        std::size_t compiling = 0;
        unsigned clients = 0;
        std::size_t next_effect = 0;
        std::atomic<bool> stop_requested{false}, started{false}, running{false};
        std::atomic<std::uint64_t> ticks{0}, commands{0}, compiled_graphs{0}, submissions{0};
        std::atomic<std::uint64_t> validation_errors{0}, validation_warnings{0};
        std::atomic<std::uint64_t> scoped_buffers_created{0};

        Impl(RenderServerConfig value, ipc::IpcResponder input, SessionIdentity session, std::size_t pending_capacity)
            : config(std::move(value)),
              failure_type(*ipc::IpcMessage::bindType<ValueEnvelope<RenderError>>(
                  ipc::IpcMessage::type<ValueEnvelope<RenderError>>(), config.implementation_code_pin)),
              responder(std::move(input)), identity(session), completion_events(pending_capacity)
        {
            manifest.failure_code_pin = config.implementation_code_pin;
            manifest.failure_ops = &failure_type.operations();
            undelivered.reserve(128);
            // Separate admission budgets share one bounded owner and one event queue.
            deferred.reserve(pending_capacity);
            addBuiltin<CreateScene, &Impl::createScene>();
            addBuiltin<RemoveScene, &Impl::removeScene>();
            addBuiltin<CreateView, &Impl::createView>();
            addBuiltin<RemoveView, &Impl::removeView>();
            addBuiltin<CreateTarget, &Impl::createTarget>();
            addBuiltin<ResizeTarget, &Impl::resizeTarget>();
            addBuiltin<CreateEffect, &Impl::createEffect>();
            addBuiltin<RemoveEffect, &Impl::removeEffect>();
            addBuiltin<Attach, &Impl::attach>();
            addBuiltin<Detach, &Impl::detach>();
            addBuiltin<Readback, &Impl::readback>();
            addBuiltin<Stop, &Impl::stopCommand>();
            addBuiltin<QueryRenderStats, &Impl::queryRenderStats>();
        }

        template <RenderCommand C, RenderResult<void> (Impl::*Method)(RenderRequest&) noexcept>
        void addBuiltin() noexcept
        {
            addCommand({&commandDescriptor<C>(), [](void* owner, CommandInvocation& invocation) noexcept {
                return (static_cast<Impl*>(owner)->*Method)(
                    static_cast<GpuAdmission*>(invocation.completion_owner)->request);
            }, this}, commandTransport<C>(), nullptr, config.implementation_code_pin);
        }

        void addCommand(CommandHandler binding, CommandTransport transport,
            const StorageDescriptor* storage = nullptr, std::shared_ptr<const void> pin = {}) noexcept
        {
            // Full registration comparison proved equivalence; retain the template's static contract.
            binding.descriptor = transport.descriptor;
            auto id = command_table.emplace(CommandEntry{{}, binding, transport, std::move(pin), storage});
            command_table.find(id)->id = id;
        }

        RenderResult<void> validateHandlers(std::span<const CommandHandler> handlers, std::span<const CommandTransport> transports) const noexcept
        {
            if (handlers.size() != transports.size()) return cxx::unexpected(RenderError{kFeatureContract});
            for (std::size_t i = 0; i < handlers.size(); ++i)
            {
                const auto& handler = handlers[i];
                if (!handler.descriptor || !handler.invoke) return cxx::unexpected(RenderError{kFeatureContract});
                if (auto valid = validateCommand(*handler.descriptor); !valid) return valid;
                if (auto valid = compatibleCommands(*handler.descriptor, *transports[i].descriptor); !valid) return valid;
                if ((handler.descriptor->completion == ECompletionContract::RESOURCE_GPU_READY) != handler.gpu_completion)
                    return cxx::unexpected(RenderError{kProtocolContract});
                for (const auto& old : command_table)
                    if (old.binding.descriptor->name.name() == handler.descriptor->name.name())
                        return cxx::unexpected(RenderError{kFeatureDuplicate});
                for (std::size_t previous = 0; previous < i; ++previous)
                    if (handlers[previous].descriptor->name.name() == handler.descriptor->name.name())
                        return cxx::unexpected(RenderError{kFeatureDuplicate});
            }
            return {};
        }

        void settle(RenderRequest request, RenderResult<void> result) noexcept
        {
            if (!request.active()) return;
            auto replied = request.fail(result ? RenderError{kRuntimeState} : result.error());
            if (!replied)
            {
                undelivered.push_back(std::move(request));
                if (undelivered.size() >= undelivered.capacity()) stop_requested.store(true);
            }
        }

        void cancelReadbacks(vulkan::ExecutionContext* context) noexcept
        {
            for (auto it = deferred.begin(); it != deferred.end();)
            {
                auto& slot = *it;
                const auto* current = std::get_if<vulkan::ExecutionContext*>(&slot.condition);
                if (!current || *current != context) { ++it; continue; }
                context->releaseReadback();
                auto result = slot.request->cancel();
                settle(std::move(*slot.request), result);
                deferred.erase(slot.id);
            }
        }

        RenderResult<void> withdraw(RenderEffect& effect, std::size_t index) noexcept
        {
            auto* context = effect.groups[index].context;
            cancelReadbacks(context);
            if (auto result = device->release(context); !result) return result;
            effect.groups.erase(effect.groups.begin() + index);
            return {};
        }

        template <class Predicate>
        RenderResult<void> admitWithdraw(Predicate matches) const noexcept
        {
            std::size_t needed = 0;
            for (const auto& effect : effects)
                for (const auto& group : effect.groups)
                    if (matches(effect, group))
                        if (auto ticket = group.context->lastUse(); ticket &&
                            ticket->serial() > device->queue().completed()) ++needed;
            const auto& retired = device->retirement();
            if (needed > retired.capacity() - retired.pending())
                return cxx::unexpected(RenderError{kRuntimeCapacity});
            return {};
        }

        RenderResult<void> attach(RenderRequest& request) noexcept
        {
            const auto* decoded = &request.get<AttachmentRequest>();
            const auto [scene_id, view_id, target_id, effect_id] = *decoded;
            const auto* scene = scenes.find(scene_id);
            const auto* view = scene ? scene->views.find(view_id) : nullptr;
            const auto* target = targets.find(target_id);
            auto* effect = effects.find(effect_id);
            const bool invalid = !view || !target || !effect;
            if (invalid) return cxx::unexpected(RenderError{kRuntimeIdentity});
            for (const auto& existing : effects)
                for (const auto& group : existing.groups)
                    if (group.target == target_id) return cxx::unexpected(RenderError{kRuntimeState});
            auto prepared = device->prepare(effect->code, target->description, view->domain, effect->arguments);
            if (!prepared) return cxx::unexpected(prepared.error());
            (*prepared)->setColor(view->color);
            effect->groups.push_back({scene_id, view_id, target_id, *prepared});
            scoped_buffers_created.store(device->resources().creations());
            return request.reply();
        }

        RenderResult<void> resize(TargetId id, Target& target, const TargetDesc& description) noexcept
        {
            ExecutionGroup* group = nullptr;
            std::shared_ptr<const vulkan::CompiledGraph> code;
            std::span<const StorageArgument> arguments;
            for (auto& effect : effects)
                for (auto& candidate : effect.groups)
                    if (candidate.target == id) { group = &candidate; code = effect.code; arguments = effect.arguments; }
            if (group)
            {
                if (auto admitted = admitWithdraw([&](const auto&, const auto& current) {
                    return current.target == id;
                }); !admitted) return admitted;
                // One Target writer, proven on attach. Resize builds one candidate without recompilation.
                auto candidate = device->prepare(std::move(code), description,
                    scenes.find(group->scene)->views.find(group->view)->domain, arguments);
                if (!candidate) return cxx::unexpected(candidate.error());
                (*candidate)->setColor(group->context->color());
                cancelReadbacks(group->context);
                if (auto result = device->release(group->context); !result)
                {
                    if (!device->release(*candidate)) std::terminate(); // never submitted
                    return result;
                }
                group->context = *candidate;
            }
            target.description = description;
            ++target.epoch;
            scoped_buffers_created.store(device->resources().creations());
            return {};
        }

        RenderResult<void> readback(RenderRequest& request) noexcept
        {
            const auto* input = &request.get<AttachmentRequest>();
            auto* effect = effects.find(input->effect);
            if (!effect) return cxx::unexpected(RenderError{kRuntimeIdentity});
            auto found = std::find_if(effect->groups.begin(), effect->groups.end(), [&](const auto& group) {
                return group.scene == input->scene && group.view == input->view && group.target == input->target;
            });
            if (found == effect->groups.end()) return cxx::unexpected(RenderError{kRuntimeIdentity});
            const bool full = request.replyCapacity() < sizeof(ReadbackResult) ||
                found->context->readbackBytes() > request.replyCapacity() - sizeof(ReadbackResult);
            if (full) return cxx::unexpected(RenderError{kRuntimeCapacity});
            auto* slot = reserveCompletion();
            if (!slot) return cxx::unexpected(RenderError{kRuntimeCapacity});
            found->context->retainReadback();
            slot->request.emplace(std::move(request));
            slot->condition.emplace<vulkan::ExecutionContext*>(found->context);
            return {};
        }

        RenderResult<void> tick() noexcept
        {
            for (std::size_t e = 0; e < effects.size(); ++e)
            {
                auto& effect = *(effects.begin() + (next_effect + e) % effects.size());
                for (std::size_t g = 0; g < effect.groups.size(); ++g)
                {
                    auto& group = effect.groups[(effect.next_group + g) % effect.groups.size()];
                    auto result = group.context->execute();
                    if (!result) return cxx::unexpected(result.error());
                    if (*result) ++submissions;
                }
                effect.next_group = effect.groups.empty() ? 0 : (effect.next_group + 1) % effect.groups.size();
            }
            next_effect = effects.empty() ? 0 : (next_effect + 1) % effects.size();
            for (auto it = deferred.begin(); it != deferred.end();)
            {
                auto& slot = *it;
                if (!slot.request || std::holds_alternative<CpuCompletion>(slot.condition)) { ++it; continue; }
                RenderResult<void> reply;
                if (auto* context = std::get_if<vulkan::ExecutionContext*>(&slot.condition))
                {
                    auto result = (*context)->readback();
                    if (!result && result.error().type == vulkan::kBusy) { ++it; continue; }
                    (*context)->releaseReadback();
                    if (result)
                    {
                        auto owner = std::make_shared<const std::vector<std::byte>>(std::move(*result));
                        auto payload = RenderPayload::fromVector(owner, 0, owner->size());
                        if (!payload) std::terminate(); // exact vector range
                        reply = slot.request->reply(ReadbackResult{std::move(*payload)});
                    }
                    else reply = slot.request->fail(result.error());
                }
                else
                {
                    auto& gpu = std::get<GpuCompletion>(slot.condition);
#ifdef LUX_RUNTIME_TEST_SEAM
                    if (!runtime_detail::test::observeGpuCompletion()) { ++it; continue; }
#endif
                    auto progress = gpu.work.advance();
                    if (!progress) return cxx::unexpected(progress.error());
                    if (!*progress) { ++it; continue; }
                    auto ready = gpu.work.publish();
                    gpu.work.reset(); // release Native pins while the original request still owns its source
#ifdef LUX_RUNTIME_TEST_SEAM
                    if (ready) runtime_detail::test::gpuReplyPublicationStarted();
#endif
                    if (ready) reply = slot.request->publishPrepared(gpu.reply, gpu.reply_charge);
                    else
                    {
                        // Drop the unsent result before returning its funding request to a concurrent Client.
                        gpu.reply.reset();
                        reply = slot.request->fail(ready.error());
                    }

                }
                settle(std::move(*slot.request), reply);
                deferred.erase(slot.id);
            }
            return device->collect();
        }

        static VKAPI_ATTR VkBool32 VKAPI_CALL diagnostic(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
            VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT*, void* user) noexcept
        {
            auto& state = *static_cast<Impl*>(user);
            if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++state.validation_errors;
            if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++state.validation_warnings;
            return VK_FALSE;
        }

        RenderResult<RenderEffect> compileEffect(const RenderRequest& request) noexcept
        {
            const auto* input = &request.get<CreateEffectRequest>();
            const auto count = input->features.size();
            if (count == 0 || count > max_features) return cxx::unexpected(RenderError{kFeatureContract});
            std::vector<WireView> configs;
            std::vector<SelectedFeature> selected;
            std::vector<const FeatureRegistration*> registrations;
            std::vector<OwnedWireBytes> attachments;
            std::vector<PassProgram> programs;
            configs.reserve(count);
            selected.reserve(count);
            for (const auto& attachment : input->attachments) attachments.push_back(attachment.bytes());
            for (const auto& selection : input->features)
            {
                const auto registration = std::find_if(features.begin(), features.end(), [&](const auto& f) {
                    return f.definition->type.name() == selection.type;
                });
                if (registration == features.end() || registration->definition->version != selection.version)
                    return cxx::unexpected(RenderError{kFeatureContract});
                const auto& definition = *registration->definition;
                const WireView* wire = nullptr;
                if (definition.config_schema)
                {
                    if (!selection.config_schema) return cxx::unexpected(RenderError{kDataSchemaMismatch});
                    configs.push_back({*selection.config_schema, selection.config.bytes(), attachments});
                    wire = &configs.back(); // assembleFeatures checks the complete author Schema once.
                }
                else if (selection.config_schema || !selection.config.bytes().empty())
                    return cxx::unexpected(RenderError{kFeatureContract});
                selected.push_back({&definition, wire, selection.instance});
                registrations.push_back(&*registration);
            }
            auto assembly = assembleFeatures(selected, {config.max_config_bytes, SIZE_MAX});
            if (!assembly) return cxx::unexpected(assembly.error());
            for (const auto& contribution : assembly->contributions)
            {
                if (contribution.pass_count == 0) continue;
                const auto index = contribution.selection_index;
                if (!registrations[index]->programs) return cxx::unexpected(RenderError{kFeatureContract});
                const auto& selection = selected[index];
                auto values = registrations[index]->programs(selection.config, selection.instance_key);
                if (!values) return cxx::unexpected(values.error());
                const auto passes = assembly->definition.passes().subspan(contribution.first_pass,
                    contribution.pass_count);
                for (std::size_t p = 0; p < values->size(); ++p)
                {
                    const auto& value = (*values)[p];
                    if (std::ranges::none_of(passes, [&](const auto& pass) { return pass.key == value.pass_key; }))
                        return cxx::unexpected(RenderError{kFeatureOwnership, {index, p}});
                    for (std::size_t previous = 0; previous < p; ++previous)
                        if ((*values)[previous].pass_key == value.pass_key)
                            return cxx::unexpected(RenderError{kFeatureDuplicate, {index, p, previous}});
                }
                for (auto& value : *values) programs.push_back(std::move(value));
            }
            auto compiled = vulkan::compileGraph(*device, *assembly, programs, input->description);
            if (!compiled) return cxx::unexpected(compiled.error());
            auto arguments = programArguments(programs);
            if (!arguments) return cxx::unexpected(arguments.error());
            return RenderEffect{std::make_shared<const vulkan::CompiledGraph>(std::move(*compiled)), std::move(*arguments)};
        }

        RenderResult<void> createScene(RenderRequest& request) noexcept
        {
            auto domain = device->resources().createDomain(device->resources().root(), EResourceScope::SCENE);
            if (!domain) return cxx::unexpected(domain.error());
            return request.reply(SceneCreated{scenes.emplace(Scene{std::move(*domain), {}})});
        }

        RenderResult<void> removeScene(RenderRequest& request) noexcept
        {
            const auto* input = &request.get<SceneRequest>();
            const auto scene_id = input->scene;
            if (!scenes.find(scene_id)) return cxx::unexpected(RenderError{kRuntimeIdentity});
            if (auto admitted = admitWithdraw([&](const auto&, const auto& group) {
                return group.scene == scene_id;
            }); !admitted) return admitted;
            for (auto& effect : effects)
                for (std::size_t i = effect.groups.size(); i-- > 0;)
                    if (effect.groups[i].scene == scene_id)
                        if (auto result = withdraw(effect, i); !result) return result;
            (void)device->resources().closeDomain(scenes.find(scene_id)->domain);
            scenes.erase(scene_id);
            return request.reply();
        }

        RenderResult<void> createView(RenderRequest& request) noexcept
        {
            const auto* input = &request.get<CreateViewRequest>();
            auto* scene = scenes.find(input->scene);
            if (!scene) return cxx::unexpected(RenderError{kRuntimeIdentity});
            if (!std::ranges::all_of(input->color, [](float x) { return std::isfinite(x); }))
                return cxx::unexpected(RenderError{kInvalidWire});
            auto domain = device->resources().createDomain(scene->domain, EResourceScope::VIEW);
            if (!domain) return cxx::unexpected(domain.error());
            return request.reply(ViewCreated{scene->views.emplace(View{input->color, std::move(*domain)})});
        }

        RenderResult<void> removeView(RenderRequest& request) noexcept
        {
            const auto* input = &request.get<ViewRequest>();
            auto* scene = scenes.find(input->scene);
            if (!scene || !scene->views.find(input->view)) return cxx::unexpected(RenderError{kRuntimeIdentity});
            if (auto admitted = admitWithdraw([&](const auto&, const auto& group) {
                return group.scene == input->scene && group.view == input->view;
            }); !admitted) return admitted;
            for (auto& effect : effects)
                for (std::size_t i = effect.groups.size(); i-- > 0;)
                    if (effect.groups[i].scene == input->scene && effect.groups[i].view == input->view)
                        if (auto result = withdraw(effect, i); !result) return result;
            (void)device->resources().closeDomain(scene->views.find(input->view)->domain);
            scene->views.erase(input->view);
            return request.reply();
        }

        RenderResult<void> createTarget(RenderRequest& request) noexcept
        {
            const auto* input = &request.get<CreateTargetRequest>();
            if (auto valid = input->description.validate(); !valid) return valid;
            if (input->description.kind != ETargetKind::OFFSCREEN)
                return cxx::unexpected(RenderError{kRuntimeUnsupported});
            return request.reply(TargetCreated{targets.emplace(Target{input->description}), 1});
        }

        RenderResult<void> resizeTarget(RenderRequest& request) noexcept
        {
            const auto* input = &request.get<ResizeTargetRequest>();
            if (auto valid = input->description.validate(); !valid) return valid;
            if (input->description.kind != ETargetKind::OFFSCREEN)
                return cxx::unexpected(RenderError{kRuntimeUnsupported});
            auto* target = targets.find(input->target);
            if (!target) return cxx::unexpected(RenderError{kRuntimeIdentity});
            if (target->epoch == UINT64_MAX) return cxx::unexpected(RenderError{kRuntimeCapacity});
            if (auto changed = resize(input->target, *target, input->description); !changed) return changed;
            return request.reply(TargetCreated{input->target, target->epoch});
        }

        DeferredCompletion* reserveCompletion(bool for_compile = false) noexcept
        {
            const bool full = for_compile ? compiling >= config.compile_capacity :
                deferred.size() - compiling >= config.completion_capacity;
            if (full) return nullptr;
            auto id = deferred.tryEmplacePrepared();
            if (!id) return nullptr;
            auto& slot = *deferred.find(*id);
            slot.id = *id;
            slot.session = identity;
            slot.events = &completion_events;
            slot.publishers = &completion_publishers;
            slot.owner = &deferred;
            return &slot;
        }

        static void completeCpu(void* value, ECommandOutcome outcome, RenderError error, void* response) noexcept
        {
            auto& slot = *static_cast<DeferredCompletion*>(value);
            auto& cpu = std::get<CpuCompletion>(slot.condition);
            cpu.outcome = outcome;
            cpu.error = error;
            if (outcome == ECommandOutcome::SUCCESS)
            {
                auto charge = slot.request->prepareResponse(response, cpu.reply);
                if (charge) cpu.reply_charge = *charge;
                else { cpu.outcome = ECommandOutcome::FAILED; cpu.error = charge.error(); }
            }
            slot.post()(nullptr, nullptr, {});
        }

        RenderResult<void> deferCommand(RenderRequest& request, void* sender, const CommandAsyncOps& operations) noexcept
        {
            // A plain finite CPU sender is not evidence of a Native GPU completion.
            if (!request.active()) return cxx::unexpected(RenderError{kRuntimeState});
            if (request.gpuReady()) return cxx::unexpected(RenderError{kProtocolContract});
            auto* slot = reserveCompletion();
            if (!slot) return cxx::unexpected(RenderError{kRuntimeCapacity});
            slot->request.emplace(std::move(request));
            auto& cpu = slot->condition.emplace<CpuCompletion>();
            cpu.functions = &operations;
            cpu.operation = operations.connect(sender, {slot, completeCpu, slot->stop.get_token()});
            // start runs on the RenderThread. Event draining cannot reenter this stack; even immediate
            // completion is only published, then adopted at the next owner-loop safety point.
            operations.start(cpu.operation);
            return {};
        }

        struct CompileState final
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
                void set_error(RenderError error) && noexcept
                {
                    slot->post()(nullptr, nullptr, cxx::unexpected(error));
                }
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
                : owner(&server), slot(&pending), operation(stdexec::connect(
                    server.config.cpu.schedule([](void* value) noexcept {
                        auto& state = *static_cast<CompileState*>(value);
                        state.candidate = state.owner->compileEffect(*state.slot->request);
                    }, this, server.config.implementation_code_pin, pending.request->codePin()), Receiver{&pending})) {}

            static RenderResult<void> adopt(void* value, RenderRequest& request) noexcept
            {
                auto& state = *static_cast<CompileState*>(value);
                if (state.owner->stop_requested.load()) return request.cancel();
                if (!state.slot->cpu_result) return request.fail(state.slot->cpu_result.error());
                if (!state.candidate) return request.fail(state.candidate.error());
                const auto effect = state.owner->effects.emplace(std::move(*state.candidate));
                auto result = request.reply(EffectCreated{effect});
                if (result) ++state.owner->compiled_graphs;
                else state.owner->effects.erase(effect);
                return result;
            }
        };

        RenderResult<void> createEffect(RenderRequest& request) noexcept
        {
            if (!config.cpu) return cxx::unexpected(RenderError{kRuntimeUnsupported});
            auto* slot = reserveCompletion(true);
            if (!slot) return cxx::unexpected(RenderError{kRuntimeCapacity});
            slot->request.emplace(std::move(request));
            auto& cpu = slot->condition.emplace<CpuCompletion>();
            static constexpr CommandAsyncOps operations{
                nullptr,
                [](void* state) noexcept { stdexec::start(static_cast<CompileState*>(state)->operation); },
                [](void* state) noexcept { delete static_cast<CompileState*>(state); }
            };
            cpu.functions = &operations;
            cpu.compiling = true;
            cpu.adopt = CompileState::adopt;
            cpu.operation = new CompileState(*this, *slot);
            ++compiling;
            operations.start(cpu.operation);
            return {};
        }

        void drainCompletions(std::size_t budget = 128) noexcept
        {
            CompletionEvent event;
            for (std::size_t i = 0; i < budget &&
                completion_events.tryPop(event) == cxx::EQueuePopResult::VALUE; ++i)
            {
                // A copied event identity is checked once at its actual cross-thread boundary.
                if (event.session != identity) continue;
                auto* slot = deferred.find(event.id);
                if (!slot) continue;
                const auto resume = std::exchange(slot->resume, nullptr);
                if (resume) resume(slot->continuation, std::move(slot->cpu_result));
                auto* cpu = std::get_if<CpuCompletion>(&slot->condition);
                if (!cpu) continue; // Native continuation resumes the same GPU request, not a second owner.
                RenderResult<void> result;
                if (cpu->adopt) result = cpu->adopt(cpu->operation, *slot->request);
                else if (stop_requested.load()) { cpu->reply.reset(); result = slot->request->cancel(); }
                else if (cpu->outcome == ECommandOutcome::SUCCESS)
                    result = slot->request->publishPrepared(cpu->reply, cpu->reply_charge);
                else if (cpu->outcome == ECommandOutcome::CANCELLED)
                { cpu->reply.reset(); result = slot->request->cancel(); }
                else { cpu->reply.reset(); result = slot->request->fail(cpu->error); }
                if (cpu->compiling) --compiling;
                settle(std::move(*slot->request), result);
                deferred.erase(event.id);
            }
        }

        RenderResult<void> removeEffect(RenderRequest& request) noexcept
        {
            const auto* input = &request.get<EffectRequest>();
            auto* effect = effects.find(input->effect);
            if (!effect) return cxx::unexpected(RenderError{kRuntimeIdentity});
            if (auto admitted = admitWithdraw([&](const auto& owner, const auto&) {
                return &owner == effect;
            }); !admitted) return admitted;
            while (!effect->groups.empty())
                if (auto result = withdraw(*effect, effect->groups.size() - 1); !result) return result;
            effects.erase(input->effect);
            return request.reply();
        }

        RenderResult<void> detach(RenderRequest& request) noexcept
        {
            const auto* input = &request.get<AttachmentRequest>();
            auto* effect = effects.find(input->effect);
            if (!effect) return cxx::unexpected(RenderError{kRuntimeIdentity});
            for (std::size_t i = 0; i < effect->groups.size(); ++i)
            {
                const auto& group = effect->groups[i];
                if (group.scene == input->scene && group.view == input->view && group.target == input->target)
                {
                    if (auto admitted = admitWithdraw([&](const auto&, const auto& current) {
                        return current.context == group.context;
                    }); !admitted) return admitted;
                    if (auto result = withdraw(*effect, i); !result) return result;
                    return request.reply();
                }
            }
            return cxx::unexpected(RenderError{kRuntimeIdentity});
        }

        RenderResult<void> queryRenderStats(RenderRequest& request) noexcept
        {
            return request.reply(RenderStatsSnapshot{ticks.load(), commands.load(),
                compiled_graphs.load(), submissions.load()});
        }

        RenderResult<void> stopCommand(RenderRequest& request) noexcept
        {
            stop_requested.store(true);
            return request.reply();
        }

        void dispatch(ipc::ReceivedRequest received) noexcept
        {
            const auto frame = received.message().metadata<RequestMetadata>();
            if (frame.request.isNull() || frame.reply_capacity < sizeof(RenderError))
            {
                received.reset(); // No correlatable/funded terminal: IPC returns the original ABORTED input.
                ++commands;
                return;
            }
            static const auto hello_transport = commandTransport<Hello>();
            const auto* entry = frame.command.isNull() ? nullptr : command_table.find(frame.command);
            const auto* transport = frame.command.isNull() ? &hello_transport : (entry ? &entry->transport : nullptr);
            const auto* actual_type = received.message().operations();
            const bool valid_type = frame.command.isNull() ?
                actual_type && bool(ipc::IpcMessage::bindType<ValueEnvelope<HelloRequest>>(*actual_type)) :
                transport && received.message().holds(*transport->request);
            const auto* descriptor = frame.command.isNull() ? &commandDescriptor<Hello>() :
                (entry ? entry->binding.descriptor : nullptr);
            const bool valid_identity = frame.command.isNull() ||
                (entry && entry->available && frame.session == identity);
            const bool valid_capacity = frame.reply_capacity <= config.max_reply_bytes;
            bool funded = false;
            if (valid_identity && valid_type && valid_capacity)
            {
                auto& message = const_cast<ipc::IpcMessage&>(received.message());
                const auto reply_overhead = std::max(transport->response->size() - descriptor->response.size,
                    sizeof(ValueEnvelope<RenderError>) - sizeof(RenderError));
                funded = transport->request_budget(message).bytes() >=
                    transport->request->size() + frame.reply_capacity + reply_overhead;
            }
            RenderRequest request(std::move(received), responder, identity, frame.request, frame.reply_capacity,
                descriptor, funded ? transport : nullptr, failure_type,
                entry ? entry->code_pin : config.implementation_code_pin);
            RenderResult<void> result;
            if (!valid_identity) result = cxx::unexpected(RenderError{kRuntimeIdentity});
            else if (!valid_type) result = cxx::unexpected(RenderError{kProtocolContract});
            else if (!valid_capacity || !funded) result = cxx::unexpected(RenderError{kRuntimeCapacity});
            else if (frame.command.isNull())
            {
                if (clients == 2) result = cxx::unexpected(RenderError{kRuntimeCapacity});
                else { result = request.reply(manifest); if (result) ++clients; }
            }
            else if (descriptor->response.size > frame.reply_capacity)
                result = cxx::unexpected(RenderError{kRuntimeCapacity});
            else
            {
                // Compiling cannot change during this serialized handler call; this is the ordinary
                // completion budget expressed as an absolute limit in the single shared slot map.
                GpuAdmission admission{*device, deferred, config.completion_capacity + compiling, completion_events,
                    completion_publishers, identity, config.implementation_code_pin, request, this,
                    [](void* owner, RenderRequest& input, void* sender, const CommandAsyncOps& operations) noexcept {
                        return static_cast<Impl*>(owner)->deferCommand(input, sender, operations);
                    }};
                vulkan::GpuCompletionAccess access{&gpu_completion_functions, &admission};
                auto invocation = request.invocation(admission, entry->binding.gpu_completion ? &access : nullptr);
                result = entry->binding.invoke(entry->binding.user, invocation);
            }
            settle(std::move(request), result);
            ++commands;
        }

        void shutdown() noexcept
        {
            stop_requested.store(true);
            for (auto& slot : deferred)
            {
                slot.stop.request_stop();
                if (auto* gpu = std::get_if<GpuCompletion>(&slot.condition)) gpu->work.cancel();
            }
            // Only shutdown waits for accepted GPU work. Cancellation prevents another Transfer submit;
            // pending CPU preparation and its source request stay owned until their actual event arrives.
            // Drain before polling work to its terminal state, which may itself require this real wait.
            if (device)
            {
                auto drained = device->drain();
                if (!drained && !device->deviceLost()) std::terminate();
            }
            // Shutdown consumes actual CPU notifications. The same pending owners retain all source and
            // Native state while already accepted work runs to its safe terminal point.
            while (!deferred.empty())
            {
                drainCompletions();
                for (auto it = deferred.begin(); it != deferred.end();)
                {
                    auto& slot = *it;
                    if (std::holds_alternative<CpuCompletion>(slot.condition)) { ++it; continue; }
                    const bool was_submitted = std::holds_alternative<GpuCompletion>(slot.condition);
                    if (was_submitted)
                    {
                        auto& gpu = std::get<GpuCompletion>(slot.condition);
                        auto progress = gpu.work.advance();
                        if (!progress && !device->deviceLost()) std::terminate();
                        if (progress && !*progress) { ++it; continue; }
                        gpu.work.reset();
                        gpu.reply.reset();
                    }
                    else if (auto* context = std::get_if<vulkan::ExecutionContext*>(&slot.condition))
                        (*context)->releaseReadback();
                    auto result = was_submitted ? slot.request->unknown() : slot.request->cancel();
                    settle(std::move(*slot.request), result);
                    deferred.erase(slot.id);
                }
                if (!deferred.empty()) std::this_thread::yield();
            }
            while (completion_publishers.load(std::memory_order_acquire) != 0) std::this_thread::yield();
            undelivered.clear();
            responder.reset();
            effects.clear();
            device.reset();
            instance.reset();
            running.store(false);
        }
    };

    RenderServer::RenderServer(std::unique_ptr<Impl> state) noexcept : impl_(std::move(state)) {}

    RenderServer::~RenderServer() noexcept
    {
        if (impl_->running.load()) std::terminate(); // host must stop + join
    }

    RenderResult<std::unique_ptr<RenderServer>> RenderServer::create(RenderServerConfig config) noexcept
    {
        const auto pending_capacity = cxx::checkedAdd(config.completion_capacity, config.compile_capacity);
        const bool invalid_pending_capacity = !pending_capacity || *pending_capacity > 136;
        const bool invalid = config.requests_per_tick == 0 || config.requests_per_tick > 128 ||
            config.max_config_bytes == 0 || config.max_config_bytes > config_limits.max_wire_bytes ||
            config.max_reply_bytes < sizeof(RenderError) || config.max_reply_bytes > 256 * 1024 * 1024 ||
            config.completion_capacity == 0 || config.completion_capacity > 128 ||
            config.retirement_capacity == 0 || config.compile_capacity == 0 || config.compile_capacity > 8 ||
            invalid_pending_capacity;
        if (invalid) return cxx::unexpected(RenderError{kRuntimeCapacity});
        SessionIdentity identity{};
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(identity.data()), sizeof(identity),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return cxx::unexpected(RenderError{kRuntimeState});
        auto responder = ipc::IpcCore::instance().createResponder(config.channel_name);
        if (!responder) return cxx::unexpected(RenderError{kRuntimeState});
        return std::unique_ptr<RenderServer>(new RenderServer(
            std::make_unique<Impl>(std::move(config), std::move(*responder), identity, *pending_capacity)));
    }

    RenderResult<void> RenderServer::Impl::registerFeature(FeatureRegistration feature, std::span<const CommandTransport> transports) noexcept
    {
        auto& s = *this;
        const bool invalid = s.started.load() || !feature.definition ||
            (feature.definition && (!feature.definition->describe || feature.definition->type.name().empty()));
        if (invalid) return cxx::unexpected(RenderError{kRuntimeState});
        for (const auto& current : s.features)
            if (current.definition->type.name() == feature.definition->type.name())
                return cxx::unexpected(RenderError{kFeatureDuplicate});
        if (feature.handlers.size() != feature.definition->commands.size())
            return cxx::unexpected(RenderError{kFeatureContract});
        if (auto valid = s.validateHandlers(feature.handlers, transports); !valid) return valid;
        for (std::size_t i = 0; i < feature.handlers.size(); ++i)
        {
            const auto& handler = feature.handlers[i];
            if (auto valid = compatibleCommands(feature.definition->commands[i], *handler.descriptor); !valid)
                return valid;
        }
        for (std::size_t i = 0; i < feature.handlers.size(); ++i)
            s.addCommand(feature.handlers[i], transports[i], nullptr, feature.code_pin);
        s.features.push_back(feature);
        return {};
    }

    RenderResult<void> RenderServer::Impl::run() noexcept
    {
        auto& s = *this;
        if (s.started.exchange(true)) return cxx::unexpected(RenderError{kRuntimeState});
        if (s.stop_requested.load()) { s.shutdown(); return {}; }
        s.running.store(true);
        auto instance = vulkan::InstanceContext::create({{}, s.config.validation, Impl::diagnostic, &s});
        if (!instance) { s.shutdown(); return cxx::unexpected(instance.error()); }
        s.instance.emplace(std::move(*instance));
        vulkan::DeviceOptions options;
        options.dynamic_rendering = true;
        options.multiple_queues = s.config.transfer_queues;
        options.prefer_dedicated_queues = s.config.prefer_dedicated_transfer;
        options.texture_compression_bc = s.config.texture_compression_bc;
        std::uint32_t required = 0, optional = 0;
        for (const auto& type : s.storage_types)
        {
            if (type.optional) optional |= type.description->required_capabilities;
            else required |= type.description->required_capabilities;
            optional |= type.description->optional_capabilities;
        }
        options.texture_descriptors = (required & 1) != 0;
        options.texture_compression_bc |= (required & 2) != 0;
        options.optional_texture_descriptors = (optional & 1) != 0;
        options.optional_texture_compression_bc = (optional & 2) != 0;
        options.host_query_reset = s.config.uploads && s.config.uploads->measure_gpu;
        options.physical_device_index = s.config.physical_device_index;
        auto device = vulkan::DeviceSession::create(*s.instance, options, 3, s.config.retirement_capacity);
        if (!device) { s.shutdown(); return cxx::unexpected(device.error()); }
        s.device = std::move(*device);
        if (s.config.uploads)
        {
            auto uploads = s.device->configureUploads(*s.config.uploads, s.config.cpu);
            if (!uploads) { s.shutdown(); return cxx::unexpected(uploads.error()); }
        }
        auto& resources = s.device->resources();
        for (const auto& type : s.storage_types)
        {
            if (const auto* factory = resources.factory(type.description->type))
            {
                if (auto valid = validateStorageFactory(*type.description, *factory, EGraphicsApi::VULKAN); !valid)
                { s.shutdown(); return cxx::unexpected(valid.error()); }
            }
            else if (auto installed = resources.installFactory(type.factory); !installed)
            { s.shutdown(); return cxx::unexpected(installed.error()); }
            if (auto configured = resources.configureType(*type.description, type.configuration); !configured)
            { s.shutdown(); return cxx::unexpected(configured.error()); }
        }
        resources.freeze();
        const std::uint32_t capabilities = (s.device->device().textureDescriptors() ? 1u : 0u) |
            (s.device->device().textureCompressionBc() ? 2u : 0u);
        for (auto& entry : s.command_table)
        {
            entry.available = !entry.storage || (entry.storage->required_capabilities & ~capabilities) == 0;
            if (entry.binding.owner == ECommandOwner::DEVICE_SESSION) entry.binding.user = s.device.get();
            if (!entry.available) continue;
            const auto& descriptor = *entry.binding.descriptor;
            s.manifest.capabilities.push_back({entry.code_pin, std::string(descriptor.name.name()), entry.id,
                descriptor.version, descriptor.request, descriptor.response, descriptor.completion,
                entry.transport.request, entry.transport.response});
        }
        RenderResult<void> result;
        while (!s.stop_requested.load())
        {
            for (std::uint32_t i = 0; i < s.config.requests_per_tick && !s.stop_requested.load(); ++i)
            {
                ipc::ReceivedRequest request;
                if (s.responder.tryReceive(request) == ipc::EIpcReceive::RECEIVED) s.dispatch(std::move(request));
            }
            if (s.stop_requested.load()) break;
            s.drainCompletions();
            if (auto rendered = s.tick(); !rendered) { result = cxx::unexpected(rendered.error()); break; }
            ++s.ticks;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        s.shutdown();
        return result;
    }

    void RenderServer::stop() noexcept { impl_->stop(); }

    RenderServerStats RenderServer::Impl::stats() const noexcept
    {
        const auto& s = *this;
        return {s.ticks.load(), s.commands.load(), s.compiled_graphs.load(), s.submissions.load(),
            s.validation_errors.load(), s.validation_warnings.load(), s.scoped_buffers_created.load()};
    }

    RenderResult<void> RenderServer::registerFeatureImpl(FeatureRegistration feature, std::span<const CommandTransport> transports) noexcept
    {
        return impl_->registerFeature(std::move(feature), transports);
    }

    RenderResult<void> RenderServer::Impl::registerStorage(StorageRegistration storage, std::span<const CommandTransport> transports) noexcept
    {
        if (started.load() || !storage.description) return cxx::unexpected(RenderError{kRuntimeState});
        if (auto valid = validateStorage(*storage.description); !valid) return valid;
        if (auto valid = validateStorageFactory(*storage.description, storage.factory, EGraphicsApi::VULKAN); !valid)
            return valid;
        for (const auto& existing : storage_types)
        {
            if (existing.description->type != storage.description->type) continue;
            if (auto match = compatibleStorage(*existing.description, *storage.description); !match) return match;
            return cxx::unexpected(RenderError{kFeatureDuplicate});
        }
        if (auto valid = validateHandlers(storage.handlers, transports); !valid) return valid;
        for (std::size_t i = 0; i < storage.handlers.size(); ++i)
            addCommand(storage.handlers[i], transports[i], storage.description, storage.factory.code_pin);
        storage_types.push_back(std::move(storage));
        return {};
    }

    RenderResult<void> RenderServer::registerStorageImpl(StorageRegistration storage, std::span<const CommandTransport> transports) noexcept
    {
        return impl_->registerStorage(std::move(storage), transports);
    }

    RenderResult<void> RenderServer::run() noexcept { return impl_->run(); }

    RenderServerStats RenderServer::stats() const noexcept { return impl_->stats(); }
}
