#include <lux/engine/runtime/render/scene/detail/residency/RenderResourceService.hpp>
#include <lux/engine/function/render/client/core/RenderFatal.hpp>

#include <algorithm>
#include <utility>

namespace lux::runtime
{
    /// Main-thread release authority shared by leases and owner-creating RPC
    /// routers. It never owns the service/subservices or render dependencies;
    /// Residency close must make the control quiescent before invalidation.
    class ResidentHandleReleaseControl final
    {
    public:
        explicit ResidentHandleReleaseControl(
            RenderResourceService& service) noexcept
            : service_(&service)
        {}

        void retain(lux::ecs::EResourceDomain domain,
                    std::uint64_t bits) noexcept
        {
            if (bits == 0)
                lux::render::renderFatal(
                    "Resident handle control cannot retain a null owner"
                );
            if (service_ == nullptr)
                lux::render::renderFatal(
                    "Non-null resident handle reached an inactive release "
                    "control"
                );
            if (!service_->hasDomain(domain))
                lux::render::renderFatal(
                    "Resident handle domain disappeared before adoption"
                );
            ++live_leases_;
        }

        void release(lux::ecs::EResourceDomain domain,
                     std::uint64_t bits) noexcept
        {
            if (bits == 0 || live_leases_ == 0)
                lux::render::renderFatal(
                    "Resident handle release is unbalanced"
                );
            auto* const service = service_;
            if (service == nullptr)
                lux::render::renderFatal(
                    "Resident GPU owner outlived its release service"
                );

            // Publish the lease as released before calling domain code: destroy
            // may synchronously re-enter close/diagnostics.
            --live_leases_;
            if (!service->destroyHandle(domain, bits))
                lux::render::renderFatal(
                    "Resident handle domain disappeared before release"
                );
        }

        [[nodiscard]] std::size_t liveLeases() const noexcept
        {
            return live_leases_;
        }

        void invalidate() noexcept { service_ = nullptr; }

    private:
        RenderResourceService* service_{nullptr};
        std::size_t            live_leases_{0};
    };

    ResidentResourceLease::~ResidentResourceLease() noexcept
    {
        releaseOwned();
    }

    ResidentResourceLease::ResidentResourceLease(
        ResidentResourceLease&& other) noexcept
        : release_(std::move(other.release_))
        , domain_(other.domain_)
        , bits_(std::exchange(other.bits_, 0))
    {}

    ResidentResourceLease& ResidentResourceLease::operator=(
        ResidentResourceLease&& other) noexcept
    {
        if (this == &other) return *this;
        releaseOwned();
        release_ = std::move(other.release_);
        domain_  = other.domain_;
        bits_    = std::exchange(other.bits_, 0);
        return *this;
    }

    void ResidentResourceLease::releaseOwned() noexcept
    {
        auto release = std::move(release_);
        const std::uint64_t bits = std::exchange(bits_, 0);
        if (bits == 0)
            return;
        if (!release)
            lux::render::renderFatal(
                "Resident GPU owner has no release capability"
            );
        release->release(domain_, bits);
    }

    ResidentResourceAdoptResult ResidentHandleReleaseEndpoint::adopt(
        std::uint64_t bits) const noexcept
    {
        if (bits == 0)
            return lux::cxx::unexpected(
                EResidentResourceAdoptError::NullHandle
            );
        if (!release_)
            lux::render::renderFatal(
                "Non-null resident handle reached an empty release endpoint"
            );

        release_->retain(domain_, bits);
        return ResidentResourceLease(release_, domain_, bits);
    }

    RenderResourceService::RenderResourceService()
        : subscription_control_(
              std::make_shared<SubscriptionControl>(*this))
        , release_control_(
              std::make_shared<ResidentHandleReleaseControl>(*this))
    {}

    RenderResourceService::~RenderResourceService()
    {
        if (!releaseQuiescent())
            lux::render::renderFatal(
                "RenderResourceService destroyed with live resident handles "
                "or upload release capabilities"
            );
        release_control_->invalidate();
        release_control_.reset();

        // Ticket only keeps a weak control. Invalidate it before member
        // destruction releases callbacks which may themselves own a Ticket.
        subscription_control_->invalidate();
        subscription_control_.reset();
    }

    // ── Ticket 退订(reset 在头内联 —— 嵌套类不随外层 dllexport)────────

    void RenderResourceService::eraseWait(const lux::asset::asset_id_t& id,
                                          std::uint64_t token) noexcept
    {
        auto it = waits_.find(id);
        if (it != waits_.end())
        {
            std::erase_if(it->second,
                          [token](const Wait& wait)
                          { return wait.token == token; });
            if (it->second.empty()) waits_.erase(it);
        }

        // notify moves its batch out of waits_ before invoking callbacks. A
        // callback may reset a peer ticket from that local batch; walk the
        // lexical dispatch stack so the peer is skipped later this same turn.
        for (auto* dispatch = subscription_control_->dispatchHead();
             dispatch != nullptr;
             dispatch = dispatch->previous())
            dispatch->deactivate(token);
    }

    void RenderResourceService::eraseInvalidation(std::uint64_t token) noexcept
    {
        for (const auto& subscription : invalidation_)
            if (subscription->token == token)
                subscription->active = false;
        std::erase_if(invalidation_,
                      [token](const auto& subscription)
                      { return subscription->token == token; });
    }

    bool RenderResourceService::waitActive(
        const lux::asset::asset_id_t& id,
        std::uint64_t token) const noexcept
    {
        const auto found = waits_.find(id);
        if (found != waits_.end())
            for (const auto& wait : found->second)
                if (wait.token == token)
                    return wait.active;

        for (auto* dispatch = subscription_control_->dispatchHead();
             dispatch != nullptr;
             dispatch = dispatch->previous())
            if (dispatch->active(token))
                return true;
        return false;
    }

    // ── 域子服务 ────────────────────────────────────────────────────────

    void RenderResourceService::addSubservice(
        std::unique_ptr<IRenderResourceSubservice> sub)
    {
        if (!sub)
            lux::render::renderFatal(
                "Cannot register a null render-resource subservice"
            );

        const auto domain = sub->domain();
        if (subservices_.contains(domain))
            lux::render::renderFatal(
                "A render-resource domain may be registered only once"
            );

        subservices_.emplace(domain, std::move(sub));
    }

    bool RenderResourceService::hasDomain(lux::ecs::EResourceDomain d) const noexcept
    {
        return subservices_.contains(d);
    }

    bool RenderResourceService::submit(lux::ecs::EResourceDomain d,
                                       const lux::asset::asset_id_t& id,
                                       IRenderResourceSubservice::SubmitDone done)
    {
        auto it = subservices_.find(d);
        if (it == subservices_.end()) return false;
        it->second->submit(id, std::move(done));
        return true;
    }

    ResidentResourceAdoptResult RenderResourceService::adoptHandle(
        lux::ecs::EResourceDomain d, std::uint64_t bits) noexcept
    {
        if (bits == 0)
            return lux::cxx::unexpected(EResidentResourceAdoptError::NullHandle);
        auto endpoint = releaseEndpoint(d);
        if (!endpoint)
            return lux::cxx::unexpected(endpoint.error());
        return endpoint->adopt(bits);
    }

    ResidentHandleReleaseEndpointResult
    RenderResourceService::releaseEndpoint(
        lux::ecs::EResourceDomain d) const noexcept
    {
        if (!hasDomain(d))
            return lux::cxx::unexpected(
                EResidentResourceAdoptError::DomainUnavailable
            );
        return ResidentHandleReleaseEndpoint(release_control_, d);
    }

    bool RenderResourceService::releaseQuiescent() const noexcept
    {
        return release_control_->liveLeases() == 0
            && release_control_.use_count() == 1;
    }

    std::size_t RenderResourceService::liveLeases() const noexcept
    {
        return release_control_->liveLeases();
    }

    std::size_t RenderResourceService::releaseControlReferences() const noexcept
    {
        return release_control_.use_count();
    }

    bool RenderResourceService::destroyHandle(lux::ecs::EResourceDomain d,
                                              std::uint64_t bits) noexcept
    {
        auto it = subservices_.find(d);
        if (it == subservices_.end()) return false;
        it->second->destroy(bits);
        return true;
    }

    std::vector<ResourceDep> RenderResourceService::dependenciesOf(
        lux::ecs::EResourceDomain     d,
        const lux::asset::asset_id_t& id) const
    {
        auto it = subservices_.find(d);
        if (it == subservices_.end()) return {};
        return it->second->dependencies(id);
    }

    std::size_t RenderResourceService::pendingReplies(
        lux::ecs::EResourceDomain d) const noexcept
    {
        auto it = subservices_.find(d);
        return it == subservices_.end() ? 0 : it->second->pendingReplies();
    }

    void RenderResourceService::abandonPendingReplies(
        lux::ecs::EResourceDomain d) noexcept
    {
        auto it = subservices_.find(d);
        if (it != subservices_.end())
            it->second->abandonPendingReplies();
    }

    std::size_t RenderResourceService::pendingReplies() const noexcept
    {
        std::size_t pending = 0;
        for (const auto& [domain, subservice] : subservices_)
        {
            (void)domain;
            pending += subservice->pendingReplies();
        }
        return pending;
    }

    void RenderResourceService::abandonPendingReplies() noexcept
    {
        for (auto& [domain, subservice] : subservices_)
        {
            (void)domain;
            subservice->abandonPendingReplies();
        }
    }

    bool RenderResourceService::ownerControlsQuiescent() const noexcept
    {
        for (const auto& [domain, subservice] : subservices_)
        {
            (void)domain;
            if (!subservice->ownerControlsQuiescent())
                return false;
        }
        return true;
    }

    // ── 等待注册表 ──────────────────────────────────────────────────────

    RenderResourceService::WaitTicket
    RenderResourceService::await(const lux::asset::asset_id_t& id, WaitFn fn)
    {
        const std::uint64_t token = next_token_++;
        waits_[id].push_back(Wait{token, std::move(fn), true});
        WaitTicket t;
        t.control_ = subscription_control_;
        t.id_      = id;
        t.token_   = token;
        return t;
    }

    RenderResourceService::InvalidationTicket
    RenderResourceService::watchInvalidation(InvalidationFn fn)
    {
        const std::uint64_t token = next_token_++;
        auto subscription = std::make_shared<InvalidationSubscription>();
        subscription->token = token;
        subscription->fn    = std::move(fn);
        invalidation_.push_back(std::move(subscription));
        InvalidationTicket t;
        t.control_ = subscription_control_;
        t.token_ = token;
        return t;
    }

    // ── 扇出 ────────────────────────────────────────────────────────────

    void RenderResourceService::notifyReady(const lux::asset::asset_id_t& id,
                                            std::uint64_t bits)
    {
        auto it = waits_.find(id);
        if (it == waits_.end()) return;
        // 先移出再触发:回调体内可能再次 await 同 id(链式),不得自喂。
        auto batch = std::move(it->second);
        waits_.erase(it);
        WaitDispatch dispatch{subscription_control_, batch};
        for (auto& wait : batch)
        {
            if (!dispatch.ownerAlive())
                break;
            if (!wait.active)
                continue;
            wait.active = false;
            auto fn = std::move(wait.fn);
            fn(bits, nullptr);
        }
    }

    void RenderResourceService::notifyFailed(const lux::asset::asset_id_t& id,
                                             const lux::ecs::ResourceFailure& fail)
    {
        auto it = waits_.find(id);
        if (it == waits_.end()) return;
        // A waiter may synchronously invalidate/erase the row that supplied
        // `fail`. Keep one notification-local snapshot so later waiters never
        // observe storage invalidated by an earlier re-entrant callback.
        const lux::ecs::ResourceFailure stable_fail = fail;
        auto batch = std::move(it->second);
        waits_.erase(it);
        WaitDispatch dispatch{subscription_control_, batch};
        for (auto& wait : batch)
        {
            if (!dispatch.ownerAlive())
                break;
            if (!wait.active)
                continue;
            wait.active = false;
            auto fn = std::move(wait.fn);
            fn(0, &stable_fail);
        }
    }

    void RenderResourceService::notifyInvalidated(
        const std::vector<lux::asset::asset_id_t>& ids)
    {
        // Snapshot stable nodes, not vector iterators. A callback added during
        // this dispatch waits for the next notification; one removed by an
        // earlier callback has `active == false` and is skipped. Keeping a
        // shared node in the local batch also makes self-unsubscribe safe until
        // the current invocation returns. Dispatch stays linear in subscriber
        // count and remains main-thread confined (no user-level lock).
        const auto control = subscription_control_;
        const auto batch = invalidation_;
        for (const auto& subscription : batch)
        {
            if (!control->ownerAlive())
                break;
            if (subscription->active)
                subscription->fn(ids);
        }
    }

    std::size_t
    RenderResourceService::pendingWaits(const lux::asset::asset_id_t& id) const
    {
        const auto it = waits_.find(id);
        return it != waits_.end() ? it->second.size() : 0;
    }

} // namespace lux::runtime
