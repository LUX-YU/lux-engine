#pragma once
/**
 * @file AsyncRenderUploadService.hpp
 * @brief Coordinator-owned bridge from AsyncRuntime to RenderUploadSession.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>
#include <lux/engine/function/render/client/RenderUploadClient.hpp>
#include <lux/engine/runtime/render/scene/visibility.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace lux::exec
{
    class AsyncRuntime;
}

namespace lux::render
{
    class RenderUploadSession;
    struct RenderChannelSync;
}

namespace lux::runtime
{
    class AsyncRenderUploadCloseSender;
    class AsyncRenderUploadService;
    struct AsyncRenderUploadCloseReport;

    namespace detail
    {
        LUX_RUNTIME_RENDER_SCENE_PUBLIC void subscribeRenderUploadClose(
            AsyncRenderUploadService& service,
            lux::cxx::move_only_function<void(
                AsyncRenderUploadCloseReport)> completion) noexcept;
    }

    /// Keeps the upload service's accepted-work count balanced while an
    /// operation is still owned by AsyncRuntime's typed queue. The coordinator
    /// disarms it when dispatch begins; queue rejection/destruction releases
    /// the accepted count through this non-generic lifetime trampoline.
    class SubmitRenderUploadAdmission final
    {
    public:
        SubmitRenderUploadAdmission() noexcept = default;
        SubmitRenderUploadAdmission(
            std::shared_ptr<void> owner,
            void (*release)(void*) noexcept) noexcept
            : owner_(std::move(owner))
            , release_(release)
        {}
        SubmitRenderUploadAdmission(
            const SubmitRenderUploadAdmission&) = delete;
        SubmitRenderUploadAdmission& operator=(
            const SubmitRenderUploadAdmission&) = delete;
        SubmitRenderUploadAdmission(
            SubmitRenderUploadAdmission&& other) noexcept
            : owner_(std::move(other.owner_))
            , release_(std::exchange(other.release_, nullptr))
        {}
        SubmitRenderUploadAdmission& operator=(
            SubmitRenderUploadAdmission&& other) noexcept
        {
            if (this == &other)
                return *this;
            reset();
            owner_ = std::move(other.owner_);
            release_ = std::exchange(other.release_, nullptr);
            return *this;
        }
        ~SubmitRenderUploadAdmission() { reset(); }

        void disarm() noexcept
        {
            release_ = nullptr;
            owner_.reset();
        }

    private:
        void reset() noexcept
        {
            auto owner = std::move(owner_);
            auto release = std::exchange(release_, nullptr);
            if (owner && release)
                release(owner.get());
        }

        std::shared_ptr<void> owner_;
        void (*release_)(void*) noexcept{nullptr};
    };

    struct SubmitRenderUpload final
    {
        using Value = void;
        using Error = lux::render::ERenderUploadSubmitError;

        std::shared_ptr<lux::render::detail::PreparedUpload> prepared;
        SubmitRenderUploadAdmission admission;
    };

    struct AsyncRenderUploadCloseReport final
    {
        std::size_t pending_backpressure{0};
        std::size_t active_replies{0};
        std::size_t accepted_inflight{0};
        std::uint64_t retry_attempts{0u};
        std::size_t retry_high_water{0u};
        bool        clean{true};
    };

    /// Process-domain owner of upload submission and reply pumping.  The
    /// service object is a small lifetime handle; its State is registered in
    /// AsyncRuntime and is touched only by the coordinator after bind().
    class LUX_RUNTIME_RENDER_SCENE_PUBLIC AsyncRenderUploadService final
    {
    public:
        [[nodiscard]] static lux::cxx::expected<
            AsyncRenderUploadService,
            lux::exec::AsyncAssemblyFailure>
        addTo(lux::exec::AsyncRuntimeBuilder& builder);

        AsyncRenderUploadService(const AsyncRenderUploadService&) = delete;
        AsyncRenderUploadService& operator=(
            const AsyncRenderUploadService&) = delete;
        AsyncRenderUploadService(AsyncRenderUploadService&&) noexcept;
        AsyncRenderUploadService& operator=(
            AsyncRenderUploadService&&) noexcept;
        ~AsyncRenderUploadService();

        [[nodiscard]] bool bind(
            lux::exec::AsyncRuntime& runtime,
            lux::render::RenderUploadSession& session,
            const std::shared_ptr<lux::render::RenderChannelSync>& sync) noexcept;

        [[nodiscard]] lux::render::RenderUploadClient client() const noexcept;

        /// Closes producer admission. Accepted operations retain the state and
        /// continue to an exactly-once reply; the host keeps AsyncRuntime and
        /// the render server alive until report().clean becomes true.
        [[nodiscard]] AsyncRenderUploadCloseSender closeAsync() noexcept;
        [[nodiscard]] AsyncRenderUploadCloseReport report() const noexcept;

        /// Removes the raw wake trampoline before either side's storage can
        /// disappear. Admission must be closed and report().clean must hold;
        /// the render thread may already be stopped or may still be idle.
        void unbind() noexcept;

    private:
        struct State;

        explicit AsyncRenderUploadService(
            std::shared_ptr<State> state) noexcept
            : state_(std::move(state))
        {}

        std::shared_ptr<State> state_;

        friend class AsyncRenderUploadCloseSender;
        friend void detail::subscribeRenderUploadClose(
            AsyncRenderUploadService&,
            lux::cxx::move_only_function<void(
                AsyncRenderUploadCloseReport)>) noexcept;
    };
}
