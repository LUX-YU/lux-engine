#pragma once

#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/native/vulkan/transfer/GpuWork.hpp>

namespace lux::render
{
    // A synchronous Handler-only right to one already reserved Server slot. No request is moved until commit.
    class GpuCompletionReservation final
    {
    public:
        // Backend-only borrow and physical-queue admission. Ordinary handlers use OperationContext::resources().
        [[nodiscard]] static vulkan::DeviceSession& device(OperationContext& context) noexcept
        {
            return *context.native_;
        }
        [[nodiscard]] static RenderResult<GpuCompletionReservation> reserve(
            OperationContext& context, vulkan::SubmissionQueue& queue,
            std::span<const std::byte> reply = {}, OwnedWireBytes attachment = {}
        ) noexcept
        {
            return context.reserveGpuCompletion(queue, reply, std::move(attachment));
        }

        ~GpuCompletionReservation() noexcept;
        GpuCompletionReservation(GpuCompletionReservation&& other) noexcept;
        GpuCompletionReservation& operator=(GpuCompletionReservation&&) = delete;
        GpuCompletionReservation(const GpuCompletionReservation&) = delete;
        GpuCompletionReservation& operator=(const GpuCompletionReservation&) = delete;
        // Same reserved byte extent, new values prepared before submit; no growth after admission.
        [[nodiscard]] RenderResult<void> setReply(std::span<const std::byte>) noexcept;
        [[nodiscard]] RenderResult<void> commitWork(vulkan::GpuWork&) noexcept;

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
        vulkan::DeviceSession* native_;
        runtime_detail::DeferredCompletion* slot_;
        RenderRequest* request_;
    };

}
