#pragma once

#include <lux/engine/render/graph/Definition.hpp>
#include <lux/engine/render/runtime/RuntimeMessages.ops.hpp>
#include <lux/engine/render/runtime/Types.hpp>
#include <lux/engine/render/transport/Transport.hpp>

namespace lux::render
{
    class RenderRuntimeBuilder;

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
        std::uint32_t diagnostics{64};
    };

    // One reception obligation, no per-request heap state. Destruction abandons
    // unread completion capacity. The R2 arena remains safe after Runtime death.
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

    class RenderRuntime
    {
    public:
        ~RenderRuntime() noexcept;
        RenderRuntime(const RenderRuntime&) = delete;
        RenderRuntime& operator=(const RenderRuntime&) = delete;

        // Owner-thread PROGRAM. A capacity rejection retains the immutable
        // candidate; retryPublication() admits that exact candidate before a
        // later publication can be accepted. Simulation never waits for a frame.
        [[nodiscard]] RenderResult<void> publishScenePass(
            RuntimeScene scene,
            RuntimePath path,
            GraphPassId pass,
            std::uint64_t revision,
            std::span<const std::byte> scalars
        ) noexcept;
        [[nodiscard]] RenderResult<void> publishViewPass(
            RuntimeView view,
            GraphPassId pass,
            std::uint64_t revision,
            const std::array<float, 16>& camera,
            std::span<const std::byte> scalars
        ) noexcept;
        [[nodiscard]] RenderResult<void> retryPublication() noexcept;

        [[nodiscard]] RenderResult<RuntimeRequest> resize(
            RuntimeTarget target,
            std::uint32_t width,
            std::uint32_t height
        ) noexcept;
        [[nodiscard]] RenderResult<RuntimeRequest> setActive(RuntimeView view, bool active) noexcept;
        [[nodiscard]] RenderResult<RuntimeRequest> remove(RuntimeView view) noexcept;
        [[nodiscard]] RenderResult<RuntimeRequest> remove(RuntimeScene scene) noexcept;
        [[nodiscard]] RenderResult<RuntimeRequest> remove(RuntimeTarget target) noexcept;
        [[nodiscard]] RenderResult<RuntimeRequest> setPath(RuntimeView view, RuntimePath path) noexcept;
        // Explicit cold diagnostic request. Completion returns at most 224 owned bytes.
        [[nodiscard]] RenderResult<RuntimeRequest> readback(
            RuntimeView view,
            GraphPassId pass,
            std::uint32_t input,
            std::uint64_t offset,
            std::uint32_t bytes
        ) noexcept;
        [[nodiscard]] RenderResult<RuntimeRequest> readbackTarget(
            RuntimeTarget target,
            std::uint64_t offset,
            std::uint32_t bytes
        ) noexcept;

        void requestStop() noexcept;
        [[nodiscard]] ERuntimeState state() const noexcept;
        [[nodiscard]] RenderResult<bool> waitStopped(std::uint32_t timeout_ms) noexcept;
        [[nodiscard]] RuntimeProgress progress() const noexcept;
        [[nodiscard]] std::optional<RenderError> terminalError() const noexcept;
        [[nodiscard]] std::optional<RenderError> pollDiagnostic() noexcept;
        [[nodiscard]] std::uint64_t droppedDiagnostics() const noexcept;

    private:
        friend class RenderRuntimeBuilder;
        struct Impl;
        explicit RenderRuntime(std::unique_ptr<Impl> implementation) noexcept;
        [[nodiscard]] RenderResult<RuntimeRequest> control(RuntimeControl request) noexcept;
        [[nodiscard]] RenderResult<RuntimeRequest> readbackRequest(RuntimeReadback request) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::render
