#pragma once
/**
 * @file ScheduleBuilder.hpp
 * @brief 尚未发布的 schedule —— 装配期唯一可写的那一面。
 *
 * ── 它解决什么 ──────────────────────────────────────────────────────────
 *
 * `Schedule::addSystem` 收下系统后**立即**调 `onAdded`,而 `onAdded` 会连信号、
 * 折入存量、注册世界级表项。装配是一串 addSystem,所以第 5 个失败时,前 4 个的
 * 观察者**已经挂在世界上了** —— 调用方拿到一个失败返回,世界却已经被改过一半。
 * 今天靠「往未发布的 candidate World 上装」绕过去,但那只是把半成品藏起来,
 * 不是让它不发生。
 *
 * builder 把「构造 + 收集」与「校验 + 交付」分成两段:装配期先把系统都造出来
 * 收进 builder(此时世界一动不动),`commit()` 校验通过后才一次性交给 live
 * schedule 并逐个 `onAdded`。任何一步失败,**一个系统都没进去、一个 onAdded 都
 * 没跑**,builder 析构时把它们一起带走。
 *
 * ── 校验分工(不要在这里overclaim)────────────────────────────────────────
 *
 * `commit()` **交付前**在 pending ∪ live 的并集上运行与 `Schedule::compile()`
 * 相同的纯拓扑分析：空系统、重复类型、硬前置缺失与 ordering cycle 任一失败，
 * 都不会调用 `onAdded` 或发布 staged services。悬空的 before/after 是有意允许的
 * 可选边，仍作为非致命 `SortReport::unknown` 由宿主在正式 compile 时报告。
 *
 * 分析算法只有 `detail::analyzeScheduleTopology()` 一份；builder 只构造节点快照，
 * 不复制拓扑规则，也不缓存预检顺序。通过预检后，整批声明先冻结进 Schedule slot，
 * 然后才运行任何 `onAdded`；正式 `compile()` 只读这些冻结声明，给后续合法增删重建
 * 执行序与批次。
 *
 * ── 为什么 descriptor 不是 emplace 的实参 ────────────────────────────────
 *
 * `emplace<T>(phase, args...)` 只收相位与构造实参。「我读哪些组件、写哪些资源、
 * 能不能上 worker、要哪些渲染 feature」全部由**类型自己**回答(`ISystem` 上的虚
 * 声明面)。这样以后往 descriptor 里加字段时,包代码一行都不用改 —— 而如果让包
 * 在 emplace 处写 descriptor,每加一个字段就要扫一遍所有包。
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/SceneServices.hpp>
#include <lux/engine/function/visibility.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace lux::ecs
{
    enum class EScheduleBuildError : std::uint8_t
    {
        NullSystem = 0,
        /// 同一个类型在本 builder 里被收了两次。
        DuplicateType = 1,
        /// 已经 commit 过,builder 不再接受新节点。
        AlreadyCommitted = 2,
        /// commit 正在运行；生命周期回调不得继续扩张同一批节点。
        CommitInProgress = 3,
    };

    [[nodiscard]] constexpr std::string_view toString(
        EScheduleBuildError error) noexcept
    {
        switch (error)
        {
        case EScheduleBuildError::NullSystem:       return "null_system";
        case EScheduleBuildError::DuplicateType:    return "duplicate_type";
        case EScheduleBuildError::AlreadyCommitted: return "already_committed";
        case EScheduleBuildError::CommitInProgress: return "commit_in_progress";
        }
        return "unknown";
    }

    enum class EScheduleCommitError : std::uint8_t
    {
        /// 与 live schedule 里已有的系统撞类型。
        DuplicateType = 0,
        /// 声明的前置系统在 pending ∪ live 的并集里都不存在。
        MissingPrerequisite = 1,
        /// live schedule 正在 tick、barrier 或另一笔拓扑变更。
        ScheduleBusy = 2,
        /// 已经 commit 过。
        AlreadyCommitted = 3,
        /// pending ∪ live 的显式 before/after 声明形成环。
        TopologyCycle = 4,
        /// staged service 与 commit 前新进入 base 的同型服务冲突。
        ServiceConflict = 5,
    };

    [[nodiscard]] constexpr std::string_view toString(
        EScheduleCommitError error) noexcept
    {
        switch (error)
        {
        case EScheduleCommitError::DuplicateType:       return "duplicate_type";
        case EScheduleCommitError::MissingPrerequisite: return "missing_prerequisite";
        case EScheduleCommitError::TopologyCycle:       return "topology_cycle";
        case EScheduleCommitError::ScheduleBusy:        return "schedule_busy";
        case EScheduleCommitError::AlreadyCommitted:    return "already_committed";
        case EScheduleCommitError::ServiceConflict:     return "service_conflict";
        }
        return "unknown";
    }

    struct ScheduleCommitFailure
    {
        EScheduleCommitError error{EScheduleCommitError::DuplicateType};
        /// 出事的那个系统的类型名(诊断用;身份仍是 type token)。
        std::string_view     subject{};
        /// MissingPrerequisite 时:缺的那个前置的类型名。
        std::string_view     detail{};
        /// Structured preflight evidence. Cycle failures retain every exact
        /// SCC member; missing-prerequisite failures retain every missing pair.
        Schedule::SortReport topology{};
    };

    using ScheduleCommitResult =
        lux::cxx::expected<void, ScheduleCommitFailure>;

    struct ScheduleBuilderInstalledRange final
    {
        InstalledSystemBatch systems;
        InstalledSceneServiceBatch services;
    };

    class ScheduleBuilder;

    /// An unpublished node identity. The token owns and borrows nothing: the
    /// originating builder must be supplied explicitly to get() before commit
    /// or handle() after commit. It can therefore escape a scope without
    /// turning into a dangling builder/system pointer (although using it with a
    /// different builder fails closed through an assembly-generation check).
    template <class System>
    class PendingSystemToken final
    {
    public:
        PendingSystemToken() = default;

        [[nodiscard]] bool valid() const noexcept
        {
            return builder_identity_ != 0 && node_identity_ != 0 &&
                   index_ != kInvalidIndex;
        }
        [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

    private:
        friend class ScheduleBuilder;
        static constexpr std::size_t kInvalidIndex =
            static_cast<std::size_t>(-1);

        PendingSystemToken(
            std::uint64_t builder_identity,
            std::uint64_t node_identity,
            std::size_t   index
        ) noexcept
            : builder_identity_(builder_identity),
              node_identity_(node_identity),
              index_(index)
        {
        }

        std::uint64_t builder_identity_{0};
        std::uint64_t node_identity_{0};
        std::size_t   index_{kInvalidIndex};
    };

    class LUX_FUNCTION_PUBLIC ScheduleBuilder final
    {
    private:
        enum class EState : std::uint8_t
        {
            Idle,
            Committing,
            RollingBack,
            Committed,
        };

        class OperationStateGuard final
        {
        public:
            OperationStateGuard(EState& state, EState entered) noexcept
                : state_(state), previous_(state), entered_(entered)
            {
                state_ = entered_;
            }

            ~OperationStateGuard() noexcept
            {
                if (state_ == entered_)
                    state_ = previous_;
            }

            OperationStateGuard(const OperationStateGuard&) = delete;
            OperationStateGuard& operator=(const OperationStateGuard&) = delete;

        private:
            EState& state_;
            EState  previous_;
            EState  entered_;
        };

    public:
        class Checkpoint final
        {
        public:
            Checkpoint() = default;

            [[nodiscard]] bool valid() const noexcept
            {
                return builder_identity_ != 0u;
            }

        private:
            friend class ScheduleBuilder;

            std::uint64_t builder_identity_{0u};
            std::size_t pending_count_{0u};
            std::size_t service_registrations_{0u};
            std::size_t deferred_service_edits_{0u};
        };

        ScheduleBuilder(
            Schedule&      schedule,
            SceneServices& services
        ) noexcept
            : schedule_(schedule),
              services_(services),
              identity_(allocateIdentity())
        {
        }
        ~ScheduleBuilder() noexcept;

        ScheduleBuilder(const ScheduleBuilder&)            = delete;
        ScheduleBuilder& operator=(const ScheduleBuilder&) = delete;
        // Nodes and the service transaction contain stable borrows into this
        // assembly, so the builder itself remains a fixed-address owner.
        ScheduleBuilder(ScheduleBuilder&&)                 = delete;
        ScheduleBuilder& operator=(ScheduleBuilder&&)      = delete;

        [[nodiscard]] World& world() noexcept { return schedule_.world(); }
        [[nodiscard]] const World& world() const noexcept { return schedule_.world(); }

        // ★ 这里**没有** `emplace<System>(args...)`。它与
        //   `emplace<System>(phase, args...)` 天生打架:构造实参第一个是 int 的
        //   系统会被静默当成「指定了相位」。收一个造好的 unique_ptr 没有这个歧义,
        //   而且与既有的 `Schedule::addSystem` 形状一致,迁移是机械的。
        template <class System>
            requires std::derived_from<System, ISystem> &&
                     (!std::same_as<System, ISystem>)
        [[nodiscard]] lux::cxx::expected<
            PendingSystemToken<System>,
            EScheduleBuildError>
        add(std::unique_ptr<System> system, int phase = kPhaseSimulation)
        {
            if (state_ != EState::Idle && state_ != EState::Committed)
                return lux::cxx::unexpected<EScheduleBuildError>(
                    EScheduleBuildError::CommitInProgress);
            if (state_ == EState::Committed)
                return lux::cxx::unexpected<EScheduleBuildError>(
                    EScheduleBuildError::AlreadyCommitted);
            if (!system)
                return lux::cxx::unexpected<EScheduleBuildError>(
                    EScheduleBuildError::NullSystem);

            constexpr SystemType type = systemType<System>();
            for (const auto& node : nodes_)
                if (sameSystemType(node.type, type))
                    return lux::cxx::unexpected<EScheduleBuildError>(
                        EScheduleBuildError::DuplicateType);

            auto node_identity = next_node_identity_++;
            if (node_identity == 0)
                node_identity = next_node_identity_++;
            nodes_.push_back(Node{
                std::move(system),
                type,
                phase,
                node_identity,
            });
            handles_.push_back(RawHandle{});
            return PendingSystemToken<System>{
                identity_,
                node_identity,
                nodes_.size() - 1,
            };
        }

        [[nodiscard]] std::size_t pendingCount() const noexcept
        {
            return nodes_.size();
        }

        [[nodiscard]] bool committed() const noexcept
        {
            return state_ == EState::Committed;
        }

        [[nodiscard]] bool committedTo(
            const Schedule& schedule,
            const SceneServices& services) const noexcept
        {
            return committed() && &schedule_ == &schedule &&
                &services_.base_ == &services;
        }

        /// Capture an unpublished assembly savepoint. A caller which builds a
        /// dependency closure in several callbacks can restore this point on
        /// any ordinary validation failure without publishing a partial
        /// system/service set.
        [[nodiscard]] Checkpoint checkpoint() const noexcept
        {
            if (state_ != EState::Idle)
                return {};
            const auto service_checkpoint = services_.checkpoint();
            Checkpoint result;
            result.builder_identity_ = identity_;
            result.pending_count_ = nodes_.size();
            result.service_registrations_ =
                service_checkpoint.registrations;
            result.deferred_service_edits_ =
                service_checkpoint.deferred_edits;
            return result;
        }

        /// Restore a savepoint captured from this builder. Destruction order
        /// remains deferred edits -> systems -> services, matching complete
        /// builder abandonment.
        [[nodiscard]] bool rollbackTo(Checkpoint checkpoint) noexcept
        {
            if (state_ != EState::Idle ||
                checkpoint.builder_identity_ != identity_ ||
                checkpoint.pending_count_ > nodes_.size())
            {
                return false;
            }
            rollbackAssemblyTo(
                checkpoint.pending_count_,
                SceneServiceTransaction::Checkpoint{
                    checkpoint.service_registrations_,
                    checkpoint.deferred_service_edits_});
            return true;
        }

        /// The unpublished service overlay belonging to the same assembly
        /// transaction as the pending systems.
        [[nodiscard]] SceneServiceTransaction& services() noexcept
        {
            return services_;
        }
        [[nodiscard]] const SceneServiceTransaction& services() const noexcept
        {
            return services_;
        }

        /// Base table behind the unpublished service overlay. Runtime
        /// contribution assembly uses this only to construct its generic
        /// build context; lookups still route through services() so staged
        /// values win.
        [[nodiscard]] SceneServices& baseServices() noexcept
        {
            return services_.base_;
        }

        /// 校验通过则一次性交付:逐个移进 schedule 并调 `onAdded`。
        /// 失败时 schedule **一点没动**,节点仍归 builder。
        [[nodiscard]] ScheduleCommitResult commit();

        /// Claim one disjoint logical ownership range after commit. This lets
        /// an outer unpublished transaction retain per-contribution removal
        /// tokens without installing those systems and services a second time.
        [[nodiscard]] bool canClaimCommittedRange(
            Checkpoint first,
            Checkpoint last) const noexcept;
        [[nodiscard]] std::optional<ScheduleBuilderInstalledRange>
        claimCommittedRange(Checkpoint first, Checkpoint last) noexcept;

        /// Borrow a staged instance before commit. The returned pointer is
        /// bounded by this builder and becomes null once ownership is handed
        /// to Schedule. Token/type mismatches fail closed.
        template <class System>
        [[nodiscard]] System* get(PendingSystemToken<System> token) noexcept
        {
            if (state_ != EState::Idle ||
                token.builder_identity_ != identity_ ||
                token.index_ >= nodes_.size())
                return nullptr;
            auto& node = nodes_[token.index_];
            if (!node.system || node.identity != token.node_identity_ ||
                !sameSystemType(node.type, systemType<System>()))
                return nullptr;
            return static_cast<System*>(node.system.get());
        }

        template <class System>
        [[nodiscard]] const System* get(
            PendingSystemToken<System> token
        ) const noexcept
        {
            if (state_ != EState::Idle ||
                token.builder_identity_ != identity_ ||
                token.index_ >= nodes_.size())
                return nullptr;
            const auto& node = nodes_[token.index_];
            if (!node.system || node.identity != token.node_identity_ ||
                !sameSystemType(node.type, systemType<System>()))
                return nullptr;
            return static_cast<const System*>(node.system.get());
        }

        /// Resolve the stable slot handle after commit. Before commit, or for
        /// an invalid/mismatched token, this returns an invalid handle.
        template <class System>
        [[nodiscard]] SystemHandle<System> handle(
            PendingSystemToken<System> token
        ) const noexcept
        {
            if (state_ != EState::Committed ||
                token.builder_identity_ != identity_ ||
                token.index_ >= handles_.size() ||
                token.index_ >= nodes_.size() ||
                nodes_[token.index_].identity != token.node_identity_ ||
                !sameSystemType(
                    nodes_[token.index_].type,
                    systemType<System>()))
                return SystemHandle<System>{};
            const auto raw = handles_[token.index_];
            if (raw.generation == 0)
                return SystemHandle<System>{};
            return SystemHandle<System>{
                raw.owner_identity,
                raw.slot,
                raw.generation,
            };
        }

    private:
        [[nodiscard]] static std::uint64_t allocateIdentity() noexcept;

        /// Restore one unpublished assembly checkpoint. The implementation
        /// keeps builder and Schedule mutation guards active while it destroys
        /// deferred closures, then systems, then the services they may borrow.
        /// Plan failure and plain builder abandonment deliberately share this
        /// one rollback primitive so destructor reentry has no unguarded path.
        void rollbackAssemblyTo(
            std::size_t                         pending_count,
            SceneServiceTransaction::Checkpoint services_checkpoint
        ) noexcept;

        struct Node
        {
            std::unique_ptr<ISystem> system;
            SystemType               type{};
            int                      phase{kPhaseSimulation};
            std::uint64_t            identity{0};
        };

        struct RawHandle
        {
            std::uint64_t owner_identity{0};
            std::uint32_t slot{0};
            std::uint32_t generation{0};
        };

        // Declared before nodes_: on a failed build, reverse destruction tears
        // systems down before the staged services they may borrow.
        Schedule&               schedule_;
        SceneServiceTransaction services_;
        std::vector<Node>        nodes_;
        std::vector<RawHandle>   handles_;
        std::vector<bool>        handles_claimed_;
        std::uint64_t            identity_{0};
        std::uint64_t            next_node_identity_{1};
        EState                   state_{EState::Idle};
    };

} // namespace lux::ecs
