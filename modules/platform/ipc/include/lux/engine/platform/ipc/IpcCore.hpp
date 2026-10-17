#pragma once

#include <atomic>
#include <string_view>
#include <lux/engine/platform/ipc/IpcMessage.hpp>

namespace lux::platform::ipc
{
    namespace detail { struct Name; struct Channel; struct Link; }
    class IpcCore;
    class IpcResponder;

    struct IpcQueueConfig final
    {
        // Initial segment sizes, not total queued-message limits.
        std::size_t request_segment_size = 64;
        std::size_t reply_segment_size = 64;
    };

    class LUX_PLATFORM_IPC_PUBLIC ReceivedRequest final
    {
    public:
        ReceivedRequest() noexcept = default;
        ~ReceivedRequest() noexcept;
        ReceivedRequest(ReceivedRequest&& other) noexcept;
        ReceivedRequest& operator=(ReceivedRequest&& other) noexcept;
        ReceivedRequest(const ReceivedRequest&) = delete;
        ReceivedRequest& operator=(const ReceivedRequest&) = delete;
        [[nodiscard]] const IpcMessage& message() const noexcept { return message_; }
        [[nodiscard]] bool active() const noexcept { return link_ != nullptr; }
        // Borrowed from the responder: settle/destroy BEFORE responder reset.
        // Same logical owner as responder; disconnected owner disposal is cold.
        void reset() noexcept;

    private:
        friend class IpcResponder;
        detail::Channel* channel_ = nullptr;
        detail::Link* link_ = nullptr;
        IpcMessage message_;
    };

    class LUX_PLATFORM_IPC_PUBLIC IpcRequester final
    {
    public:
        IpcRequester() noexcept = default;
        ~IpcRequester() noexcept;
        IpcRequester(IpcRequester&& other) noexcept;
        IpcRequester& operator=(IpcRequester&& other) noexcept;
        IpcRequester(const IpcRequester&) = delete;
        IpcRequester& operator=(const IpcRequester&) = delete;
        [[nodiscard]] bool ready() const noexcept;
        [[nodiscard]] Result<void> trySend(IpcMessage& message) noexcept;
        // Only RECEIVED modifies output. Output must be empty/settled on success.
        [[nodiscard]] EIpcReceive tryReceive(IpcMessage& output) noexcept;
        // Cold. Closes participation without waiting for Ring access or consumption.
        // Host serializes this with this requester's own send/receive operations.
        void reset() noexcept;

    private:
        friend class IpcCore;
        detail::Channel* channel_ = nullptr;
        detail::Link* link_ = nullptr;
    };

    class LUX_PLATFORM_IPC_PUBLIC IpcResponder final
    {
    public:
        IpcResponder() noexcept = default;
        ~IpcResponder() noexcept;
        IpcResponder(IpcResponder&& other) noexcept;
        IpcResponder& operator=(IpcResponder&& other) noexcept;
        IpcResponder(const IpcResponder&) = delete;
        IpcResponder& operator=(const IpcResponder&) = delete;
        // At most one pending Link adopted and eight Link visits per call.
        // Only RECEIVED modifies output; an active output must not be overwritten.
        [[nodiscard]] EIpcReceive tryReceive(ReceivedRequest& output) noexcept;
        [[nodiscard]] Result<void> tryReply(ReceivedRequest& request, IpcMessage& reply) noexcept;
        // Cold; all borrowed ReceivedRequest values must already be settled.
        // Serialized with this responder's methods and request disposal.
        void reset() noexcept;

    private:
        friend class IpcCore;
        detail::Channel* channel_ = nullptr;
        detail::Link* pending_ = nullptr;
        detail::Link* cursor_ = nullptr;
    };

    class LUX_PLATFORM_IPC_PUBLIC IpcCore final
    {
    public:
        [[nodiscard]] static IpcCore& instance() noexcept;
        [[nodiscard]] Result<IpcRequester>
        createRequester(std::string_view name, IpcQueueConfig config = {}) noexcept;
        [[nodiscard]] Result<IpcResponder> createResponder(std::string_view name) noexcept;
        IpcCore(const IpcCore&) = delete;
        IpcCore& operator=(const IpcCore&) = delete;

    private:
        IpcCore() noexcept = default;
        ~IpcCore() noexcept;
        [[nodiscard]] detail::Name* findName(std::string_view name) noexcept;
        std::atomic<detail::Name*> names_{nullptr};
    };
}
