#include "EffectAttachment.hpp"
#include "RuntimeChannel.hpp"
#include <algorithm>
#include <chrono>
#include <lux/cxx/container/BasicSparseSet.hpp>
#include <lux/engine/render/runtime/DeviceSession.hpp>

namespace lux::render
{
    namespace
    {
        constexpr auto kHostFailure = error::errorId("lux.render.runtime.host_failure");
        using Clock = std::chrono::steady_clock;

        template <typename Key> std::uint64_t scopeIdentity(Key key) noexcept
        {
            return (std::uint64_t{key.gen} << 32) | (std::uint64_t{key.index} + 1);
        }

        template <typename Key> RuntimeReply identityReply(Key key, std::uint64_t frame) noexcept
        {
            RuntimeReply reply;
            reply.frame_serial = frame;
            reply.index = key.index;
            reply.generation = key.gen;
            return reply;
        }
    } // namespace

    struct RenderBackend::Impl
    {
        // Native bootstrap owner order: Session is destroyed before Instance.
        std::unique_ptr<vulkan::InstanceContext> instance;
        std::unique_ptr<vulkan::DeviceSession> session;
        cxx::SlotMap<Scene, RenderSceneTag> scenes;
        cxx::SlotMap<Target, RenderTargetTag> targets;
        cxx::SlotMap<RenderEffect, RenderEffectTag> effects;
        cxx::SlotKeySparseSet<RenderEffectId, std::vector<std::unique_ptr<EffectAttachment>>> effect_bindings;
        std::vector<PreparedExecution> jobs;
        std::shared_ptr<detail::RuntimeChannel> channel;
        RuntimeCapacity capacity;
        RuntimeOptions options;
        RuntimeProgress progress;
        HANDLE worker{};
        const std::uint64_t assembly_thread{detail::transportThreadIdentity()};
        bool started{};

        Impl(
            std::unique_ptr<vulkan::InstanceContext> host,
            std::unique_ptr<vulkan::DeviceSession> native,
            std::shared_ptr<detail::RuntimeChannel> endpoint,
            RuntimeCapacity limits,
            RuntimeOptions settings
        ) noexcept
            : instance(std::move(host)), session(std::move(native)), channel(std::move(endpoint)), capacity(limits),
              options(settings)
        {
            jobs.reserve(capacity.bindings);
            scenes.reserve(capacity.scenes);
            targets.reserve(capacity.targets);
            effects.reserve(capacity.effects);
            effect_bindings.prepareCapacity(capacity.effects, capacity.effects);
        }

        View* find(ViewRef ref) noexcept
        {
            auto* scene = scenes.find(ref.scene);
            return scene ? scene->views.find(ref.view) : nullptr;
        }

        EffectAttachment* job(RenderEffectId effect, EffectBinding binding) noexcept
        {
            const auto* relations = effect_bindings.tryGet(effect);
            if (!relations)
            {
                return nullptr;
            }
            for (const auto& attachment : *relations)
            {
                if (attachment->binding == binding)
                {
                    return attachment.get();
                }
            }
            return nullptr;
        }

        void refreshSchedule() noexcept
        {
            for (auto& prepared : jobs)
            {
                prepared.view = find(prepared.attachment->binding.view);
                if (!prepared.view)
                {
                    std::terminate();
                }
            }
        }

        RenderResult<void> prepareResources(EffectAttachment& attachment, const RenderEffect& effect) noexcept
        {
            auto collected = session->resources.scoped.collect();
            if (!collected)
            {
                return collected;
            }
            for (const auto& request : effect.buffers)
            {
                vulkan::ResourceScopeKey scope;
                scope.scope = request.scope;
                if (request.scope != EResourceScope::GLOBAL)
                {
                    scope.scene = attachment.binding.view.scene;
                }
                if (request.scope == EResourceScope::VIEW || request.scope == EResourceScope::ATTACHMENT)
                {
                    scope.view = attachment.binding.view.view;
                }
                if (request.scope == EResourceScope::ATTACHMENT)
                {
                    scope.effect = attachment.effect;
                    scope.target = attachment.binding.target;
                }
                const auto& resource = attachment.code.logical().identity().resources[request.resource.value() - 1];
                auto usage = attachment.code.resourceUsage(request.resource);
                if (!usage)
                {
                    return cxx::unexpected(vulkan::resourceError(usage.error()));
                }
                auto factory = session->resources.bufferFactory(
                    resource.buffer().byte_size,
                    static_cast<VkBufferUsageFlags>(*usage) | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                );
                auto lease = session->resources.scoped.getOrCreate<vulkan::Buffer>(scope, request.key, factory);
                if (!lease)
                {
                    return cxx::unexpected(vulkan::resourceError(lease.error()));
                }
                const auto identity = GraphBackingId{scopeIdentity(lease->id())};
                attachment.imports.push_back({request.resource, identity, 0, 1});
                attachment.native_imports.push_back(
                    {request.resource,
                     identity,
                     std::cref(lease->get()),
                     {VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_IMAGE_LAYOUT_UNDEFINED,
                      session->device.queues[0]->nativeQueue().family},
                     lease->ready(),
                     true}
                );
                attachment.leases.push_back(std::move(*lease));
            }
            return {};
        }

        template <typename Predicate> bool bindingUsed(Predicate predicate) const noexcept
        {
            for (const auto& relations : effect_bindings.values())
            {
                if (std::any_of(
                        relations.begin(),
                        relations.end(),
                        [&](const auto& a) { return predicate(a->binding); }
                    ))
                {
                    return true;
                }
            }
            return false;
        }

        void snapshot() noexcept
        {
            const std::array values{
                progress.render_frames,
                progress.native_invocations,
                progress.reused_scene_passes,
                progress.publications,
                progress.native_compilations,
                progress.native_cache_hits
            };
            for (std::size_t i = 0; i < values.size(); ++i)
            {
                channel->counters[i].store(values[i], std::memory_order_relaxed);
            }
        }

        void fail(RenderError error) noexcept
        {
            error = vulkan::resourceError(error);
            if (!channel->failure)
            {
                channel->failure = error;
                channel->diagnose(error);
            }
            channel->stop();
        }

        RenderResult<std::uint32_t> attach(
            RenderGraphId graph_id,
            const GraphInvocationData& initial,
            std::span<const vulkan::NativeImportBinding> imports
        ) noexcept
        {
            auto& graph = *session->graphs.find(graph_id);
            const std::array samplers{std::pair{GraphSampler{1}, std::cref(session->resources.sampler)}};
            const vulkan::NativeInvocationInputs inputs{
                session->device.device,
                session->device.allocator,
                imports,
                initial,
                samplers,
                capacity.frames_in_flight
            };
            auto* pool = session->resources.executions.tryGet(graph_id);
            if (!pool)
            {
                auto native = vulkan::GraphExecutionContext::create(graph.code, inputs);
                if (!native)
                {
                    return cxx::unexpected(vulkan::resourceError(native.error()));
                }
                auto context =
                    std::make_unique<vulkan::GraphExecution>(vulkan::GraphExecution{std::move(*native), {}, {}});
                context->trace.resize(context->native.traceCapacity());
                context->bindings.reserve(capacity.bindings);
                context->bindings.push_back({true, initial});
                session->resources.executions.insert(graph_id, std::move(context));
                return 0;
            }
            auto& context = **pool;
            for (std::uint32_t i = 0; i < context.bindings.size(); ++i)
            {
                if (context.bindings[i].occupied)
                {
                    continue;
                }
                bool ready = true;
                for (std::uint32_t slot = 0; slot < capacity.frames_in_flight; ++slot)
                {
                    auto result = context.native.slotReady(i * capacity.frames_in_flight + slot);
                    if (!result)
                    {
                        return cxx::unexpected(vulkan::resourceError(result.error()));
                    }
                    ready = ready && *result;
                }
                if (ready)
                {
                    context.bindings[i] = vulkan::BindingExecution{true, initial};
                    return i;
                }
            }
            if (context.bindings.size() >= capacity.bindings)
            {
                return cxx::unexpected(RenderError{vulkan::kBusy});
            }
            auto appended = context.native.append(inputs);
            if (!appended)
            {
                return cxx::unexpected(vulkan::resourceError(appended.error()));
            }
            const auto index = static_cast<std::uint32_t>(context.bindings.size());
            if (*appended != index * capacity.frames_in_flight)
            {
                std::terminate();
            }
            context.bindings.push_back({true, initial});
            return index;
        }

        RenderResult<RuntimeReply> control(const RuntimeControl& request) noexcept;
        RenderResult<void> publish(const RuntimeBindingPublication& request, std::span<const std::byte> bytes) noexcept;
        RenderResult<void> execute(std::uint64_t time) noexcept;
        void run() noexcept;
    };

    RenderResult<RuntimeReply> RenderBackend::Impl::control(const RuntimeControl& request) noexcept
    {
        if (request.owner != channel->owner)
        {
            return cxx::unexpected(RenderError{kTransportWrongOwner});
        }
        const auto invalid = [] { return cxx::unexpected(RenderError{vulkan::kInvalidArgument}); };
        const auto busy = [] { return cxx::unexpected(RenderError{vulkan::kBusy}); };
        if (request.operation == ERuntimeControl::CREATE_SCENE)
        {
            if (scenes.size() >= capacity.scenes)
            {
                return cxx::unexpected(RenderError{vulkan::kCapacity});
            }
            Scene scene;
            scene.views.reserve(capacity.views);
            const auto id = scenes.emplace(std::move(scene));
            refreshSchedule();
            return identityReply(id, progress.render_frames);
        }
        if (request.operation == ERuntimeControl::CREATE_TARGET)
        {
            if (!request.first || !request.second)
            {
                return invalid();
            }
            if (targets.size() >= capacity.targets)
            {
                return cxx::unexpected(RenderError{vulkan::kCapacity});
            }
            return identityReply(targets.emplace(Target{request.first, request.second}), progress.render_frames);
        }
        if (request.operation == ERuntimeControl::CREATE_VIEW)
        {
            const RenderSceneId scene_id{request.scene_index, request.scene_generation};
            auto* scene = scenes.find(scene_id);
            if (!scene)
            {
                return invalid();
            }
            std::size_t count{};
            for (const auto& value : scenes)
            {
                count += value.views.size();
            }
            if (count >= capacity.views)
            {
                return cxx::unexpected(RenderError{vulkan::kCapacity});
            }
            const auto view_id = scene->views.emplace(View{});
            refreshSchedule();
            auto reply = identityReply(view_id, progress.render_frames);
            reply.scene_index = scene_id.index;
            reply.scene_generation = scene_id.gen;
            return reply;
        }
        const RenderEffectId effect_id{request.effect_index, request.effect_generation};
        const RenderTargetId target_id{request.target_index, request.target_generation};
        const ViewRef ref{{request.scene_index, request.scene_generation}, {request.index, request.generation}};
        const EffectBinding binding{ref, target_id};
        if (request.operation == ERuntimeControl::ATTACH_EFFECT)
        {
            auto* effect = effects.find(effect_id);
            auto* target = targets.find(target_id);
            if (!effect || !target || !find(ref))
            {
                return invalid();
            }
            if (job(effect_id, binding))
            {
                return RuntimeReply{};
            } // idempotent, no second Native slot/job
            // P2-A has no Target composition: reject another writer explicitly.
            if (bindingUsed([&](const auto& entry) { return entry.target == target_id; }))
            {
                return busy();
            }
            if (jobs.size() >= capacity.bindings)
            {
                return cxx::unexpected(RenderError{vulkan::kCapacity});
            }
            const auto* graph = session->graphs.find(effect->graph);
            if (!graph || graph->width != target->width || graph->height != target->height)
            {
                return invalid();
            }
            auto candidate = std::make_unique<EffectAttachment>(effect_id, binding, effect->graph, graph->code);
            auto prepared = prepareResources(*candidate, *effect);
            if (!prepared)
            {
                return cxx::unexpected(vulkan::resourceError(prepared.error()));
            }
            auto execution = attach(effect->graph, effect->initial_values, candidate->native_imports);
            if (!execution)
            {
                return cxx::unexpected(vulkan::resourceError(execution.error()));
            }
            auto& values = (*session->resources.executions.tryGet(effect->graph))->bindings[*execution].values;
            values.scene = scopeIdentity(ref.scene);
            values.view = scopeIdentity(ref.view);
            values.target = scopeIdentity(target_id);
            auto* relations = effect_bindings.tryGet(effect_id);
            if (!relations)
            {
                effect_bindings.insert(effect_id, std::vector<std::unique_ptr<EffectAttachment>>{});
                relations = effect_bindings.tryGet(effect_id);
            }
            candidate->context = session->resources.executions.tryGet(effect->graph)->get();
            candidate->execution = *execution;
            jobs.push_back({candidate.get(), find(ref)});
            relations->push_back(std::move(candidate));
            return RuntimeReply{};
        }
        if (request.operation == ERuntimeControl::DETACH_EFFECT)
        {
            auto* entry = job(effect_id, binding);
            if (!entry)
            {
                return invalid();
            }
            std::erase_if(jobs, [&](const auto& prepared) { return prepared.attachment == entry; });
            std::erase_if(*effect_bindings.tryGet(effect_id), [&](const auto& a) { return a.get() == entry; });
            auto collected = session->resources.scoped.collect();
            if (!collected)
            {
                // Logical detach has committed. Keep pending backing and report collection failure separately.
                channel->diagnose(vulkan::resourceError(collected.error()));
            }
            return RuntimeReply{};
        }
        if (request.operation == ERuntimeControl::REMOVE_SCENE)
        {
            const RenderSceneId key{request.index, request.generation};
            auto* scene = scenes.find(key);
            if (!scene)
            {
                return invalid();
            }
            if (!scene->views.empty())
            {
                return busy();
            }
            scenes.erase(key);
            refreshSchedule();
            return RuntimeReply{};
        }
        if (request.operation == ERuntimeControl::REMOVE_TARGET)
        {
            const RenderTargetId key{request.index, request.generation};
            auto* target = targets.find(key);
            if (!target)
            {
                return invalid();
            }
            if (bindingUsed([&](const auto& entry) { return entry.target == key; }))
            {
                return busy();
            }
            targets.erase(key);
            return RuntimeReply{};
        }
        if (request.operation == ERuntimeControl::REMOVE_EFFECT)
        {
            const RenderEffectId key{request.index, request.generation};
            auto* effect = effects.find(key);
            if (!effect)
            {
                return invalid();
            }
            const auto* relations = effect_bindings.tryGet(key);
            if (relations && !relations->empty())
            {
                return busy();
            }
            const auto graph_id = effect->graph;
            effect_bindings.erase(key);
            effects.erase(key);
            const bool referenced =
                std::any_of(effects.begin(), effects.end(), [&](const auto& other) { return other.graph == graph_id; });
            if (!referenced)
            {
                // Native destruction waits all actual tickets, including partial submissions.
                session->resources.executions.erase(graph_id);
                session->graphs.erase(graph_id);
            }
            return RuntimeReply{};
        }
        auto* view = find(ref);
        if (!view)
        {
            return invalid();
        }
        if (request.operation == ERuntimeControl::SET_VIEW_ACTIVE)
        {
            view->active = request.first != 0;
            return RuntimeReply{};
        }
        if (request.operation == ERuntimeControl::REMOVE_VIEW)
        {
            if (bindingUsed([&](const auto& entry) { return entry.view == ref; }))
            {
                return busy();
            }
            scenes.find(ref.scene)->views.erase(ref.view);
            refreshSchedule();
            return RuntimeReply{};
        }
        auto* execution_job = job(effect_id, binding);
        if (!execution_job)
        {
            return invalid();
        }
        auto& context = *execution_job->context;
        auto& execution = context.bindings[execution_job->execution];
        if (request.operation == ERuntimeControl::QUERY_GRAPH)
        {
            return identityReply(execution_job->graph, progress.render_frames);
        }
        if (request.operation == ERuntimeControl::READBACK || request.operation == ERuntimeControl::READBACK_TARGET)
        {
            if (!execution.latest || !request.bytes || request.bytes > RuntimeReply{}.bytes.size())
            {
                return invalid();
            }
            auto last = context.native.lastUse();
            if (!last)
            {
                return cxx::unexpected(vulkan::resourceError(last.error()));
            }
            if (!*last)
            {
                return invalid();
            }
            while (!channel->stopping.load(std::memory_order_acquire))
            {
                auto done = session->device.queues[0]->wait(**last, 10'000'000);
                if (!done)
                {
                    return cxx::unexpected(vulkan::resourceError(done.error()));
                }
                if (*done)
                {
                    break;
                }
            }
            if (channel->stopping.load(std::memory_order_acquire))
            {
                return cxx::unexpected(RenderError{kTransportStopping});
            }
            RuntimeReply reply{execution.latest_frame, request.bytes};
            const bool final_output = request.operation == ERuntimeControl::READBACK_TARGET;
            const auto pass =
                final_output ? execution_job->code.logical().executionOrder().back() : GraphPassId{request.first};
            auto read = context.native.readbackPass(
                *execution.latest,
                pass,
                final_output ? 0 : request.second,
                request.offset,
                std::span{reply.bytes}.first(request.bytes)
            );
            if (!read)
            {
                return cxx::unexpected(vulkan::resourceError(read.error()));
            }
            return reply;
        }
        return invalid();
    }

    RenderResult<void> RenderBackend::Impl::publish(
        const RuntimeBindingPublication& request,
        std::span<const std::byte> bytes
    ) noexcept
    {
        if (request.owner != channel->owner)
        {
            return cxx::unexpected(RenderError{kTransportWrongOwner});
        }
        const ViewRef ref{{request.scene_index, request.scene_generation}, {request.index, request.generation}};
        auto* j =
            job({request.effect_index, request.effect_generation},
                {ref, {request.target_index, request.target_generation}});
        if (!j)
        {
            return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
        }
        auto& execution = j->context->bindings[j->execution];
        const auto& passes = j->code.logical().identity().passes;
        const bool invalid_pass = request.pass == 0 || request.pass > passes.size();
        if (invalid_pass)
        {
            return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
        }
        auto& values = execution.values.passes[request.pass - 1];
        const bool invalid_values = passes[request.pass - 1].scope == EExecutionScope::SCENE ||
                                    bytes.size() != values.scalars.size() || request.revision <= execution.revision;
        if (invalid_values)
        {
            return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
        }
        std::copy(bytes.begin(), bytes.end(), values.scalars.begin());
        execution.revision = request.revision;
        ++progress.publications;
        return {};
    }

    RenderResult<void> RenderBackend::Impl::execute(std::uint64_t time) noexcept
    {
        const auto serial = progress.render_frames + 1;
        const auto frame_slot = static_cast<std::uint32_t>(serial % capacity.frames_in_flight);
        std::array<std::uint64_t, 3> budget{};
        for (const auto& prepared : jobs)
        {
            const auto& j = *prepared.attachment;
            if (!prepared.view->active)
            {
                continue;
            }
            auto& context = *j.context;
            auto ready = context.native.slotReady(j.execution * capacity.frames_in_flight + frame_slot);
            if (!ready)
            {
                return cxx::unexpected(vulkan::resourceError(ready.error()));
            }
            if (!*ready)
            {
                return cxx::unexpected(RenderError{vulkan::kBusy});
            }
            const auto needed = context.native.submissionBudget();
            for (unsigned q = 0; q < 3; ++q)
            {
                budget[q] += needed[q];
            }
        }
        for (auto* queue : session->device.queues)
        {
            std::uint64_t needed{};
            for (unsigned q = 0; q < 3; ++q)
            {
                if (session->device.queues[q] == queue)
                {
                    needed += budget[q];
                }
            }
            if (needed > queue->capacity())
            {
                return cxx::unexpected(RenderError{vulkan::kCapacity});
            }
            if (needed > queue->capacity() - (queue->submitted() - queue->completed()))
            {
                return cxx::unexpected(RenderError{vulkan::kBusy});
            }
        }
        for (const auto& prepared : jobs)
        {
            const auto& j = *prepared.attachment;
            if (!prepared.view->active)
            {
                continue;
            }
            auto& context = *j.context;
            auto& execution = context.bindings[j.execution];
            const auto& logical = j.code.logical();
            auto bindings =
                FrameGraphBindings::create(logical, {serial, time, frame_slot}, j.imports, execution.values);
            if (!bindings)
            {
                return cxx::unexpected(vulkan::resourceError(bindings.error()));
            }
            std::optional<vulkan::GraphSubmission> donor;
            for (const auto& prior_job : jobs)
            {
                const auto& candidate = *prior_job.attachment;
                if (candidate.graph != j.graph || candidate.execution == j.execution)
                {
                    continue;
                }
                const auto& prior = context.bindings[candidate.execution];
                const bool eligible = prior.occupied && candidate.binding.view.scene == j.binding.view.scene &&
                                      prior.latest && prior.latest_frame == serial && !prior.scene_borrowed;
                if (!eligible)
                {
                    continue;
                }
                auto previous =
                    FrameGraphBindings::create(logical, {serial, time, frame_slot}, candidate.imports, prior.values);
                if (!previous)
                {
                    return cxx::unexpected(vulkan::resourceError(previous.error()));
                }
                bool share = false;
                for (auto pass : logical.executionOrder())
                {
                    if (!logical.sceneShareEligible()[pass.value() - 1])
                    {
                        continue;
                    }
                    share = mayShareSceneInvocation(pass, *previous, *bindings);
                    if (!share)
                    {
                        break;
                    }
                }
                if (share)
                {
                    donor = prior.latest;
                    break;
                }
            }
            prepared.attachment->prepareImports();
            auto submitted = vulkan::executeGraph(
                context.native,
                j.execution * capacity.frames_in_flight + frame_slot,
                *bindings,
                j.native_imports,
                context.trace,
                donor
            );
            prepared.attachment->retainAccepted(session->device.queues);
            if (!submitted)
            {
                return cxx::unexpected(vulkan::resourceError(submitted.error()));
            }
            execution.latest = *submitted;
            execution.latest_frame = serial;
            execution.scene_borrowed = donor.has_value();
            ++progress.native_invocations;
            for (std::uint32_t i = 0; i < submitted->trace_count; ++i)
            {
                progress.reused_scene_passes += context.trace[i].operation == vulkan::ENativeTrace::REUSE_SCENE;
            }
        }
        ++progress.render_frames;
        return {};
    }

    void RenderBackend::Impl::run() noexcept
    {
        auto upload = channel->transport->makeUploadPacket();
        const auto origin = Clock::now();
        auto deadline = origin;
        const auto interval = std::chrono::nanoseconds{options.frame_interval_ns};
        while (!channel->stopping.load(std::memory_order_acquire))
        {
            for (unsigned i = 0; i < 3; ++i)
            {
                auto result = channel->transport->pollControl(
                    [&](RenderPacketView& packet) noexcept
                    {
                        for (std::size_t record = 0; record < packet.size(); ++record)
                        {
                            auto request = packet.read(channel->control, record);
                            auto answer = request ? control(*request)
                                                  : RenderResult<RuntimeReply>{
                                                        cxx::unexpected(vulkan::resourceError(request.error()))
                                                    };
                            auto done = answer ? packet.complete(record, *answer) : packet.fail(record, answer.error());
                            if (!done && done.error().type != kTransportReply &&
                                done.error().type != kTransportStopping)
                            {
                                channel->diagnose(done.error());
                            }
                            if (!answer && answer.error().type == vulkan::kNativeFailure &&
                                answer.error().args[0] == static_cast<std::uint32_t>(VK_ERROR_DEVICE_LOST))
                            {
                                fail(answer.error());
                            }
                        }
                    }
                );
                if (!result)
                {
                    fail(result.error());
                    break;
                }
                if (!*result)
                {
                    break;
                }
            }
            for (unsigned i = 0; i < 3; ++i)
            {
                auto result = channel->transport->pollProgram(
                    [&](RenderPacketView& packet) noexcept
                    {
                        for (std::size_t record = 0; record < packet.size(); ++record)
                        {
                            auto request = packet.read(channel->publication, record);
                            if (!request)
                            {
                                channel->diagnose(request.error());
                                continue;
                            }
                            auto bytes = packet.blob(request->scalars);
                            if (!bytes)
                            {
                                channel->diagnose(bytes.error());
                                continue;
                            }
                            auto applied = publish(*request, *bytes);
                            if (!applied)
                            {
                                channel->diagnose(applied.error());
                            }
                        }
                    }
                );
                if (!result)
                {
                    fail(result.error());
                    break;
                }
                if (!*result)
                {
                    break;
                }
            }
            if (channel->stopping.load(std::memory_order_acquire))
            {
                break;
            }
            const auto now = Clock::now();
            if (!jobs.empty() && now >= deadline)
            {
                const auto time = std::chrono::duration_cast<std::chrono::nanoseconds>(now - origin).count();
                auto rendered = execute(time);
                if (!rendered && rendered.error().type != vulkan::kBusy)
                {
                    fail(rendered.error());
                    break;
                }
                deadline = now + interval;
            }
            snapshot();
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            const DWORD timeout = jobs.empty() ? INFINITE : static_cast<DWORD>(std::max<std::int64_t>(1, remaining));
            if (WaitForSingleObject(channel->wake, timeout) == WAIT_FAILED)
            {
                fail({kHostFailure, {GetLastError()}});
            }
        }
        channel->state.store(ERuntimeState::STOPPING, std::memory_order_release);
        channel->transport->requestStop();
        while (!channel->transport->drained())
        {
            const auto epoch = channel->transport->wake().snapshot();
            auto a = channel->transport->pollProgram([](RenderPacketView&) noexcept {});
            auto b = channel->transport->pollControl([](RenderPacketView&) noexcept {});
            auto c = channel->transport->pollUpload(upload, [](RenderPacketView&) noexcept {});
            if (!a || !b || !c)
            {
                std::terminate();
            }
            if (!*a && !*b && !*c && !channel->transport->drained())
            {
                channel->transport->wake().wait(epoch);
            }
        }
        // Includes partial-submit tickets, not merely the last successful receipt.
        jobs.clear();
        effect_bindings.clear();
        session->resources.executions.clear();
        snapshot();
        channel->state.store(
            channel->failure ? ERuntimeState::FAILED : ERuntimeState::STOPPED,
            std::memory_order_release
        );
    }

    RenderBackend::RenderBackend(std::unique_ptr<Impl> implementation) noexcept : impl_(std::move(implementation)) {}

    RenderBackend::~RenderBackend() noexcept
    {
        requestStop();
        if (impl_->worker)
        {
            if (WaitForSingleObject(impl_->worker, INFINITE) != WAIT_OBJECT_0)
            {
                std::terminate();
            }
            CloseHandle(impl_->worker);
        }
    }

    RenderResult<std::unique_ptr<RenderBackend>> RenderBackend::create(
        const vulkan::BackendOptions& native,
        RuntimeOptions options
    ) noexcept
    {
        const auto& c = native.capacity;
        const bool invalid = !c.scenes || !c.views || !c.bindings || !c.targets || !c.effects || !c.graphs ||
                             !c.frames_in_flight || c.frames_in_flight > 3 || !c.queue_submissions ||
                             !options.frame_interval_ns || options.frame_interval_ns > 10'000'000'000ull;
        if (invalid)
        {
            return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
        }
        auto instance = vulkan::VulkanInstance::create(native.instance);
        if (!instance)
        {
            return cxx::unexpected(vulkan::resourceError(instance.error()));
        }
        auto host = std::make_unique<vulkan::InstanceContext>(vulkan::InstanceContext{std::move(*instance)});
        auto session = vulkan::DeviceSession::create(*host, native);
        if (!session)
        {
            return cxx::unexpected(vulkan::resourceError(session.error()));
        }
        RenderRouteTable routes{2};
        auto publication = routes.registerOperation<RuntimeBindingPublication>();
        auto control = routes.registerOperation<RuntimeControl>();
        if (!publication || !control)
        {
            std::terminate();
        }
        const auto owner = routes.ownerIdentity();
        auto transport = RenderTransport::create(std::move(routes), options.transport);
        if (!transport)
        {
            return cxx::unexpected(vulkan::resourceError(transport.error()));
        }
        HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!wake)
        {
            return cxx::unexpected(RenderError{kHostFailure, {GetLastError()}});
        }
        auto channel = std::shared_ptr<detail::RuntimeChannel>{
            new detail::RuntimeChannel{std::move(*transport), *publication, *control, owner, wake}
        };
        return std::unique_ptr<RenderBackend>{new RenderBackend(
            std::make_unique<Impl>(std::move(host), std::move(*session), std::move(channel), c, options)
        )};
    }

    RenderResult<std::reference_wrapper<const vulkan::VulkanDevice>> RenderBackend::assemblyDevice() const noexcept
    {
        if (impl_->assembly_thread != detail::transportThreadIdentity())
        {
            return cxx::unexpected(RenderError{kTransportWrongThread});
        }
        if (impl_->started)
        {
            return cxx::unexpected(RenderError{vulkan::kBusy});
        }
        return std::cref(impl_->session->device.device);
    }

    RenderResult<RuntimeEffect> RenderBackend::addEffect(vulkan::EffectSource source) noexcept
    {
        auto permitted = assemblyDevice();
        if (!permitted)
        {
            return cxx::unexpected(vulkan::resourceError(permitted.error()));
        }
        if (impl_->effects.size() >= impl_->capacity.effects)
        {
            return cxx::unexpected(RenderError{vulkan::kCapacity});
        }
        auto logical = compileLogicalGraph(source.definition);
        if (!logical)
        {
            return cxx::unexpected(vulkan::resourceError(logical.error()));
        }
        // P1 has no external GPU asset registry or multi-View Target composition.
        if (!source.width || !source.height || logical->imports().size() != source.buffers.size())
        {
            return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
        }
        for (auto id : logical->imports())
        {
            const auto count = std::count_if(
                source.buffers.begin(),
                source.buffers.end(),
                [&](const auto& r) { return r.resource == id; }
            );
            if (count != 1)
            {
                return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
            }
            const auto& request = *std::find_if(
                source.buffers.begin(),
                source.buffers.end(),
                [&](const auto& r) { return r.resource == id; }
            );
            const auto& resource = logical->identity().resources[id.value() - 1];
            const bool valid_scope = request.scope == EResourceScope::GLOBAL ||
                                     request.scope == EResourceScope::SCENE || request.scope == EResourceScope::VIEW ||
                                     request.scope == EResourceScope::ATTACHMENT;
            const bool valid_buffer = resource.kind() == EGraphResourceKind::BUFFER &&
                                      resource.buffer().byte_size % 4 == 0 && resource.buffer().byte_size != 0;
            if (!valid_scope || !valid_buffer || request.key.empty())
            {
                return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
            }
            // R3-A prepares immutable initialized buffers. Mutable providers and cross-family
            // ownership recipes require R3-B/R3-D's explicit ResourcePlan and transfer contract.
            for (std::size_t i = 0; i < logical->identity().passes.size(); ++i)
            {
                for (const auto& use : logical->identity().passes[i].uses)
                {
                    if (use.resource != id)
                    {
                        continue;
                    }
                    const auto command = std::find_if(
                        source.commands.begin(),
                        source.commands.end(),
                        [&](const auto& c) { return c.pass.value() == i + 1; }
                    );
                    const bool supported = use.access == EGraphAccess::READ && command != source.commands.end() &&
                                           command->queue == vulkan::EQueueRole::GRAPHICS;
                    if (!supported)
                    {
                        return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
                    }
                }
            }
        }
        std::sort(
            source.buffers.begin(),
            source.buffers.end(),
            [](const auto& a, const auto& b) { return a.resource.value() < b.resource.value(); }
        );
        // The P1 Offscreen endpoint publishes the complete graph's final image.
        // Hidden copy/tonemap/present work outside this definition is not permitted.
        if (logical->executionOrder().empty())
        {
            return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
        }
        const auto& final_pass = logical->identity().passes[logical->executionOrder().back().value() - 1];
        const bool invalid_output = final_pass.kind != EPassKind::HOST_READBACK || final_pass.uses.size() != 1;
        if (invalid_output)
        {
            return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
        }
        const auto& output = logical->identity().resources[final_pass.uses[0].resource.value() - 1];
        if (output.kind() != EGraphResourceKind::IMAGE)
        {
            return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
        }
        const auto& texture = output.texture();
        const bool wrong_extent = texture.extent_kind != EExtentKind::TARGET_RELATIVE &&
                                  (texture.width != source.width || texture.height != source.height);
        const bool unsupported_output = wrong_extent || texture.depth != 1 || texture.array_layers != 1 ||
                                        texture.mip_count != 1 || texture.samples != 1;
        if (unsupported_output)
        {
            return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
        }
        std::vector<vulkan::ProgramBindings> binding_inputs;
        binding_inputs.reserve(source.programs.size());
        for (const auto& program : source.programs)
        {
            if (toolchain::shaderSchemaIdentity(program.schema) != program.shader.identity().schema_identity)
            {
                return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
            }
            binding_inputs.push_back(program.bindings);
        }
        RenderGraphId graph_id;
        for (const auto& graph : impl_->session->graphs)
        {
            const bool same_structure = graph.width == source.width && graph.height == source.height &&
                                        graph.code.logical().cacheIdentity() == logical->cacheIdentity() &&
                                        graph.commands == source.commands && graph.program_bindings == binding_inputs;
            if (!same_structure)
            {
                continue;
            }
            const auto programs = graph.code.shaderPrograms();
            const bool same_programs = programs.size() == source.programs.size() &&
                                       std::equal(
                                           programs.begin(),
                                           programs.end(),
                                           source.programs.begin(),
                                           [](const auto& a, const auto& b)
                                           {
                                               return a.identity().pass == b.pass &&
                                                      a.identity().shader == b.shader.identity() &&
                                                      a.identity().pipeline == b.pipeline;
                                           }
                                       );
            if (same_programs)
            {
                graph_id = graph.id;
                ++impl_->progress.native_cache_hits;
                break;
            }
        }
        if (graph_id.isNull())
        {
            if (impl_->session->graphs.size() >= impl_->capacity.graphs)
            {
                return cxx::unexpected(RenderError{vulkan::kCapacity});
            }
            std::vector<vulkan::NativeShaderProgram> programs;
            programs.reserve(source.programs.size());
            for (const auto& input : source.programs)
            {
                std::vector<vulkan::OwnerAssignment> assignments;
                assignments.reserve(input.bindings.assignments.size());
                for (const auto& assignment : input.bindings.assignments)
                {
                    assignments.push_back({assignment.field_path, assignment.owner});
                }
                auto program = vulkan::NativeShaderProgram::create(
                    impl_->session->device.device,
                    *logical,
                    input.pass,
                    input.shader,
                    input.schema,
                    assignments,
                    input.bindings.owners,
                    input.pipeline
                );
                if (!program)
                {
                    return cxx::unexpected(vulkan::resourceError(program.error()));
                }
                programs.push_back(std::move(*program));
            }
            auto code = vulkan::compileVulkanGraph(
                {*logical,
                 impl_->session->device.device,
                 impl_->session->device.queues,
                 source.commands,
                 true,
                 {source.width, source.height}},
                std::move(programs)
            );
            if (!code)
            {
                return cxx::unexpected(vulkan::resourceError(code.error()));
            }
            graph_id = impl_->session->graphs.emplace(vulkan::CompiledGraphEntry{
                {},
                std::move(*code),
                std::move(source.commands),
                std::move(binding_inputs),
                source.width,
                source.height
            });
            impl_->session->graphs.find(graph_id)->id = graph_id;
            ++impl_->progress.native_compilations;
        }
        auto values = makeGraphInvocationData(source.definition);
        const auto effect_id = impl_->effects.emplace(
            RenderEffect{std::move(source.definition), std::move(values), graph_id, std::move(source.buffers)}
        );
        return RuntimeEffect{impl_->channel->owner, effect_id};
    }

    RenderResult<std::unique_ptr<RenderRuntime>> RenderBackend::start() noexcept
    {
        auto permitted = assemblyDevice();
        if (!permitted)
        {
            return cxx::unexpected(vulkan::resourceError(permitted.error()));
        }
        if (impl_->channel->stopping.load(std::memory_order_acquire))
        {
            return cxx::unexpected(RenderError{kTransportStopping});
        }
        impl_->worker = CreateThread(
            nullptr,
            0,
            [](void* value) -> DWORD
            {
                static_cast<Impl*>(value)->run();
                return 0;
            },
            impl_.get(),
            0,
            nullptr
        );
        if (!impl_->worker)
        {
            return cxx::unexpected(RenderError{kHostFailure, {GetLastError()}});
        }
        impl_->started = true;
        return std::unique_ptr<RenderRuntime>{new RenderRuntime(impl_->channel)};
    }

    RenderResult<void> RenderBackend::removeEffect(RuntimeEffect effect) noexcept
    {
        auto permitted = assemblyDevice();
        if (!permitted)
        {
            return cxx::unexpected(vulkan::resourceError(permitted.error()));
        }
        RuntimeControl request;
        request.owner = effect.owner;
        request.index = effect.local.index;
        request.generation = effect.local.gen;
        request.operation = ERuntimeControl::REMOVE_EFFECT;
        auto result = impl_->control(request);
        if (!result)
        {
            return cxx::unexpected(vulkan::resourceError(result.error()));
        }
        return {};
    }

    void RenderBackend::requestStop() noexcept
    {
        impl_->channel->stop();
    }

    RenderResult<bool> RenderBackend::waitStopped(std::uint32_t timeout_ms) noexcept
    {
        if (!impl_->worker)
        {
            return true;
        }
        const auto result = WaitForSingleObject(impl_->worker, timeout_ms);
        if (result == WAIT_FAILED)
        {
            return cxx::unexpected(RenderError{kHostFailure, {GetLastError()}});
        }
        return result == WAIT_OBJECT_0;
    }
} // namespace lux::render
