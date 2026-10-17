#include "ServerImpl.hpp"

namespace lux::render
{
    RenderResult<void> RenderServer::Impl::advanceRendering() noexcept
    {
        for (std::size_t e = 0; e < effects.size(); ++e)
        {
            auto& effect = *(effects.begin() + (next_effect + e) % effects.size());
            for (std::size_t g = 0; g < effect.groups.size(); ++g)
            {
                auto& group = effect.groups[(effect.next_group + g) % effect.groups.size()];
                auto result = group.context->execute();
                if (!result)
                {
                    return cxx::unexpected(result.error());
                }
                if (*result == EExecutionOutcome::EXECUTED)
                {
                    ++submissions;
                }
            }
            effect.next_group = effect.groups.empty() ? 0 : (effect.next_group + 1) % effect.groups.size();
        }
        next_effect = effects.empty() ? 0 : (next_effect + 1) % effects.size();
        auto collected = device->collect();
#ifdef LUX_RUNTIME_TEST_SEAM
        if (runtime_detail::test::observeGpuCompletion())
        {
            device->dispatchCompletions();
        }
#else
        device->dispatchCompletions();
#endif
        return collected;
    }

    void RenderServer::Impl::queryExecutionProgress(
        const QueryExecutionProgress::Request& _request,
        QueryExecutionProgress::Response& output_response
    ) noexcept
    {
        const auto& input = _request;
        const auto* effect = effects.find(input.effect);
        if (!effect)
        {
            output_response = cxx::unexpected(RenderError{kRuntimeIdentity});
            return;
        }
        const auto found = std::ranges::find_if(
            effect->groups,
            [&](const auto& group)
            { return group.scene == input.scene && group.view == input.view && group.target == input.target; }
        );
        if (found == effect->groups.end())
        {
            output_response = cxx::unexpected(RenderError{kRuntimeIdentity});
            return;
        }
        output_response = found->context->progress();
    }

    void RenderServer::Impl::queryRenderStats(
        const QueryRenderStats::Request& _request,
        QueryRenderStats::Response& output_response
    ) noexcept
    {
        output_response =
            RenderStatsSnapshot{ticks.load(), commands.load(), compiled_graphs.load(), submissions.load()};
    }

    void RenderServer::Impl::stopCommand(const Stop::Request& _request, Stop::Response& output_response) noexcept
    {
        stop_requested.store(true);
        output_response = Ack{};
    }

    void RenderServer::Impl::dispatchRequest(ipc::ReceivedRequest received) noexcept
    {
        const ResponseRoute route{received.requesterId(), RequestId{received.correlation()}};
        const auto* entry = command_registry_.findCommand(commandIdFromTag(received.tag()));
        if (!entry || route.request_id.isNull())
        {
            response_outbox_.emplaceResponse<RenderError>(route, RenderError{kProtocolContract});
        }
        else if (received.message().typeHash() != entry->cold->binding.descriptor()->request.type.hash())
        {
            response_outbox_.emplaceResponse<RenderError>(route, RenderError{kProtocolContract});
        }
        else if (!entry->active.invoke)
        {
            entry->cold->binding.makeError(response_outbox_.prepare(route), RenderError{kRuntimeUnsupported});
        }
        else
        {
            CommandDispatchContext context{
                received.message(),
                route,
                response_outbox_,
                pending_operations_,
                completion_events,
                completion_publishers
            };
            const auto active = entry->active;
            active.invoke(active.object, context);
        }
        ++commands;
    }

    void RenderServer::Impl::shutdown() noexcept
    {
        stop_requested.store(true);
        // Snapshot identities because stop callbacks may complete synchronously.
        std::vector<CompletionId> pending;
        pending.reserve(pending_operations_.size());
        for (const auto& slot : pending_operations_)
        {
            pending.push_back(slot.id);
        }
        for (const auto id : pending)
        {
            if (auto* slot = pending_operations_.find(id))
            {
                slot->stop.request_stop();
            }
        }
        response_outbox_.flushTo(responder, config.request_work_budget);
        // Only shutdown waits. CPU/source owners remain in the same slots; cancellation prevents
        // their eventual CPU event from submitting another GPU chunk.
        if (device)
        {
            auto drained = device->drain();
            if (!drained && !device->deviceLost())
            {
                std::terminate();
            }
        }
        while (!pending_operations_.empty())
        {
            drainCompletions();
            response_outbox_.flushTo(responder, config.request_work_budget);
            if (device)
            {
                auto collected = device->collect();
                device->dispatchCompletions();
                if (!collected && !device->deviceLost())
                {
                    std::terminate();
                }
            }
            if (!pending_operations_.empty())
            {
                std::this_thread::yield();
            }
        }
        while (completion_publishers.load(std::memory_order_acquire) != 0)
        {
            std::this_thread::yield();
        }
        response_outbox_.flushTo(responder, SIZE_MAX);
        response_outbox_.clear(); // Connection closes after safe operations; caller owns any unknown outcomes.
        responder.reset();
        for (auto& effect : effects)
        {
            while (!effect.groups.empty())
            {
                if (!detachExecutionGroup(effect, effect.groups.size() - 1))
                {
                    std::terminate();
                }
            }
        }
        effects.clear();
        scenes.clear();
        command_registry_.clear(); // All operations ended; professional Handler borrows still have a live Device.
        device.reset();
        instance.reset();
        running.store(false);
    }

    RenderResult<void> RenderServer::Impl::run() noexcept
    {
        auto& s = *this;
        if (s.started.exchange(true))
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        if (s.stop_requested.load())
        {
            s.shutdown();
            return {};
        }
        s.running.store(true);
        RenderResult<void> result;
        auto& notifications = ipc::IpcCore::instance();
        while (!s.stop_requested.load())
        {
            // Snapshot precedes every readiness check. A publication in the check/wait gap
            // changes the epoch, so WaitOnAddress/futex cannot sleep through that publication.
            const auto observed = notifications.workEpoch();
            const auto submitted_before = s.submissions.load();
            bool progressed = s.drainCompletions() != 0;
#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
            test::beforeDispatch();
#endif
            for (std::uint32_t i = 0; i < s.config.request_work_budget && !s.stop_requested.load(); ++i)
            {
                ipc::ReceivedRequest request;
                if (s.responder.tryReceive(request) == ipc::EIpcReceive::RECEIVED)
                {
                    s.dispatchRequest(std::move(request));
                    progressed = true;
                }
            }
            if (s.stop_requested.load())
            {
                break;
            }
            progressed |= s.response_outbox_.flushTo(s.responder, s.config.request_work_budget) != 0;
            if (auto rendered = s.advanceRendering(); !rendered)
            {
                result = cxx::unexpected(rendered.error());
                break;
            }
            progressed |= s.drainCompletions() != 0;
            progressed |= s.response_outbox_.flushTo(s.responder, s.config.request_work_budget) != 0;
            progressed |= s.submissions.load() != submitted_before;
            ++s.ticks;
            if (!progressed && !s.stop_requested.load())
            {
                // GPU fences have no CPU notification, and IPC receive inspects a bounded
                // portion of the Link roster. Request a bounded revisit deadline (subject to OS scheduling).
                // Active work never waits; request/CPU/stop notifications interrupt idle waits.
                notifications.waitUntil(observed, std::chrono::steady_clock::now() + std::chrono::milliseconds(1));
            }
        }
        s.shutdown();
        return result;
    }

    void RenderServer::stop() noexcept
    {
        impl_->stop();
    }

    RenderServerStats RenderServer::Impl::stats() const noexcept
    {
        const auto& s = *this;
        return {
            s.ticks.load(),
            s.commands.load(),
            s.compiled_graphs.load(),
            s.submissions.load(),
            s.validation_errors.load(),
            s.validation_warnings.load(),
            s.scoped_buffers_created.load()
        };
    }

    RenderResult<void> RenderServer::run() noexcept
    {
        return impl_->run();
    }

    RenderServerStats RenderServer::stats() const noexcept
    {
        return impl_->stats();
    }
} // namespace lux::render
