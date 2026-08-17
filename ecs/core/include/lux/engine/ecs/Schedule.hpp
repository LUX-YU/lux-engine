#pragma once
/**
 * @file Schedule.hpp
 * @brief Owner-thread ECS system ownership, topology and execution metadata.
 *
 * World owns data. Schedule owns behaviour. Concrete system identity is
 * captured before unique_ptr erasure with lux::cxx::type_hash/type_name.
 * Stable slots keep handles valid across topology recompilation; every handle
 * also carries its owning Schedule identity, and removal bumps the slot
 * generation. Stale and cross-scene observations therefore fail closed.
 *
 * No lock, RTTI or exception control flow is used. tick() is intentionally
 * sequential. executionBatches() is the stdexec adapter surface if parallel
 * execution is ever turned on. Per-system timing is written into preallocated
 * topology-sized storage; libraries never print profiler output.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/ecs/EcsCommandBuffer.hpp>
#include <lux/engine/ecs/SystemPhase.hpp>
#include <lux/engine/ecs/SystemUpdateContext.hpp>
#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/function/visibility.h>

#include <concepts>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace lux::ecs
{
    class World;
    class Schedule;
    class ScheduleMutationBatch;

    inline constexpr std::size_t kScheduleTracePhaseCount = 6u;

    struct ScheduleFrameTrace final
    {
        std::uint64_t frame_serial{0u};
        std::array<std::uint64_t, kScheduleTracePhaseCount>
            phase_nanoseconds{};
        std::uint64_t command_barrier_nanoseconds{0u};
    };

    struct ScheduleSystemFrameTrace final
    {
        std::uint64_t frame_serial{0u};
        SystemType system{};
        int phase{kPhaseSimulation};
        std::uint64_t wall_nanoseconds{0u};
    };

    template <class System>
    class SystemHandle final
    {
    public:
        SystemHandle() = default;

        [[nodiscard]] bool valid() const noexcept
        {
            return owner_identity_ != 0 && slot_ != kInvalidSlot &&
                   generation_ != 0;
        }

        [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
        [[nodiscard]] std::uint32_t slot() const noexcept { return slot_; }
        [[nodiscard]] std::uint32_t generation() const noexcept
        {
            return generation_;
        }

        friend bool operator==(const SystemHandle&, const SystemHandle&) = default;

    private:
        friend class Schedule;
        /// 装配期的交付方也要能造出句柄(commit 之后把槽位交还给调用方)。
        friend class ScheduleBuilder;
        static constexpr std::uint32_t kInvalidSlot = 0xFFFFFFFFu;

        constexpr SystemHandle(
            std::uint64_t owner_identity,
            std::uint32_t slot,
            std::uint32_t generation
        ) noexcept
            : owner_identity_(owner_identity),
              slot_(slot),
              generation_(generation)
        {
        }

        std::uint64_t owner_identity_{0};
        std::uint32_t slot_{kInvalidSlot};
        std::uint32_t generation_{0};
    };

    enum class EScheduleMutationError : std::uint8_t
    {
        NullSystem            = 0,
        DuplicateType         = 1,
        MutationDuringTick    = 2,
        InvalidHandle         = 3,
        TypeMismatch          = 4,
        RemovalUnsupported    = 5,
        MissingPrerequisite   = 6,
        TopologyCycle         = 7,
        RequiredByOtherSystem = 8,
        ReentrantMutation     = 9,
        SystemNotQuiescent    = 10,
    };

    [[nodiscard]] constexpr std::string_view toString(
        EScheduleMutationError error) noexcept
    {
        switch (error)
        {
        case EScheduleMutationError::NullSystem:          return "null_system";
        case EScheduleMutationError::DuplicateType:       return "duplicate_type";
        case EScheduleMutationError::MissingPrerequisite: return "missing_prerequisite";
        case EScheduleMutationError::TopologyCycle:       return "topology_cycle";
        case EScheduleMutationError::MutationDuringTick:  return "mutation_during_tick";
        case EScheduleMutationError::InvalidHandle:       return "invalid_handle";
        case EScheduleMutationError::TypeMismatch:        return "type_mismatch";
        case EScheduleMutationError::RemovalUnsupported:  return "removal_unsupported";
        case EScheduleMutationError::RequiredByOtherSystem:
            return "required_by_other_system";
        case EScheduleMutationError::ReentrantMutation:
            return "reentrant_mutation";
        case EScheduleMutationError::SystemNotQuiescent:
            return "system_not_quiescent";
        }
        return "unknown";
    }

    template <class T>
    using ScheduleMutationResult = lux::cxx::expected<T, EScheduleMutationError>;

    template <class System>
    using SystemAddResult = ScheduleMutationResult<SystemHandle<System>>;

    template <class System>
    using SystemRemoveResult = ScheduleMutationResult<void>;

    struct SystemHandleAny final
    {
        [[nodiscard]] bool valid() const noexcept
        {
            return owner_identity != 0u && slot != 0xFFFFFFFFu &&
                generation != 0u && type.hash != 0u;
        }

        std::uint64_t owner_identity{0u};
        std::uint32_t slot{0xFFFFFFFFu};
        std::uint32_t generation{0u};
        SystemType type{};
    };

    enum class EScheduleBatchError : std::uint8_t
    {
        NULL_SYSTEM,
        DUPLICATE_TYPE,
        SCHEDULE_BUSY,
        INVALID_HANDLE,
        REMOVAL_UNSUPPORTED,
        MISSING_PREREQUISITE,
        TOPOLOGY_CYCLE,
        REQUIRED_BY_OTHER_SYSTEM,
        WRONG_SCHEDULE,
        EMPTY_BATCH,
        SYSTEM_NOT_QUIESCENT
    };

    struct ScheduleBatchFailure final
    {
        EScheduleBatchError code{EScheduleBatchError::EMPTY_BATCH};
        SystemType subject{};
        SystemType related{};
    };

    template <class T>
    using ScheduleBatchResult = lux::cxx::expected<T, ScheduleBatchFailure>;

    class ScheduleMutationBatch final
    {
    public:
        ScheduleMutationBatch() = default;
        ScheduleMutationBatch(const ScheduleMutationBatch&) = delete;
        ScheduleMutationBatch& operator=(const ScheduleMutationBatch&) = delete;
        ScheduleMutationBatch(ScheduleMutationBatch&&) noexcept = default;
        ScheduleMutationBatch& operator=(ScheduleMutationBatch&&) noexcept =
            default;

        template <class System>
            requires std::derived_from<System, ISystem> &&
                     (!std::same_as<System, ISystem>)
        [[nodiscard]] lux::cxx::expected<void, EScheduleBatchError> add(
            std::unique_ptr<System> system,
            int phase = kPhaseSimulation)
        {
            if (!system)
            {
                return lux::cxx::unexpected(
                    EScheduleBatchError::NULL_SYSTEM);
            }
            constexpr auto type = systemType<System>();
            for (const auto& node : nodes_)
            {
                if (sameSystemType(node.type, type))
                {
                    return lux::cxx::unexpected(
                        EScheduleBatchError::DUPLICATE_TYPE);
                }
            }
            nodes_.push_back(Node{std::move(system), type, phase});
            return {};
        }

        [[nodiscard]] bool empty() const noexcept { return nodes_.empty(); }
        [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }

    private:
        friend class Schedule;
        struct Node final
        {
            std::unique_ptr<ISystem> system;
            SystemType type{};
            int phase{kPhaseSimulation};
        };
        std::vector<Node> nodes_;
    };

    struct InstalledSystemBatch final
    {
        InstalledSystemBatch() = default;
        InstalledSystemBatch(const InstalledSystemBatch&) = delete;
        InstalledSystemBatch& operator=(const InstalledSystemBatch&) = delete;
        InstalledSystemBatch(InstalledSystemBatch&&) noexcept = default;
        InstalledSystemBatch& operator=(InstalledSystemBatch&&) noexcept =
            default;

        [[nodiscard]] bool valid() const noexcept
        {
            return schedule_identity != 0u && !handles.empty();
        }

        std::uint64_t schedule_identity{0u};
        std::vector<SystemHandleAny> handles;
    };

    struct SystemBatchCloseState final
    {
        bool valid{false};
        bool complete{false};
        bool owner_work_pending{false};
        std::size_t pending_systems{0u};
    };

    class LUX_FUNCTION_PUBLIC Schedule final
    {
    public:
        explicit Schedule(World& world) noexcept;
        ~Schedule() noexcept;

        Schedule(const Schedule&)            = delete;
        Schedule& operator=(const Schedule&) = delete;
        Schedule(Schedule&&)                 = delete;
        Schedule& operator=(Schedule&&)      = delete;

        /// Low-level single-node safe-point mutation. The candidate is
        /// analysed with every live node before ownership transfer/onAdded.
        /// Use ScheduleBuilder when several nodes satisfy one another's hard
        /// prerequisites or must become visible atomically.
        template <class System>
            requires std::derived_from<System, ISystem> &&
                     (!std::same_as<System, ISystem>)
        [[nodiscard]] SystemAddResult<System> addSystem(
            std::unique_ptr<System> system,
            int phase = kPhaseSimulation)
        {
            constexpr SystemType type = systemType<System>();
            auto added = addSystemErased(std::move(system), type, phase);
            if (!added)
                return lux::cxx::unexpected(added.error());
            return SystemHandle<System>{
                identity_,
                added->first,
                added->second,
            };
        }

        [[nodiscard]] bool hasSystem(SystemType type) const noexcept;

        [[nodiscard]] ScheduleBatchResult<InstalledSystemBatch> installBatch(
            ScheduleMutationBatch&& batch);

        /// Install one topology mutation while retaining one handle batch per
        /// logical contribution. The whole topology is validated before any
        /// system is adopted or receives onAdded().
        [[nodiscard]] ScheduleBatchResult<std::vector<InstalledSystemBatch>>
        installBatchPartitioned(
            ScheduleMutationBatch&& batch,
            std::span<const std::size_t> partition_sizes);

        [[nodiscard]] lux::cxx::expected<void, ScheduleBatchFailure>
        removeBatch(InstalledSystemBatch&& batch);

        /// Close is deliberately separate from removal: systems keep their
        /// command producers and update slots while bounded retirement and
        /// late completions drain through normal Schedule safe points.
        [[nodiscard]] bool
        requestBatchClose(
            const InstalledSystemBatch& batch,
            SystemCloseProgressSink progress = {}) noexcept;
        [[nodiscard]] SystemBatchCloseState
        batchCloseState(const InstalledSystemBatch& batch) const noexcept;

        /// Requests close in reverse execution order so consumers release
        /// tickets before their providers. Systems stay installed and all
        /// subsequent work still flows through tick()/the unique barrier.
        void requestClose(SystemCloseProgressSink progress = {}) noexcept;
        [[nodiscard]] SystemBatchCloseState closeState() const noexcept;

        template <class System>
        [[nodiscard]] bool hasSystem() const noexcept
        {
            return hasSystem(systemType<System>());
        }

        template <class System>
        [[nodiscard]] System* get(SystemHandle<System> handle) noexcept
        {
            if (handle.owner_identity_ != identity_)
                return nullptr;
            auto* slot = findSlot(handle.slot_, handle.generation_);
            if (!slot || !sameSystemType(slot->type, systemType<System>()))
                return nullptr;
            return static_cast<System*>(slot->system.get());
        }

        template <class System>
        [[nodiscard]] const System* get(SystemHandle<System> handle) const noexcept
        {
            if (handle.owner_identity_ != identity_)
                return nullptr;
            const auto* slot = findSlot(handle.slot_, handle.generation_);
            if (!slot || !sameSystemType(slot->type, systemType<System>()))
                return nullptr;
            return static_cast<const System*>(slot->system.get());
        }

        /// Remove an opt-in system at an owner-thread safe point.
        ///
        /// Success means `onRemoved` completed synchronously, the unique owner
        /// was destroyed, and the handle generation was retired. Ownership
        /// never escapes this function. Systems borrowing scene services or
        /// owning remote state remain fixed-lifetime by default and return
        /// `RemovalUnsupported`. A removable system is still retained when
        /// another live node declares its type as a hard prerequisite.
        template <class System>
        [[nodiscard]] SystemRemoveResult<System> removeSystem(
            SystemHandle<System> handle)
        {
            if (operation_state_ == EOperationState::TopologyMutation)
            {
                recordOperationRejection();
                return lux::cxx::unexpected(
                    EScheduleMutationError::ReentrantMutation);
            }
            if (operation_state_ != EOperationState::Idle)
            {
                recordOperationRejection();
                return lux::cxx::unexpected(
                    EScheduleMutationError::MutationDuringTick);
            }

            if (handle.owner_identity_ != identity_)
                return lux::cxx::unexpected(
                    EScheduleMutationError::InvalidHandle);

            auto* slot = findSlot(handle.slot_, handle.generation_);
            if (!slot)
                return lux::cxx::unexpected(
                    EScheduleMutationError::InvalidHandle);
            if (!sameSystemType(slot->type, systemType<System>()))
                return lux::cxx::unexpected(
                    EScheduleMutationError::TypeMismatch);
            if (!slot->descriptor.supports_dynamic_removal)
                return lux::cxx::unexpected(
                    EScheduleMutationError::RemovalUnsupported);
            if (!slot->system->closeComplete())
                return lux::cxx::unexpected(
                    EScheduleMutationError::SystemNotQuiescent);

            OperationGuard guard{*this, EOperationState::TopologyMutation};
            if (auto preflight = preflightRemoval(*slot); !preflight)
                return lux::cxx::unexpected(preflight.error());

            slot->system->onRemoved(SystemRemovalContext{world_.registry()});
            slot->system.reset();
            retireSlot(*slot);
            return {};
        }

        struct SortReport
        {
            std::vector<SystemType> unknown;
            std::vector<SystemType> cycle;
            std::vector<SystemType> duplicate;
            std::vector<std::pair<SystemType, SystemType>> missing_prereq;
            /// compile() was called from a lifecycle/update/barrier callback.
            /// No cache or topology state was touched in that case.
            bool operation_rejected{false};

            [[nodiscard]] bool valid() const noexcept
            {
                return !operation_rejected && cycle.empty() && duplicate.empty() &&
                       missing_prereq.empty();
            }
        };

        struct ExecutionBatch
        {
            std::size_t first{};
            std::size_t count{};
        };

        /// Compile stable topology and conservative parallel candidates.
        /// Missing ordering targets are non-fatal; missing prerequisites and
        /// precise SCC cycles (including self cycles) make the report invalid.
        /// Re-entry from a lifecycle/update/barrier callback is rejected
        /// without touching caches and sets SortReport::operation_rejected.
        [[nodiscard]] SortReport compile();

        /// Sequential execution of the compiled order, followed by exactly one
        /// command barrier. Re-entry is ignored and counted as a protocol
        /// rejection; tick_index is not advanced.
        void tick(
            float dt,
            int through_phase = kPhaseLast,
            std::uint64_t frame_serial = 0u);

        [[nodiscard]] const ScheduleFrameTrace& latestFrameTrace() const
            noexcept
        {
            return latest_frame_trace_;
        }

        /// Owner-thread view, valid until topology changes or the next tick.
        /// compile() sizes the backing store so tick() never allocates it.
        [[nodiscard]] std::span<const ScheduleSystemFrameTrace>
        latestSystemFrameTrace() const noexcept
        {
            return latest_system_frame_trace_;
        }

        /// 应用一次结构命令 barrier —— **全项目唯一的 ECS apply 点**。
        ///
        /// `tick()` 末尾自动调一次。公开出来只为一种场合:装配收官(bring-up 的
        /// settle)要在第一次 tick 之前,把 `onAdded` 里折入存量入的那批命令发出去。
        ///
        /// 语义:先把所有分片整体换出,再按「编译后的节点序 × 分片内序号」应用。
        /// 应用期间新入队的命令落在**下一轮** —— 命令里发信号、信号里再入队是完全
        /// 正常的链条,就地消费会变成自喂循环(而且是只在特定组件组合下才发作的那种)。
        /// 生命周期/update/command apply 内的重入会在任何换出前拒绝并计数。
        /// Commands deferred by an observer in the previous barrier and those
        /// produced by the current update are armed/prepared by tick() before
        /// apply. Composition-root settle loops which call barriers directly
        /// must call prepareCommandBarrier() between them.
        void applyCommandBarrier();

        /// Owner-thread pre-entry phase. Compiles dirty topology, may grow
        /// registry storage and obtains reservation blocks;
        /// applyCommandBarrier() requires that completed preflight and never
        /// performs any of those operations itself.
        [[nodiscard]] bool prepareCommandBarrier() noexcept;

        /// Returns an empty span and records a protocol rejection when queried
        /// from another active Schedule operation.
        [[nodiscard]] std::span<const ExecutionBatch> executionBatches();
        [[nodiscard]] std::size_t systemCount() const noexcept;

        /// 生产者已消失(槽位退休或被别的系统占了)而被丢弃的命令数。
        /// fail closed 要**可见**,不是静默扔掉。
        [[nodiscard]] std::uint64_t droppedStaleCommands() const noexcept
        {
            return dropped_stale_commands_;
        }

        /// 自建立以来的 tick 计数。
        [[nodiscard]] std::uint64_t tickIndex() const noexcept
        {
            return tick_index_;
        }

        /// Owner-thread protocol violations rejected before side effects.
        /// This stays active in every build configuration (unlike assert).
        [[nodiscard]] std::uint64_t rejectedOperationCount() const noexcept
        {
            return rejected_operations_;
        }

        [[nodiscard]] World& world() noexcept { return world_; }
        [[nodiscard]] const World& world() const noexcept { return world_; }

    private:
        friend class ScheduleBuilder;

        struct SystemDescriptorSnapshot;

        /// The single mutation boundary for direct additions. Descriptor
        /// capture and activation stay inside the core DLL instead of leaking
        /// private snapshot machinery through every public template instance.
        [[nodiscard]] lux::cxx::expected<
            std::pair<std::uint32_t, std::uint32_t>,
            EScheduleMutationError>
        addSystemErased(
            std::unique_ptr<ISystem> system,
            SystemType              type,
            int                     phase
        );

        /// 类型擦除的收下路径:冻结声明、找槽并写所有权，返回 {slot, generation}。
        /// `addSystem` 与 `ScheduleBuilder::commit` 共用它 —— 插入只有一份实现,
        /// 校验各自在调用前做完(builder 要在**交付前**全部查完,见它的头注释)。
        [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> adoptSystem(
            std::unique_ptr<ISystem>   system,
            SystemType                type,
            int                       phase,
            SystemDescriptorSnapshot descriptor
        );
        void activateSystem(std::uint32_t slot);

        /// Scheduling declarations are immutable after ownership transfer.
        /// Freezing them before any onAdded callback means preflight, compile
        /// and later incremental mutations always inspect the same graph.
        struct SystemDescriptorSnapshot
        {
            std::vector<SystemType>              prerequisites;
            std::vector<SystemType>              runs_after;
            std::vector<SystemType>              runs_before;
            std::vector<ISystem::ResourceAccess> resources;
            bool access_complete{false};
            bool access_structural{true};
            bool supports_dynamic_removal{false};

            void capture(const ISystem& system);
            [[nodiscard]] ISystem::AccessDeclaration
            accessDeclaration() const noexcept
            {
                return {
                    .resources = resources,
                    .complete = access_complete,
                    .structural = access_structural,
                };
            }
        };

        struct Slot
        {
            std::unique_ptr<ISystem> system;
            SystemDescriptorSnapshot descriptor;
            SystemType               type{};
            int                      phase{kPhaseSimulation};
            std::size_t              seq{0};
            std::uint32_t            generation{1};
            /// 本节点的单生产者命令分片。归 Schedule 所有,与槽位同生共死 ——
            /// 系统只拿一个 writer 凭据(见 EcsCommandBuffer.hpp)。
            ///
            /// ⚠️ **必须是间接的**。writer 里存的是分片地址,而 `slots_` 是
            /// `std::vector`:再装一个系统就可能扩容搬家,先装那个系统的 writer 当场
            /// 变成悬垂指针 —— 装配顺序不同则症状不同,是最难查的那一类。
            /// (设计稿 §7.5:不把 vector 元素地址当身份。)
            std::unique_ptr<EcsCommandBuffer> commands;
        };

        [[nodiscard]] Slot* findSlot(
            std::uint32_t index, std::uint32_t generation) noexcept;
        [[nodiscard]] const Slot* findSlot(
            std::uint32_t index, std::uint32_t generation) const noexcept;
        void retireSlot(Slot& slot) noexcept;

        enum class EOperationState : std::uint8_t
        {
            Idle,
            TopologyMutation,
            Ticking,
            ApplyingBarrier,
        };

        class OperationGuard final
        {
        public:
            OperationGuard(
                Schedule&       schedule,
                EOperationState operation
            ) noexcept
                : schedule_(schedule), previous_(schedule.operation_state_)
            {
                schedule_.operation_state_ = operation;
            }

            ~OperationGuard() noexcept
            {
                schedule_.operation_state_ = previous_;
            }

            OperationGuard(const OperationGuard&) = delete;
            OperationGuard& operator=(const OperationGuard&) = delete;

        private:
            Schedule&       schedule_;
            EOperationState previous_;
        };

        [[nodiscard]] lux::cxx::expected<void, EScheduleMutationError>
        preflightAddition(
            const SystemDescriptorSnapshot& descriptor,
            SystemType                     type,
            int                            phase
        ) const;
        [[nodiscard]] lux::cxx::expected<void, EScheduleMutationError>
        preflightRemoval(const Slot& removed) const;
        [[nodiscard]] SortReport analyzeTopologyMutation(
            const Slot*    excluded,
            const SystemDescriptorSnapshot* addition,
            SystemType     addition_type,
            int            addition_phase
        ) const;

        [[nodiscard]] bool mutationLocked() const noexcept
        {
            return operation_state_ != EOperationState::Idle;
        }

        void recordOperationRejection() noexcept
        {
            ++rejected_operations_;
        }

        [[nodiscard]] EcsCommandWriter makeWriter(Slot& slot);

        void applyShard(EcsCommandBuffer& shard, Slot& slot);

        World&                        world_;
        const std::uint64_t           identity_;
        std::vector<Slot>             slots_;
        std::vector<std::size_t>      execution_order_;
        std::vector<ExecutionBatch>   execution_batches_;
        std::vector<ScheduleSystemFrameTrace> latest_system_frame_trace_;
        std::vector<SystemType>       rejected_duplicates_;
        /// barrier 的换出缓冲,按槽位索引。作为成员而不是局部变量:每帧一次
        /// barrier,不该每次都重新分配(设计稿 §8.3「热路径不分配」)。
        std::vector<EcsCommandBuffer> staging_;
        EcsCommandStorageReservationPlan command_storage_plan_;
        std::size_t                   next_seq_{0};
        std::uint64_t                 tick_index_{0};
        std::uint64_t                 dropped_stale_commands_{0};
        std::uint64_t                 rejected_operations_{0};
        ScheduleFrameTrace            latest_frame_trace_{};
        bool                          compiled_{true};
        EOperationState               operation_state_{EOperationState::Idle};
    };

} // namespace lux::ecs
