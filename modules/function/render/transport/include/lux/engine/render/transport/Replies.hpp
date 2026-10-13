#pragma once

#include <lux/engine/render/transport/Operation.hpp>

namespace lux::render
{
    class ReplyTicket final
    {
    public:
        [[nodiscard]] RequestId localId() const noexcept { return id_; }

    private:
        friend class RenderRequest;
        friend class ReplyInbox;
        ReplyTicket(std::shared_ptr<const detail::EndpointIdentity> owner, RequestId id) noexcept
            : owner_(std::move(owner)), id_(id) {}
        std::shared_ptr<const detail::EndpointIdentity> owner_;
        RequestId id_;
    };

    class TerminalReply final
    {
    public:
        EOperationOutcome outcome = EOperationOutcome::CANCELLED;
        RenderError error{};

        template <WireData T>
        [[nodiscard]] RenderResult<T> decode(WireLimits limits) const noexcept
        {
            auto wire = wireView();
            if (!wire) return cxx::unexpected(wire.error());
            return decodeWire<T>(*wire, limits);
        }

    private:
        friend struct detail::TransportState;
        [[nodiscard]] RenderResult<WireView> wireView() const noexcept;
        std::shared_ptr<detail::TransportState> state_;
        OperationId operation_;
        OwnedWireBytes bytes_;
    };

    class ReplyInbox final
    {
    public:
        [[nodiscard]] RenderResult<std::optional<TerminalReply>> poll(ReplyTicket ticket) const noexcept;
        [[nodiscard]] RenderResult<void> abandon(ReplyTicket ticket) const noexcept;

    private:
        friend class TransportSender;
        explicit ReplyInbox(std::shared_ptr<detail::TransportState> state) noexcept : state_(std::move(state)) {}
        std::shared_ptr<detail::TransportState> state_;
    };

    class RenderRequest final
    {
    public:
        RenderRequest(RenderRequest&&) noexcept;
        RenderRequest& operator=(RenderRequest&&) noexcept;
        RenderRequest(const RenderRequest&) = delete;
        RenderRequest& operator=(const RenderRequest&) = delete;
        ~RenderRequest();

        [[nodiscard]] ReplyTicket ticket() const noexcept;
        [[nodiscard]] RenderResult<std::optional<TerminalReply>> poll() noexcept;
        [[nodiscard]] RenderResult<void> abandon() noexcept;

    private:
        friend class TransportSender;
        RenderRequest(std::shared_ptr<detail::TransportState> state, RequestId id) noexcept
            : state_(std::move(state)), id_(id) {}
        std::shared_ptr<detail::TransportState> state_;
        RequestId id_;
    };

    // A deferred promise retains the request's input owners and budget in its
    // stable cell. Terminal settlement and release of input use are distinct.
    class ReplyPromise final
    {
    public:
        ReplyPromise(ReplyPromise&&) noexcept;
        ReplyPromise& operator=(ReplyPromise&&) noexcept;
        ReplyPromise(const ReplyPromise&) = delete;
        ReplyPromise& operator=(const ReplyPromise&) = delete;
        ~ReplyPromise();

        [[nodiscard]] RenderResult<WireView> input() const noexcept;
        [[nodiscard]] RenderResult<void> success() noexcept;
        [[nodiscard]] RenderResult<void> success(const DataSchemaDescriptor& schema, OwnedWireBytes bytes) noexcept;
        [[nodiscard]] RenderResult<void> fail(RenderError error) noexcept;
        [[nodiscard]] RenderResult<void> cancel() noexcept;

    private:
        friend class RenderTransport;
        ReplyPromise(std::shared_ptr<detail::TransportState> state, RequestId id) noexcept
            : state_(std::move(state)), id_(id) {}
        void release() noexcept;
        std::shared_ptr<detail::TransportState> state_;
        RequestId id_;
    };
}
