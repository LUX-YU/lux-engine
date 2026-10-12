#pragma once

#include <lux/engine/render/runtime/RuntimeMessages.ops.hpp>
#include <lux/engine/render/runtime/Types.hpp>
#include <lux/engine/render/transport/Transport.hpp>

namespace lux::render
{
    namespace detail
    {
        struct RuntimeChannel;
    }
    class RenderBackend;

    enum class ERuntimeState
    {
        RUNNING,
        STOPPING,
        STOPPED,
        FAILED
    };

    struct RuntimeOptions
    {
        RenderTransportCapacity transport;
        std::uint64_t frame_interval_ns{16'666'667};
    };

    class RuntimeRequest
    {
    public:
        ~RuntimeRequest() noexcept;
        RuntimeRequest(RuntimeRequest&& other) noexcept;
        RuntimeRequest& operator=(RuntimeRequest&& other) noexcept;
        RuntimeRequest(const RuntimeRequest&) = delete;
        RuntimeRequest& operator=(const RuntimeRequest&) = delete;
        [[nodiscard]] RenderResult<std::optional<RuntimeReply>> poll() noexcept;
        [[nodiscard]] RenderResult<void> abandon() noexcept;

    private:
        friend class RenderRuntime;
        RuntimeRequest(RenderReplyInbox inbox, RenderReplyTicket<RuntimeReply> ticket) noexcept;
        RenderReplyInbox inbox_;
        std::optional<RenderReplyTicket<RuntimeReply>> ticket_;
    };

    // Client owns only protocol packets and an endpoint lease, never Native state or Server.
    class RenderRuntime
    {
    public:
        ~RenderRuntime() noexcept;
        RenderRuntime(const RenderRuntime&) = delete;
        RenderRuntime& operator=(const RenderRuntime&) = delete;
        [[nodiscard]] RenderResult<RuntimeRequest> createScene() noexcept;
        [[nodiscard]] RenderResult<RuntimeRequest> createTarget(std::uint32_t width, std::uint32_t height) noexcept;
        [[nodiscard]] RenderResult<RuntimeRequest> createView(
            RuntimeScene scene,
            RuntimeTarget target,
            RuntimeEffect effect
        ) noexcept;
        [[nodiscard]] RenderResult<RuntimeRequest> remove(RuntimeView view) noexcept;
        [[nodiscard]] RenderResult<RuntimeRequest> remove(RuntimeScene scene) noexcept;
        [[nodiscard]] RenderResult<RuntimeRequest> remove(RuntimeTarget target) noexcept;
        [[nodiscard]] RenderResult<RuntimeRequest> remove(RuntimeEffect effect) noexcept;
        [[nodiscard]] RenderResult<RuntimeRequest> graph(RuntimeView view) noexcept;
        [[nodiscard]] RenderResult<RuntimeRequest> setActive(RuntimeView view, bool active) noexcept;
        [[nodiscard]] RenderResult<RuntimeRequest> readback(
            RuntimeView view,
            GraphPassId pass,
            std::uint32_t input,
            std::uint64_t offset,
            std::uint32_t bytes
        ) noexcept;
        [[nodiscard]] RenderResult<void> publishViewPass(
            RuntimeView view,
            GraphPassId pass,
            std::uint64_t revision,
            std::span<const std::byte> scalars
        ) noexcept;
        [[nodiscard]] RenderResult<RuntimeRequest> readbackTarget(
            RuntimeTarget target,
            std::uint64_t offset,
            std::uint32_t bytes
        ) noexcept;
        [[nodiscard]] RenderResult<void> retryPublication() noexcept;
        void requestStop() noexcept;
        [[nodiscard]] ERuntimeState state() const noexcept;
        [[nodiscard]] RuntimeProgress progress() const noexcept;
        [[nodiscard]] std::optional<RenderError> terminalError() const noexcept;
        [[nodiscard]] std::optional<RenderError> pollDiagnostic() noexcept;
        [[nodiscard]] std::uint64_t ownerIdentity() const noexcept;

    private:
        friend class RenderBackend;
        explicit RenderRuntime(std::shared_ptr<detail::RuntimeChannel> channel) noexcept;
        [[nodiscard]] RenderResult<RuntimeRequest> control(RuntimeControl request) noexcept;
        std::shared_ptr<detail::RuntimeChannel> channel_;
        RenderProgramPacket program_;
        RenderControlPacket controls_;
        bool retained_program_{};
        std::uint64_t owner_thread_;
    };
} // namespace lux::render
