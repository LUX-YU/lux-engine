#pragma once

#include <lux/engine/platform/ipc/IpcMessage.hpp>

#include <array>
#include <memory>
#include <string>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/protocol/Protocol.hpp>

namespace lux::render
{
    using SessionIdentity = std::array<std::uint64_t, 2>;
    struct FeatureSelection final
    {
        std::string type;
        std::string instance;
        cxx::SchemaVersion version{1, 0};
        const DataSchemaDescriptor* config_schema = nullptr; // synchronous submit borrow
        OwnedWireBytes config;
    };

    // A narrow retained-owner factory. The complete vector capacity is charged,
    // even when only a small range is exposed. No caller-supplied byte discount.
    class RenderPayload final
    {
    public:
        [[nodiscard]] static RenderResult<RenderPayload> fromVector(
            std::shared_ptr<const std::vector<std::byte>> owner, std::size_t offset, std::size_t size
        ) noexcept;
        [[nodiscard]] const OwnedWireBytes& bytes() const noexcept { return bytes_; }
        [[nodiscard]] std::size_t retainedBytes() const noexcept { return retained_bytes_; }
    private:
        OwnedWireBytes bytes_;
        std::size_t retained_bytes_ = 0;
    };

    // Cold-bound immutable contract; the session belongs to the server, RequestId to this Client.
    class OperationBinding final
    {
    public:
        [[nodiscard]] SessionIdentity session() const noexcept { return session_; }
        [[nodiscard]] OperationId operation() const noexcept { return operation_; }
        [[nodiscard]] const OperationDescriptor& descriptor() const noexcept { return descriptor_; }
    private:
        friend class RenderClient;
        OperationBinding() noexcept = default;
        OperationBinding(SessionIdentity owner, OperationId id, OperationDescriptor descriptor) noexcept
            : session_(owner), operation_(id), descriptor_(descriptor) {}
        SessionIdentity session_{};
        OperationId operation_;
        OperationDescriptor descriptor_;
    };

    template <WireData Request, typename Reply = void>
    class BoundOperation final
    {
    public:
        [[nodiscard]] const OperationBinding& binding() const noexcept { return binding_; }
    private:
        friend class RenderClient;
        explicit BoundOperation(OperationBinding value) noexcept : binding_(value) {}
        OperationBinding binding_;
    };

    enum class ERenderReceive { RECEIVED, EMPTY, CLOSED };

    struct RenderCompletion final
    {
        ReplyHeader header;
        SessionIdentity session{};
        std::optional<DataSchemaDescriptor> schema; // metadata borrows the static bound schema fields
        RenderCompletion() noexcept = default;
        ~RenderCompletion() noexcept;
        RenderCompletion(RenderCompletion&& other) noexcept;
        RenderCompletion& operator=(RenderCompletion&& other) noexcept;
        RenderCompletion(const RenderCompletion&) = delete;
        RenderCompletion& operator=(const RenderCompletion&) = delete;
        [[nodiscard]] bool active() const noexcept { return !header.request.isNull(); }
        // Borrow only while this Completion remains alive. The value owns both reply and returned input.
        [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
        [[nodiscard]] std::span<const std::span<const std::byte>> attachments() const noexcept;
    private:
        friend class RenderClient;
        platform::ipc::IpcReply backing_;
        std::shared_ptr<void> budget_;
        void (*refund_)(void*, std::size_t) noexcept = nullptr;
        std::size_t charged_ = 0;
        std::size_t payload_size_ = 0;


    };

    struct RenderClientConfig final
    {
        std::size_t retained_byte_limit = 64 * 1024 * 1024;
        std::size_t ring_capacity = 64;
    };

    // One long-lived logical SPSC owner; the host creates at most Main + Upload
    // per server epoch. No worker-per-request connections. No native dependency.
    class RenderClient final
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<RenderClient>>
        create(std::string_view channel_name, RenderClientConfig config = {}) noexcept;
        ~RenderClient() noexcept;
        RenderClient(const RenderClient&) = delete;
        RenderClient& operator=(const RenderClient&) = delete;
        [[nodiscard]] RenderResult<RequestId> hello() noexcept;
        [[nodiscard]] SessionIdentity session() const noexcept;
        [[nodiscard]] RenderResult<OperationBinding> bind(const OperationDescriptor& descriptor) const noexcept;

        template <WireData Request, typename Reply = void>
        [[nodiscard]] RenderResult<BoundOperation<Request, Reply>>
        bind(const OperationDescriptor& descriptor) const noexcept
        {
            auto expected = operationDescriptor<Request, Reply>(descriptor.type.name(), descriptor.version,
                descriptor.completion);
            if (auto same = compatibleOperations(expected, descriptor); !same) return cxx::unexpected(same.error());
            auto value = bind(descriptor);
            if (!value) return cxx::unexpected(value.error());
            return BoundOperation<Request, Reply>{*value};
        }

        template <WireData Request, typename Reply>
        [[nodiscard]] RenderResult<RequestId> submit(const BoundOperation<Request, Reply>& operation,
            const Request& payload, std::span<const RenderPayload> attachments = {},
            std::size_t reply_capacity = 128) noexcept
        {
            auto size = DataSchema<Request>::wireSize(payload);
            if (!size) return cxx::unexpected(size.error());
            return submitEncoded(operation.binding_,  *size, &payload,
                [](const void* value, cxx::BinarySpanWriter& writer) noexcept {
                    return DataSchema<Request>::encode(*static_cast<const Request*>(value), writer);
                }, attachments, reply_capacity);
        }

        // output must be inactive. Processes at most eight abandoned records per call.
        [[nodiscard]] RenderResult<ERenderReceive> tryReceive(RenderCompletion& output) noexcept;
        [[nodiscard]] RenderResult<void> abandon(RequestId request) noexcept;
        [[nodiscard]] std::size_t retainedBytes() const noexcept;
    private:
        [[nodiscard]] RenderResult<RequestId> submitEncoded(const OperationBinding& operation, std::size_t size,
            const void* value, RenderResult<void> (*encode)(const void*, cxx::BinarySpanWriter&) noexcept,
            std::span<const RenderPayload> attachments, std::size_t reply_capacity) noexcept;
        struct Impl;
        explicit RenderClient(std::unique_ptr<Impl> state) noexcept;
        std::unique_ptr<Impl> impl_;
    };

    inline constexpr auto kRuntimeState = error::errorId("lux.render.runtime.state");
    inline constexpr auto kRuntimeCapacity = error::errorId("lux.render.runtime.capacity");
    inline constexpr auto kRuntimeIdentity = error::errorId("lux.render.runtime.identity");
    inline constexpr auto kRuntimeUnsupported = error::errorId("lux.render.runtime.unsupported");
    [[nodiscard]] std::span<const error::ErrorDescriptor> renderRuntimeErrorDescriptors() noexcept;
}
