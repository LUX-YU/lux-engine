#include <lux/engine/render/frame/FrameError.hpp>
#include <lux/engine/render/runtime/RenderClient.hpp>
#include <algorithm>
#include <chrono>
#include <exception>
#include <lux/cxx/container/SlotMap.hpp>
#include <limits>

namespace lux::render
{
    namespace ipc = lux::platform::ipc;

    struct RenderClient::Impl final
    {
        struct CorrelationTag;
        using Key = cxx::SlotKey<CorrelationTag>;

        struct WindowReceiver final
        {
            RenderJobId job;
            JobReplyHandler handler;
        };

        struct PendingResponse final
        {
            Key key;
            std::variant<ResponseNode*, WindowReceiver> receiver;

            [[nodiscard]] bool matches(const ipc::IpcMessage& message) const noexcept
            {
                if (const auto* node = std::get_if<ResponseNode*>(&receiver))
                    return message.typeHash() == (*node)->response_type.hash();
                return message.typeHash() == cxx::make_basic_type_info<JobSubmitted>().hash() &&
                    message.get<JobSubmitted>().job == std::get<WindowReceiver>(receiver).job;
            }

            void finish(ipc::IpcMessage&& message, RenderError error) const noexcept
            {
                if (const auto* node = std::get_if<ResponseNode*>(&receiver))
                {
                    RenderClient::deliver(**node, std::move(message), error);
                    return;
                }
                const auto& run = std::get<WindowReceiver>(receiver);
                auto reply = error.type ? JobSubmitted{error, run.job} : std::move(message.get<JobSubmitted>());
                run.handler.complete(run.handler.context, std::move(reply));
            }
        };

        explicit Impl(ipc::IpcRequester requester) noexcept : requester(std::move(requester))
        {
        }

        static std::uint64_t correlation(Key key) noexcept
        {
            return (std::uint64_t(key.gen) << 32) | (std::uint64_t(key.index) + 1);
        }

        static Key key(std::uint64_t value) noexcept
        {
            return {static_cast<std::uint32_t>(value) - 1, static_cast<std::uint32_t>(value >> 32)};
        }

        ipc::IpcRequester requester;
        cxx::SlotMap<PendingResponse, CorrelationTag> pending;
        std::array<CommandId, std::tuple_size_v<CommonCommands>> commands;
        RenderError failure;
    };

    RenderClient::RenderClient(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    RenderClient::CreateResult RenderClient::create(ipc::IpcRequester requester) noexcept
    {
        static constexpr auto names = std::apply([](auto... commands) constexpr
        {
            return std::array{CommandName{decltype(commands)::name}...};
        }, CommonCommands{});
        // The endpoint has no business requests yet. Only this fixed bootstrap response can arrive;
        // after creation, dispatchResponses remains the sole receiver. Names outlive even a failed link.
        ipc::IpcMessage message;
        message.emplace<QueryCommandIds::Request>(names);
        const auto sent = requester.trySend(commandTag(QueryCommandIds::id), 1, message);
        if (!sent)
        {
            return cxx::unexpected(RenderError{
                sent.error() == ipc::EIpcError::WOULD_BLOCK ? kRuntimeCapacity : kRuntimeState
            });
        }
        auto& notifications = ipc::IpcCore::instance();
        for (;;)
        {
            const auto observed = notifications.workEpoch();
            std::uint64_t correlation = 0;
            const auto status = requester.tryReceive(correlation, message);
            if (status == ipc::EIpcReceive::CLOSED)
            {
                return cxx::unexpected(RenderError{kOutcomeUnknown});
            }
            if (status == ipc::EIpcReceive::RECEIVED)
            {
                constexpr auto response_type = cxx::make_basic_type_info<QueryCommandIds::Response>();
                const bool invalid_response = correlation != 1 || message.typeHash() != response_type.hash();
                if (invalid_response) return cxx::unexpected(RenderError{kCommandContract});
                auto& response = message.get<QueryCommandIds::Response>();
                if (response.error.type != 0) return cxx::unexpected(response.error);
                const bool invalid_ids = response.ids.size() != names.size() ||
                    std::ranges::any_of(response.ids, [](CommandId id) noexcept { return id.isNull(); });
                if (invalid_ids) return cxx::unexpected(RenderError{kCommandContract});
                auto impl = std::make_unique<Impl>(std::move(requester));
                std::ranges::copy(response.ids, impl->commands.begin());
                return std::unique_ptr<RenderClient>{new RenderClient(std::move(impl))};
            }
            notifications.waitUntil(observed, std::chrono::steady_clock::time_point::max());
        }
    }

    RenderClient::~RenderClient() noexcept
    {
        if (!impl_->pending.empty())
        {
            std::terminate(); // The owner must settle accepted operations before destroying their endpoint.
        }
    }

    CommandId RenderClient::commonCommand(std::size_t index) const noexcept
    {
        return impl_->commands[index];
    }

    RenderResult<void> RenderClient::send(CommandId command, ipc::IpcMessage& message, ResponseNode& receiver) noexcept
    {
        if (impl_->failure.type != 0)
        {
            return cxx::unexpected(impl_->failure);
        }
        if (command == commonCommand(commonIndex<RequestRenderJob>()) &&
            message.typeHash() == cxx::make_basic_type_info<RequestRenderJob::Request>().hash() &&
            !message.get<RequestRenderJob::Request>().additional_runs.empty())
            return cxx::unexpected(RenderError{kCommandContract});
        const auto id = impl_->pending.emplace(Impl::PendingResponse{{}, &receiver});
        impl_->pending.find(id)->key = id;
        auto sent = impl_->requester.trySend(commandTag(command), Impl::correlation(id), message);
        if (!sent)
        {
            impl_->pending.erase(id);
            return cxx::unexpected(RenderError{
                sent.error() == ipc::EIpcError::WOULD_BLOCK ? kRuntimeCapacity : kRuntimeState
            });
        }
        return {};
    }

    RenderResult<void> RenderClient::submitWindow(
        std::span<const RenderJobId> jobs, WindowUpdates& updates, JobReplyHandler handler
    ) noexcept
    {
        if (impl_->failure.type) return cxx::unexpected(impl_->failure);
        if (jobs.empty() || !handler.complete) return cxx::unexpected(RenderError{kFrameContract});
        ipc::IpcMessage message;
        auto& input = message.emplace<RequestRenderJob::Request>();
        input.job = jobs.front();
        input.additional_runs.reserve(jobs.size() - 1);
        const auto register_reply = [&](RenderJobId job) noexcept
        {
            const auto key = impl_->pending.emplace(Impl::PendingResponse{{}, Impl::WindowReceiver{job, handler}});
            impl_->pending.find(key)->key = key;
            return RequestId{Impl::correlation(key)};
        };
        const auto first = register_reply(input.job);
        for (auto job : jobs.subspan(1)) input.additional_runs.push_back({job, register_reply(job)});
        input.updates = std::move(updates);
        const auto command = commonCommand(commonIndex<RequestRenderJob>());
        const auto sent = impl_->requester.trySend(commandTag(command), first.value, message);
        if (sent) return {};
        updates = std::move(input.updates);
        input.forEachRun(first, [&](const auto& run) noexcept { impl_->pending.erase(Impl::key(run.reply.value)); });
        return cxx::unexpected(RenderError{
            sent.error() == ipc::EIpcError::WOULD_BLOCK ? kRuntimeCapacity : kRuntimeState
        });
    }

    void RenderClient::deliver(ResponseNode& node, ipc::IpcMessage&& message, RenderError error) noexcept
    {
        // The receiver can destroy the operation. Delivery returns without accessing it again.
        node.deliver(node, std::move(message), error);
    }

    RenderResult<std::size_t> RenderClient::dispatchResponses(std::size_t limit) noexcept
    {
        std::size_t count = 0;
        for (; count < limit; ++count)
        {
            ipc::IpcMessage message;
            std::uint64_t correlation_id = 0;
            const auto status = impl_->requester.tryReceive(correlation_id, message);
            if (status != ipc::EIpcReceive::RECEIVED)
            {
                if (status == ipc::EIpcReceive::CLOSED && impl_->failure.type == 0)
                {
                    impl_->failure = {kOutcomeUnknown};
                }
                break;
            }
            const auto id = Impl::key(correlation_id);
            const auto* entry = impl_->pending.find(id);
            if (!entry)
            {
                continue;
            }
            if (!entry->matches(message))
            {
                if (impl_->failure.type == 0) impl_->failure = {kCommandContract};
                continue; // Keep this operation until Server can no longer borrow its source.
            }
            const auto pending = *entry;
            impl_->pending.erase(id);
            pending.finish(std::move(message), {});
        }
        if (impl_->failure.type != 0)
        {
            return cxx::unexpected(impl_->failure);
        }
        return count;
    }

    void RenderClient::serverStopped(RenderError error) noexcept
    {
        if (impl_->failure.type == 0) impl_->failure = error.type != 0 ? error : RenderError{kOutcomeUnknown};
        // Server is joined: no publisher remains. Actual replies take precedence over synthesized outcomes.
        static_cast<void>(dispatchResponses(std::numeric_limits<std::size_t>::max()));
        while (!impl_->pending.empty())
        {
            const auto entry = *impl_->pending.begin();
            impl_->pending.erase(entry.key);
            entry.finish({}, {kOutcomeUnknown});
        }
    }
} // namespace lux::render
