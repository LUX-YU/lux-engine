#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/HierarchyIndex.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/detail/ScheduleTopology.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>

namespace lux::ecs
{
    namespace detail
    {
        EcsCommandStorageReservationPlan*&
        activeCommandStorageReservationPlan() noexcept
        {
            static thread_local EcsCommandStorageReservationPlan* active =
                nullptr;
            return active;
        }
    } // namespace detail

    namespace
    {
        [[nodiscard]] std::uint64_t allocateScheduleIdentity() noexcept
        {
            // Identity only rejects handles crossing owner boundaries. It
            // carries no publication semantics, so relaxed ordering is enough.
            static std::atomic<std::uint64_t> next{1};
            auto identity = next.fetch_add(1, std::memory_order_relaxed);
            if (identity == 0)
                identity = next.fetch_add(1, std::memory_order_relaxed);
            return identity;
        }
    } // namespace

    EcsCommandWriter Schedule::makeWriter(Slot& slot)
    {
        if (!slot.commands)
            slot.commands = std::make_unique<EcsCommandBuffer>();
        return EcsCommandWriter{
            *slot.commands,
            slot.type,
            slot.generation,
            world_.registry()};
    }

    void Schedule::SystemDescriptorSnapshot::capture(const ISystem& system)
    {
        const auto copyTypes = [](
            std::vector<SystemType>&       destination,
            std::span<const SystemType>    source)
        {
            destination.assign(source.begin(), source.end());
        };

        copyTypes(prerequisites, system.prerequisites());
        copyTypes(runs_after, system.runsAfter());
        copyTypes(runs_before, system.runsBefore());

        const auto access = system.accessDeclaration();
        resources.assign(access.resources.begin(), access.resources.end());
        access_complete = access.complete;
        access_structural = access.structural;

        supports_dynamic_removal = system.supportsDynamicRemoval();
    }

    Schedule::Schedule(World& world) noexcept
        : world_(world), identity_(allocateScheduleIdentity())
    {
    }

    Schedule::~Schedule() noexcept
    {
        // A system destructor must not re-enter a half-destroyed schedule via
        // a captured owner-thread pointer. The counter dies with the object,
        // but the guard still prevents cache/topology mutation during teardown.
        OperationGuard teardown_guard{
            *this,
            EOperationState::TopologyMutation,
        };
        std::vector<std::size_t> live;
        live.reserve(slots_.size());
        for (std::size_t i = 0; i < slots_.size(); ++i)
            if (slots_[i].system) live.push_back(i);
        std::sort(live.begin(), live.end(), [this](std::size_t a, std::size_t b)
        {
            return slots_[a].seq > slots_[b].seq;
        });
        for (const auto i : live)
            slots_[i].system.reset();
    }

    lux::cxx::expected<
        std::pair<std::uint32_t, std::uint32_t>,
        EScheduleMutationError>
    Schedule::addSystemErased(
        std::unique_ptr<ISystem> system,
        SystemType              type,
        int                     phase
    )
    {
        if (operation_state_ == EOperationState::TopologyMutation)
        {
            recordOperationRejection();
            return lux::cxx::unexpected(
                EScheduleMutationError::ReentrantMutation
            );
        }
        if (operation_state_ != EOperationState::Idle)
        {
            recordOperationRejection();
            return lux::cxx::unexpected(
                EScheduleMutationError::MutationDuringTick
            );
        }
        if (!system)
            return lux::cxx::unexpected(
                EScheduleMutationError::NullSystem
            );
        if (hasSystem(type))
        {
            rejected_duplicates_.push_back(type);
            return lux::cxx::unexpected(
                EScheduleMutationError::DuplicateType
            );
        }

        OperationGuard guard{*this, EOperationState::TopologyMutation};
        SystemDescriptorSnapshot descriptor;
        descriptor.capture(*system);
        if (auto preflight = preflightAddition(descriptor, type, phase);
            !preflight)
            return lux::cxx::unexpected(preflight.error());

        const auto adopted = adoptSystem(
            std::move(system),
            type,
            phase,
            std::move(descriptor)
        );
        activateSystem(adopted.first);
        return adopted;
    }

    std::pair<std::uint32_t, std::uint32_t> Schedule::adoptSystem(
        std::unique_ptr<ISystem>   system,
        SystemType                type,
        int                       phase,
        SystemDescriptorSnapshot descriptor
    )
    {
        std::size_t slot_index = slots_.size();
        for (std::size_t i = 0; i < slots_.size(); ++i)
            if (!slots_[i].system)
            {
                slot_index = i;
                break;
            }

        if (slot_index == slots_.size())
        {
            slots_.push_back(Slot{});
            staging_.resize(slots_.size());
        }

        auto& slot = slots_[slot_index];
        if (slot.generation == 0)
            slot.generation = 1;
        slot.type   = type;
        slot.phase  = phase;
        slot.seq    = next_seq_++;
        slot.descriptor = std::move(descriptor);
        slot.system = std::move(system);

        compiled_ = false;
        execution_order_.clear();
        execution_batches_.clear();
        return {static_cast<std::uint32_t>(slot_index), slot.generation};
    }

    void Schedule::activateSystem(std::uint32_t slot_index)
    {
        auto& slot = slots_[slot_index];

        // ⚠️ 槽位复用时**不**清上一任留下的命令。它们带着旧代次,barrier 会
        //    判掉并计数 —— 走一遍 fail-closed 的正路,而不是绕过它悄悄扔掉。
        slot.system->onAdded(
            SystemSetupContext{world_.registry(), makeWriter(slot)}
        );
    }

    bool Schedule::hasSystem(SystemType type) const noexcept
    {
        for (const auto& slot : slots_)
            if (slot.system && sameSystemType(slot.type, type))
                return true;
        return false;
    }

    lux::cxx::expected<InstalledSystemBatch, ScheduleBatchFailure>
    Schedule::installBatch(ScheduleMutationBatch&& batch)
    {
        const std::size_t partition_size = batch.nodes_.size();
        const std::array partitions{partition_size};
        auto installed = installBatchPartitioned(
            std::move(batch),
            partitions);
        if (!installed)
            return lux::cxx::unexpected(installed.error());
        return std::move(installed->front());
    }

    lux::cxx::expected<
        std::vector<InstalledSystemBatch>,
        ScheduleBatchFailure>
    Schedule::installBatchPartitioned(
        ScheduleMutationBatch&& batch,
        std::span<const std::size_t> partition_sizes)
    {
        if (operation_state_ != EOperationState::Idle)
        {
            recordOperationRejection();
            return lux::cxx::unexpected(ScheduleBatchFailure{
                EScheduleBatchError::SCHEDULE_BUSY});
        }
        if (batch.nodes_.empty())
        {
            return lux::cxx::unexpected(ScheduleBatchFailure{
                EScheduleBatchError::EMPTY_BATCH});
        }

        std::size_t partition_total = 0u;
        for (const auto size : partition_sizes)
        {
            if (size > batch.nodes_.size() -
                    std::min(partition_total, batch.nodes_.size()))
            {
                return lux::cxx::unexpected(ScheduleBatchFailure{
                    EScheduleBatchError::EMPTY_BATCH});
            }
            partition_total += size;
        }
        if (partition_sizes.empty() ||
            partition_total != batch.nodes_.size())
        {
            return lux::cxx::unexpected(ScheduleBatchFailure{
                EScheduleBatchError::EMPTY_BATCH});
        }

        std::vector<SystemDescriptorSnapshot> descriptors;
        descriptors.reserve(batch.nodes_.size());
        std::vector<detail::ScheduleTopologyNodeView> topology;
        topology.reserve(systemCount() + batch.nodes_.size());
        for (const auto& slot : slots_)
        {
            if (!slot.system)
                continue;
            topology.push_back(detail::ScheduleTopologyNodeView{
                .type = slot.type,
                .phase = slot.phase,
                .sequence = slot.seq,
                .prerequisites = slot.descriptor.prerequisites,
                .runs_after = slot.descriptor.runs_after,
                .runs_before = slot.descriptor.runs_before,
                .access = slot.descriptor.accessDeclaration()});
        }
        const auto live_count = topology.size();
        for (std::size_t index = 0u; index < batch.nodes_.size(); ++index)
        {
            const auto& node = batch.nodes_[index];
            if (!node.system)
            {
                return lux::cxx::unexpected(ScheduleBatchFailure{
                    EScheduleBatchError::NULL_SYSTEM,
                    node.type});
            }
            if (hasSystem(node.type))
            {
                return lux::cxx::unexpected(ScheduleBatchFailure{
                    EScheduleBatchError::DUPLICATE_TYPE,
                    node.type});
            }
            auto& descriptor = descriptors.emplace_back();
            descriptor.capture(*node.system);
            topology.push_back(detail::ScheduleTopologyNodeView{
                .type = node.type,
                .phase = node.phase,
                .sequence = next_seq_ + index,
                .prerequisites = descriptor.prerequisites,
                .runs_after = descriptor.runs_after,
                .runs_before = descriptor.runs_before,
                .access = descriptor.accessDeclaration()});
        }

        const auto analysis = detail::analyzeScheduleTopology(topology);
        if (!analysis.report.duplicate.empty())
        {
            return lux::cxx::unexpected(ScheduleBatchFailure{
                EScheduleBatchError::DUPLICATE_TYPE,
                analysis.report.duplicate.front()});
        }
        if (!analysis.report.missing_prereq.empty())
        {
            const auto [subject, related] =
                analysis.report.missing_prereq.front();
            return lux::cxx::unexpected(ScheduleBatchFailure{
                EScheduleBatchError::MISSING_PREREQUISITE,
                subject,
                related});
        }
        if (!analysis.report.cycle.empty())
        {
            return lux::cxx::unexpected(ScheduleBatchFailure{
                EScheduleBatchError::TOPOLOGY_CYCLE,
                analysis.report.cycle.front()});
        }

        std::vector<InstalledSystemBatch> installed;
        installed.resize(partition_sizes.size());
        for (std::size_t index = 0u; index < partition_sizes.size(); ++index)
        {
            installed[index].schedule_identity = identity_;
            installed[index].handles.reserve(partition_sizes[index]);
        }
        std::vector<std::uint32_t> adopted_slots(batch.nodes_.size());
        {
            OperationGuard guard{*this, EOperationState::TopologyMutation};
            std::size_t partition = 0u;
            std::size_t in_partition = 0u;
            for (std::size_t index = 0u; index < batch.nodes_.size(); ++index)
            {
                while (partition < partition_sizes.size() &&
                       in_partition == partition_sizes[partition])
                {
                    ++partition;
                    in_partition = 0u;
                }
                auto& node = batch.nodes_[index];
                const auto adopted = adoptSystem(
                    std::move(node.system),
                    node.type,
                    node.phase,
                    std::move(descriptors[index]));
                adopted_slots[index] = adopted.first;
                installed[partition].handles.push_back(SystemHandleAny{
                    identity_,
                    adopted.first,
                    adopted.second,
                    node.type});
                ++in_partition;
            }
            for (const auto topology_index : analysis.order)
            {
                if (topology_index < live_count)
                    continue;
                activateSystem(adopted_slots[topology_index - live_count]);
            }
        }
        batch.nodes_.clear();
        (void)compile();
        return installed;
    }

    lux::cxx::expected<void, ScheduleBatchFailure>
    Schedule::removeBatch(InstalledSystemBatch&& batch)
    {
        if (operation_state_ != EOperationState::Idle)
        {
            recordOperationRejection();
            return lux::cxx::unexpected(ScheduleBatchFailure{
                EScheduleBatchError::SCHEDULE_BUSY});
        }
        if (!batch.valid())
        {
            return lux::cxx::unexpected(ScheduleBatchFailure{
                EScheduleBatchError::EMPTY_BATCH});
        }
        if (batch.schedule_identity != identity_)
        {
            return lux::cxx::unexpected(ScheduleBatchFailure{
                EScheduleBatchError::WRONG_SCHEDULE});
        }
        if (!compiled_)
            (void)compile();
        const auto old_order = execution_order_;

        std::vector<std::uint32_t> removed_slots;
        removed_slots.reserve(batch.handles.size());
        for (const auto& handle : batch.handles)
        {
            if (handle.owner_identity != identity_)
            {
                return lux::cxx::unexpected(ScheduleBatchFailure{
                    EScheduleBatchError::WRONG_SCHEDULE,
                    handle.type});
            }
            auto* slot = findSlot(handle.slot, handle.generation);
            if (!slot || !sameSystemType(slot->type, handle.type))
            {
                return lux::cxx::unexpected(ScheduleBatchFailure{
                    EScheduleBatchError::INVALID_HANDLE,
                    handle.type});
            }
            if (!slot->descriptor.supports_dynamic_removal)
            {
                return lux::cxx::unexpected(ScheduleBatchFailure{
                    EScheduleBatchError::REMOVAL_UNSUPPORTED,
                    slot->type});
            }
            if (!slot->system->closeComplete())
            {
                return lux::cxx::unexpected(ScheduleBatchFailure{
                    EScheduleBatchError::SYSTEM_NOT_QUIESCENT,
                    slot->type});
            }
            if (std::ranges::find(removed_slots, handle.slot) !=
                removed_slots.end())
            {
                return lux::cxx::unexpected(ScheduleBatchFailure{
                    EScheduleBatchError::INVALID_HANDLE,
                    handle.type});
            }
            removed_slots.push_back(handle.slot);
        }

        std::vector<detail::ScheduleTopologyNodeView> remaining;
        remaining.reserve(systemCount() - removed_slots.size());
        for (std::uint32_t index = 0u; index < slots_.size(); ++index)
        {
            const auto& slot = slots_[index];
            if (!slot.system ||
                std::ranges::find(removed_slots, index) != removed_slots.end())
                continue;
            remaining.push_back(detail::ScheduleTopologyNodeView{
                .type = slot.type,
                .phase = slot.phase,
                .sequence = slot.seq,
                .prerequisites = slot.descriptor.prerequisites,
                .runs_after = slot.descriptor.runs_after,
                .runs_before = slot.descriptor.runs_before,
                .access = slot.descriptor.accessDeclaration()});
        }
        const auto analysis = detail::analyzeScheduleTopology(remaining);
        if (!analysis.report.missing_prereq.empty())
        {
            const auto [subject, related] =
                analysis.report.missing_prereq.front();
            return lux::cxx::unexpected(ScheduleBatchFailure{
                EScheduleBatchError::REQUIRED_BY_OTHER_SYSTEM,
                subject,
                related});
        }
        if (!analysis.report.cycle.empty())
        {
            return lux::cxx::unexpected(ScheduleBatchFailure{
                EScheduleBatchError::TOPOLOGY_CYCLE,
                analysis.report.cycle.front()});
        }

        {
            OperationGuard guard{*this, EOperationState::TopologyMutation};
            for (auto order = old_order.rbegin(); order != old_order.rend(); ++order)
            {
                const auto slot_index = static_cast<std::uint32_t>(*order);
                if (std::ranges::find(removed_slots, slot_index) ==
                    removed_slots.end())
                    continue;
                auto& slot = slots_[slot_index];
                slot.system->onRemoved(
                    SystemRemovalContext{world_.registry()});
                slot.system.reset();
                retireSlot(slot);
            }
        }
        batch.handles.clear();
        batch.schedule_identity = 0u;
        (void)compile();
        return {};
    }

    bool Schedule::requestBatchClose(
        const InstalledSystemBatch& batch,
        SystemCloseProgressSink progress) noexcept
    {
        if (operation_state_ != EOperationState::Idle || !batch.valid() ||
            batch.schedule_identity != identity_)
        {
            return false;
        }
        if (!compiled_)
            (void)compile();
        for (const auto& handle : batch.handles)
        {
            const auto* slot = findSlot(handle.slot, handle.generation);
            if (handle.owner_identity != identity_ || !slot ||
                !sameSystemType(slot->type, handle.type))
            {
                return false;
            }
        }
        for (auto order = execution_order_.rbegin();
             order != execution_order_.rend(); ++order)
        {
            const auto handle = std::ranges::find(
                batch.handles,
                static_cast<std::uint32_t>(*order),
                &SystemHandleAny::slot);
            if (handle == batch.handles.end())
                continue;
            slots_[*order].system->requestClose(progress);
        }
        return true;
    }

    SystemBatchCloseState Schedule::batchCloseState(
        const InstalledSystemBatch& batch) const noexcept
    {
        SystemBatchCloseState result;
        if (operation_state_ != EOperationState::Idle || !batch.valid() ||
            batch.schedule_identity != identity_)
        {
            return result;
        }
        result.valid = true;
        result.complete = true;
        for (const auto& handle : batch.handles)
        {
            const auto* slot = findSlot(handle.slot, handle.generation);
            if (handle.owner_identity != identity_ || !slot ||
                !sameSystemType(slot->type, handle.type))
            {
                return {};
            }
            if (!slot->system->closeComplete())
            {
                result.complete = false;
                ++result.pending_systems;
                result.owner_work_pending =
                    result.owner_work_pending ||
                    slot->system->closeNeedsOwnerTick();
            }
        }
        return result;
    }

    void Schedule::requestClose(SystemCloseProgressSink progress) noexcept
    {
        if (operation_state_ != EOperationState::Idle)
        {
            recordOperationRejection();
            return;
        }
        if (!compiled_)
            (void)compile();
        for (auto order = execution_order_.rbegin();
             order != execution_order_.rend(); ++order)
        {
            auto& slot = slots_[*order];
            if (slot.system)
                slot.system->requestClose(progress);
        }
    }

    SystemBatchCloseState Schedule::closeState() const noexcept
    {
        SystemBatchCloseState result;
        if (operation_state_ != EOperationState::Idle)
            return result;
        result.valid = true;
        result.complete = true;
        for (const auto& slot : slots_)
        {
            if (!slot.system || slot.system->closeComplete())
                continue;
            result.complete = false;
            ++result.pending_systems;
            result.owner_work_pending =
                result.owner_work_pending ||
                slot.system->closeNeedsOwnerTick();
        }
        return result;
    }

    Schedule::SortReport Schedule::analyzeTopologyMutation(
        const Slot*                     excluded,
        const SystemDescriptorSnapshot* addition,
        SystemType                      addition_type,
        int                             addition_phase
    ) const
    {
        std::vector<detail::ScheduleTopologyNodeView> nodes;
        nodes.reserve(slots_.size() + (addition ? 1u : 0u));

        for (const auto& slot : slots_)
        {
            if (!slot.system || &slot == excluded) continue;
            nodes.push_back(detail::ScheduleTopologyNodeView{
                .type = slot.type,
                .phase = slot.phase,
                .sequence = slot.seq,
                .prerequisites = slot.descriptor.prerequisites,
                .runs_after = slot.descriptor.runs_after,
                .runs_before = slot.descriptor.runs_before,
                .access = slot.descriptor.accessDeclaration(),
            });
        }

        if (addition)
        {
            nodes.push_back(detail::ScheduleTopologyNodeView{
                .type = addition_type,
                .phase = addition_phase,
                .sequence = next_seq_,
                .prerequisites = addition->prerequisites,
                .runs_after = addition->runs_after,
                .runs_before = addition->runs_before,
                .access = addition->accessDeclaration(),
            });
        }

        return detail::analyzeScheduleTopology(nodes).report;
    }

    lux::cxx::expected<void, EScheduleMutationError>
    Schedule::preflightAddition(
        const SystemDescriptorSnapshot& descriptor,
        SystemType                     type,
        int                            phase
    ) const
    {
        const auto report = analyzeTopologyMutation(
            nullptr,
            &descriptor,
            type,
            phase
        );
        if (!report.duplicate.empty())
            return lux::cxx::unexpected(
                EScheduleMutationError::DuplicateType
            );
        if (!report.missing_prereq.empty())
            return lux::cxx::unexpected(
                EScheduleMutationError::MissingPrerequisite
            );
        if (!report.cycle.empty())
            return lux::cxx::unexpected(
                EScheduleMutationError::TopologyCycle
            );
        return {};
    }

    lux::cxx::expected<void, EScheduleMutationError>
    Schedule::preflightRemoval(const Slot& removed) const
    {
        const auto report = analyzeTopologyMutation(
            &removed,
            nullptr,
            {},
            kPhaseSimulation
        );
        for (const auto& [subject, required] : report.missing_prereq)
        {
            (void)subject;
            if (sameSystemType(required, removed.type))
                return lux::cxx::unexpected(
                    EScheduleMutationError::RequiredByOtherSystem
                );
        }
        return {};
    }

    Schedule::Slot* Schedule::findSlot(
        std::uint32_t index, std::uint32_t generation) noexcept
    {
        if (index >= slots_.size()) return nullptr;
        auto& slot = slots_[index];
        return slot.system && slot.generation == generation ? &slot : nullptr;
    }

    const Schedule::Slot* Schedule::findSlot(
        std::uint32_t index, std::uint32_t generation) const noexcept
    {
        if (index >= slots_.size()) return nullptr;
        const auto& slot = slots_[index];
        return slot.system && slot.generation == generation ? &slot : nullptr;
    }

    void Schedule::retireSlot(Slot& slot) noexcept
    {
        slot.type  = {};
        slot.phase = kPhaseSimulation;
        slot.seq   = 0;
        ++slot.generation;
        if (slot.generation == 0) ++slot.generation;
        compiled_ = false;
        execution_order_.clear();
        execution_batches_.clear();
    }

    Schedule::SortReport Schedule::compile()
    {
        if (operation_state_ != EOperationState::Idle)
        {
            recordOperationRejection();
            SortReport rejected;
            rejected.operation_rejected = true;
            return rejected;
        }

        execution_order_.clear();
        execution_batches_.clear();

        std::vector<std::size_t> live;
        live.reserve(slots_.size());
        for (std::size_t i = 0; i < slots_.size(); ++i)
            if (slots_[i].system) live.push_back(i);

        std::vector<detail::ScheduleTopologyNodeView> nodes;
        nodes.reserve(live.size());
        for (const auto slot_index : live)
        {
            const auto& slot = slots_[slot_index];
            nodes.push_back(detail::ScheduleTopologyNodeView{
                .type = slot.type,
                .phase = slot.phase,
                .sequence = slot.seq,
                .prerequisites = slot.descriptor.prerequisites,
                .runs_after = slot.descriptor.runs_after,
                .runs_before = slot.descriptor.runs_before,
                .access = slot.descriptor.accessDeclaration(),
            });
        }

        auto analysis = detail::analyzeScheduleTopology(nodes);
        analysis.report.duplicate.insert(
            analysis.report.duplicate.end(),
            rejected_duplicates_.begin(),
            rejected_duplicates_.end()
        );
        rejected_duplicates_.clear();

        execution_batches_ = std::move(analysis.batches);
        execution_order_.reserve(live.size());
        for (const auto local : analysis.order)
            execution_order_.push_back(live[local]);
        latest_system_frame_trace_.resize(execution_order_.size());
        for (std::size_t index = 0u;
             index < execution_order_.size();
             ++index)
        {
            const auto& slot = slots_[execution_order_[index]];
            latest_system_frame_trace_[index] = ScheduleSystemFrameTrace{
                .system = slot.type,
                .phase = slot.phase,
            };
        }
        compiled_ = true;
        return std::move(analysis.report);
    }

    void Schedule::tick(
        float dt,
        int through_phase,
        std::uint64_t frame_serial)
    {
        if (operation_state_ != EOperationState::Idle)
        {
            recordOperationRejection();
            return;
        }
        if (!compiled_) (void)compile();

        const std::uint64_t tick_index = tick_index_++;
        latest_frame_trace_ = {};
        latest_frame_trace_.frame_serial = frame_serial != 0u
            ? frame_serial
            : tick_index + 1u;

        const auto phaseIndex = [](int phase) noexcept -> std::size_t
        {
            if (phase <= kPhaseInput) return 0u;
            if (phase <= kPhasePreTransform) return 1u;
            if (phase <= kPhaseSimulation) return 2u;
            if (phase <= kPhasePreRender) return 3u;
            if (phase <= kPhaseRender) return 4u;
            return 5u;
        };
        for (auto& trace : latest_system_frame_trace_)
        {
            trace.frame_serial = latest_frame_trace_.frame_serial;
            trace.wall_nanoseconds = 0u;
        }

        {
            OperationGuard guard{*this, EOperationState::Ticking};
            for (std::size_t order_index = 0u;
                 order_index < execution_order_.size();
                 ++order_index)
            {
                const auto index = execution_order_[order_index];
                auto& slot = slots_[index];
                if (slot.phase > through_phase) continue;
                const auto phase = phaseIndex(slot.phase);
                const auto started = std::chrono::steady_clock::now();
                slot.system->update(SystemUpdateContext{
                    world_.registry(), makeWriter(slot), dt, tick_index});
                const auto elapsed = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - started).count());
                latest_system_frame_trace_[order_index].wall_nanoseconds =
                    elapsed;
                latest_frame_trace_.phase_nanoseconds[phase] += elapsed;
            }
        }

        if (!prepareCommandBarrier())
            std::abort();

        // 唯一的 apply 点,在 tick 之外 —— 不在任何系统 update 的中途,也不在任何
        // 信号派发的中途。宿主的帧序保证此刻帧仍然开着(FrameCoordinator:
        // … → Schedule::tick → submit),所以命令里发渲染命令是合法的。
        const auto barrier_started = std::chrono::steady_clock::now();
        applyCommandBarrier();
        latest_frame_trace_.command_barrier_nanoseconds =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - barrier_started)
                    .count());
    }

    void Schedule::applyCommandBarrier()
    {
        if (operation_state_ != EOperationState::Idle)
        {
            recordOperationRejection();
            return;
        }
        if (!compiled_)
            std::abort();

        OperationGuard guard{*this, EOperationState::ApplyingBarrier};
        auto admission_scope =
            world_.registry().closePublicationAdmission();
        static_cast<void>(admission_scope);

        // ① 先把**所有**分片整体换出,再开始应用。
        //
        //    不能边换边应用:那样前一个节点的命令在应用时入队给后一个节点,会在
        //    同一轮里被消费掉 —— 自喂循环就从后门回来了,而且只在特定组件组合下
        //    才发作。整体换出之后,应用期新入队的一律落在下一轮。
        if (staging_.size() != slots_.size())
            std::abort();

        bool any = false;
        for (std::size_t i = 0; i < slots_.size(); ++i)
        {
            if (!slots_[i].commands || slots_[i].commands->empty()) continue;
            slots_[i].commands->takeInto(staging_[i]);
            any = true;
        }
        if (!any)
        {
            if (auto* hierarchy =
                    world_.registry().ctx().find<HierarchyIndex>())
            {
                (void)hierarchy->refresh();
            }
            return;
        }

        // Every deferred/re-entrant command was armed before entering this
        // barrier. A missing token is a protocol violation, never an excuse
        // to reach upstream from the apply phase.
        for (auto& shard : staging_)
        {
            if (!shard.empty() && !shard.reservationsReady())
                std::abort();
        }

        // ② 按「编译后的节点序 × 分片内序号」应用:同样的输入给出字节级相同的
        //    apply 序,trace 才可重复。
        for (const auto index : execution_order_)
            applyShard(staging_[index], slots_[index]);

        // ③ 不在执行序里的槽位 —— 已退休、或本轮还没编译进来。它们的命令一定
        //    没有活着的生产者,计数丢弃(fail closed 要可见)。
        for (std::size_t i = 0; i < staging_.size(); ++i)
        {
            if (staging_[i].empty()) continue;
            dropped_stale_commands_ += staging_[i].size();
            staging_[i].clear();
        }

        // Parent observers only invalidate during individual component writes.
        // Rebuild once after the complete deterministic command batch, never in
        // the middle of publishing a partially-linked hierarchy.
        if (auto* hierarchy =
                world_.registry().ctx().find<HierarchyIndex>())
        {
            (void)hierarchy->refresh();
        }
    }

    void Schedule::applyShard(EcsCommandBuffer& shard, Slot& slot)
    {
        auto& registry = world_.registry();
        for (auto& header : shard.headers())
        {
            // 生产者没了、或槽位已被别的系统占用(代次不等)→ 丢弃并计数。
            // 命令**不去访问**那个 owner:这正是把 weak_ptr 兜底换成代次的意义。
            if (!slot.system || header.producer_generation != slot.generation)
            {
                ++dropped_stale_commands_;
                continue;
            }
            if (header.publication_bytes == 0u)
            {
                header.apply(
                    registry, *slot.system, shard.payloadOf(header));
                continue;
            }
            auto publication_scope =
                header.publication_reservation.enter();
            static_cast<void>(publication_scope);
            header.apply(registry, *slot.system, shard.payloadOf(header));
        }
        shard.clear();
    }

    bool Schedule::prepareCommandBarrier() noexcept
    {
        if (operation_state_ != EOperationState::Idle)
        {
            recordOperationRejection();
            return false;
        }
        if (!compiled_)
        {
            (void)compile();
            if (!compiled_)
                return false;
        }
        EcsCommandStorageReservationScope storage_scope{
            command_storage_plan_};
        static_cast<void>(storage_scope);
        for (auto& slot : slots_)
        {
            if (slot.commands && !slot.commands->empty() &&
                !slot.commands->armReservations(world_.registry()))
            {
                return false;
            }
        }
        return true;
    }

    std::span<const Schedule::ExecutionBatch> Schedule::executionBatches()
    {
        if (operation_state_ != EOperationState::Idle)
        {
            recordOperationRejection();
            return {};
        }
        if (!compiled_) (void)compile();
        return execution_batches_;
    }

    std::size_t Schedule::systemCount() const noexcept
    {
        std::size_t count = 0;
        for (const auto& slot : slots_)
            if (slot.system) ++count;
        return count;
    }

} // namespace lux::ecs
