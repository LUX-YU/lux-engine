#pragma once

#include <chrono>
#include <stop_token>
#include <lux/engine/render/transport/Routes.hpp>
#include <lux/engine/render/transport/Replies.hpp>

namespace lux::render
{
    struct TransportCapacity final
    {
        std::uint32_t records = 64;
        std::uint32_t attachments = 8;
        std::uint32_t pending_commands = 2;
        std::uint32_t pending_priority = 2;
        std::uint32_t pending_bulk = 64;
        std::uint32_t replies = 256;
        std::size_t command_bytes = 64 * 1024;
        std::size_t bulk_bytes = 256 * 1024 * 1024;
        std::size_t reply_bytes = 64 * 1024;
    };

    class CommandBatch final
    {
    public:
        CommandBatch(CommandBatch&&) noexcept;
        CommandBatch& operator=(CommandBatch&&) noexcept;
        CommandBatch(const CommandBatch&) = delete;
        CommandBatch& operator=(const CommandBatch&) = delete;
        [[nodiscard]] std::size_t size() const noexcept { return size_; }
        void clear() noexcept;

        template <WireData Payload, typename Reply>
        [[nodiscard]] RenderResult<void> append(
            const BoundOperation<Payload, Reply>& operation,
            OwnedWireBytes bytes,
            std::span<const OwnedWireBytes> attachments = {}
        ) noexcept
        {
            auto valid = validateWire(
                {DataSchema<Payload>::describe(), bytes, attachments}, {byte_limit_, byte_limit_}
            );
            if (!valid) return valid;
            return appendErased(operation.binding_, std::move(bytes), attachments);
        }

    private:
        friend class TransportSender;
        struct Record final
        {
            detail::Binding binding;
            OwnedWireBytes bytes;
            std::vector<OwnedWireBytes> attachments;
            RequestId reserved;
        };
        explicit CommandBatch(TransportCapacity capacity) noexcept;
        [[nodiscard]] RenderResult<void>
        appendErased(
            detail::Binding binding, OwnedWireBytes bytes, std::span<const OwnedWireBytes> attachments
        ) noexcept;
        std::vector<Record> records_;
        std::size_t size_ = 0;
        std::size_t byte_limit_ = 0;
        std::uint32_t attachment_limit_ = 0;
    };

    // Reusable output avoids allocating a ticket vector on each submit.
    class BatchRequest final
    {
    public:
        [[nodiscard]] std::span<RenderRequest> requests() noexcept { return requests_; }

        void clear() noexcept { requests_.clear(); }

    private:
        friend class TransportSender;
        explicit BatchRequest(std::size_t capacity) noexcept { requests_.reserve(capacity); }
        std::vector<RenderRequest> requests_;
    };

    class TransportSender final
    {
    public:
        template <WireData Payload, typename Reply = void>
        [[nodiscard]] RenderResult<BoundOperation<Payload, Reply>>
        bind(const OperationDescriptor& descriptor) const noexcept
        {
            auto typed = operationDescriptor<Payload, Reply>(
                descriptor.type.name(), descriptor.version, descriptor.completion
            );
            auto binding = bindErased(descriptor, typed);
            if (!binding) return cxx::unexpected(binding.error());
            return BoundOperation<Payload, Reply>(std::move(*binding));
        }

        [[nodiscard]] CommandBatch makeBatch() const noexcept;
        [[nodiscard]] BatchRequest makeRequests() const noexcept;
        [[nodiscard]] ReplyInbox inbox() const noexcept { return ReplyInbox(state_); }
        [[nodiscard]] RenderResult<void>
        submit(CommandBatch& batch, BatchRequest& output, EDelivery delivery) const noexcept;
        [[nodiscard]] bool stopping() const noexcept;
        [[nodiscard]] std::size_t bulkBytes() const noexcept;
        [[nodiscard]] std::uint64_t wakeEpoch() const noexcept;
        // Snapshot before checking work. Spurious wakes are allowed; every wait
        // also exits on endpoint stop, caller stop or deadline.
        [[nodiscard]] bool waitUntil(
            std::uint64_t observed,
            std::chrono::steady_clock::time_point deadline,
            std::stop_token stop = {}
        ) const noexcept;

    private:
        friend class RenderTransport;
        explicit TransportSender(std::shared_ptr<detail::TransportState> state) noexcept : state_(std::move(state)) {}
        [[nodiscard]] RenderResult<detail::Binding>
        bindErased(const OperationDescriptor& descriptor, const OperationDescriptor& typed) const noexcept;
        std::shared_ptr<detail::TransportState> state_;
    };

    class RenderTransport final
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<RenderTransport>>
        create(RenderRouteTable&& routes, TransportCapacity capacity = {}) noexcept;
        ~RenderTransport();
        RenderTransport(const RenderTransport&) = delete;
        RenderTransport& operator=(const RenderTransport&) = delete;
        [[nodiscard]] TransportSender sender() const noexcept { return TransportSender(state_); }
        [[nodiscard]] RenderResult<bool> poll(EDelivery delivery) noexcept;
        void requestStop() noexcept;
        [[nodiscard]] bool drained() const noexcept;

    private:
        explicit RenderTransport(std::shared_ptr<detail::TransportState> state) noexcept : state_(std::move(state)) {}
        std::shared_ptr<detail::TransportState> state_;
    };
}
