#pragma once

#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/transfer/Transfer.hpp>
#include <utility>

namespace lux::render::vulkan
{
    // One Native Transfer and its professional publication candidate. There is no erased GPU driver:
    // all pending work uses the same concrete Transfer state machine and its real Queue callbacks.
    class GpuWork final
    {
    public:
        GpuWork() noexcept = default;
        ~GpuWork() noexcept { reset(); }
        GpuWork(GpuWork&&) noexcept = default;
        GpuWork& operator=(GpuWork&& other) noexcept
        {
            if (this == &other) return *this;
            reset();
            session_ = other.session_;
            transfer_ = std::move(other.transfer_);
            owner_ = std::move(other.owner_);
            ready_ = std::exchange(other.ready_, nullptr);
            accepted_ready_ = std::exchange(other.accepted_ready_, nullptr);
            accepted_ = std::exchange(other.accepted_, std::nullopt);
            return *this;
        }
        GpuWork(const GpuWork&) = delete;
        GpuWork& operator=(const GpuWork&) = delete;

        template <auto Ready, class T>
        [[nodiscard]] static RenderResult<GpuWork> pending(
            DeviceSession& session, std::unique_ptr<Transfer>& transfer, std::unique_ptr<T>& candidate
        ) noexcept
        {
            if (!transfer || !candidate || !transfer->unstartedIn(session))
                return cxx::unexpected(RenderError{kWrongOwner});
            GpuWork result;
            result.session_ = &session;
            result.transfer_ = std::move(transfer);
            result.owner_ = {candidate.release(), [](void* value) noexcept { delete static_cast<T*>(value); }};
            result.ready_ = [](void* value, Transfer& work, DeviceSession& device) noexcept {
                return Ready(*static_cast<T*>(value), work, device);
            };
            return result;
        }

        [[nodiscard]] static GpuWork accepted(DeviceSession& session, SubmissionTicket ticket,
            void* owner, void (*destroy)(void*) noexcept,
            RenderResult<void> (*ready)(void*, DeviceSession&) noexcept) noexcept;
        [[nodiscard]] bool unstartedIn(const DeviceSession& session) const noexcept
        {
            return owner_ && transfer_ && session_ == &session && transfer_->unstartedIn(session);
        }
        [[nodiscard]] RenderResult<void> prepareCompletion() noexcept;
        void start(CompletionPost post, void* owner, Transfer::TerminalFn terminal,
            std::shared_ptr<const void> implementation, std::shared_ptr<const void> registration) noexcept;
        [[nodiscard]] RenderResult<void> publish() noexcept;
        void cancel() noexcept { if (transfer_) transfer_->cancel(); }
        void reset() noexcept
        {
            // Candidate rollback destroys dependent views/samplers before Transfer releases its image.
            // Transfer teardown borrows only DeviceSession/pages/Permits, never the candidate Storage.
            owner_.reset();
            transfer_.reset();
            ready_ = nullptr;
            accepted_ready_ = nullptr;
            accepted_.reset();
        }

    private:
        DeviceSession* session_ = nullptr;
        std::unique_ptr<Transfer> transfer_;
        std::unique_ptr<void, void (*)(void*) noexcept> owner_{nullptr, [](void*) noexcept {}};
        RenderResult<void> (*ready_)(void*, Transfer&, DeviceSession&) noexcept = nullptr;
        RenderResult<void> (*accepted_ready_)(void*, DeviceSession&) noexcept = nullptr;
        std::optional<SubmissionTicket> accepted_;
    };
}
