#include <lux/engine/render/runtime/ServerImpl.hpp>

namespace lux::render
{
    RenderResult<void> RenderServer::Impl::advanceRendering() noexcept
    {
        if (auto observed = device->collect(); !observed)
        {
            return observed;
        }
        device->dispatchCompletions();
        // GPU reuse notifications resume admitted writers on the Owner. Consume this ready
        // prefix before closing frame input, otherwise continuous drawing can starve retries.
        drainCompletions();
        const auto& active = active_targets.keys();
        frame_contexts.clear();
        for (std::size_t i = 0; i < active.size(); ++i)
        {
            const auto& target = *targets.find(active[(next_target + i) % active.size()]);
            frame_contexts.push_back(attachments.find(target.attachment)->context.get());
        }
        auto accepted = device->render(frame_contexts);
        if (!accepted)
        {
            return cxx::unexpected(accepted.error());
        }
        submissions += *accepted;
        next_target = active.empty() ? 0 : (next_target + 1) % active.size();
        return {};
    }

    void RenderServer::Impl::queryExecutionProgress(
        const QueryExecutionProgress::Request& _request,
        QueryExecutionProgress::Response& output_response
    ) noexcept
    {
        const auto* attachment = attachments.find(_request);
        if (!attachment)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        output_response = {{}, attachment->context->progress()};
    }

    void RenderServer::Impl::queryRenderStats(
        const QueryRenderStats::Request& _request,
        QueryRenderStats::Response& output_response
    ) noexcept
    {
        output_response =
            RenderStatsSnapshot{{}, ticks.load(), commands.load(), compiled_graphs.load(), submissions.load()};
    }

    void RenderServer::Impl::stopCommand(const Stop::Request& _request, Stop::Response& output_response) noexcept
    {
        stop_requested.store(true);
        output_response = {};
    }

    void RenderServer::Impl::dispatchRequest(ipc::ReceivedRequest received) noexcept
    {
        const ResponseRoute route{received.requesterId(), RequestId{received.correlation()}};
        const auto* entry = command_registry_.findCommand(commandIdFromTag(received.tag()));
        if (!entry || route.request_id.isNull())
        {
            response_outbox_.emplaceResponse<AckResponse>(route, AckResponse{RenderError{kCommandContract}});
        }
        else if (received.message().typeHash() != entry->description->request.token.hash())
        {
            entry->make_error(response_outbox_.prepare(route), RenderError{kCommandContract});
        }
        else if (!entry->selectedBinding())
        {
            entry->make_error(response_outbox_.prepare(route), RenderError{kRuntimeUnsupported});
        }
        else
        {
            CommandDispatchContext context{received.message(), route, response_outbox_, command_scope_, render_loop_};
            const auto active = entry->selectedBinding()->call;
            active.invoke(active.object, context);
        }
        ++commands;
    }

    void RenderServer::Impl::shutdown() noexcept
    {
        stop_requested.store(true);
        command_scope_.close();
        command_scope_.request_stop();
        std::atomic<bool> joined{false};

        struct Joined final
        {
            using receiver_concept = stdexec::receiver_t;
            std::atomic<bool>* done;

            auto get_env() const noexcept
            {
                return stdexec::env{stdexec::prop{stdexec::get_scheduler, stdexec::inline_scheduler{}}};
            }

            void set_value() && noexcept
            {
                done->store(true, std::memory_order_release);
            }
        };

        auto join = stdexec::connect(command_scope_.join(), Joined{&joined});
        stdexec::start(join);
        response_outbox_.flushTo(responder, config.request_work_budget);
        // Keep the Owner and GPU completion paths advancing until every accepted Sender settles.
        while (!joined.load(std::memory_order_acquire) || !render_loop_.empty())
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
            if (!joined.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
        }
        while (render_loop_.activePublishers() != 0)
        {
            std::this_thread::yield();
        }
        response_outbox_.flushTo(responder, SIZE_MAX);
        response_outbox_.clear(); // Connection closes after safe operations; caller owns any unknown outcomes.
        responder.reset();
        while (!active_targets.empty())
        {
            auto& target = *targets.find(active_targets.keys().back());
            if (!detachAttachment(target.attachment))
            {
                std::terminate();
            }
        }
        // End plan owners while physical cleanup is still admitted. The Session closes only
        // after Scene/Global Storage and every other producer have transferred their resources.
        for (auto& effect : effects)
        {
            device->deferRelease([code = std::move(effect.code)]() mutable noexcept { code.reset(); });
        }
        effects.clear();
        targets.clear();
        scenes.clear();
        command_registry_.clear(); // All operations ended; professional Handler borrows still have a live Device.
        device.reset();
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
                if (notifications.workEpoch() != observed || !s.render_loop_.empty())
                {
                    continue;
                }
                if (s.device->pendingGpuWork())
                {
                    if (auto advanced = s.device->waitForProgress(s.config.gpu_wait_ns); !advanced)
                    {
                        result = cxx::unexpected(advanced.error());
                        break;
                    }
                }
                else
                {
                    // Receive only inspected a bounded prefix of Links. Preserve its finite revisit.
                    notifications.waitUntil(observed, std::chrono::steady_clock::now() + std::chrono::milliseconds(1));
                }
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
            s.storages_created.load()
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
