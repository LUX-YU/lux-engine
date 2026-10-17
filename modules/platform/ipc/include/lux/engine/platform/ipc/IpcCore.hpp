#pragma once

#include <atomic>
#include <chrono>
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

    // An unforgeable, non-owning route. Only its issuing Channel epoch can dereference the Link.
    class RequesterId final
    {
    public:
        RequesterId() noexcept = default;
        [[nodiscard]] bool isNull() const noexcept { return link_ == nullptr; }
        bool operator==(const RequesterId&) const noexcept = default;

    private:
        friend class IpcRequester;
        friend class IpcResponder;
        RequesterId(const detail::Name* name, std::uint64_t epoch, detail::Link* link) noexcept
            : name_(name), epoch_(epoch), link_(link) {}
        const detail::Name* name_ = nullptr;
        std::uint64_t epoch_ = 0;
        detail::Link* link_ = nullptr;
    };

    class LUX_PLATFORM_IPC_PUBLIC ReceivedRequest final
    {
    public:
        ReceivedRequest() noexcept = default;
        ReceivedRequest(ReceivedRequest&&) noexcept = default;
        ReceivedRequest& operator=(ReceivedRequest&&) noexcept = default;
        ReceivedRequest(const ReceivedRequest&) = delete;
        ReceivedRequest& operator=(const ReceivedRequest&) = delete;
        [[nodiscard]] const IpcMessage& message() const noexcept { return message_; }
        [[nodiscard]] IpcMessage& message() noexcept { return message_; }
        [[nodiscard]] RequesterId requesterId() const noexcept { return requester_id_; }
        [[nodiscard]] std::uint64_t tag() const noexcept { return tag_; }
        [[nodiscard]] std::uint64_t correlation() const noexcept { return correlation_; }
        [[nodiscard]] bool active() const noexcept { return !message_.empty(); }
        void reset() noexcept { message_.reset(); requester_id_ = {}; tag_ = correlation_ = 0; }

    private:
        friend class IpcResponder;
        RequesterId requester_id_;
        std::uint64_t tag_ = 0;
        std::uint64_t correlation_ = 0;
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
        [[nodiscard]] RequesterId id() const noexcept;
        [[nodiscard]] Result<void> trySend(std::uint64_t tag, std::uint64_t correlation, IpcMessage& message) noexcept;
        // Only RECEIVED modifies output. Output must be empty/settled on success.
        [[nodiscard]] EIpcReceive tryReceive(std::uint64_t& correlation, IpcMessage& output) noexcept;
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
        [[nodiscard]] Result<void> trySendResponse(RequesterId requester, std::uint64_t correlation, IpcMessage& response) noexcept;
        // Cold; the host settles pending operations and responses before closing the responder.
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
        // Observe before checking work. Publication changes this epoch before waking waiters.
        // This process-wide hint owns no callbacks; unrelated channels may cause harmless wakes.
        [[nodiscard]] std::uint64_t workEpoch() const noexcept;
        void notifyWork() noexcept;
        void waitUntil(std::uint64_t observed, std::chrono::steady_clock::time_point deadline) noexcept;
        IpcCore(const IpcCore&) = delete;
        IpcCore& operator=(const IpcCore&) = delete;

    private:
        IpcCore() noexcept = default;
        ~IpcCore() noexcept;
        [[nodiscard]] detail::Name* findName(std::string_view name) noexcept;
        std::atomic<detail::Name*> names_{nullptr};
        std::atomic<std::uint64_t> work_epoch_{0};
    };
}
