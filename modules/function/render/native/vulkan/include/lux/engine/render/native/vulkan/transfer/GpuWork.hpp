#pragma once

#include <lux/engine/render/native/vulkan/DeviceSession.hpp>

namespace lux::render::vulkan
{
    // A single Native work owner, erased once at admission. It never owns a RenderRequest or a second queue.
    class GpuWork final
    {
    public:
        GpuWork() noexcept = default;
        GpuWork(GpuWork&&) noexcept = default;
        GpuWork& operator=(GpuWork&&) noexcept = default;
        GpuWork(const GpuWork&) = delete;
        GpuWork& operator=(const GpuWork&) = delete;

        template <auto Ready, class T>
        [[nodiscard]] static RenderResult<GpuWork> pending(DeviceSession& session, std::unique_ptr<T>& value) noexcept
        {
            if (!value || !value->unstartedIn(session)) return cxx::unexpected(RenderError{kWrongOwner});
            GpuWork result;
            result.session_ = &session;
            for (std::size_t role = 0; role < result.before_.size(); ++role)
                result.before_[role] = session.queue(static_cast<EQueueRole>(role)).submitted();
            result.owner_ = {value.release(), [](void* p) noexcept { delete static_cast<T*>(p); }};
            result.advance_ = [](void* p) noexcept { return static_cast<T*>(p)->advance(); };
            result.cancel_ = [](void* p) noexcept { static_cast<T*>(p)->cancel(); };
            result.ticket_ = [](const void* p) noexcept { return static_cast<const T*>(p)->lastUse(); };
            result.ready_ = [](void* p, DeviceSession& device) noexcept { return Ready(*static_cast<T*>(p), device); };
            return result;
        }

        [[nodiscard]] static GpuWork accepted(DeviceSession& session, SubmissionTicket ticket,
            void* owner, void (*destroy)(void*) noexcept,
            RenderResult<void> (*ready)(void*, DeviceSession&) noexcept) noexcept;
        [[nodiscard]] bool unstartedIn(const DeviceSession& session) const noexcept
        {
            return session_ == &session && owner_ && advance_ && !started_;
        }
        [[nodiscard]] RenderResult<bool> advance() noexcept;
        [[nodiscard]] RenderResult<void> publish() noexcept;
        void reset() noexcept { owner_.reset(); }
        [[nodiscard]] std::optional<SubmissionTicket> lastUse() const noexcept
        {
            return ticket_ ? ticket_(owner_.get()) : accepted_;
        }

    private:
        DeviceSession* session_ = nullptr;
        std::unique_ptr<void, void (*)(void*) noexcept> owner_{nullptr, [](void*) noexcept {}};
        RenderResult<bool> (*advance_)(void*) noexcept = nullptr;
        void (*cancel_)(void*) noexcept = nullptr;
        std::optional<SubmissionTicket> (*ticket_)(const void*) noexcept = nullptr;
        RenderResult<void> (*ready_)(void*, DeviceSession&) noexcept = nullptr;
        std::optional<SubmissionTicket> accepted_;
        std::optional<RenderError> failure_;
        std::array<std::uint64_t, 3> before_{};
        bool started_ = false, completed_ = false;
    };
}
