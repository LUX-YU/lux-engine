#include "Wire.hpp"
#include "Deferred.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
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
        struct View final { std::array<float, 4> color; };
        struct Scene final { cxx::SlotMap<View, ViewTag> views; };
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
            std::vector<ExecutionGroup> groups;
            std::size_t next_group = 0;
        };
        struct OperationEntry final
        {
            OperationId id;
            OperationHandler binding;
        };
    }

    struct RenderServer::Impl final
    {
        [[nodiscard]] RenderResult<void> registerFeature(FeatureRegistration feature) noexcept;
        [[nodiscard]] RenderResult<void> run() noexcept;
        void stop() noexcept { stop_requested.store(true); }
        [[nodiscard]] RenderServerStats stats() const noexcept;

        struct CompileJob final
        {
            Impl* owner;
            std::shared_ptr<RenderRequest> request;
            RenderResult<std::shared_ptr<const vulkan::CompiledGraph>> candidate =
                cxx::unexpected(RenderError{kRuntimeState});
            std::atomic<bool> done{false};
            CompileJob(Impl& server, RenderRequest input)
                : owner(&server), request(std::make_shared<RenderRequest>(std::move(input))) {}
        };
        std::vector<std::unique_ptr<CompileJob>> compiling;
        RenderServerConfig config;
        ipc::IpcResponder responder;
        SessionIdentity identity;
        std::vector<FeatureRegistration> features;
        std::vector<BufferInitializer> initializers;
        cxx::SlotMap<OperationEntry, OperationTag> operations;
        std::vector<std::byte> manifest;
        cxx::SlotMap<Scene, SceneTag> scenes;
        cxx::SlotMap<Target, TargetTag> targets;
        cxx::SlotMap<RenderEffect, EffectTag> effects;
        std::optional<vulkan::InstanceContext> instance;
        std::unique_ptr<vulkan::DeviceSession> device;
        std::vector<RenderRequest> undelivered;
        std::vector<DeferredCompletion> deferred;
        unsigned clients = 0;
        std::size_t next_effect = 0;
        std::atomic<bool> stop_requested{false}, started{false}, running{false};
        std::atomic<std::uint64_t> ticks{0}, commands{0}, compiled_graphs{0}, submissions{0};
        std::atomic<std::uint64_t> validation_errors{0}, validation_warnings{0};
        std::atomic<std::uint64_t> scoped_buffers_created{0};

        Impl(RenderServerConfig value, ipc::IpcResponder input, SessionIdentity session)
            : config(std::move(value)), responder(std::move(input)), identity(session)
        {
            undelivered.reserve(128);
            deferred.resize(config.completion_capacity);
            compiling.resize(config.compile_capacity);
            addOperation({
                &operations::createScene(),
                [](void* user, RenderRequest& request, OperationContext&) noexcept {
                    return static_cast<Impl*>(user)->createScene(request);
                },
                this
            });
            addOperation({
                &operations::removeScene(),
                [](void* user, RenderRequest& request, OperationContext&) noexcept {
                    return static_cast<Impl*>(user)->removeScene(request);
                },
                this
            });
            addOperation({
                &operations::createView(),
                [](void* user, RenderRequest& request, OperationContext&) noexcept {
                    return static_cast<Impl*>(user)->createView(request);
                },
                this
            });
            addOperation({
                &operations::removeView(),
                [](void* user, RenderRequest& request, OperationContext&) noexcept {
                    return static_cast<Impl*>(user)->removeView(request);
                },
                this
            });
            addOperation({
                &operations::createTarget(),
                [](void* user, RenderRequest& request, OperationContext&) noexcept {
                    return static_cast<Impl*>(user)->createTarget(request);
                },
                this
            });
            addOperation({
                &operations::resizeTarget(),
                [](void* user, RenderRequest& request, OperationContext&) noexcept {
                    return static_cast<Impl*>(user)->resizeTarget(request);
                },
                this
            });
            addOperation({
                &operations::createEffect(),
                [](void* user, RenderRequest& request, OperationContext&) noexcept {
                    return static_cast<Impl*>(user)->createEffect(request);
                },
                this
            });
            addOperation({
                &operations::removeEffect(),
                [](void* user, RenderRequest& request, OperationContext&) noexcept {
                    return static_cast<Impl*>(user)->removeEffect(request);
                },
                this
            });
            addOperation({
                &operations::attach(),
                [](void* user, RenderRequest& request, OperationContext&) noexcept {
                    return static_cast<Impl*>(user)->attach(request);
                },
                this
            });
            addOperation({
                &operations::detach(),
                [](void* user, RenderRequest& request, OperationContext&) noexcept {
                    return static_cast<Impl*>(user)->detach(request);
                },
                this
            });
            addOperation({
                &operations::readback(),
                [](void* user, RenderRequest& request, OperationContext&) noexcept {
                    return static_cast<Impl*>(user)->readback(request);
                },
                this
            });
            addOperation({
                &operations::stop(),
                [](void* user, RenderRequest& request, OperationContext&) noexcept {
                    return static_cast<Impl*>(user)->stopOperation(request);
                },
                this
            });
        }

        void addOperation(OperationHandler binding) noexcept
        {
            auto id = operations.emplace(OperationEntry{{}, binding});
            operations.find(id)->id = id;
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
            for (auto& slot : deferred)
            {
                const auto* current = std::get_if<vulkan::ExecutionContext*>(&slot.condition);
                if (!current || *current != context) continue;
                context->releaseReadback();
                auto result = slot.request->cancel();
                settle(std::move(*slot.request), result);
                slot = {};
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
            auto decoded = request.decode<AttachmentRequest>();
            if (!decoded) return cxx::unexpected(decoded.error());
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
            auto prepared = device->prepare(effect->code, target->description, {scene_id, view_id}, initializers);
            if (!prepared) return cxx::unexpected(prepared.error());
            (*prepared)->setColor(view->color);
            effect->groups.push_back({scene_id, view_id, target_id, *prepared});
            scoped_buffers_created.store(device->scopedBufferCreations());
            return request.reply();
        }

        RenderResult<void> resize(TargetId id, Target& target, const TargetDesc& description) noexcept
        {
            ExecutionGroup* group = nullptr;
            std::shared_ptr<const vulkan::CompiledGraph> code;
            for (auto& effect : effects)
                for (auto& candidate : effect.groups)
                    if (candidate.target == id) { group = &candidate; code = effect.code; }
            if (group)
            {
                if (auto admitted = admitWithdraw([&](const auto&, const auto& current) {
                    return current.target == id;
                }); !admitted) return admitted;
                // One Target writer, proven on attach. Resize builds one candidate without recompilation.
                auto candidate = device->prepare(std::move(code), description,
                    {group->scene, group->view}, initializers);
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
            scoped_buffers_created.store(device->scopedBufferCreations());
            return {};
        }

        RenderResult<void> readback(RenderRequest& request) noexcept
        {
            auto input = request.decode<AttachmentRequest>();
            if (!input) return cxx::unexpected(input.error());
            auto* effect = effects.find(input->effect);
            if (!effect) return cxx::unexpected(RenderError{kRuntimeIdentity});
            auto found = std::find_if(effect->groups.begin(), effect->groups.end(), [&](const auto& group) {
                return group.scene == input->scene && group.view == input->view && group.target == input->target;
            });
            if (found == effect->groups.end()) return cxx::unexpected(RenderError{kRuntimeIdentity});
            auto slot = std::find_if(deferred.begin(), deferred.end(), [](const auto& value) {
                return std::holds_alternative<std::monostate>(value.condition);
            });
            const bool full = slot == deferred.end() || request.replyCapacity() < reply_size + 20 ||
                found->context->readbackBytes() > request.replyCapacity() - reply_size - 20;
            if (full) return cxx::unexpected(RenderError{kRuntimeCapacity});
            found->context->retainReadback();
            slot->request.emplace(std::move(request));
            slot->condition = found->context;
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
            for (auto& slot : deferred)
            {
                if (!slot.request) continue;
                RenderResult<void> reply;
                if (auto* context = std::get_if<vulkan::ExecutionContext*>(&slot.condition))
                {
                    auto result = (*context)->readback();
                    if (!result && result.error().type == vulkan::kBusy) continue;
                    (*context)->releaseReadback();
                    auto data = result ? own(std::move(*result)) : OwnedWireBytes{};
                    const auto size = data.size();
                    reply = result ? slot.request->reply(ReadbackResult{{0, 0, size}}, std::move(data)) :
                        slot.request->fail(result.error());
                }
                else
                {
                    auto& gpu = std::get<GpuCompletion>(slot.condition);
#ifdef LUX_RUNTIME_TEST_SEAM
                    if (!runtime_detail::test::observeGpuCompletion()) continue;
#endif
                    RenderResult<void> ready;
                    if (gpu.upload)
                    {
                        auto progress = gpu.upload->advance();
                        if (!progress)
                        {
                            if (!gpu.failure) gpu.failure = progress.error();
                            gpu.upload->cancel(); // retain previous tickets, source, and CPU slice until safe
                            continue;
                        }
                        if (!*progress) continue;
                        ready = gpu.failure ? RenderResult<void>(cxx::unexpected(*gpu.failure)) :
                            gpu.texture_ready(gpu.owner.get(), *gpu.upload, *device);
                        gpu.upload.reset();
                    }
                    else
                    {
                        auto completed = gpu.queue->poll();
                        if (!completed) return cxx::unexpected(completed.error()); // shutdown drains before releasing pins
                        if (*completed < gpu.ticket->serial()) continue;
                        ready = gpu.ready(gpu.owner.get(), *device);
                    }
                    gpu.owner.reset(); // release Native pins while the source request still owns its input
#ifdef LUX_RUNTIME_TEST_SEAM
                    if (ready) runtime_detail::test::gpuReplyPublicationStarted();
#endif
                    reply = ready ? slot.request->publish(EOperationOutcome::SUCCESS, {},
                        std::span<const std::byte>(gpu.reply).first(gpu.size), std::move(gpu.attachment)) :
                        slot.request->fail(ready.error());

                }
                settle(std::move(*slot.request), reply);
                slot = {};
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

        RenderResult<std::shared_ptr<const vulkan::CompiledGraph>> compileEffect(const std::shared_ptr<RenderRequest>& request) noexcept
        {
            auto bytes = OwnedWireBytes::fromOwner(request, request->message().bytes().subspan(request_size));
            auto input = decodeWire<CreateEffectRequest>(
                {DataSchema<CreateEffectRequest>::describe(), bytes, {}}, wire_limits
            );
            if (!input) return cxx::unexpected(input.error());
            cxx::BinaryReader reader(input->selections.view(), config.max_wire_bytes);
            std::uint32_t count = 0;
            if (!reader.readUnsigned(count) || count == 0 || count > max_features)
                return cxx::unexpected(RenderError{kInvalidWire});
            std::vector<WireView> configs;
            std::vector<SelectedFeature> selected;
            std::vector<const FeatureRegistration*> registrations;
            std::vector<OwnedWireBytes> config_owners, attachments;
            std::vector<vulkan::PassProgram> programs;
            configs.reserve(count);
            selected.reserve(count);
            config_owners.reserve(count);
            for (auto view : request->message().attachments())
                attachments.push_back(OwnedWireBytes::fromOwner(request, view));
            for (std::uint32_t i = 0; i < count; ++i)
            {
                std::string_view type, name;
                cxx::SchemaVersion version;
                std::span<const std::byte> schema, config_bytes;
                const bool valid = readText(reader, type) && readText(reader, name) &&
                    reader.readUnsigned(version.major) && reader.readUnsigned(version.minor) &&
                    readBlock(reader, schema) && readBlock(reader, config_bytes);
                if (!valid) return cxx::unexpected(RenderError{kInvalidWire});
                const auto registration = std::find_if(features.begin(), features.end(), [&](const auto& f) {
                    return f.definition->type.name() == type;
                });
                if (registration == features.end() || registration->definition->version != version)
                    return cxx::unexpected(RenderError{kFeatureContract});
                const auto& definition = *registration->definition;
                const WireView* wire = nullptr;
                if (definition.config_schema)
                {
                    if (!std::ranges::equal(schemaBytes(*definition.config_schema), schema))
                        return cxx::unexpected(RenderError{kDataSchemaMismatch});
                    config_owners.push_back(OwnedWireBytes::fromOwner(request, config_bytes));
                    configs.push_back({*definition.config_schema, config_owners.back(), attachments});
                    wire = &configs.back();
                }
                else if (!schema.empty() || !config_bytes.empty()) return cxx::unexpected(RenderError{kInvalidWire});
                selected.push_back({&definition, wire, name});
                registrations.push_back(&*registration);
            }
            if (!reader.requireFullyConsumed()) return cxx::unexpected(RenderError{kInvalidWire});
            auto assembly = assembleFeatures(selected, {config.max_wire_bytes, SIZE_MAX});
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
            return std::make_shared<const vulkan::CompiledGraph>(std::move(*compiled));
        }

        RenderResult<void> createScene(RenderRequest& request) noexcept
        {
            return request.reply(SceneCreated{scenes.emplace()});
        }

        RenderResult<void> removeScene(RenderRequest& request) noexcept
        {
            auto input = request.decode<SceneRequest>();
            if (!input) return cxx::unexpected(input.error());
            const auto scene_id = input->scene;
            if (!scenes.find(scene_id)) return cxx::unexpected(RenderError{kRuntimeIdentity});
            if (auto admitted = admitWithdraw([&](const auto&, const auto& group) {
                return group.scene == scene_id;
            }); !admitted) return admitted;
            for (auto& effect : effects)
                for (std::size_t i = effect.groups.size(); i-- > 0;)
                    if (effect.groups[i].scene == scene_id)
                        if (auto result = withdraw(effect, i); !result) return result;
            device->releaseScope(scene_id);
            scenes.erase(scene_id);
            return request.reply();
        }

        RenderResult<void> createView(RenderRequest& request) noexcept
        {
            auto input = request.decode<CreateViewRequest>();
            if (!input) return cxx::unexpected(input.error());
            auto* scene = scenes.find(input->scene);
            if (!scene) return cxx::unexpected(RenderError{kRuntimeIdentity});
            if (!std::ranges::all_of(input->color, [](float x) { return std::isfinite(x); }))
                return cxx::unexpected(RenderError{kInvalidWire});
            return request.reply(ViewCreated{scene->views.emplace(View{input->color})});
        }

        RenderResult<void> removeView(RenderRequest& request) noexcept
        {
            auto input = request.decode<ViewRequest>();
            if (!input) return cxx::unexpected(input.error());
            auto* scene = scenes.find(input->scene);
            if (!scene || !scene->views.find(input->view)) return cxx::unexpected(RenderError{kRuntimeIdentity});
            if (auto admitted = admitWithdraw([&](const auto&, const auto& group) {
                return group.scene == input->scene && group.view == input->view;
            }); !admitted) return admitted;
            for (auto& effect : effects)
                for (std::size_t i = effect.groups.size(); i-- > 0;)
                    if (effect.groups[i].scene == input->scene && effect.groups[i].view == input->view)
                        if (auto result = withdraw(effect, i); !result) return result;
            device->releaseScope(input->scene, input->view);
            scene->views.erase(input->view);
            return request.reply();
        }

        RenderResult<void> createTarget(RenderRequest& request) noexcept
        {
            auto input = request.decode<CreateTargetRequest>();
            if (!input) return cxx::unexpected(input.error());
            if (auto valid = input->description.validate(); !valid) return valid;
            if (input->description.kind != ETargetKind::OFFSCREEN)
                return cxx::unexpected(RenderError{kRuntimeUnsupported});
            return request.reply(TargetCreated{targets.emplace(Target{input->description}), 1});
        }

        RenderResult<void> resizeTarget(RenderRequest& request) noexcept
        {
            auto input = request.decode<ResizeTargetRequest>();
            if (!input) return cxx::unexpected(input.error());
            if (auto valid = input->description.validate(); !valid) return valid;
            if (input->description.kind != ETargetKind::OFFSCREEN)
                return cxx::unexpected(RenderError{kRuntimeUnsupported});
            auto* target = targets.find(input->target);
            if (!target) return cxx::unexpected(RenderError{kRuntimeIdentity});
            if (target->epoch == UINT64_MAX) return cxx::unexpected(RenderError{kRuntimeCapacity});
            if (auto changed = resize(input->target, *target, input->description); !changed) return changed;
            return request.reply(TargetCreated{input->target, target->epoch});
        }

        static void compileTask(void* value) noexcept
        {
            auto& job = *static_cast<CompileJob*>(value);
            if (!job.owner->stop_requested.load()) job.candidate = job.owner->compileEffect(job.request);
            job.done.store(true, std::memory_order_release);
        }

        RenderResult<void> createEffect(RenderRequest& request) noexcept
        {
            if (!config.enqueue_compile) return cxx::unexpected(RenderError{kRuntimeUnsupported});
            const auto slot = std::find_if(compiling.begin(), compiling.end(), [](const auto& job) { return !job; });
            if (slot == compiling.end()) return cxx::unexpected(RenderError{kRuntimeCapacity});
            auto job = std::make_unique<CompileJob>(*this, std::move(request));
            if (!config.enqueue_compile(config.compile_user, compileTask, job.get()))
            {
                request = std::move(*job->request);
                return cxx::unexpected(RenderError{kRuntimeCapacity});
            }
            *slot = std::move(job);
            return {};
        }

        void publishCompiled(bool cancelled = false) noexcept
        {
            for (auto& slot : compiling)
            {
                if (!slot || !slot->done.load(std::memory_order_acquire)) continue;
                auto& job = *slot;
                RenderResult<void> result;
                if (cancelled) result = job.request->cancel();
                else if (!job.candidate) result = job.request->fail(job.candidate.error());
                else
                {
                    const auto effect = effects.emplace(RenderEffect{std::move(*job.candidate)});
                    result = job.request->reply(EffectCreated{effect});
                    if (result) ++compiled_graphs;
                    else effects.erase(effect); // never exposed or submitted; rejected publication owns no Effect
                }
                settle(std::move(*job.request), result);
                slot.reset();
            }
        }

        RenderResult<void> removeEffect(RenderRequest& request) noexcept
        {
            auto input = request.decode<EffectRequest>();
            if (!input) return cxx::unexpected(input.error());
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
            auto input = request.decode<AttachmentRequest>();
            if (!input) return cxx::unexpected(input.error());
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

        RenderResult<void> stopOperation(RenderRequest& request) noexcept
        {
            stop_requested.store(true);
            return request.reply();
        }

        void dispatch(ipc::ReceivedRequest received) noexcept
        {
            auto frame = readRequest(received.message().bytes());
            if (!frame)
            {
                // Uncorrelatable malformed framing closes this epoch; IPC preserves opaque original input.
                received.reset();
                stop_requested.store(true);
                return;
            }
            if (frame->reply_capacity < reply_size)
            {
                received.reset(); // malformed admission cannot fund a wire terminal; IPC returns original ABORTED
                ++commands;
                return;
            }
            const auto* entry = frame->operation.isNull() ? nullptr : operations.find(frame->operation);
            RenderRequest request(std::move(received), responder, identity, frame->request,
                frame->reply_capacity, entry ? entry->binding.descriptor : nullptr);
            const bool invalid_capacity = frame->reply_capacity < reply_size ||
                frame->reply_capacity > config.max_reply_bytes;
            RenderResult<void> result;
            if (invalid_capacity) result = cxx::unexpected(RenderError{kRuntimeCapacity});
            else if (frame->operation.isNull())
            {
                if (!frame->payload.empty() || clients == 2) result = cxx::unexpected(RenderError{kRuntimeCapacity});
                else { result = request.reply(std::span<const std::byte>(manifest)); if (result) ++clients; }
            }
            else if (!entry || frame->session != identity) result = cxx::unexpected(RenderError{kRuntimeIdentity});
            else if (entry->binding.descriptor->reply &&
                entry->binding.descriptor->reply->fixed_size > frame->reply_capacity - reply_size)
                result = cxx::unexpected(RenderError{kRuntimeCapacity});
            else
            {
                result = validateBoundWire(request.payload(), {config.max_wire_bytes, SIZE_MAX});
                if (result)
                {
                    OperationContext context(*device, deferred, request);
                    result = entry->binding.invoke(entry->binding.user, request, context);
                }
            }
            settle(std::move(request), result);
            ++commands;
        }

        void shutdown() noexcept
        {
            // Close admission first, then wait for all accepted callbacks before releasing any request/Device borrow.
            stop_requested.store(true);
            if (config.join_compile) config.join_compile(config.compile_user);
            if (device) device->joinUploads();
            publishCompiled(true);
            // GPU-ready owners may contain real in-flight staging. Drain only here, never in tick().
            const bool has_gpu = std::ranges::any_of(deferred, [](const auto& slot) {
                return slot.request && std::holds_alternative<GpuCompletion>(slot.condition);
            });
            if (has_gpu && device)
            {
                auto drained = device->drain();
                if (!drained && !device->deviceLost()) std::terminate();
            }
            for (auto& slot : deferred)
            {
                if (!slot.request) continue;
                if (auto* context = std::get_if<vulkan::ExecutionContext*>(&slot.condition))
                    (*context)->releaseReadback();
                const bool was_submitted = std::holds_alternative<GpuCompletion>(slot.condition);
                if (was_submitted)
                {
                    auto& gpu = std::get<GpuCompletion>(slot.condition);
                    gpu.upload.reset();
                    gpu.owner.reset();
                }
                // Accepted GPU work may already have side effects; cancellation is not provable.
                auto result = was_submitted ?
                    slot.request->publish(EOperationOutcome::OUTCOME_UNKNOWN, {}, {}, {}) : slot.request->cancel();
                settle(std::move(*slot.request), result);
                slot = {};
            }
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
        const bool invalid = config.requests_per_tick == 0 || config.requests_per_tick > 128 ||
            config.max_wire_bytes < reply_size || config.max_wire_bytes > wire_limits.max_wire_bytes ||
            config.max_reply_bytes < reply_size || config.max_reply_bytes > 256 * 1024 * 1024 ||
            config.completion_capacity == 0 || config.completion_capacity > 128 ||
            config.retirement_capacity == 0 || config.compile_capacity == 0 || config.compile_capacity > 8 ||
            bool(config.enqueue_compile) != bool(config.join_compile) ||
            bool(config.uploads.enqueue) != bool(config.uploads.join);
        if (invalid) return cxx::unexpected(RenderError{kRuntimeCapacity});
        SessionIdentity identity{};
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(identity.data()), sizeof(identity),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return cxx::unexpected(RenderError{kRuntimeState});
        auto responder = ipc::IpcCore::instance().createResponder(config.channel_name);
        if (!responder) return cxx::unexpected(RenderError{kRuntimeState});
        return std::unique_ptr<RenderServer>(new RenderServer(
            std::make_unique<Impl>(std::move(config), std::move(*responder), identity)));
    }

    RenderResult<void> RenderServer::Impl::registerFeature(FeatureRegistration feature) noexcept
    {
        auto& s = *this;
        const bool invalid = s.started.load() || !feature.definition ||
            (feature.definition && (!feature.definition->describe || feature.definition->type.name().empty()));
        if (invalid) return cxx::unexpected(RenderError{kRuntimeState});
        for (const auto& current : s.features)
            if (current.definition->type.name() == feature.definition->type.name())
                return cxx::unexpected(RenderError{kFeatureDuplicate});
        for (std::size_t i = 0; i < feature.buffers.size(); ++i)
        {
            const auto& buffer = feature.buffers[i];
            if (buffer.type.name().empty() || !buffer.initialize) return cxx::unexpected(RenderError{kFeatureContract});
            for (std::size_t j = 0; j < i; ++j)
            {
                const auto& earlier = feature.buffers[j];
                const bool conflict = earlier.type.name() == buffer.type.name() && earlier.abi == buffer.abi &&
                    earlier.initialize != buffer.initialize;
                if (conflict) return cxx::unexpected(RenderError{kFeatureDuplicate});
            }
            for (const auto& current : s.initializers)
                if (current.type.name() == buffer.type.name() && current.abi == buffer.abi &&
                    current.initialize != buffer.initialize) return cxx::unexpected(RenderError{kFeatureDuplicate});
        }
        if (feature.handlers.size() != feature.definition->operations.size())
            return cxx::unexpected(RenderError{kFeatureContract});
        for (std::size_t i = 0; i < feature.handlers.size(); ++i)
        {
            const auto& handler = feature.handlers[i];
            if (!handler.descriptor || !handler.invoke) return cxx::unexpected(RenderError{kFeatureContract});
            if ((handler.descriptor->completion == ECompletionContract::RESOURCE_GPU_READY) != handler.gpu_completion)
                return cxx::unexpected(RenderError{kProtocolContract});
            if (auto valid = compatibleOperations(feature.definition->operations[i], *handler.descriptor); !valid)
                return valid;
            for (const auto& old : s.operations)
                if (old.binding.descriptor->type.name() == handler.descriptor->type.name())
                    return cxx::unexpected(RenderError{kFeatureDuplicate});
            for (std::size_t previous = 0; previous < i; ++previous)
                if (feature.handlers[previous].descriptor->type.name() == handler.descriptor->type.name())
                    return cxx::unexpected(RenderError{kFeatureDuplicate});
        }
        for (const auto& handler : feature.handlers) s.addOperation(handler);
        for (const auto& buffer : feature.buffers)
            if (std::ranges::none_of(s.initializers, [&](const auto& current) {
                return current.type.name() == buffer.type.name() && current.abi == buffer.abi;
            })) s.initializers.push_back(buffer);
        s.features.push_back(feature);
        return {};
    }

    RenderResult<void> RenderServer::Impl::run() noexcept
    {
        auto& s = *this;
        if (s.started.exchange(true)) return cxx::unexpected(RenderError{kRuntimeState});
        if (s.stop_requested.load()) { s.shutdown(); return {}; }
        cxx::BinaryVectorWriter manifest;
        manifest.writeUnsigned<std::uint32_t>(static_cast<std::uint32_t>(s.operations.size()));
        for (const auto& entry : s.operations)
        {
            manifest.writeUnsigned(entry.id.index);
            manifest.writeUnsigned(entry.id.gen);
            writeBlock(manifest, operationBytes(*entry.binding.descriptor));
        }
        s.manifest = std::move(manifest).take();
        s.running.store(true);
        auto instance = vulkan::InstanceContext::create({{}, s.config.validation, Impl::diagnostic, &s});
        if (!instance) { s.shutdown(); return cxx::unexpected(instance.error()); }
        s.instance.emplace(std::move(*instance));
        vulkan::DeviceOptions options;
        options.dynamic_rendering = true;
        options.multiple_queues = s.config.transfer_queues;
        options.prefer_dedicated_queues = s.config.prefer_dedicated_transfer;
        options.texture_compression_bc = s.config.uploads.enqueue != nullptr;
        options.physical_device_index = s.config.physical_device_index;
        auto device = vulkan::DeviceSession::create(*s.instance, options, 3, s.config.retirement_capacity);
        if (!device) { s.shutdown(); return cxx::unexpected(device.error()); }
        s.device = std::move(*device);
        if (s.config.uploads.enqueue)
        {
            auto uploads = s.device->configureUploads(s.config.uploads);
            if (!uploads) { s.shutdown(); return cxx::unexpected(uploads.error()); }
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
            s.publishCompiled();
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

    RenderResult<void> RenderServer::registerFeature(FeatureRegistration feature) noexcept
    {
        return impl_->registerFeature(feature);
    }

    RenderResult<void> RenderServer::run() noexcept { return impl_->run(); }

    RenderServerStats RenderServer::stats() const noexcept { return impl_->stats(); }
}
