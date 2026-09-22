#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/RenderLease.hpp>
#include <lux/engine/function/render/client/detail/RenderReleaseRecords.hpp>

namespace lux::render
{
RenderResourceUse::RenderResourceUse(std::shared_ptr<detail::ResourceReleaseRecord> record) noexcept
    : record_(std::move(record))
{
}

RenderResourceUse::~RenderResourceUse() noexcept
{
    reset();
}

RenderResourceUse::RenderResourceUse(RenderResourceUse &&other) noexcept : record_(std::move(other.record_))
{
}

RenderResourceUse &RenderResourceUse::operator=(RenderResourceUse &&other) noexcept
{
    if (this != &other)
    {
        reset();
        record_ = std::move(other.record_);
    }
    return *this;
}

void RenderResourceUse::reset() noexcept
{
    if (record_)
    {
        record_->uses.fetch_sub(1, std::memory_order_release);
        record_.reset();
    }
}

RenderResourceUse RenderResourceUse::retain() const noexcept
{
    if (!record_)
    {
        return {};
    }
    record_->uses.fetch_add(1, std::memory_order_relaxed);
    return RenderResourceUse(record_);
}

RenderSceneLease::~RenderSceneLease() noexcept
{
    deferOwnedRelease();
}

RenderSceneLease::RenderSceneLease(RenderControlSession &session,
                                   std::shared_ptr<detail::SceneReleaseRecord> record) noexcept
    : session_(&session), record_(std::move(record))
{
}

RenderSceneLease::RenderSceneLease(RenderSceneLease &&other) noexcept
    : session_(std::exchange(other.session_, nullptr)), record_(std::move(other.record_))
{
}

RenderSceneLease &RenderSceneLease::operator=(RenderSceneLease &&other) noexcept
{
    if (this != &other)
    {
        deferOwnedRelease();
        session_ = std::exchange(other.session_, nullptr);
        record_ = std::move(other.record_);
    }
    return *this;
}

RenderSceneId RenderSceneLease::id() const noexcept
{
    return record_ ? record_->scene : RenderSceneId{};
}
RenderSceneLease::operator bool() const noexcept
{
    return bool(record_);
}
SceneResourceStatus RenderSceneLease::status() const noexcept
{
    return record_ ? record_->status() : SceneResourceStatus{ESceneResourceState::RETIRED};
}
SceneResourceStatus RenderSceneReceipt::status() const noexcept
{
    return record_ ? record_->status() : SceneResourceStatus{ESceneResourceState::RETIRED};
}
RenderSceneReceipt RenderSceneLease::receipt() const noexcept
{
    RenderSceneReceipt result;
    result.record_ = record_;
    return result;
}
FeatureHandle RenderSceneLease::feature(FeatureTypeId type) const noexcept
{
    if (record_)
    {
        for (const auto &item : record_->features)
        {
            if (item.first == type)
            {
                return item.second;
            }
        }
    }
    return {};
}
RenderSceneLease RenderSceneLease::retain() const noexcept
{
    if (!record_ || record_->requested || record_->state == ESceneResourceState::RETIRED)
    {
        return {};
    }
    record_->uses.fetch_add(1, std::memory_order_relaxed);
    return RenderSceneLease(*session_, record_);
}

ERenderLeaseCloseStatus RenderSceneLease::close() noexcept
{
    if (!record_)
    {
        return ERenderLeaseCloseStatus::AlreadyClosed;
    }
    const auto result = session_->closeScene(*record_);
    if (result == ERenderLeaseCloseStatus::Released)
    {
        deferOwnedRelease();
    }
    return result;
}

void RenderSceneLease::deferOwnedRelease() noexcept
{
    if (record_)
    {
        // May be the last Program attachment on the render thread. Only
        // the atomic use count changes; all request work stays on Main.
        record_->uses.fetch_sub(1, std::memory_order_release);
        record_.reset();
    }
    session_ = nullptr;
}

void RenderSceneLease::retireAfterBackendStopped() noexcept
{
    if (record_)
    {
        record_->submitted = true;
        record_->state = ESceneResourceState::RETIRED;
    }
    deferOwnedRelease();
}

RenderViewLease::~RenderViewLease() noexcept
{
    deferOwnedRelease();
}

RenderViewLease::RenderViewLease(RenderViewLease &&other) noexcept
    : session_(std::exchange(other.session_, nullptr)), scene_id_(std::exchange(other.scene_id_, {})),
      view_(std::exchange(other.view_, {})), record_(std::exchange(other.record_, nullptr))
{
}

RenderViewLease &RenderViewLease::operator=(RenderViewLease &&other) noexcept
{
    if (this != &other)
    {
        deferOwnedRelease();
        session_ = std::exchange(other.session_, nullptr);
        scene_id_ = std::exchange(other.scene_id_, {});
        view_ = std::exchange(other.view_, {});
        record_ = std::exchange(other.record_, nullptr);
    }
    return *this;
}

ERenderLeaseCloseStatus RenderViewLease::close() noexcept
{
    if (!record_)
    {
        return ERenderLeaseCloseStatus::AlreadyClosed;
    }
    const auto result = session_->closeView(*record_);
    if (result == ERenderLeaseCloseStatus::Released)
    {
        deferOwnedRelease();
    }
    return result;
}

void RenderViewLease::deferOwnedRelease() noexcept
{
    if (record_)
    {
        record_->requested = true;
        record_->owned = false;
        record_ = nullptr;
    }
    session_ = nullptr;
    scene_id_ = {};
    view_ = {};
}

RenderTargetLease::~RenderTargetLease() noexcept
{
    deferOwnedRelease();
}

RenderTargetLease::RenderTargetLease(RenderTargetLease &&other) noexcept
    : session_(std::exchange(other.session_, nullptr)), target_(std::exchange(other.target_, {})),
      record_(std::exchange(other.record_, nullptr))
{
}

RenderTargetLease &RenderTargetLease::operator=(RenderTargetLease &&other) noexcept
{
    if (this != &other)
    {
        deferOwnedRelease();
        session_ = std::exchange(other.session_, nullptr);
        target_ = std::exchange(other.target_, {});
        record_ = std::exchange(other.record_, nullptr);
    }
    return *this;
}

RenderTargetCloseResult RenderTargetLease::close() noexcept
{
    if (!record_)
    {
        return lux::cxx::unexpected(ERenderTargetCloseError::AlreadyClosed);
    }
    auto result = session_->closeTarget(*record_);
    if (result)
    {
        deferOwnedRelease();
    }
    return result;
}

void RenderTargetLease::deferOwnedRelease() noexcept
{
    if (record_)
    {
        record_->requested = true;
        record_->owned = false;
        record_ = nullptr;
    }
    session_ = nullptr;
    target_ = {};
}
} // namespace lux::render
