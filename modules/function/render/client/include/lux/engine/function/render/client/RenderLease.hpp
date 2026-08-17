#pragma once
/**
 * @file RenderLease.hpp
 * @brief Move-only ownership for render-server objects.
 *
 * An id is a copyable observation. A lease is the unique responsibility to
 * return that id to the render server. Destruction never blocks and never
 * assumes that a frame is currently open: it queues the release in the
 * longer-lived RenderControlSession. Its explicit drain publishes the
 * fallback independently of frame admission.
 *
 * Main-thread confined by contract. No lock protects lease/session state;
 * composition-root ownership is the synchronization mechanism.
 */

#include <lux/engine/function/visibility.h>
#include <lux/engine/function/render/client/RenderRequest.hpp>
#include <lux/engine/function/render/client/RenderProtocol.hpp>
#include <lux/engine/function/render/client/core/FeatureHandle.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/move_only_function.hpp>

#include <utility>

namespace lux::render
{
    class RenderControlSession;

    using RenderViewReleaseObserver =
        lux::cxx::move_only_function<void(const GenericOkReply&)>;

    using RenderTargetReleaseObserver =
        lux::cxx::move_only_function<void(const TargetReleasedReply&)>;

    enum class ERenderTargetCloseError
    {
        AlreadyClosed,
        Stopping
    };

    using RenderTargetCloseResult = lux::cxx::expected<RenderRequest<TargetReleasedReply>, ERenderTargetCloseError>;

    enum class ERenderLeaseCloseStatus
    {
        Released,
        Deferred,
        Stopping,
        AlreadyClosed
    };

    class LUX_FUNCTION_PUBLIC RenderSceneLease final
    {
    public:
        RenderSceneLease() noexcept = default;
        ~RenderSceneLease() noexcept;

        RenderSceneLease(const RenderSceneLease&)            = delete;
        RenderSceneLease& operator=(const RenderSceneLease&) = delete;

        RenderSceneLease(RenderSceneLease&& other) noexcept;
        RenderSceneLease& operator=(RenderSceneLease&& other) noexcept;

        [[nodiscard]] RenderSceneId id() const noexcept { return id_; }
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return session_ != nullptr && id_.isValid();
        }

        /**
         * Consume this lease and publish the control-plane release.
         */
        [[nodiscard]] ERenderLeaseCloseStatus close() noexcept;

    private:
        friend class RenderControlSession;

        RenderSceneLease(RenderControlSession& session, RenderSceneId id) noexcept
            : session_(&session), id_(id)
        {
        }

        void deferOwnedRelease() noexcept;

        RenderControlSession* session_{nullptr};
        RenderSceneId  id_{};
    };

    /// Unique client-side responsibility for one view inside a render scene.
    /// The containing RenderSceneLease must outlive this child lease.
    class LUX_FUNCTION_PUBLIC RenderViewLease final
    {
    public:
        RenderViewLease() noexcept = default;
        ~RenderViewLease() noexcept;

        RenderViewLease(const RenderViewLease&)            = delete;
        RenderViewLease& operator=(const RenderViewLease&) = delete;

        RenderViewLease(RenderViewLease&& other) noexcept;
        RenderViewLease& operator=(RenderViewLease&& other) noexcept;

        [[nodiscard]] RenderSceneId scene() const noexcept { return scene_id_; }
        [[nodiscard]] ViewHandle id() const noexcept { return view_; }
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return session_ != nullptr &&
                   scene_id_.isValid() && view_.isValid();
        }

        /// Consume this lease and publish removeView through the control lane.
        [[nodiscard]] ERenderLeaseCloseStatus close() noexcept;

    private:
        friend class RenderControlSession;

        RenderViewLease(RenderControlSession& session,
                        RenderSceneId scene_id,
                        ViewHandle view,
                        RenderViewReleaseObserver observer) noexcept
            : session_(&session), scene_id_(scene_id), view_(view),
              observer_(std::move(observer))
        {
        }

        void deferOwnedRelease() noexcept;

        RenderControlSession* session_{nullptr};
        RenderSceneId  scene_id_{};       // observation; parent scene outlives us
        ViewHandle     view_{};
        RenderViewReleaseObserver observer_{};
    };

    /// Unique ownership of an offscreen or surface render target.
    ///
    /// Destruction is only a non-blocking leak backstop. Surface owners must
    /// explicitly close and wait for the control-plane acknowledgement
    /// before destroying the native window. The type intentionally exposes
    /// that acknowledgement instead of pretending remote teardown is a
    /// synchronous C++ destructor.
    class LUX_FUNCTION_PUBLIC RenderTargetLease final
    {
    public:
        RenderTargetLease() noexcept = default;
        ~RenderTargetLease() noexcept;

        RenderTargetLease(const RenderTargetLease&)            = delete;
        RenderTargetLease& operator=(const RenderTargetLease&) = delete;

        RenderTargetLease(RenderTargetLease&& other) noexcept;
        RenderTargetLease& operator=(RenderTargetLease&& other) noexcept;

        [[nodiscard]] RenderTargetId id() const noexcept { return target_; }
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return session_ != nullptr &&
                   target_.isValid();
        }

        /// Consume this lease and record DestroyTarget. Failure does not
        /// consume ownership, so callers may retry while the control lane lives.
        [[nodiscard]] RenderTargetCloseResult close() noexcept;

    private:
        friend class RenderControlSession;

        RenderTargetLease(RenderControlSession& session, RenderTargetId target,
                          RenderTargetReleaseObserver observer) noexcept
            : session_(&session), target_(target), observer_(std::move(observer))
        {
        }

        void deferOwnedRelease() noexcept;

        RenderControlSession* session_{nullptr};
        RenderTargetId target_{};
        RenderTargetReleaseObserver observer_{};
    };

} // namespace lux::render
