#include <lux/engine/function/render/client/RenderLease.hpp>

#include <lux/engine/function/render/client/RenderControlSession.hpp>

namespace lux::render
{
    RenderSceneLease::~RenderSceneLease() noexcept
    {
        deferOwnedRelease();
    }

    RenderSceneLease::RenderSceneLease(RenderSceneLease&& other) noexcept
        : session_(std::exchange(other.session_, nullptr)),
          id_(std::exchange(other.id_, {}))
    {
    }

    RenderSceneLease&
    RenderSceneLease::operator=(RenderSceneLease&& other) noexcept
    {
        if (this == &other)
            return *this;

        // Move assignment is passive ownership replacement. Do not make its
        // behaviour depend on whether a frame happens to be open at this line.
        deferOwnedRelease();
        session_ = std::exchange(other.session_, nullptr);
        id_      = std::exchange(other.id_, {});
        return *this;
    }

    ERenderLeaseCloseStatus RenderSceneLease::close() noexcept
    {
        if (!session_ || !id_.isValid())
            return ERenderLeaseCloseStatus::AlreadyClosed;

        if (!session_->destroyScene(id_))
            return ERenderLeaseCloseStatus::Stopping;
        (void)std::exchange(id_, {});
        (void)std::exchange(session_, nullptr);
        return ERenderLeaseCloseStatus::Released;
    }

    void RenderSceneLease::deferOwnedRelease() noexcept
    {
        if (!session_ || !id_.isValid())
            return;

        const auto id = std::exchange(id_, {});
        std::exchange(session_, nullptr)->deferDestroyScene(id);
    }

    RenderViewLease::~RenderViewLease() noexcept
    {
        deferOwnedRelease();
    }

    RenderViewLease::RenderViewLease(RenderViewLease&& other) noexcept
        : session_(std::exchange(other.session_, nullptr)),
          scene_id_(std::exchange(other.scene_id_, {})),
          view_(std::exchange(other.view_, {})),
          observer_(std::move(other.observer_))
    {
    }

    RenderViewLease& RenderViewLease::operator=(RenderViewLease&& other) noexcept
    {
        if (this == &other)
            return *this;

        deferOwnedRelease();
        session_  = std::exchange(other.session_, nullptr);
        scene_id_ = std::exchange(other.scene_id_, {});
        view_     = std::exchange(other.view_, {});
        observer_ = std::move(other.observer_);
        return *this;
    }

    ERenderLeaseCloseStatus RenderViewLease::close() noexcept
    {
        if (!session_ || !scene_id_.isValid() || !view_.isValid())
            return ERenderLeaseCloseStatus::AlreadyClosed;

        auto request = session_->removeView(scene_id_, view_);
        if (request.isReady() && request.failed())
            return ERenderLeaseCloseStatus::Stopping;
        (void)std::exchange(scene_id_, {});
        (void)std::exchange(view_, {});
        (void)std::exchange(session_, nullptr);
        if (observer_)
            request.then(std::move(observer_));
        return ERenderLeaseCloseStatus::Released;
    }

    void RenderViewLease::deferOwnedRelease() noexcept
    {
        if (!session_ || !scene_id_.isValid() || !view_.isValid())
            return;

        const auto scene_id = std::exchange(scene_id_, {});
        const auto view     = std::exchange(view_, {});
        std::exchange(session_, nullptr)->deferRemoveView(
            scene_id,
            view,
            std::move(observer_)
        );
    }

    RenderTargetLease::~RenderTargetLease() noexcept
    {
        deferOwnedRelease();
    }

    RenderTargetLease::RenderTargetLease(RenderTargetLease&& other) noexcept
        : session_(std::exchange(other.session_, nullptr)),
          target_(std::exchange(other.target_, {})),
          observer_(std::move(other.observer_))
    {
    }

    RenderTargetLease&
    RenderTargetLease::operator=(RenderTargetLease&& other) noexcept
    {
        if (this == &other)
            return *this;

        deferOwnedRelease();
        session_  = std::exchange(other.session_, nullptr);
        target_   = std::exchange(other.target_, {});
        observer_ = std::move(other.observer_);
        return *this;
    }

    RenderTargetCloseResult RenderTargetLease::close() noexcept
    {
        if (!session_ || !target_.isValid())
            return lux::cxx::unexpected(
                ERenderTargetCloseError::AlreadyClosed);
        auto request = session_->destroyRenderTarget(target_);
        if (request.isReady() && request.failed())
            return lux::cxx::unexpected(ERenderTargetCloseError::Stopping);
        (void)std::exchange(target_, {});
        (void)std::exchange(session_, nullptr);
        if (observer_)
            request.then(std::move(observer_));
        return request;
    }

    void RenderTargetLease::deferOwnedRelease() noexcept
    {
        if (!session_ || !target_.isValid())
            return;

        const auto target = std::exchange(target_, {});
        std::exchange(session_, nullptr)->deferDestroyTarget(
            target,
            std::move(observer_)
        );
    }

} // namespace lux::render
