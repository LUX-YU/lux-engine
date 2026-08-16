#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/detail/ScheduleTopology.hpp>

#include <algorithm>
#include <atomic>

namespace lux::ecs
{
    ScheduleBuilder::~ScheduleBuilder() noexcept
    {
        if (state_ != EState::Committed)
        {
            rollbackAssemblyTo(
                0,
                SceneServiceTransaction::Checkpoint{}
            );
            // All callback-owning containers are empty now. Keep the object
            // fail-closed until its trivial member destruction completes.
            state_ = EState::RollingBack;
        }
    }

    void ScheduleBuilder::rollbackAssemblyTo(
        std::size_t                         pending_count,
        SceneServiceTransaction::Checkpoint services_checkpoint
    ) noexcept
    {
        if (state_ == EState::Committed || pending_count > nodes_.size())
            return;

        OperationStateGuard rollback_state{state_, EState::RollingBack};
        Schedule::OperationGuard topology_state{
            schedule_,
            Schedule::EOperationState::TopologyMutation,
        };
        SceneServices::OperationGuard service_state{
            services_.base_,
            SceneServices::EState::MutationBlocked,
        };

        // A closure may borrow a staged system, and a system may borrow a
        // staged service. Keep this order; each destructor may call back into
        // the builder, transaction, or schedule and will be rejected above.
        services_.discardDeferredFrom(services_checkpoint);
        nodes_.resize(pending_count);
        handles_.resize(pending_count);
        services_.rollbackRegistrationsTo(services_checkpoint);
    }

    std::uint64_t ScheduleBuilder::allocateIdentity() noexcept
    {
        // Assembly-only identity; relaxed ordering is sufficient because the
        // value carries no publication semantics. It only prevents a token
        // from being accepted by another (or later stack-reused) builder.
        static std::atomic<std::uint64_t> next{1};
        auto identity = next.fetch_add(1, std::memory_order_relaxed);
        if (identity == 0)
            identity = next.fetch_add(1, std::memory_order_relaxed);
        return identity;
    }

    ScheduleCommitResult ScheduleBuilder::commit()
    {
        if (state_ == EState::Committing ||
            state_ == EState::RollingBack)
            return lux::cxx::unexpected<ScheduleCommitFailure>(
                ScheduleCommitFailure{
                    EScheduleCommitError::ScheduleBusy, {}, {}});
        if (state_ == EState::Committed)
            return lux::cxx::unexpected<ScheduleCommitFailure>(
                ScheduleCommitFailure{
                    EScheduleCommitError::AlreadyCommitted, {}, {}});

        OperationStateGuard commit_state{state_, EState::Committing};

        Schedule& schedule = schedule_;

        // 装配不该发生在 tick / barrier 中途 —— 那时 `slots_` 正被引用着。
        if (schedule.mutationLocked())
        {
            schedule.recordOperationRejection();
            return lux::cxx::unexpected<ScheduleCommitFailure>(
                ScheduleCommitFailure{
                    EScheduleCommitError::ScheduleBusy, {}, {}});
        }

        Schedule::OperationGuard mutation_guard{
            schedule,
            Schedule::EOperationState::TopologyMutation,
        };

        // ── 交付**之前**把能查的全查完 ────────────────────────────────────
        //
        // 这是 builder 存在的理由:任何一条不过,schedule 一点没动、一个 onAdded
        // 都没跑。builder 析构时把这批系统一起带走,世界回到装配前的样子。

        for (const auto& node : nodes_)
            if (schedule.hasSystem(node.type))
                return lux::cxx::unexpected<ScheduleCommitFailure>(
                    ScheduleCommitFailure{
                        EScheduleCommitError::DuplicateType,
                        node.type.name, {}});

        // 对 live ∪ pending 做一次纯快照分析。这里不采用预检算出的 order：
        // 正式 compile 还要把局部顺序映射回稳定槽位，并在后续合法增删后重建缓存。
        std::vector<detail::ScheduleTopologyNodeView> topology;
        topology.reserve(schedule.systemCount() + nodes_.size());
        using Descriptor = Schedule::SystemDescriptorSnapshot;
        std::vector<Descriptor> pending_descriptors;
        pending_descriptors.reserve(nodes_.size());

        for (const auto& slot : schedule.slots_)
        {
            if (!slot.system) continue;
            topology.push_back(detail::ScheduleTopologyNodeView{
                .type = slot.type,
                .phase = slot.phase,
                .sequence = slot.seq,
                .prerequisites = slot.descriptor.prerequisites,
                .runs_after = slot.descriptor.runs_after,
                .runs_before = slot.descriptor.runs_before,
                .access = slot.descriptor.accessDeclaration(),
            });
        }

        for (std::size_t i = 0; i < nodes_.size(); ++i)
        {
            const auto& node = nodes_[i];
            auto& descriptor = pending_descriptors.emplace_back();
            descriptor.capture(*node.system);
            topology.push_back(detail::ScheduleTopologyNodeView{
                .type = node.type,
                .phase = node.phase,
                .sequence = schedule.next_seq_ + i,
                .prerequisites = descriptor.prerequisites,
                .runs_after = descriptor.runs_after,
                .runs_before = descriptor.runs_before,
                .access = descriptor.accessDeclaration(),
            });
        }

        auto analysis = detail::analyzeScheduleTopology(topology);
        if (!analysis.report.missing_prereq.empty())
        {
            const auto [subject, required] =
                analysis.report.missing_prereq.front();
            return lux::cxx::unexpected<ScheduleCommitFailure>(
                ScheduleCommitFailure{
                    EScheduleCommitError::MissingPrerequisite,
                    subject.name,
                    required.name,
                    std::move(analysis.report),
                });
        }
        if (!analysis.report.cycle.empty())
        {
            const auto subject = analysis.report.cycle.front();
            return lux::cxx::unexpected<ScheduleCommitFailure>(
                ScheduleCommitFailure{
                    EScheduleCommitError::TopologyCycle,
                    subject.name,
                    {},
                    std::move(analysis.report),
                });
        }

        // Re-check staged ∩ base at the last failure-capable boundary. Base is
        // owner-thread confined but may have been deliberately extended after
        // this builder was created. Publishing now also makes duplicate
        // registration from onAdded fail normally instead of corrupting the
        // service table; everything after this point is ownership transfer and
        // lifecycle callbacks, whose protocol has no recoverable failure branch
        // (project code does not use exception control flow).
        if (auto published = services_.publish(); !published)
            return lux::cxx::unexpected<ScheduleCommitFailure>(
                ScheduleCommitFailure{
                    EScheduleCommitError::ServiceConflict,
                    "<services>",
                    published.error().name,
                });

        // ── 交付。先收下并冻结整批，再运行任何 onAdded ───────────────────
        // 某个早节点的 onAdded 即使修改了另一个 pending 实例的成员，也不能
        // 改写已经通过预检的图；调度声明属于 slot snapshot，不属于运行期状态。
        for (std::size_t i = 0; i < nodes_.size(); ++i)
        {
            const auto adopted = schedule.adoptSystem(
                std::move(nodes_[i].system),
                nodes_[i].type,
                nodes_[i].phase,
                std::move(pending_descriptors[i])
            );
            handles_[i] = RawHandle{
                schedule.identity_,
                adopted.first,
                adopted.second,
            };
        }
        for (const auto handle : handles_)
            schedule.activateSystem(handle.slot);

        state_ = EState::Committed;
        return {};
    }

    bool ScheduleBuilder::canClaimCommittedRange(
        Checkpoint first,
        Checkpoint last) const noexcept
    {
        if (state_ != EState::Committed ||
            first.builder_identity_ != identity_ ||
            last.builder_identity_ != identity_ ||
            first.pending_count_ > last.pending_count_ ||
            last.pending_count_ > handles_.size() ||
            first.service_registrations_ > last.service_registrations_ ||
            !services_.canClaimPublished(
                first.service_registrations_,
                last.service_registrations_))
        {
            return false;
        }
        if (!handles_claimed_.empty() && std::ranges::any_of(
                    handles_claimed_.begin() + static_cast<std::ptrdiff_t>(
                        first.pending_count_),
                    handles_claimed_.begin() + static_cast<std::ptrdiff_t>(
                        last.pending_count_),
                    [](bool claimed) noexcept { return claimed; }))
        {
            return false;
        }
        return std::ranges::all_of(
            handles_.begin() + static_cast<std::ptrdiff_t>(
                first.pending_count_),
            handles_.begin() + static_cast<std::ptrdiff_t>(
                last.pending_count_),
            [](const RawHandle& handle) noexcept
            {
                return handle.generation != 0u;
            });
    }

    std::optional<ScheduleBuilderInstalledRange>
    ScheduleBuilder::claimCommittedRange(
        Checkpoint first,
        Checkpoint last) noexcept
    {
        if (!canClaimCommittedRange(first, last))
            return std::nullopt;
        if (handles_claimed_.empty())
            handles_claimed_.assign(handles_.size(), false);

        ScheduleBuilderInstalledRange result;
        if (first.pending_count_ != last.pending_count_)
        {
            result.systems.schedule_identity = schedule_.identity_;
            result.systems.handles.reserve(
                last.pending_count_ - first.pending_count_);
            for (auto index = first.pending_count_;
                 index < last.pending_count_;
                 ++index)
            {
                const auto handle = handles_[index];
                result.systems.handles.push_back(SystemHandleAny{
                    handle.owner_identity,
                    handle.slot,
                    handle.generation,
                    nodes_[index].type});
            }
        }

        result.services = services_.claimPublished(
            first.service_registrations_,
            last.service_registrations_);
        for (auto index = first.pending_count_;
             index < last.pending_count_;
             ++index)
        {
            handles_claimed_[index] = true;
        }
        return result;
    }

} // namespace lux::ecs
