#include "ServerImpl.hpp"
#include <lux/engine/render/native/vulkan/Readback.hpp>

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

    RenderResult<void> RenderServer::Impl::readback(RenderRequest& request) noexcept
    {
        const auto& input = request.get<AttachmentRequest>();
        auto* effect = effects.find(input.effect);
        if (!effect) return cxx::unexpected(RenderError{kRuntimeIdentity});
        auto found = std::find_if(
            effect->groups.begin(), effect->groups.end(), [&](const auto& group) {
                return group.scene == input.scene && group.view == input.view && group.target == input.target;
            }
        );
        if (found == effect->groups.end()) return cxx::unexpected(RenderError{kRuntimeIdentity});
        const bool full = request.replyCapacity() < sizeof(ReadbackResult) ||
            found->context->readbackBytes() > request.replyCapacity() - sizeof(ReadbackResult);
        if (full) return cxx::unexpected(RenderError{kRuntimeCapacity});
        auto invocation = deferCommand(request, sizeof(ReadbackResult) + found->context->readbackBytes());
        if (!invocation) return cxx::unexpected(invocation.error());
        auto pending = protocol_detail::ReplyAccess::make<Readback>(**invocation);
        return pending.completeWhen(stdexec::then(
            vulkan::ReadbackSender{*found->context, &submissions, [](void* value) noexcept {
                ++*static_cast<std::atomic<std::uint64_t>*>(value);
            }},
            [pin = config.implementation_code_pin](std::vector<std::byte> bytes) noexcept {
                auto owner = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
                auto payload = RenderPayload::fromVector(owner, 0, owner->size(), pin);
                if (!payload) std::terminate(); // exact vector range
                return ReadbackResult{std::move(*payload)};
            }
        ));
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
        if (for_compile)
        {
            slot.quota_use = &compiling;
            ++compiling;
        }
        return &slot;
    }

    void RenderServer::Impl::completeOperation(
        void* value, ECommandOutcome outcome, RenderError error, void* response
    ) noexcept
    {
        auto& slot = *static_cast<DeferredCompletion*>(value);
        auto& cpu = slot.operation;
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
        if (reply_bytes > request.replyCapacity()) return cxx::unexpected(RenderError{kRuntimeCapacity});
        auto* slot = reserveCompletion();
        if (!slot) return cxx::unexpected(RenderError{kRuntimeCapacity});
        slot->request.emplace(std::move(request));

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
            auto& completion = slot.operation;
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
            return slot.operation.outcome == ECommandOutcome::OUTCOME_UNKNOWN;
        },
        [](void* value, void* sender, const CommandAsyncOps& operations) noexcept -> RenderResult<void>
        {
            auto& slot = *static_cast<DeferredCompletion*>(value);
            auto& completion = slot.operation;
            completion.destroy = operations.destroy;
            auto& server = *static_cast<Impl*>(slot.server);
            completion.operation = operations.connect(sender, {&slot, completeOperation, slot.stop.get_token(),
                slot.post(), &server.config.implementation_code_pin, &slot.request->codePin()});
            // Owner-thread start cannot reenter event draining. The Receiver only publishes.
            operations.start(completion.operation);
            return {};
        },
        nullptr,
        // A dropped capability proves neither rollback nor absence of accepted side effects.
        [](void* value) noexcept { completeOperation(value, ECommandOutcome::OUTCOME_UNKNOWN, {}, nullptr); }
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
                resume(slot->continuation, std::move(slot->continuation_result));
                // A Native continuation may finish and erase this exact slot synchronously.
                continue;
            }
            auto* cpu = &slot->operation;
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
            auto& request = static_cast<CommandCall*>(owner)->request;
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
        [](const void* owner) noexcept { return static_cast<const CommandCall*>(owner)->request.active(); },
        nullptr,
        [](void* owner, std::size_t reply_bytes) noexcept -> RenderResult<CommandInvocation*>
        {
            auto& admission = *static_cast<CommandCall*>(owner);
            return admission.server.deferCommand(admission.request, reply_bytes);
        }
    };

} // namespace lux::render
