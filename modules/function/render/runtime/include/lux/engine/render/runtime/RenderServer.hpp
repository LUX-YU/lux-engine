#pragma once

#include <lux/engine/render/runtime/Operations.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/features/Feature.hpp>
#include <lux/engine/render/native/vulkan/Program.hpp>
#include <lux/engine/render/native/vulkan/transfer/UploadOptions.hpp>

namespace lux::render
{
    namespace vulkan { class DeviceSession; class SubmissionTicket; class SubmissionQueue; class TextureUpload; }
    namespace runtime_detail { struct DeferredCompletion; }
    class OperationContext;

    struct RenderServerConfig final
    {
        std::string channel_name;
        bool validation = false;
        std::uint32_t physical_device_index = UINT32_MAX;
        std::uint32_t requests_per_tick = 8;
        std::size_t max_wire_bytes = 1024 * 1024;
        std::size_t max_reply_bytes = 16 * 1024 * 1024;
        std::size_t retirement_capacity = 64;
        // Host-owned bounded cold executor. enqueue never executes inline; accepted callbacks run exactly once.
        // Rejection executes nothing and retains no pointer.
        // join_compile waits for every callback accepted through this server's borrowed execution resource.
        // Both callbacks are null for a control-only host; CreateEffect then rejects before retaining its input.
        void* compile_user = nullptr;
        bool (*enqueue_compile)(void*, void (*)(void*) noexcept, void*) noexcept = nullptr;
        void (*join_compile)(void*) noexcept = nullptr;
        std::size_t compile_capacity = 2;
        std::size_t completion_capacity = 128;
        // Cold explicit Native capability request. Aliased roles still share one physical queue owner.
        bool transfer_queues = false;
        bool prefer_dedicated_transfer = true;
        vulkan::TextureUploadOptions uploads{}; // null host callbacks disable texture upload admission
        std::size_t texture_capacity = 128;
        std::uint64_t texture_bytes = 256 * 1024 * 1024;
    };

    // One source-qualified request. Move only for real deferred work; settle before Responder teardown.
    class RenderRequest final
    {
    public:
        RenderRequest(RenderRequest&&) noexcept = default;
        RenderRequest& operator=(RenderRequest&&) noexcept = default;
        RenderRequest(const RenderRequest&) = delete;
        RenderRequest& operator=(const RenderRequest&) = delete;
        [[nodiscard]] bool active() const noexcept { return input_.active(); }
        [[nodiscard]] RequestId id() const noexcept { return request_; }
        [[nodiscard]] BorrowedWireView payload() const noexcept;
        [[nodiscard]] const platform::ipc::IpcMessage& message() const noexcept { return input_.message(); }
        [[nodiscard]] std::size_t replyCapacity() const noexcept { return reply_capacity_; }
        [[nodiscard]] RenderResult<void>
        reply(std::span<const std::byte> payload = {}, OwnedWireBytes attachment = {}) noexcept;
        [[nodiscard]] RenderResult<void> fail(RenderError error) noexcept;
        [[nodiscard]] RenderResult<void> cancel() noexcept;

        template <BorrowedWireData T>
        [[nodiscard]] RenderResult<T> decode() const noexcept
        {
            // Handler contract: T is its registered static payload type. Cold registration proves metadata;
            // dispatch validates untrusted values/ranges once before entering the handler.
            const auto wire = payload();
            cxx::BinaryReader reader(wire.bytes.first(static_cast<std::size_t>(wire.schema.fixed_size)), wire.bytes.size());
            auto value = DataSchema<T>::decode(reader, wire);
            if (!value) return cxx::unexpected(value.error());
            if (!reader.requireFullyConsumed()) return cxx::unexpected(RenderError{kInvalidWire});
            return value;
        }

        template <WireData T>
        [[nodiscard]] RenderResult<void> reply(const T& value, OwnedWireBytes attachment = {}) noexcept
        {
            std::array<std::byte, 128> bytes;
            auto size = DataSchema<T>::wireSize(value);
            if (!size) return cxx::unexpected(size.error());
            if (*size > bytes.size()) return cxx::unexpected(RenderError{kWireLimit});
            cxx::BinarySpanWriter writer(std::span(bytes).first(*size));
            if (auto encoded = DataSchema<T>::encode(value, writer); !encoded) return encoded;
            if (!writer.good() || writer.size() != *size) return cxx::unexpected(RenderError{kInvalidWire});
            return reply(std::span<const std::byte>(bytes).first(*size), std::move(attachment));
        }
    private:
        friend class RenderServer;
        friend class OperationContext;
        friend class GpuCompletionReservation;
        [[nodiscard]] RenderResult<void>
        validateReply(std::span<const std::byte> payload, const OwnedWireBytes& attachment) const noexcept;
        RenderRequest(platform::ipc::ReceivedRequest input, platform::ipc::IpcResponder& responder,
            SessionIdentity session, RequestId request, std::size_t capacity, const OperationDescriptor* descriptor) noexcept;
        [[nodiscard]] RenderResult<void> publish(EOperationOutcome outcome, RenderError error,
            std::span<const std::byte> payload, OwnedWireBytes attachment) noexcept;
        platform::ipc::ReceivedRequest input_;
        std::unique_ptr<platform::ipc::IpcMessage> unreplied_; // allocated only on failed publication, cold disposal
        platform::ipc::IpcResponder* responder_;
        SessionIdentity session_;
        RequestId request_;
        std::size_t reply_capacity_;
        const OperationDescriptor* descriptor_;
    };

    // A synchronous Handler-only right to one already reserved Server slot. No request is moved until commit.
    class GpuCompletionReservation final
    {
    public:
        ~GpuCompletionReservation() noexcept;
        GpuCompletionReservation(GpuCompletionReservation&& other) noexcept;
        GpuCompletionReservation& operator=(GpuCompletionReservation&&) = delete;
        GpuCompletionReservation(const GpuCompletionReservation&) = delete;
        GpuCompletionReservation& operator=(const GpuCompletionReservation&) = delete;
        // Same reserved byte extent, new values prepared before submit; no growth after admission.
        [[nodiscard]] RenderResult<void> setReply(std::span<const std::byte>) noexcept;
        [[nodiscard]] RenderResult<void> commitUpload(std::unique_ptr<vulkan::TextureUpload>&) noexcept;

        // Ready runs ONCE after the real Ticket completes, before SUCCESS: certify output and commit business state.
        // Owner pins staging/backing/code. It is destroyed only after completion or cold GPU drain on shutdown.
        // Invalid Ticket/qualification preserves owner and request; a valid commit cannot fail capacity admission.
        template <auto Ready, class Owner>
        [[nodiscard]] RenderResult<void>
        commit(const vulkan::SubmissionTicket& ticket, std::unique_ptr<Owner>& owner) noexcept
        {
            static_assert(std::is_nothrow_destructible_v<Owner>);
            if (!owner) return cxx::unexpected(RenderError{kRuntimeState});
            auto result = commitOwned(ticket, owner.get(), [](void* p) noexcept { delete static_cast<Owner*>(p); },
                [](void* p, vulkan::DeviceSession& session) noexcept -> RenderResult<void> {
                    return Ready(*static_cast<Owner*>(p), session);
                });
            if (result) (void)owner.release();
            return result;
        }

        // A multi-chunk Native candidate enters the SAME reserved slot before its first prepare/submit.
        // Only TextureUpload's completed, queue-proven state can reach Ready and publish SUCCESS.
        template <auto Ready, class Owner>
        [[nodiscard]] RenderResult<void>
        commitUpload(std::unique_ptr<vulkan::TextureUpload>& upload, std::unique_ptr<Owner>& owner) noexcept
        {
            static_assert(std::is_nothrow_destructible_v<Owner>);
            if (!owner) return cxx::unexpected(RenderError{kRuntimeState});
            auto result = commitUploadOwned(upload, owner.get(), [](void* p) noexcept { delete static_cast<Owner*>(p); },
                [](void* p, vulkan::TextureUpload& value, vulkan::DeviceSession& session) noexcept -> RenderResult<void> {
                    return Ready(*static_cast<Owner*>(p), value, session);
                });
            if (result) (void)owner.release();
            return result;
        }

    private:
        friend class OperationContext;
        GpuCompletionReservation(vulkan::DeviceSession& native, runtime_detail::DeferredCompletion& slot,
            RenderRequest& request) noexcept : native_(&native), slot_(&slot), request_(&request) {}
        [[nodiscard]] RenderResult<void> commitOwned(
            const vulkan::SubmissionTicket&,
            void*,
            void (*destroy)(void*) noexcept,
            RenderResult<void> (*ready)(void*, vulkan::DeviceSession&) noexcept
        ) noexcept;
        [[nodiscard]] RenderResult<void> commitUploadOwned(
            std::unique_ptr<vulkan::TextureUpload>&,
            void*,
            void (*destroy)(void*) noexcept,
            RenderResult<void> (*ready)(void*, vulkan::TextureUpload&, vulkan::DeviceSession&) noexcept
        ) noexcept;
        vulkan::DeviceSession* native_;
        runtime_detail::DeferredCompletion* slot_;
        RenderRequest* request_;
    };

    // Borrow valid only during this Server Handler invocation; it cannot escape to a worker.
    class OperationContext final
    {
    public:
        OperationContext(const OperationContext&) = delete;
        OperationContext& operator=(const OperationContext&) = delete;
        [[nodiscard]] vulkan::DeviceSession& native() const noexcept { return *native_; }
        // Bounded reply bytes and retained attachment are funded before submit.
        [[nodiscard]] RenderResult<GpuCompletionReservation> reserveGpuCompletion(
            std::span<const std::byte> reply = {}, OwnedWireBytes attachment = {}) noexcept;
        [[nodiscard]] RenderResult<GpuCompletionReservation> reserveGpuCompletion(
            vulkan::SubmissionQueue& queue, std::span<const std::byte> reply = {},
            OwnedWireBytes attachment = {}) noexcept;
    private:
        friend class RenderServer;
        OperationContext(vulkan::DeviceSession& native, std::span<runtime_detail::DeferredCompletion> slots,
            RenderRequest& request) noexcept : native_(&native), slots_(slots), request_(&request) {}
        vulkan::DeviceSession* native_;
        std::span<runtime_detail::DeferredCompletion> slots_;
        RenderRequest* request_;
        bool reserved_ = false; // at most one qualification issued by this invocation
    };

    struct OperationHandler final
    {
        const OperationDescriptor* descriptor;
        RenderResult<void> (*invoke)(void*, RenderRequest&, OperationContext&) noexcept;
        void* user = nullptr; // static/cold owner must outlive run and every deferred request
        bool gpu_completion = false; // explicit registration opt-in; ordinary reply still cannot assert GPU readiness
    };

    struct FeatureRegistration final
    {
        const FeatureDefinition* definition;
        RenderResult<std::vector<vulkan::PassProgram>> (*programs)(const WireView*, std::string_view) noexcept;
        std::span<const BufferInitializer> buffers;
        std::span<const OperationHandler> handlers;
        // Immutable metadata/code remain loaded through run()/shutdown.
    };

    struct RenderServerStats final
    {
        std::uint64_t ticks = 0, commands = 0, compiled_graphs = 0, submissions = 0;
        std::uint64_t validation_errors = 0, validation_warnings = 0;
        std::uint64_t scoped_buffers_created = 0;
    };

    // The host owns the rendering thread. Create/register are cold; run owns all
    // object and Native mutation. Only stop()/stats() may race with run().
    class RenderServer final
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<RenderServer>> create(RenderServerConfig config) noexcept;
        ~RenderServer() noexcept;
        RenderServer(const RenderServer&) = delete;
        RenderServer& operator=(const RenderServer&) = delete;
        [[nodiscard]] RenderResult<void> registerFeature(FeatureRegistration feature) noexcept;
        [[nodiscard]] RenderResult<void> run() noexcept;
        void stop() noexcept;
        [[nodiscard]] RenderServerStats stats() const noexcept;
    private:
        struct Impl;
        explicit RenderServer(std::unique_ptr<Impl> state) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}
