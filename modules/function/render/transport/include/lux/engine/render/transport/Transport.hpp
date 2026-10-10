#pragma once

#include <lux/engine/render/transport/Packet.hpp>

namespace lux::render
{
    struct RenderTransportCapacity final
    {
        RenderPacketCapacity packet;
        std::uint32_t pending_programs{2};
        std::uint32_t pending_controls{2};
        std::uint32_t pending_uploads{64};
        std::uint64_t upload_bytes{256 * 1024 * 1024};
        std::uint32_t replies{256};
    };

    // Owns transport mechanisms, not threads or rendering. The host must join
    // callers before destroying this object. Packets, inboxes and deferred reply
    // promises independently retain their value storage after transport death.
    class RenderTransport final
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<RenderTransport>>
        create(RenderRouteTable&& routes, RenderTransportCapacity capacity = {}) noexcept;
        ~RenderTransport();
        RenderTransport(const RenderTransport&) = delete;
        RenderTransport& operator=(const RenderTransport&) = delete;

        template <PacketValue T>
        [[nodiscard]] RenderResult<BoundRenderRoute<T>> bind() const noexcept { return routes_->bind<T>(); }

        [[nodiscard]] RenderProgramPacket makeProgramPacket() const noexcept
        {
            return RenderProgramPacket{makeStorage()};
        }

        [[nodiscard]] RenderControlPacket makeControlPacket() const noexcept
        {
            return RenderControlPacket{makeStorage()};
        }

        [[nodiscard]] RenderUploadPacket makeUploadPacket() const noexcept { return RenderUploadPacket{makeStorage()}; }

        [[nodiscard]] RenderReplyInbox replyInbox() const noexcept { return RenderReplyInbox{replies_}; }

        [[nodiscard]] RenderResult<void> submit(RenderProgramPacket& packet) noexcept;
        [[nodiscard]] RenderResult<void> submit(RenderControlPacket& packet) noexcept;
        [[nodiscard]] RenderResult<void> submit(RenderUploadPacket& packet) noexcept;

        template <typename F> requires std::is_nothrow_invocable_v<F&, RenderPacketView&>
        [[nodiscard]] RenderResult<bool> pollProgram(F&& handler) noexcept
        {
            return pollProgramErased(&handler, [](void* context, RenderPacketView& packet) noexcept {
                (*static_cast<std::remove_reference_t<F>*>(context))(packet);
            });
        }

        template <typename F> requires std::is_nothrow_invocable_v<F&, RenderPacketView&>
        [[nodiscard]] RenderResult<bool> pollControl(F&& handler) noexcept
        {
            return pollControlErased(&handler, [](void* context, RenderPacketView& packet) noexcept {
                (*static_cast<std::remove_reference_t<F>*>(context))(packet);
            });
        }

        // Each concurrent upload consumer owns a distinct, empty scratch packet.
        template <typename F> requires std::is_nothrow_invocable_v<F&, RenderPacketView&>
        [[nodiscard]] RenderResult<bool> pollUpload(RenderUploadPacket& scratch, F&& handler) noexcept
        {
            return pollUploadErased(scratch.storage_, &handler, [](void* context, RenderPacketView& packet) noexcept {
                (*static_cast<std::remove_reference_t<F>*>(context))(packet);
            });
        }

        // Non-blocking close. Already acquired admissions may finish publishing;
        // consumers drain/cancel them. Completion vs stop has one atomic winner.
        void requestStop() noexcept;
        [[nodiscard]] bool stopping() const noexcept;
        [[nodiscard]] bool drained() const noexcept;
        [[nodiscard]] std::uint64_t uploadBytes() const noexcept;
        [[nodiscard]] const TransportWake& wake() const noexcept { return *wake_; }

    private:
        struct Impl;
        using Dispatch = void (*)(void*, RenderPacketView&) noexcept;
        RenderTransport(RenderRouteTable&& routes, RenderTransportCapacity capacity) noexcept;
        [[nodiscard]] detail::PacketStorage makeStorage() const noexcept;
        [[nodiscard]] RenderResult<bool> pollProgramErased(void* context, Dispatch dispatch) noexcept;
        [[nodiscard]] RenderResult<bool> pollControlErased(void* context, Dispatch dispatch) noexcept;
        [[nodiscard]] RenderResult<bool>
        pollUploadErased(detail::PacketStorage& scratch, void* context, Dispatch dispatch) noexcept;
        [[nodiscard]] RenderResult<void> checkOwner(const detail::PacketStorage& storage) const noexcept;

        std::shared_ptr<const RenderRouteTable> routes_;
        std::shared_ptr<TransportWake> wake_;
        std::shared_ptr<detail::ReplyArena> replies_;
        RenderTransportCapacity capacity_;
        std::unique_ptr<Impl> impl_;
    };
}
