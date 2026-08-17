#pragma once
/**
 * @file RenderResourceService.hpp — 驻留实现内部的渲染资源服务。
 *
 * **引擎层、进程域、类型无关**(与 RenderResourceStateManager 并排,
 * 切场景不重建)。核心只认三样:asset_id(不透明键)、类型擦除回调、
 * 域子服务插件 —— 不认识组件、registry、AssetManager。
 *
 * 两块内容:
 *  1. **域子服务注册表**(IRenderResourceSubservice 多态插件):每资源域
 *     一个,基础接口 submit/destroy —— **何时调用由引擎编排决定**,
 *     子服务零自主。加新域 = 新子服务 + 装配声明,本类零改动(开闭)。
 *  2. **等待注册表(RAII)**:asset_id → 回调列表。场景侧发起等待拿
 *     WaitTicket 句柄,**析构自动退订**(DomainEvents Subscription 同款
 *     先例)—— 场景先亡时在途等待随句柄消亡,无野指针,无需任何
 *     显式取消协议。失效观察(watchInvalidation)同款 RAII。
 *
 * 通知的触发者是编排(notifyReady/notifyFailed/notifyInvalidated),
 * 本类只做扇出。回调在主线程 safe point 执行；它不依赖 frame OPEN。
 */

#include <lux/engine/runtime/render/scene/visibility.h>

#include <lux/engine/resource/asset/Asset.hpp>                       // asset_id_t(键)
#include <lux/engine/ecs/render/RenderResourceEvents.hpp>   // 域/失败词汇
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/move_only_function.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::runtime
{
    class RenderResourceService;
    class ResidentHandleReleaseControl;
    class ResidentHandleReleaseEndpoint;

    /// One physical GPU allocation owned by the process-domain residency
    /// table. Components and dependency consumers share asset interest, but
    /// they only observe `bits()`; they never share ownership of this object.
    ///
    /// Main-thread confined. A lease holds a release capability, never a raw
    /// service pointer. Residency close must release every lease before the
    /// service invalidates that capability; violating this contract is fatal,
    /// never a weak no-op that leaks the GPU owner.
    class LUX_RUNTIME_RENDER_SCENE_PUBLIC ResidentResourceLease final
    {
    public:
        ResidentResourceLease() noexcept = default;
        ~ResidentResourceLease() noexcept;

        ResidentResourceLease(const ResidentResourceLease&)            = delete;
        ResidentResourceLease& operator=(const ResidentResourceLease&) = delete;

        ResidentResourceLease(ResidentResourceLease&& other) noexcept;
        ResidentResourceLease& operator=(ResidentResourceLease&& other) noexcept;

        [[nodiscard]] std::uint64_t bits() const noexcept { return bits_; }
        [[nodiscard]] lux::ecs::EResourceDomain domain() const noexcept
        {
            return domain_;
        }
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return release_ != nullptr && bits_ != 0;
        }

    private:
        friend class RenderResourceService;
        friend class ResidentHandleReleaseControl;
        friend class ResidentHandleReleaseEndpoint;

        ResidentResourceLease(
            std::shared_ptr<ResidentHandleReleaseControl> release,
            lux::ecs::EResourceDomain domain,
            std::uint64_t bits) noexcept
            : release_(std::move(release)), domain_(domain), bits_(bits)
        {}

        void releaseOwned() noexcept;

        std::shared_ptr<ResidentHandleReleaseControl> release_;
        lux::ecs::EResourceDomain domain_{lux::ecs::EResourceDomain::TEXTURE};
        std::uint64_t             bits_{0};
    };

    enum class EResidentResourceAdoptError : std::uint8_t
    {
        NullHandle,
        DomainUnavailable,
    };

    template <typename T>
    using ResidentResourceAdoptExp =
        lux::cxx::expected<T, EResidentResourceAdoptError>;

    using ResidentResourceAdoptResult = ResidentResourceAdoptExp<ResidentResourceLease>;

    /// Domain-bound capability used by an owner-creating RPC continuation.
    /// It shares only a lifetime/generation control, not the service, session,
    /// subservice, or GPU owner. A non-null reply must be adopted while this
    /// endpoint is active; an inactive endpoint is a fatal close-order breach.
    class LUX_RUNTIME_RENDER_SCENE_PUBLIC ResidentHandleReleaseEndpoint final
    {
    public:
        ResidentHandleReleaseEndpoint() noexcept = default;

        ResidentHandleReleaseEndpoint(
            const ResidentHandleReleaseEndpoint&) = delete;
        ResidentHandleReleaseEndpoint& operator=(
            const ResidentHandleReleaseEndpoint&) = delete;
        ResidentHandleReleaseEndpoint(
            ResidentHandleReleaseEndpoint&&) noexcept = default;
        ResidentHandleReleaseEndpoint& operator=(
            ResidentHandleReleaseEndpoint&&) noexcept = default;

        [[nodiscard]] ResidentResourceAdoptResult adopt(
            std::uint64_t bits) const noexcept;
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return release_ != nullptr;
        }

    private:
        friend class RenderResourceService;

        ResidentHandleReleaseEndpoint(
            std::shared_ptr<ResidentHandleReleaseControl> release,
            lux::ecs::EResourceDomain domain) noexcept
            : release_(std::move(release)), domain_(domain)
        {}

        std::shared_ptr<ResidentHandleReleaseControl> release_;
        lux::ecs::EResourceDomain domain_{lux::ecs::EResourceDomain::TEXTURE};
    };

    using ResidentHandleReleaseEndpointResult =
        ResidentResourceAdoptExp<ResidentHandleReleaseEndpoint>;

    /// 资源间依赖(域知识的申报形):材质→贴图槽/父级 这类「我要用它的
    /// 句柄」关系。编排据此在 submit 前把依赖带到**结算**(就绪或终败),
    /// 并写入表行级联边(内容变更波前)与票据代持(T5)。
    struct ResourceDep
    {
        lux::asset::asset_id_t  id;
        lux::ecs::EResourceDomain domain;
    };

    /// Generation-aware MainThreadMailbox trampoline used by owner-reply reapers.
    /// True transfers ownership and guarantees exactly one execution before
    /// executor shutdown completes. False retains nothing and executes
    /// nothing; the reaper synchronously takes its compensation-only path so a
    /// non-null reply owner can never be dropped.
    using TryPostToMain =
        std::function<bool(lux::cxx::move_only_function<void()>)>;

    /// 域子服务基类(用户词汇「子服务」):本域的上传/销毁执行者。
    /// 纯功能面,零 ecs 词汇 —— 组件观察在每世界胶水(T6R2),不在这里。
    /// 实现者持有它需要的资源(RenderFrameSession&/AssetManager& 等,构造给)。
    class IRenderResourceSubservice
    {
    public:
        virtual ~IRenderResourceSubservice() = default;

        [[nodiscard]] virtual lux::ecs::EResourceDomain domain() const = 0;

        /// 申报 @p id 的资源依赖(域知识:只有本域会解析自己的载荷)。
        /// 前置:CPU 数据已在账(编排在加载段之后才查)。无依赖域不覆写。
        /// 依赖失败**不挡** submit(槽位策略是配方的事,例如坏贴图留空槽);
        /// 环由编排的祖先链检测,终败可见。
        [[nodiscard]] virtual std::vector<ResourceDep>
        dependencies(const lux::asset::asset_id_t&) const { return {}; }

        /// 完成回执:bits(0=失败,此时 fail 给原因)。由编排以闭包传入,
        /// 子服务在 RPC 回执处调用恰好一次 —— 回执的双条件验证(status+
        /// 句柄)是实现者的义务(疤痕①)。
        using SubmitDone = lux::cxx::move_only_function<
            void(std::uint64_t bits, std::string_view fail)>;

        /// 发起 @p id 的上传。前置(编排保证):CPU 数据已在账、帧
        /// builder 开着。
        virtual void submit(const lux::asset::asset_id_t& id, SubmitDone done) = 0;

        /// 销毁一枚本域句柄(怎么销毁是域知识)。
        virtual void destroy(std::uint64_t handle_bits) noexcept = 0;

        /// 不可取消 RPC 在 owner stop 后仍由域 reaper 观察的回执数。
        /// 即使本域没有独立 reaper，也必须显式返回 0：新增域不能靠
        /// 默认实现静默绕过进程关停证明。
        [[nodiscard]] virtual std::size_t pendingReplies() const noexcept = 0;

        /// Stronger terminal proof than the wire/post count: no sender,
        /// trampoline, runtime capability, or domain transaction may still
        /// retain a borrowed host endpoint. Mandatory so a new domain cannot
        /// accidentally make close terminal before its owner graph joins.
        [[nodiscard]] virtual bool ownerControlsQuiescent() const noexcept = 0;

        /// Forced-close escape hatch for owner-creating RPCs which cannot be
        /// cancelled server-side.  The owning AsyncScope must already be
        /// stopped: implementations detach replies which have not reached the
        /// main queue and synchronously destroy every owner already known to
        /// their transaction state.  Replies already posted to the main queue
        /// remain counted by pendingReplies() and run in compensation-only
        /// mode; the caller must drain them before destroying the service.
        virtual void abandonPendingReplies() noexcept = 0;
    };

    class LUX_RUNTIME_RENDER_SCENE_PUBLIC RenderResourceService
    {
    private:
        class WaitDispatch;

        /// One main-thread lifetime generation for every subscription. Tickets
        /// observe it weakly, so neither a late reset nor address reuse can
        /// reach a destroyed (or replacement) service.
        struct SubscriptionControl
        {
            explicit SubscriptionControl(
                RenderResourceService& service) noexcept
                : service_(&service)
            {}

            void eraseWait(const lux::asset::asset_id_t& id,
                           std::uint64_t token) noexcept;
            [[nodiscard]] bool waitActive(
                const lux::asset::asset_id_t& id,
                std::uint64_t token) const noexcept;
            void eraseInvalidation(std::uint64_t token) noexcept;
            void invalidate() noexcept { service_ = nullptr; }
            [[nodiscard]] bool ownerAlive() const noexcept
            {
                return service_ != nullptr;
            }
            [[nodiscard]] WaitDispatch* dispatchHead() const noexcept
            {
                return dispatch_head_;
            }
            [[nodiscard]] WaitDispatch* pushDispatch(
                WaitDispatch* dispatch) noexcept
            {
                return std::exchange(dispatch_head_, dispatch);
            }
            void restoreDispatch(WaitDispatch* previous) noexcept
            {
                dispatch_head_ = previous;
            }

        private:
            /// Main-thread-confined observer; never read or written from a
            /// worker. The weak control, not this pointer, escapes the service.
            RenderResourceService* service_{nullptr};
            /// Lexical wait dispatches keep the control alive, so this stack
            /// remains valid even if a callback destroys the service itself.
            WaitDispatch* dispatch_head_{nullptr};
        };

    public:
        RenderResourceService();
        ~RenderResourceService();
        RenderResourceService(const RenderResourceService&)            = delete;
        RenderResourceService& operator=(const RenderResourceService&) = delete;

        // ── 域子服务(装配期挂载;引擎按包/宿主声明)────────────────────
        void addSubservice(std::unique_ptr<IRenderResourceSubservice> sub);
        [[nodiscard]] bool hasDomain(lux::ecs::EResourceDomain) const noexcept;
        /// 编排调用:转发给对应域子服务。无该域 = false(编排据此诊断)。
        bool submit(lux::ecs::EResourceDomain, const lux::asset::asset_id_t&,
                    IRenderResourceSubservice::SubmitDone done);
        /// Convert an upload reply into the unique owner immediately. A raw
        /// handle is never stored in the state table before this succeeds.
        [[nodiscard]] ResidentResourceAdoptResult adoptHandle(
            lux::ecs::EResourceDomain, std::uint64_t bits) noexcept;
        /// Acquire the domain-bound adoption/release capability before an RPC
        /// continuation leaves the service call. It may cross that callback
        /// boundary, but must be gone before terminal service destruction.
        [[nodiscard]] ResidentHandleReleaseEndpointResult releaseEndpoint(
            lux::ecs::EResourceDomain) const noexcept;
        /// True only when no lease and no RPC/router capability remains. This
        /// is an owner-thread close invariant, not a cross-thread query.
        [[nodiscard]] bool releaseQuiescent() const noexcept;
        [[nodiscard]] std::size_t liveLeases() const noexcept;
        [[nodiscard]] std::size_t releaseControlReferences() const noexcept;
        /// 依赖发现转发(无该域/无依赖 = 空)。
        [[nodiscard]] std::vector<ResourceDep>
        dependenciesOf(lux::ecs::EResourceDomain,
                       const lux::asset::asset_id_t&) const;
        [[nodiscard]] std::size_t pendingReplies(
            lux::ecs::EResourceDomain) const noexcept;
        void abandonPendingReplies(lux::ecs::EResourceDomain) noexcept;
        /// Aggregate every registered subservice. Residency close must not
        /// repeat an enum-domain list which silently becomes stale when a new
        /// owner-creating domain is installed.
        [[nodiscard]] std::size_t pendingReplies() const noexcept;
        void abandonPendingReplies() noexcept;
        [[nodiscard]] bool ownerControlsQuiescent() const noexcept;

        // ── 等待注册表(RAII;类型擦除)─────────────────────────────────
        using WaitFn = lux::cxx::move_only_function<
            void(std::uint64_t bits, const lux::ecs::ResourceFailure* fail)>;

        class WaitTicket
        {
        public:
            WaitTicket() noexcept = default;
            WaitTicket(WaitTicket&& o) noexcept { swap(o); }
            WaitTicket& operator=(WaitTicket&& o) noexcept
            { reset(); swap(o); return *this; }
            WaitTicket(const WaitTicket&)            = delete;
            WaitTicket& operator=(const WaitTicket&) = delete;
            ~WaitTicket() { reset(); }
            void reset() noexcept;   ///< 退订(幂等)
            [[nodiscard]] bool active() const noexcept;
        private:
            friend class RenderResourceService;
            void swap(WaitTicket& o) noexcept
            {
                std::swap(control_, o.control_);
                std::swap(id_, o.id_);
                std::swap(token_, o.token_);
            }
            std::weak_ptr<SubscriptionControl> control_;
            lux::asset::asset_id_t id_{};
            std::uint64_t          token_{0};
        };

        /// 登记「@p id 就绪/终败时调我」。回调恰好一次(触发即自动失效);
        /// 句柄析构先于触发 = 静默退订。句柄可安全晚于服务析构；此时
        /// weak control 已失效，reset/destruction 是确定的 no-op。
        [[nodiscard]] WaitTicket await(const lux::asset::asset_id_t& id, WaitFn fn);

        // ── 失效观察(RAII;每世界胶水订阅,收到后扫组件摘句柄重请求)──
        using InvalidationFn = lux::cxx::move_only_function<
            void(const std::vector<lux::asset::asset_id_t>&)>;

        class InvalidationTicket
        {
        public:
            InvalidationTicket() noexcept = default;
            InvalidationTicket(InvalidationTicket&& o) noexcept { swap(o); }
            InvalidationTicket& operator=(InvalidationTicket&& o) noexcept
            { reset(); swap(o); return *this; }
            InvalidationTicket(const InvalidationTicket&)            = delete;
            InvalidationTicket& operator=(const InvalidationTicket&) = delete;
            ~InvalidationTicket() { reset(); }
            void reset() noexcept;
        private:
            friend class RenderResourceService;
            void swap(InvalidationTicket& o) noexcept
            {
                std::swap(control_, o.control_);
                std::swap(token_, o.token_);
            }
            std::weak_ptr<SubscriptionControl> control_;
            std::uint64_t          token_{0};
        };

        [[nodiscard]] InvalidationTicket watchInvalidation(InvalidationFn fn);

        // ── 扇出(编排触发;主线程 safe point)─────────────────────────
        void notifyReady (const lux::asset::asset_id_t& id, std::uint64_t bits);
        void notifyFailed(const lux::asset::asset_id_t& id,
                          const lux::ecs::ResourceFailure& fail);
        void notifyInvalidated(const std::vector<lux::asset::asset_id_t>& ids);

        [[nodiscard]] std::size_t pendingWaits(const lux::asset::asset_id_t& id) const;

    private:
        friend class ResidentResourceLease;
        friend class ResidentHandleReleaseControl;

        struct Wait
        {
            std::uint64_t token{0};
            WaitFn        fn;
            bool          active{true};
        };
        /// Intrusive lexical stack for synchronous/re-entrant wait dispatch.
        /// It makes reset() able to deactivate a peer already moved out of the
        /// registry without allocating one heap node per waiter.
        class WaitDispatch final
        {
        public:
            WaitDispatch(std::shared_ptr<SubscriptionControl> control,
                         std::vector<Wait>& batch) noexcept
                : control_(std::move(control))
                , batch_(batch)
            {
                previous_ = control_->pushDispatch(this);
            }
            ~WaitDispatch() noexcept
            {
                control_->restoreDispatch(previous_);
            }

            WaitDispatch(const WaitDispatch&) = delete;
            WaitDispatch& operator=(const WaitDispatch&) = delete;

            void deactivate(std::uint64_t token) noexcept
            {
                for (auto& wait : batch_)
                    if (wait.token == token)
                        wait.active = false;
            }
            [[nodiscard]] bool active(std::uint64_t token) const noexcept
            {
                for (const auto& wait : batch_)
                    if (wait.token == token)
                        return wait.active;
                return false;
            }
            [[nodiscard]] WaitDispatch* previous() const noexcept
            {
                return previous_;
            }
            [[nodiscard]] bool ownerAlive() const noexcept
            {
                return control_->ownerAlive();
            }

        private:
            std::shared_ptr<SubscriptionControl> control_;
            WaitDispatch*                       previous_{nullptr};
            std::vector<Wait>&                   batch_;
        };
        struct InvalidationSubscription
        {
            std::uint64_t token{0};
            InvalidationFn fn;
            bool active{true};
        };

        bool destroyHandle(lux::ecs::EResourceDomain, std::uint64_t bits) noexcept;
        void eraseWait(const lux::asset::asset_id_t& id, std::uint64_t token) noexcept;
        [[nodiscard]] bool waitActive(
            const lux::asset::asset_id_t& id,
            std::uint64_t token) const noexcept;
        void eraseInvalidation(std::uint64_t token) noexcept;

        std::unordered_map<lux::ecs::EResourceDomain,
                           std::unique_ptr<IRenderResourceSubservice>> subservices_;
        std::unordered_map<lux::asset::asset_id_t, std::vector<Wait>> waits_;
        std::vector<std::shared_ptr<InvalidationSubscription>>         invalidation_;
        std::uint64_t next_token_{1};
        std::shared_ptr<SubscriptionControl> subscription_control_;
        /// Declared last so the service destructor can validate and invalidate
        /// it before subservices disappear. External holders never own those
        /// subservices or any render/session dependency.
        std::shared_ptr<ResidentHandleReleaseControl> release_control_;
    };

    // 嵌套类成员不随外层类 dllexport(MSVC)—— reset 内联在头里定义。
    inline void RenderResourceService::SubscriptionControl::eraseWait(
        const lux::asset::asset_id_t& id,
        std::uint64_t token) noexcept
    {
        if (service_ != nullptr)
            service_->eraseWait(id, token);
    }

    inline void RenderResourceService::SubscriptionControl::eraseInvalidation(
        std::uint64_t token) noexcept
    {
        if (service_ != nullptr)
            service_->eraseInvalidation(token);
    }

    inline bool RenderResourceService::SubscriptionControl::waitActive(
        const lux::asset::asset_id_t& id,
        std::uint64_t token) const noexcept
    {
        return service_ != nullptr && service_->waitActive(id, token);
    }

    inline bool RenderResourceService::WaitTicket::active() const noexcept
    {
        const auto control = control_.lock();
        return control && control->waitActive(id_, token_);
    }

    inline void RenderResourceService::WaitTicket::reset() noexcept
    {
        const auto control = control_.lock();
        if (control)
            control->eraseWait(id_, token_);
        control_.reset();
        id_    = {};
        token_ = 0;
    }

    inline void RenderResourceService::InvalidationTicket::reset() noexcept
    {
        const auto control = control_.lock();
        if (control)
            control->eraseInvalidation(token_);
        control_.reset();
        token_ = 0;
    }

} // namespace lux::runtime
