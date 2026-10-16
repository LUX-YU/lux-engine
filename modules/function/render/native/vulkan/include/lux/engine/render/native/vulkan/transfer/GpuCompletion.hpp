#pragma once

#include <lux/engine/render/protocol/Protocol.hpp>
#include <lux/engine/render/native/vulkan/transfer/GpuWork.hpp>

namespace lux::render::protocol_detail
{
    // Installed because the Native extension template below has external consumers.
    struct ReplyAccess final
    {
        template <RenderCommand C>
        static CommandInvocation& invocation(Reply<C>& context) noexcept { return *context.invocation_; }
    };
}

namespace lux::render::vulkan
{
    struct GpuCompletionFns;

    // One already-funded Server completion slot. The source request remains with its
    // original owner until commit; the reservation owns neither the Device nor a request.
    class GpuCompletionReservation final
    {
    public:
        [[nodiscard]] static RenderResult<GpuCompletionReservation>
        reserve(CommandInvocation& invocation, SubmissionQueue& queue, void* response) noexcept;

        template<class C>
        [[nodiscard]] static RenderResult<GpuCompletionReservation>
        reserve(CommandInvocation& invocation, SubmissionQueue& queue, typename C::Response& response) noexcept
        {
            return reserve(invocation, queue, static_cast<void*>(&response));
        }

        template <RenderCommand C>
        [[nodiscard]] static RenderResult<GpuCompletionReservation>
        reserve(Reply<C>& context, SubmissionQueue& queue, typename C::Response& response) noexcept
        {
            return reserve(protocol_detail::ReplyAccess::invocation(context), queue, &response);
        }

        ~GpuCompletionReservation() noexcept;
        GpuCompletionReservation(GpuCompletionReservation&& other) noexcept;
        GpuCompletionReservation& operator=(GpuCompletionReservation&&) = delete;
        GpuCompletionReservation(const GpuCompletionReservation&) = delete;
        GpuCompletionReservation& operator=(const GpuCompletionReservation&) = delete;

        [[nodiscard]] RenderResult<void> setReply(void* response) noexcept;

        template<class C>
        [[nodiscard]] RenderResult<void> setReply(typename C::Response& response) noexcept
        {
            return setReply(static_cast<void*>(&response));
        }

        // Transfer callback admission is prepared before moving either work or the original request.
        // Failure retains both; success starts the one Native Transfer with no further quota admission.
        [[nodiscard]] RenderResult<void> commitWork(GpuWork& work) noexcept;

        // The accepted Ticket proves new work on the reserved physical Queue. Failure
        // preserves the caller's owner; successful commit cannot fail capacity admission.
        template<auto Ready, class Owner>
        [[nodiscard]] RenderResult<void>
        commit(const SubmissionTicket& ticket, std::unique_ptr<Owner>& owner) noexcept
        {
            static_assert(std::is_nothrow_destructible_v<Owner>);
            if (!owner) return cxx::unexpected(RenderError{kProtocolContract});
            auto result = commitOwned(ticket, owner.get(), [](void* value) noexcept {
                delete static_cast<Owner*>(value);
            }, [](void* value, DeviceSession& session) noexcept -> RenderResult<void> {
                return Ready(*static_cast<Owner*>(value), session);
            });
            if (result) (void)owner.release();
            return result;
        }

    private:
        GpuCompletionReservation(void* slot, const GpuCompletionFns& functions) noexcept
            : slot_(slot), functions_(&functions) {}

        [[nodiscard]] RenderResult<void> commitOwned(
            const SubmissionTicket& ticket,
            void* owner,
            void (*destroy)(void*) noexcept,
            RenderResult<void> (*ready)(void*, DeviceSession&) noexcept
        ) noexcept;

        void* slot_;
        const GpuCompletionFns* functions_;
    };
}
