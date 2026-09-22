#pragma once
/**
 * @file RenderLease.hpp
 * @brief Move-only ownership for render-server objects.
 *
 * An id is a copyable observation. A lease is the unique responsibility to
 * return that id to the render server. Destruction never blocks and never
 * assumes that a frame is currently open. Adoption reserves a stable release
 * record in the longer-lived RenderControlSession. Destruction marks that
 * record for release; finite maintenance publishes it when capacity permits.
 *
 * Main-thread confined by contract. No lock protects lease/session state;
 * composition-root ownership is the synchronization mechanism.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/function/render/client/RenderProtocol.hpp>
#include <lux/engine/function/render/client/RenderRequest.hpp>
#include <lux/engine/function/render/client/core/FeatureHandle.hpp>
#include <lux/engine/function/render/client/core/FeatureTypeId.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/function/visibility.h>

#include <utility>
#include <vector>

namespace lux::render
{
class RenderControlSession;
namespace detail
{
struct SceneReleaseRecord;
struct ViewReleaseRecord;
struct TargetReleaseRecord;
struct ResourceReleaseRecord;
} // namespace detail

using RenderViewReleaseObserver = lux::cxx::move_only_function<void(const GenericOkReply &)>;

using RenderTargetReleaseObserver = lux::cxx::move_only_function<void(const TargetReleasedReply &)>;

// A Feature owns its concrete creation/reply/handle state. Core admission
// retains that state before the first asynchronous request, and calls its
// release step on Main after the last CPU/Program use has gone away.
// The callback consumes only accepted command units; true means no further
// work or resource ownership remains. It must not borrow a Scene/Registry.
using RenderResourceReleaseStep = bool (*)(void *, RenderControlSession &, std::size_t &,
                                           bool backend_retired) noexcept;

class LUX_FUNCTION_PUBLIC RenderResourceUse final
{
  public:
    RenderResourceUse() noexcept = default;
    ~RenderResourceUse() noexcept;
    RenderResourceUse(RenderResourceUse &&) noexcept;
    RenderResourceUse &operator=(RenderResourceUse &&) noexcept;
    RenderResourceUse(const RenderResourceUse &) = delete;
    RenderResourceUse &operator=(const RenderResourceUse &) = delete;
    [[nodiscard]] RenderResourceUse retain() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return bool(record_);
    }

  private:
    friend class RenderControlSession;
    explicit RenderResourceUse(std::shared_ptr<detail::ResourceReleaseRecord>) noexcept;
    void reset() noexcept;
    std::shared_ptr<detail::ResourceReleaseRecord> record_;
};

enum class ERenderTargetCloseError
{
    AlreadyClosed,
    Stopping,
    Busy,
    ALLOCATION_FAILURE
};

using RenderTargetCloseResult = lux::cxx::expected<RenderRequest<TargetReleasedReply>, ERenderTargetCloseError>;

enum class ERenderLeaseCloseStatus
{
    Released,
    Deferred,
    Stopping,
    AlreadyClosed,
    ALLOCATION_FAILURE
};

enum class ESceneResourceState : std::uint8_t
{
    QUEUED,
    CREATING,
    ATTACHING,
    READY,
    RELEASING,
    RETIRED,
};

struct SceneFeatureAttachment final
{
    FeatureTypeId type{};
    std::uint32_t registered_type{};
    std::vector<std::byte> configuration;
    std::shared_ptr<const void> code;
};

struct SceneResourceStatus final
{
    ESceneResourceState state{ESceneResourceState::QUEUED};
    RenderSceneId scene;
    RenderError failure;
    std::uint64_t request{};
    FeatureTypeId feature{};
};

// An observation does not prolong resource use. The original result remains
// readable after the system and the corresponding lease have gone away.
class LUX_FUNCTION_PUBLIC RenderSceneReceipt final
{
  public:
    [[nodiscard]] SceneResourceStatus status() const noexcept;

  private:
    friend class RenderSceneLease;
    std::shared_ptr<const detail::SceneReleaseRecord> record_;
};

class LUX_FUNCTION_PUBLIC RenderSceneLease final
{
  public:
    RenderSceneLease() noexcept = default;
    ~RenderSceneLease() noexcept;

    RenderSceneLease(const RenderSceneLease &) = delete;
    RenderSceneLease &operator=(const RenderSceneLease &) = delete;

    RenderSceneLease(RenderSceneLease &&other) noexcept;
    RenderSceneLease &operator=(RenderSceneLease &&other) noexcept;

    [[nodiscard]] RenderSceneId id() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] SceneResourceStatus status() const noexcept;
    [[nodiscard]] FeatureHandle feature(FeatureTypeId type) const noexcept;
    [[nodiscard]] RenderSceneReceipt receipt() const noexcept;
    // Used by admitted Programs and Views, not by result observers.
    [[nodiscard]] RenderSceneLease retain() const noexcept;

    /**
     * Try to publish the release without waiting or pumping replies.
     * Deferred retains this lease for a later retry.
     */
    [[nodiscard]] ERenderLeaseCloseStatus close() noexcept;

    // The runtime owner must prove that the backend has destroyed its
    // Scenes and all GPU/CPU program references. No new Control packet is
    // published.
    void retireAfterBackendStopped() noexcept;

  private:
    friend class RenderControlSession;

    RenderSceneLease(RenderControlSession &session, std::shared_ptr<detail::SceneReleaseRecord> record) noexcept;

    void deferOwnedRelease() noexcept;

    RenderControlSession *session_{nullptr};
    std::shared_ptr<detail::SceneReleaseRecord> record_;
};

/// Unique client-side responsibility for one view inside a render scene.
/// The containing RenderSceneLease must outlive this child lease.
class LUX_FUNCTION_PUBLIC RenderViewLease final
{
  public:
    RenderViewLease() noexcept = default;
    ~RenderViewLease() noexcept;

    RenderViewLease(const RenderViewLease &) = delete;
    RenderViewLease &operator=(const RenderViewLease &) = delete;

    RenderViewLease(RenderViewLease &&other) noexcept;
    RenderViewLease &operator=(RenderViewLease &&other) noexcept;

    [[nodiscard]] RenderSceneId scene() const noexcept
    {
        return scene_id_;
    }
    [[nodiscard]] ViewHandle id() const noexcept
    {
        return view_;
    }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return session_ != nullptr && scene_id_.isValid() && view_.isValid();
    }

    /// Consume this lease and publish removeView through the control lane.
    [[nodiscard]] ERenderLeaseCloseStatus close() noexcept;

  private:
    friend class RenderControlSession;

    RenderViewLease(RenderControlSession &session, RenderSceneId scene_id, ViewHandle view,
                    detail::ViewReleaseRecord &record) noexcept
        : session_(&session), scene_id_(scene_id), view_(view), record_(&record)
    {
    }

    void deferOwnedRelease() noexcept;

    RenderControlSession *session_{nullptr};
    RenderSceneId scene_id_{}; // observation; parent scene outlives us
    ViewHandle view_{};
    detail::ViewReleaseRecord *record_{};
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

    RenderTargetLease(const RenderTargetLease &) = delete;
    RenderTargetLease &operator=(const RenderTargetLease &) = delete;

    RenderTargetLease(RenderTargetLease &&other) noexcept;
    RenderTargetLease &operator=(RenderTargetLease &&other) noexcept;

    [[nodiscard]] RenderTargetId id() const noexcept
    {
        return target_;
    }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return session_ != nullptr && target_.isValid();
    }

    /// Consume this lease and record DestroyTarget. Failure does not
    /// consume ownership, so callers may retry while the control lane lives.
    [[nodiscard]] RenderTargetCloseResult close() noexcept;

  private:
    friend class RenderControlSession;

    RenderTargetLease(RenderControlSession &session, RenderTargetId target,
                      detail::TargetReleaseRecord &record) noexcept
        : session_(&session), target_(target), record_(&record)
    {
    }

    void deferOwnedRelease() noexcept;

    RenderControlSession *session_{nullptr};
    RenderTargetId target_{};
    detail::TargetReleaseRecord *record_{};
};

} // namespace lux::render
