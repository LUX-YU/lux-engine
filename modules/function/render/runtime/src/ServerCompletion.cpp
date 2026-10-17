#include "ServerImpl.hpp"

namespace lux::render
{
    void RenderServer::Impl::settle(RenderRequest request, RenderResult<void> result) noexcept
    {
        if (!request.active()) return;
        auto replied = request.fail(result ? RenderError{kRuntimeState} : result.error());
        if (!replied)
        {
            undelivered.push_back(std::move(request));
            if (undelivered.size() >= undelivered.capacity()) stop_requested.store(true);
        }
    }

    void RenderServer::Impl::cancelReadbacks(vulkan::ExecutionContext* context) noexcept
    {
        for (auto it = deferred.begin(); it != deferred.end();)
        {
            auto& slot = *it;
            auto* current = std::get_if<ReadbackCompletion>(&slot.condition);
            if (!current || current->context != context)
            {
                ++it;
                continue;
            }
            current->context = nullptr;
            context->releaseReadback();
            // The context may now retire. Its original Ticket node stays alive and its eventual
            // callback settles the request without touching the retired context.
            ++it;
        }
    }

    RenderResult<void> RenderServer::Impl::readback(RenderRequest& request) noexcept
    {
        const auto* input = &request.get<AttachmentRequest>();
        auto* effect = effects.find(input->effect);
        if (!effect) return cxx::unexpected(RenderError{kRuntimeIdentity});
        auto found = std::find_if(
            effect->groups.begin(),
            effect->groups.end(),
            [&](const auto& group)
            { return group.scene == input->scene && group.view == input->view && group.target == input->target; }
        );
        if (found == effect->groups.end()) return cxx::unexpected(RenderError{kRuntimeIdentity});
        const bool full = request.replyCapacity() < sizeof(ReadbackResult) ||
                          found->context->readbackBytes() > request.replyCapacity() - sizeof(ReadbackResult);
        if (full) return cxx::unexpected(RenderError{kRuntimeCapacity});
        auto* slot = reserveCompletion();
        if (!slot) return cxx::unexpected(RenderError{kRuntimeCapacity});
        auto& pending = slot->condition.emplace<ReadbackCompletion>();
        if (auto reserved = device->queue().reserve(pending.callback); !reserved)
        {
            deferred.erase(slot->id);
            return reserved;
        }
        found->context->retainReadback();
        slot->request.emplace(std::move(request));
        pending.context = found->context;
        armReadback(*slot);
        return {};
    }

    void RenderServer::Impl::armReadback(DeferredCompletion& slot) noexcept
    {
        auto& pending = std::get<ReadbackCompletion>(slot.condition);
        if (!pending.context->lastUse())
        {
            // A newly attached View is a valid Readback consumer before its first frame Submit.
            auto executed = pending.context->execute();
            if (!executed)
            {
                finishReadback(&slot, cxx::unexpected(executed.error()));
                return;
            }
            if (*executed) ++submissions;
        }
        auto ticket = pending.context->lastUse();
        if (!ticket) ticket = device->queue().oldestPending();
        if (!ticket)
        {
            // begin can only be capacity-blocked here; overlapping recording violates its owner contract.
            finishReadback(&slot, cxx::unexpected(RenderError{kRuntimeState}));
            return;
        }
        if (!device->queue().onComplete(*ticket, pending.callback, finishReadback, &slot)) std::terminate();
    }

    void RenderServer::Impl::finishReadback(void* value, RenderResult<void> completed) noexcept
    {
        auto& slot = *static_cast<DeferredCompletion*>(value);
        auto& server = *static_cast<Impl*>(slot.server);
        auto& pending = std::get<ReadbackCompletion>(slot.condition);
        RenderResult<void> reply;
        if (!pending.context || server.stop_requested.load())
        {
            if (pending.context) std::exchange(pending.context, nullptr)->releaseReadback();
            reply = slot.request->cancel();
        }
        else if (!completed)
        {
            std::exchange(pending.context, nullptr)->releaseReadback();
            reply = slot.request->fail(completed.error());
        }
        else
        {
            const auto ticket = pending.context->lastUse();
            if (!ticket || ticket->serial() > ticket->owner().completed())
            {
                // A capacity notification is not proof that this View's output is complete.
                server.armReadback(slot);
                return;
            }
            auto result = pending.context->readback();
            std::exchange(pending.context, nullptr)->releaseReadback();
            if (result)
            {
                auto owner = std::make_shared<const std::vector<std::byte>>(std::move(*result));
                auto payload =
                    RenderPayload::fromVector(owner, 0, owner->size(), server.config.implementation_code_pin);
                if (!payload) std::terminate(); // exact vector range
                reply = slot.request->reply(ReadbackResult{std::move(*payload)});
            }
            else
                reply = slot.request->fail(result.error());
        }
        server.settle(std::move(*slot.request), reply);
        server.deferred.erase(slot.id);
    }

    void RenderServer::Impl::finishGpu(void* value, RenderResult<void> completed) noexcept
    {
        auto& slot = *static_cast<DeferredCompletion*>(value);
        auto& server = *static_cast<Impl*>(slot.server);
        auto& gpu = std::get<GpuCompletion>(slot.condition);
        // Device loss can reach a Transfer terminal before collect reports it to run().
        // Accepted work cannot promise cancellation even when its reply is still unpublished.
        const bool stopping = server.stop_requested.load() || server.device->deviceLost();
        auto ready = completed;
        if (ready && !stopping) ready = gpu.work.publish();
        gpu.work.reset(); // Native/candidate destruction precedes release of the original source request.
        RenderResult<void> reply;
        if (stopping)
        {
            gpu.reply.reset();
            reply = slot.request->unknown();
        }
        else if (ready)
        {
#ifdef LUX_RUNTIME_TEST_SEAM
            runtime_detail::test::gpuReplyPublicationStarted();
#endif
            reply = slot.request->publishPrepared(gpu.reply, gpu.reply_charge);
        }
        else
        {
            gpu.reply.reset();
            reply = slot.request->fail(ready.error());
        }
        server.settle(std::move(*slot.request), reply);
        server.deferred.erase(slot.id);
    }

    DeferredCompletion* RenderServer::Impl::reserveCompletion(bool for_compile) noexcept
    {
        const bool full = for_compile ? compiling >= config.compile_capacity
                                      : deferred.size() - compiling >= config.completion_capacity;
        if (full) return nullptr;
        auto id = deferred.tryEmplacePrepared();
        if (!id) return nullptr;
        auto& slot = *deferred.find(*id);
        slot.id = *id;
        slot.session = identity;
        slot.events = &completion_events;
        slot.publishers = &completion_publishers;
        slot.server = this;
        return &slot;
    }

    void RenderServer::Impl::completeCpu(
        void* value, ECommandOutcome outcome, RenderError error, void* response
    ) noexcept
    {
        auto& slot = *static_cast<DeferredCompletion*>(value);
        auto& cpu = std::get<CpuCompletion>(slot.condition);
        cpu.outcome = outcome;
        cpu.error = error;
        if (outcome == ECommandOutcome::SUCCESS)
        {
            auto charge = slot.request->prepareResponse(response, cpu.reply);
            if (charge)
                cpu.reply_charge = *charge;
            else
            {
                cpu.outcome = ECommandOutcome::FAILED;
                cpu.error = charge.error();
            }
        }
        slot.post()(nullptr, nullptr, {});
    }

    RenderResult<CommandInvocation*> RenderServer::Impl::deferCommand(
        RenderRequest& request, std::size_t reply_bytes
    ) noexcept
    {
        if (!request.active()) return cxx::unexpected(RenderError{kRuntimeState});
        if (request.gpuReady()) return cxx::unexpected(RenderError{kProtocolContract});
        if (reply_bytes > request.replyCapacity()) return cxx::unexpected(RenderError{kRuntimeCapacity});
        auto* slot = reserveCompletion();
        if (!slot) return cxx::unexpected(RenderError{kRuntimeCapacity});
        slot->request.emplace(std::move(request));
        slot->condition.emplace<CpuCompletion>();
        slot->invocation = slot->request->invocation(slot, deferred_completion_functions);
#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
        test::completionRegistered(slot->id, slot->request->id());
#endif
        return &slot->invocation;
    }

    const CommandCompletionFns RenderServer::Impl::deferred_completion_functions{
        [](void* value, ECommandOutcome outcome, RenderError error, void* response) noexcept -> RenderResult<void>
        {
            auto& slot = *static_cast<DeferredCompletion*>(value);
            auto& completion = std::get<CpuCompletion>(slot.condition);
            if (outcome == ECommandOutcome::SUCCESS)
            {
                auto charge = slot.request->prepareResponse(response, completion.reply);
                if (!charge) return cxx::unexpected(charge.error());
                completion.reply_charge = *charge;
            }
            completion.outcome = outcome;
            completion.error = error;
            slot.post()(nullptr, nullptr, {});
            return {};
        },
        [](const void* value) noexcept
        {
            const auto& slot = *static_cast<const DeferredCompletion*>(value);
            return std::get<CpuCompletion>(slot.condition).outcome == ECommandOutcome::OUTCOME_UNKNOWN;
        },
        [](void* value, void* sender, const CommandAsyncOps& operations) noexcept -> RenderResult<void>
        {
            auto& slot = *static_cast<DeferredCompletion*>(value);
            auto& completion = std::get<CpuCompletion>(slot.condition);
            completion.destroy = operations.destroy;
            auto& server = *static_cast<Impl*>(slot.server);
            completion.operation = operations.connect(sender, {&slot, completeCpu, slot.stop.get_token(),
                slot.post(), &server.config.implementation_code_pin, &slot.request->codePin()});
            // Owner-thread start cannot reenter event draining. The Receiver only publishes.
            operations.start(completion.operation);
            return {};
        },
        nullptr,
        // A dropped capability proves neither rollback nor absence of accepted side effects.
        [](void* value) noexcept { completeCpu(value, ECommandOutcome::OUTCOME_UNKNOWN, {}, nullptr); }
    };

    void RenderServer::Impl::drainCompletions(std::size_t budget) noexcept
    {
#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
        auto* cost_drain = test::completionDrainBegin(completion_events);
        std::size_t cost_consumed = 0;
#endif
        CompletionEvent event;
        for (std::size_t i = 0; i < budget && completion_events.tryPop(event) == cxx::EQueuePopResult::VALUE; ++i)
        {
#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
            ++cost_consumed;
            auto* cost_record = test::completionPopped(event.id);
#endif
            // A copied event identity is checked once at its actual cross-thread boundary.
            if (event.session != identity) continue;
            auto* slot = deferred.find(event.id);
            if (!slot) continue;
            const auto resume = std::exchange(slot->resume, nullptr);
            if (resume)
            {
                resume(slot->continuation, std::move(slot->cpu_result));
                // A Native continuation may finish and erase this exact slot synchronously.
                continue;
            }
            auto* cpu = std::get_if<CpuCompletion>(&slot->condition);
            if (!cpu) continue; // Native continuation resumes the same GPU request, not a second owner.
#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
            test::completionAdoptBegin(cost_record);
#endif
            RenderResult<void> result;
            if (cpu->adopt)
                result = cpu->adopt(cpu->operation, this, *slot->request);
            else if (cpu->outcome == ECommandOutcome::OUTCOME_UNKNOWN)
                result = slot->request->unknown(cpu->error);
            else if (cpu->outcome == ECommandOutcome::SUCCESS)
                result = slot->request->publishPrepared(cpu->reply, cpu->reply_charge);
            else if (cpu->outcome == ECommandOutcome::CANCELLED)
            {
                cpu->reply.reset();
                result = slot->request->cancel();
            }
            else
            {
                cpu->reply.reset();
                result = slot->request->fail(cpu->error);
            }
            if (cpu->compiling) --compiling;
            settle(std::move(*slot->request), result);
            deferred.erase(event.id);
#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
            test::completionAdoptEnd(cost_record);
#endif
        }
#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
        test::completionDrainEnd(cost_drain, cost_consumed);
#endif
    }

    const CommandCompletionFns RenderServer::Impl::command_completion_functions{
        [](void* owner, ECommandOutcome outcome, RenderError error, void* response) noexcept
        {
            auto& request = static_cast<GpuAdmission*>(owner)->request;
            switch (outcome)
            {
            case ECommandOutcome::SUCCESS:
                return request.complete(response);
            case ECommandOutcome::FAILED:
                return request.fail(error);
            case ECommandOutcome::CANCELLED:
                return request.cancel();
            default:
                return request.unknown();
            }
        },
        [](const void* owner) noexcept { return static_cast<const GpuAdmission*>(owner)->request.active(); },
        nullptr,
        [](void* owner, std::size_t reply_bytes) noexcept -> RenderResult<CommandInvocation*>
        {
            auto& admission = *static_cast<GpuAdmission*>(owner);
            return admission.server.deferCommand(admission.request, reply_bytes);
        }
    };

    const vulkan::GpuCompletionFns RenderServer::Impl::gpu_completion_functions{
        [](void* owner, vulkan::SubmissionQueue& queue, void* response) noexcept -> RenderResult<void*>
        {
            auto& admission = *static_cast<GpuAdmission*>(owner);
            auto& server = admission.server;
            auto& native = *server.device;
            const bool is_session_queue = &queue == &native.queue(vulkan::EQueueRole::GRAPHICS) ||
                                          &queue == &native.queue(vulkan::EQueueRole::COMPUTE) ||
                                          &queue == &native.queue(vulkan::EQueueRole::TRANSFER);
            const bool invalid = !is_session_queue || admission.reserved || !admission.request.gpuReady();
            if (invalid) return cxx::unexpected(RenderError{kProtocolContract});
            auto* slot = server.reserveCompletion();
            if (!slot) return cxx::unexpected(RenderError{kRuntimeCapacity});
            auto& gpu = slot->condition.emplace<GpuCompletion>();
            auto reserved = queue.reserve(gpu.callback);
            if (!reserved)
            {
                server.deferred.erase(slot->id);
                return cxx::unexpected(reserved.error());
            }
            auto charge = admission.request.prepareResponse(response, gpu.reply);
            if (!charge)
            {
                server.deferred.erase(slot->id);
                return cxx::unexpected(charge.error());
            }
            gpu.queue = &queue;
            gpu.submitted_before_reserve = queue.submitted();
            gpu.reply_charge = *charge;
            gpu.reserved_request = &admission.request;
            admission.reserved = true;
            return slot;
        },
        [](void* value, void* response) noexcept -> RenderResult<void>
        {
            auto& gpu = std::get<GpuCompletion>(static_cast<DeferredCompletion*>(value)->condition);
            platform::ipc::IpcMessage output;
            auto charge = gpu.reserved_request->prepareResponse(response, output);
            if (!charge) return cxx::unexpected(charge.error());
            gpu.reply = std::move(output);
            gpu.reply_charge = *charge;
            return {};
        },
        [](void* value, vulkan::GpuWork& work) noexcept -> RenderResult<void>
        {
            auto& slot = *static_cast<DeferredCompletion*>(value);
            auto& server = *static_cast<Impl*>(slot.server);
            auto& gpu = std::get<GpuCompletion>(slot.condition);
            if (!work.unstartedIn(*server.device)) return cxx::unexpected(RenderError{kProtocolContract});
            gpu.callback.reset(); // Transfer reserves its actual copy/acquire queues before starting.
            if (auto prepared = work.prepareCompletion(); !prepared)
            {
                // prepareCompletion rolls back all partial reservations; no other owner can consume
                // this just-released quota during the serialized call. Restore the caller's right.
                if (!gpu.queue->reserve(gpu.callback)) std::terminate();
                return prepared;
            }
            gpu.work = std::move(work);
            slot.request.emplace(std::move(*gpu.reserved_request));
            gpu.reserved_request = nullptr;
            gpu.work.start(
                slot.post(), &slot, finishGpu, server.config.implementation_code_pin, slot.request->codePin()
            );
            // start may reach a synchronous terminal. No access to slot after this point.
            return {};
        },
        [](void* value,
           const vulkan::SubmissionTicket& ticket,
           void* owner,
           void (*destroy)(void*) noexcept,
           RenderResult<void> (*ready)(void*, vulkan::DeviceSession&) noexcept) noexcept -> RenderResult<void>
        {
            auto& slot = *static_cast<DeferredCompletion*>(value);
            auto& server = *static_cast<Impl*>(slot.server);
            auto& gpu = std::get<GpuCompletion>(slot.condition);
            const bool invalid = !gpu.queue->owns(ticket) || ticket.serial() <= gpu.submitted_before_reserve;
            if (invalid) return cxx::unexpected(RenderError{kProtocolContract});
            gpu.work = vulkan::GpuWork::accepted(*server.device, ticket, owner, destroy, ready);
            slot.request.emplace(std::move(*gpu.reserved_request));
            gpu.reserved_request = nullptr;
            if (!gpu.queue->onComplete(ticket, gpu.callback, finishGpu, &slot)) std::terminate();
            return {};
        },
        [](void* value) noexcept
        {
            auto& slot = *static_cast<DeferredCompletion*>(value);
            auto& server = *static_cast<Impl*>(slot.server);
            if (!server.deferred.erase(slot.id)) std::terminate();
        }
    };
} // namespace lux::render
