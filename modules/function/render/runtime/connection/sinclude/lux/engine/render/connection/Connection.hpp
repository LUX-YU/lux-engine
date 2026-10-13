#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <vector>
#include <lux/engine/render/protocol/Protocol.hpp>

namespace lux::render::connection
{
    inline constexpr auto kCapacity = error::errorId("lux.render.connection.capacity");
    inline constexpr auto kClosed = error::errorId("lux.render.connection.closed");
    inline constexpr auto kStale = error::errorId("lux.render.connection.stale");
    inline constexpr auto kOwner = error::errorId("lux.render.connection.owner");
    inline constexpr auto kAddress = error::errorId("lux.render.connection.address");
    inline constexpr auto kCancelled = error::errorId("lux.render.connection.cancelled");
    [[nodiscard]] std::span<const error::ErrorDescriptor> connectionErrorDescriptors() noexcept;

    // Server-owned immutable directory. Descriptors use static G2 metadata.
    // Publication transfers exclusive mutation authority: no mutable aliases after listen.
    struct OperationDefinition final
    {
        OperationId id;
        OperationDescriptor descriptor;
    };
    using OperationDirectory = cxx::SlotMap<OperationDefinition, OperationTag>;

    struct ConnectionConfig final
    {
        std::size_t request_capacity = 64;
        std::size_t reply_capacity = 64;
        std::size_t byte_budget = 16 * 1024 * 1024;
        std::size_t message_reserve = 256;
        std::size_t max_attachments = 16;
    };

    struct Packet final
    {
        OwnedWireBytes bytes;
        std::shared_ptr<const std::vector<OwnedWireBytes>> attachments;

        [[nodiscard]] std::span<const OwnedWireBytes> attachmentView() const noexcept
        {
            return attachments ? std::span<const OwnedWireBytes>(*attachments) : std::span<const OwnedWireBytes>{};
        }
    };

    // Pool entries have independent ownership, never a reference back to their connection.
    // Retained decoded bytes pin an entry; unique entries alone can be written again.
    class MessageStorage final
    {
    public:
        MessageStorage(std::size_t count, std::size_t reserve) noexcept;

        template <WireData T>
        [[nodiscard]] RenderResult<OwnedWireBytes>
        encode(const T& value, std::size_t limit) noexcept
        {
            auto size = DataSchema<T>::wireSize(value);
            if (!size) return cxx::unexpected(size.error());
            if (*size > limit) return cxx::unexpected(RenderError{kWireLimit});
            for (auto& block : blocks_)
            {
                if (block.use_count() != 1) continue;
                block->resize(*size);
                cxx::BinarySpanWriter writer(*block);
                auto result = DataSchema<T>::encode(value, writer);
                if (!result) return cxx::unexpected(result.error());
                if (!writer.good() || writer.size() != *size) return cxx::unexpected(RenderError{kInvalidWire});
                return OwnedWireBytes::fromOwner(block, std::span<const std::byte>(*block));
            }
            return cxx::unexpected(RenderError{kCapacity});
        }

    private:
        std::vector<std::shared_ptr<std::vector<std::byte>>> blocks_;
    };

    struct ServerIdentity;
    struct ConnectionBacking;
    struct PendingConnect;
    class LocalListener;
    class ServerConnectionEndpoint;
    class ClientConnectionEndpoint;

    class BoundOperation final
    {
    public:
        [[nodiscard]] OperationId id() const noexcept { return id_; }

    private:
        friend class ClientConnectionEndpoint;
        std::shared_ptr<const ServerIdentity> server_;
        OperationId id_;
    };

    struct ReplyMessage final
    {
        ReplyHeader header;
        Packet result;
        Packet retired_input;
    };

    // Move-only work remains pinned across deferred completion and endpoint collection growth.
    // All methods, including destruction, belong to the server owner thread.
    class ServerWork final
    {
    public:
        ServerWork() noexcept = default;
        ~ServerWork();
        ServerWork(ServerWork&& other) noexcept;
        ServerWork& operator=(ServerWork&& other) noexcept;
        ServerWork(const ServerWork&) = delete;
        ServerWork& operator=(const ServerWork&) = delete;
        [[nodiscard]] const RequestHeader& header() const noexcept { return header_; }
        [[nodiscard]] const Packet& payload() const noexcept { return input_; }
        [[nodiscard]] bool active() const noexcept { return static_cast<bool>(backing_); }

        template <WireData T>
        [[nodiscard]] RenderResult<T> decode() const noexcept
        {
            const auto* operation = descriptor();
            if (!operation) return cxx::unexpected(RenderError{kStale});
            return decodeWire<T>({operation->payload, input_.bytes, input_.attachmentView()}, limits());
        }

        template <WireData T>
        [[nodiscard]] RenderResult<void> succeed(const T& value) noexcept
        {
            const auto* operation = descriptor();
            if (!operation) return cxx::unexpected(RenderError{kStale});
            if (!operation->reply) return fail({kProtocolContract});
            auto checked = compatibleDataSchemas(*operation->reply, DataSchema<T>::describe());
            if (!checked) return fail(checked.error());
            auto bytes = storage().encode(value, limits().max_wire_bytes);
            if (!bytes) return fail(bytes.error());
            Packet packet{std::move(*bytes), {}};
            return finish(EOperationOutcome::SUCCESS, {}, std::move(packet));
        }

        [[nodiscard]] RenderResult<void> succeed() noexcept;
        [[nodiscard]] RenderResult<void> fail(RenderError error) noexcept;
        [[nodiscard]] RenderResult<void> cancel() noexcept;

    private:
        friend class ServerConnectionEndpoint;
        ServerWork(std::shared_ptr<ConnectionBacking> backing, RequestHeader header, Packet input) noexcept;
        [[nodiscard]] const OperationDescriptor* descriptor() const noexcept;
        [[nodiscard]] WireLimits limits() const noexcept;
        [[nodiscard]] MessageStorage& storage() noexcept;
        [[nodiscard]] RenderResult<void> finish(EOperationOutcome outcome, RenderError error, Packet result) noexcept;
        std::shared_ptr<ConnectionBacking> backing_;
        RequestHeader header_;
        Packet input_;
    };

    class ClientConnectionEndpoint final
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<ClientConnectionEndpoint>>
        connect(std::string_view address) noexcept;
        ~ClientConnectionEndpoint();
        ClientConnectionEndpoint(const ClientConnectionEndpoint&) = delete;
        ClientConnectionEndpoint& operator=(const ClientConnectionEndpoint&) = delete;
        // Host must synchronize handoff and cease all use by the previous owner first.
        void adoptCurrentThread() noexcept;
        [[nodiscard]] bool ready() const noexcept;
        [[nodiscard]] RenderResult<BoundOperation> bind(const OperationDescriptor& descriptor) const noexcept;
        [[nodiscard]] RenderResult<RequestId> submit(const BoundOperation& operation, Packet& packet) noexcept;

        template <WireData T>
        [[nodiscard]] RenderResult<RequestId> submit(
            const BoundOperation& operation, const T& value, std::span<const OwnedWireBytes> attachments = {}
        ) noexcept
        {
            auto valid = validateSubmit(operation);
            if (!valid) return cxx::unexpected(valid.error());
            auto checked = compatibleDataSchemas(valid->payload, DataSchema<T>::describe());
            if (!checked) return cxx::unexpected(checked.error());
            auto bytes = storage_.encode(value, config().byte_budget);
            if (!bytes) return cxx::unexpected(bytes.error());
            Packet packet{std::move(*bytes), {}};
            if (!attachments.empty())
            {
                packet.attachments = std::make_shared<const std::vector<OwnedWireBytes>>(
                    attachments.begin(), attachments.end()
                );
            }
            return submit(operation, packet);
        }

        // A reply stream is connection-local: RequestIds never identify another endpoint.
        [[nodiscard]] RenderResult<std::optional<ReplyMessage>> poll() noexcept;
        [[nodiscard]] RenderResult<void> abandon(RequestId request) noexcept;
        void close() noexcept;
        [[nodiscard]] std::size_t pendingCount() const noexcept { return pending_.size(); }
        [[nodiscard]] std::size_t inFlightBytes() const noexcept { return in_flight_bytes_; }
        [[nodiscard]] std::size_t requestDepth() const noexcept;
        [[nodiscard]] std::size_t replyDepth() const noexcept;

    private:
        struct Pending final
        {
            RequestId id;
            std::size_t bytes = 0;
            bool abandoned = false;
        };
        explicit ClientConnectionEndpoint(std::shared_ptr<ConnectionBacking> backing) noexcept;
        [[nodiscard]] RenderResult<OperationDescriptor> validateSubmit(const BoundOperation& operation) const noexcept;
        [[nodiscard]] const ConnectionConfig& config() const noexcept;
        std::shared_ptr<ConnectionBacking> backing_;
        std::thread::id owner_;
        cxx::SlotMap<Pending, RequestTag> pending_;
        MessageStorage storage_;
        std::size_t in_flight_bytes_ = 0;
    };

    class ServerConnectionEndpoint final
    {
    public:
        explicit ServerConnectionEndpoint(std::shared_ptr<ConnectionBacking> backing) noexcept;
        ~ServerConnectionEndpoint();
        ServerConnectionEndpoint(const ServerConnectionEndpoint&) = delete;
        ServerConnectionEndpoint& operator=(const ServerConnectionEndpoint&) = delete;
        [[nodiscard]] std::optional<ServerWork> receive() noexcept;
        void close() noexcept;
        [[nodiscard]] bool disconnected() const noexcept;
        [[nodiscard]] bool reclaimable() const noexcept;

    private:
        std::shared_ptr<ConnectionBacking> backing_;
    };

    // One address is published in this initial in-process backend. Host serializes
    // listen/unlisten against connect; multiple connects may run concurrently while live.
    class LocalListener final
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<LocalListener>> listen(
            std::string_view address,
            std::shared_ptr<const OperationDirectory> operations,
            ConnectionConfig config = {}
        ) noexcept;
        ~LocalListener();
        LocalListener(const LocalListener&) = delete;
        LocalListener& operator=(const LocalListener&) = delete;
        std::size_t accept(std::size_t budget) noexcept;
        // Cold maintenance only: may run final byte-owner destructors. Never called by a receive loop.
        std::size_t collectDisconnected() noexcept;
        [[nodiscard]] std::span<const std::unique_ptr<ServerConnectionEndpoint>> endpoints() const noexcept;
        // No connect callers may remain when stop starts. Deferred ServerWork may outlive stop.
        void stop() noexcept;

    private:
        friend class ClientConnectionEndpoint;
        LocalListener(std::string_view address, std::shared_ptr<const OperationDirectory> operations,
            ConnectionConfig config) noexcept;
        std::string address_;
        std::shared_ptr<const ServerIdentity> server_;
        ConnectionConfig config_;
        std::thread::id owner_;
        std::atomic<PendingConnect*> pending_{nullptr};
        PendingConnect* accepting_ = nullptr;
        std::vector<std::unique_ptr<ServerConnectionEndpoint>> endpoints_;
        bool stopped_ = false;
    };
}
