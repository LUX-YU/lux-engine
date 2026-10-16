#include "ServerImpl.hpp"

namespace lux::render
{
    RenderResult<void> RenderServer::Impl::tick() noexcept
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
        auto collected = device->collect();
#ifdef LUX_RUNTIME_TEST_SEAM
        if (runtime_detail::test::observeGpuCompletion()) device->dispatchCompletions();
#else
        device->dispatchCompletions();
#endif
        return collected;
    }

    RenderResult<void> RenderServer::Impl::queryRenderStats(RenderRequest& request) noexcept
    {
        return request.reply(
            RenderStatsSnapshot{ticks.load(), commands.load(), compiled_graphs.load(), submissions.load()}
        );
    }

    RenderResult<void> RenderServer::Impl::stopCommand(RenderRequest& request) noexcept
    {
        stop_requested.store(true);
        return request.reply();
    }

    void RenderServer::Impl::dispatch(ipc::ReceivedRequest received) noexcept
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
        const auto* cold = entry ? entry->cold.get() : nullptr;
        const auto* transport = frame.command.isNull() ? &hello_transport : (entry ? &cold->transport : nullptr);
        const auto* actual_type = received.message().operations();
        const bool valid_type =
            frame.command.isNull()
                ? actual_type && bool(ipc::IpcMessage::bindType<ValueEnvelope<HelloRequest>>(*actual_type))
                : transport && received.message().holds(*transport->request);
        const auto* descriptor =
            frame.command.isNull() ? &commandDescriptor<Hello>() : (entry ? cold->binding.descriptor() : nullptr);
        const bool valid_identity = frame.command.isNull() || (entry && cold->available && frame.session == identity);
        const bool valid_capacity = frame.reply_capacity <= config.max_reply_bytes;
        bool funded = false;
        if (valid_identity && valid_type && valid_capacity)
        {
            auto& message = const_cast<ipc::IpcMessage&>(received.message());
            const auto reply_overhead = std::max(
                transport->response->size() - descriptor->response.size,
                sizeof(ValueEnvelope<RenderError>) - sizeof(RenderError)
            );
            funded = transport->request_budget(message).bytes() >=
                     transport->request->size() + frame.reply_capacity + reply_overhead;
        }
        RenderRequest request(
            std::move(received),
            responder,
            identity,
            frame.request,
            frame.reply_capacity,
            descriptor,
            funded ? transport : nullptr,
            failure_type,
            entry ? cold->code_pin : config.implementation_code_pin
        );
        RenderResult<void> result;
        if (!valid_identity)
            result = cxx::unexpected(RenderError{kRuntimeIdentity});
        else if (!valid_type)
            result = cxx::unexpected(RenderError{kProtocolContract});
        else if (!valid_capacity || !funded)
            result = cxx::unexpected(RenderError{kRuntimeCapacity});
        else if (frame.command.isNull())
        {
            if (clients == 2)
                result = cxx::unexpected(RenderError{kRuntimeCapacity});
            else
            {
                result = request.reply(manifest);
                if (result) ++clients;
            }
        }
        else if (descriptor->response.size > frame.reply_capacity)
            result = cxx::unexpected(RenderError{kRuntimeCapacity});
        else
        {
            GpuAdmission admission{*this, request};
            vulkan::GpuCompletionAccess access{&gpu_completion_functions, &admission};
            auto invocation = request.invocation(
                &admission, command_completion_functions, cold->binding.gpuCompletion() ? &access : nullptr
            );
            const auto active = entry->active;
            result = active.invoke(active.object, invocation);
        }
        settle(std::move(request), result);
        ++commands;
    }

    void RenderServer::Impl::shutdown() noexcept
    {
        stop_requested.store(true);
        // cancel may synchronously finish a Native operation. Snapshot only bounded generational
        // identities, not iterators or owners, before any terminal callback can erase a slot.
        std::array<CompletionId, 136> pending;
        std::size_t pending_count = 0;
        for (const auto& slot : deferred)
            pending[pending_count++] = slot.id;
        for (std::size_t i = 0; i < pending_count; ++i)
        {
            auto* slot = deferred.find(pending[i]);
            if (!slot) continue;
            slot->stop.request_stop();
            if (auto* readback = std::get_if<ReadbackCompletion>(&slot->condition))
            {
                if (readback->context) std::exchange(readback->context, nullptr)->releaseReadback();
            }
            else if (auto* gpu = std::get_if<GpuCompletion>(&slot->condition))
                gpu->work.cancel();
            // No slot access after cancel: its terminal callback may have destroyed it.
        }
        // Only shutdown waits. CPU/source owners remain in the same slots; cancellation prevents
        // their eventual CPU event from submitting another GPU chunk.
        if (device)
        {
            auto drained = device->drain();
            if (!drained && !device->deviceLost()) std::terminate();
        }
        while (!deferred.empty())
        {
            drainCompletions();
            if (device)
            {
                auto collected = device->collect();
                device->dispatchCompletions();
                if (!collected && !device->deviceLost()) std::terminate();
            }
            if (!deferred.empty()) std::this_thread::yield();
        }
        while (completion_publishers.load(std::memory_order_acquire) != 0)
            std::this_thread::yield();
        undelivered.clear();
        responder.reset();
        effects.clear();
        device.reset();
        instance.reset();
        running.store(false);
    }

    RenderResult<void> RenderServer::Impl::run() noexcept
    {
        auto& s = *this;
        if (s.started.exchange(true)) return cxx::unexpected(RenderError{kRuntimeState});
        if (s.stop_requested.load())
        {
            s.shutdown();
            return {};
        }
        s.running.store(true);
        auto& resources = s.device->resources();
        for (const auto& type : s.storage_types)
        {
            if (auto installed = resources.installFactory(type.factory); !installed)
            {
                s.shutdown();
                return cxx::unexpected(installed.error());
            }
            if (auto configured =
                    resources.configureType(*type.description, type.configuration);
                !configured)
            {
                s.shutdown();
                return cxx::unexpected(configured.error());
            }
        }
        resources.freeze();
        const std::uint32_t capabilities =
            (s.device->device().textureDescriptors() ? 1u : 0u) | (s.device->device().textureCompressionBc() ? 2u : 0u);
        for (auto& route : s.command_table)
        {
            auto& entry = *route.cold;
            entry.available = !entry.storage || (entry.storage->required_capabilities & ~capabilities) == 0;
            if (!entry.available) continue;
            const auto& descriptor = *entry.binding.descriptor();
            s.manifest.capabilities.push_back(
                {entry.code_pin,
                 std::string(descriptor.name.name()),
                 entry.id,
                 descriptor.version,
                 descriptor.request,
                 descriptor.response,
                 descriptor.completion,
                 entry.transport.request,
                 entry.transport.response}
            );
        }
        RenderResult<void> result;
        while (!s.stop_requested.load())
        {
#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
            test::beforeDispatch();
#endif
            for (std::uint32_t i = 0; i < s.config.requests_per_tick && !s.stop_requested.load(); ++i)
            {
                ipc::ReceivedRequest request;
                if (s.responder.tryReceive(request) == ipc::EIpcReceive::RECEIVED) s.dispatch(std::move(request));
            }
            if (s.stop_requested.load()) break;
            s.drainCompletions();
            if (auto rendered = s.tick(); !rendered)
            {
                result = cxx::unexpected(rendered.error());
                break;
            }
            ++s.ticks;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
