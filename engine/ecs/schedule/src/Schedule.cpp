#include <lux/engine/ecs/Schedule.hpp>

#include <lux/engine/ecs/core/detail/CommandStorage.hpp>
#include <lux/engine/ecs/schedule/detail/ScheduleTestAccess.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace lux::ecs::detail
{
    namespace
    {
        [[nodiscard]] std::uint64_t nextScheduleOwner() noexcept
        {
            static std::atomic_uint64_t next{1};
            auto result = next.fetch_add(1, std::memory_order_relaxed);
            if (result == 0)
                result = next.fetch_add(1, std::memory_order_relaxed);
            return result;
        }

        [[nodiscard]] constexpr std::size_t phaseIndex(
            SystemPhase phase
        ) noexcept
        {
            return static_cast<std::size_t>(phase);
        }
    } // namespace

    struct HandleKey final
    {
        std::uint64_t owner{};
        std::uint32_t slot{};
        std::uint32_t generation{};

        [[nodiscard]] bool operator==(const HandleKey&) const noexcept = default;
    };

    [[nodiscard]] HandleKey key(AnySystemHandle handle) noexcept
    {
        return HandleKey{
            SystemHandleAccess::owner(handle),
            SystemHandleAccess::slot(handle),
            SystemHandleAccess::generation(handle)};
    }

    [[nodiscard]] AnySystemHandle handle(HandleKey value) noexcept
    {
        return SystemHandleAccess::make(
            value.owner, value.slot, value.generation
        );
    }

    struct OwnedSet final
    {
        std::uint64_t hash{};
        std::string name;

        [[nodiscard]] bool operator==(const OwnedSet& other) const noexcept
        {
            return hash == other.hash && name == other.name;
        }
    };

    [[nodiscard]] OwnedSet own(SystemSetId set)
    {
        return OwnedSet{set.id.hash(), std::string(set.id.name())};
    }

    struct SetMembership final
    {
        HandleKey system;
        OwnedSet set;
    };

    enum class EOrderTarget : std::uint8_t
    {
        SYSTEM,
        SET,
    };

    struct OrderRelation final
    {
        HandleKey source;
        EOrderTarget target_kind{EOrderTarget::SYSTEM};
        HandleKey target;
        OwnedSet set;
        bool before{};
    };

    struct Requirement final
    {
        HandleKey consumer;
        HandleKey provider;
    };

    struct ShardRecord final
    {
        std::uint64_t storage{};
        Entity entity{NullEntity};
        EComponentChangeKind kind{EComponentChangeKind::MODIFIED};
    };

    struct ChangeShard final
    {
        std::vector<ShardRecord> records;
        bool overflow{};

        void reserve(std::size_t count)
        {
            records.reserve(count);
        }

        void begin() noexcept
        {
            records.clear();
            overflow = false;
        }

        [[nodiscard]] ChangeRecorder recorder() noexcept
        {
            return ChangeRecorder{
                this,
                [](void* context, std::uint64_t storage, Entity entity,
                   EComponentChangeKind kind) noexcept
                {
                    auto& shard = *static_cast<ChangeShard*>(context);
                    if (shard.overflow)
                        return;
                    try
                    {
                        shard.records.push_back(
                            ShardRecord{storage, entity, kind}
                        );
                    }
                    catch (...)
                    {
                        shard.records.clear();
                        shard.overflow = true;
                    }
                }};
        }
    };

    struct ScheduleSlot final
    {
        std::unique_ptr<System> system;
        lux::cxx::TypeToken type;
        SystemPhase phase{SystemPhase::Update};
        ESystemExecutionAffinity affinity{
            ESystemExecutionAffinity::WORKER_ELIGIBLE};
        bool (*affinity_validator)(const System&) noexcept{};
        std::vector<ComponentAccess> component_access;
        std::vector<ExternalAccess> external_access;
        bool access_complete{};
        std::unique_ptr<CommandShard> commands{std::make_unique<CommandShard>()};
        ChangeShard changes;
        std::uint64_t sequence{};
        bool stop_requested{};
    };

    struct ExecutionPlan final
    {
        std::array<std::vector<std::vector<std::uint32_t>>, 3> waves;
        std::array<std::vector<std::uint32_t>, 3> phase_order;
    };

    struct CandidateNode final
    {
        HandleKey key;
        ScheduleSlot* slot{};
        std::size_t candidate_index{};
    };

    [[nodiscard]] bool tokenCollision(
        lux::cxx::TypeToken left,
        lux::cxx::TypeToken right
    ) noexcept
    {
        return left.hash() == right.hash() && left.name() != right.name();
    }

    [[nodiscard]] bool accessConflict(
        const ScheduleSlot& left,
        const ScheduleSlot& right
    ) noexcept
    {
        if (!left.access_complete || !right.access_complete)
            return true;
        if (left.affinity == ESystemExecutionAffinity::OWNER_THREAD ||
            right.affinity == ESystemExecutionAffinity::OWNER_THREAD)
            return true;

        for (const ComponentAccess& a : left.component_access)
        {
            for (const ComponentAccess& b : right.component_access)
            {
                if (a.type == b.type &&
                    (a.mode == EAccessMode::WRITE || b.mode == EAccessMode::WRITE))
                    return true;
            }
        }
        for (const ExternalAccess& a : left.external_access)
        {
            for (const ExternalAccess& b : right.external_access)
            {
                if (a.type == b.type &&
                    (a.mode == EAccessMode::WRITE || b.mode == EAccessMode::WRITE))
                    return true;
            }
        }
        return false;
    }
} // namespace lux::ecs::detail

namespace lux::ecs
{
    struct Schedule::Impl final
    {
        explicit Impl(World& owner) noexcept
            : world(&owner), owner_id(detail::nextScheduleOwner()),
              owner_thread(std::this_thread::get_id())
        {
        }

        World* world{};
        std::uint64_t owner_id{};
        std::thread::id owner_thread;
        std::vector<std::unique_ptr<detail::ScheduleSlot>> slots;
        std::vector<std::uint32_t> generations;
        std::vector<bool> reserved;
        std::vector<detail::SetMembership> memberships;
        std::vector<detail::OrderRelation> orders;
        std::vector<detail::Requirement> requirements;
        detail::ExecutionPlan plan;
        std::uint64_t next_sequence{1};
        std::size_t retired_discarded{};
        std::size_t retired_command_allocations{};
        bool edit_open{};
        bool executing{};
        bool closing{};

        [[nodiscard]] detail::ScheduleSlot* find(detail::HandleKey key) noexcept
        {
            if (key.owner != owner_id || key.slot >= slots.size() ||
                key.slot >= generations.size() ||
                generations[key.slot] != key.generation)
                return nullptr;
            return slots[key.slot].get();
        }

        [[nodiscard]] const detail::ScheduleSlot* find(
            detail::HandleKey key
        ) const noexcept
        {
            return const_cast<Impl*>(this)->find(key);
        }

        [[nodiscard]] bool dependentStopped(detail::HandleKey provider) const noexcept
        {
            for (const detail::Requirement& requirement : requirements)
            {
                if (requirement.provider != provider)
                    continue;
                const auto* consumer = find(requirement.consumer);
                if (consumer != nullptr &&
                    (!consumer->stop_requested || !consumer->system->stopped()))
                    return false;
            }
            return true;
        }
    };

    struct ScheduleEdit::Impl final
    {
        struct Addition final
        {
            detail::HandleKey key;
            std::unique_ptr<detail::ScheduleSlot> slot;
        };

        std::vector<Addition> additions;
        std::vector<detail::HandleKey> removals;
        std::vector<detail::SetMembership> memberships;
        std::vector<detail::OrderRelation> orders;
        std::vector<detail::Requirement> requirements;
        std::optional<ScheduleFailure> failure;
        bool committed{};
    };

    namespace
    {
        [[nodiscard]] bool contains(
            std::span<const detail::HandleKey> values,
            detail::HandleKey key
        ) noexcept
        {
            return std::find(values.begin(), values.end(), key) != values.end();
        }

        template <class EditImpl>
        [[nodiscard]] detail::ScheduleSlot* stagedSlot(
            EditImpl& edit,
            detail::HandleKey key
        ) noexcept
        {
            for (auto& addition : edit.additions)
            {
                if (addition.key == key)
                    return addition.slot.get();
            }
            return nullptr;
        }

        template <class ScheduleImpl, class EditImpl>
        [[nodiscard]] bool candidateHandle(
            ScheduleImpl& schedule,
            EditImpl& edit,
            detail::HandleKey key
        ) noexcept
        {
            if (key.owner != schedule.owner_id ||
                key.slot >= schedule.generations.size() ||
                schedule.generations[key.slot] != key.generation ||
                contains(edit.removals, key))
                return false;
            return schedule.find(key) != nullptr || stagedSlot(edit, key) != nullptr;
        }

        [[nodiscard]] bool nodeLess(
            const detail::CandidateNode& left,
            const detail::CandidateNode& right
        ) noexcept
        {
            if (left.slot->sequence != right.slot->sequence)
                return left.slot->sequence < right.slot->sequence;
            return left.key.slot < right.key.slot;
        }

        [[nodiscard]] ScheduleFailure failure(
            EScheduleError code,
            lux::cxx::TypeToken subject = {},
            lux::cxx::TypeToken related = {}
        ) noexcept
        {
            return ScheduleFailure{code, subject, related};
        }

        [[nodiscard]] lux::cxx::expected<void, ScheduleFailure>
        captureAccess(detail::ScheduleSlot& slot) noexcept
        {
            try
            {
                const SystemAccess declared = slot.system->access();
                slot.component_access.assign(
                    declared.components.begin(), declared.components.end()
                );
                slot.external_access.assign(
                    declared.external.begin(), declared.external.end()
                );
                slot.access_complete = declared.complete;
                slot.commands->reserve(64);
                slot.changes.reserve(256);

                for (std::size_t left{};
                     left < slot.component_access.size(); ++left)
                {
                    for (std::size_t right = left + 1;
                         right < slot.component_access.size(); ++right)
                    {
                        const auto a = slot.component_access[left].type;
                        const auto b = slot.component_access[right].type;
                        if (detail::tokenCollision(a, b))
                            return lux::cxx::unexpected(failure(
                                EScheduleError::TYPE_TOKEN_COLLISION, a, b
                            ));
                        if (a == b)
                            return lux::cxx::unexpected(failure(
                                EScheduleError::INVALID_RELATION, a, b
                            ));
                    }
                }
                for (std::size_t left{};
                     left < slot.external_access.size(); ++left)
                {
                    for (std::size_t right = left + 1;
                         right < slot.external_access.size(); ++right)
                    {
                        const auto a = slot.external_access[left].type;
                        const auto b = slot.external_access[right].type;
                        if (detail::tokenCollision(a, b))
                            return lux::cxx::unexpected(failure(
                                EScheduleError::TYPE_TOKEN_COLLISION, a, b
                            ));
                        if (a == b)
                            return lux::cxx::unexpected(failure(
                                EScheduleError::INVALID_RELATION, a, b
                            ));
                    }
                }
            }
            catch (...)
            {
                return lux::cxx::unexpected(failure(
                    EScheduleError::ALLOCATION_FAILURE
                ));
            }
            return {};
        }

        [[nodiscard]] bool setCollision(
            std::span<const detail::SetMembership> memberships,
            std::span<const detail::OrderRelation> orders
        ) noexcept
        {
            const auto conflicts = [](const detail::OwnedSet& a,
                                      const detail::OwnedSet& b) noexcept
            {
                return a.hash == b.hash && a.name != b.name;
            };
            for (std::size_t left{}; left < memberships.size(); ++left)
            {
                for (std::size_t right = left + 1;
                     right < memberships.size(); ++right)
                {
                    if (conflicts(memberships[left].set, memberships[right].set))
                        return true;
                }
                for (const auto& order : orders)
                {
                    if (order.target_kind == detail::EOrderTarget::SET &&
                        conflicts(memberships[left].set, order.set))
                        return true;
                }
            }
            for (std::size_t left{}; left < orders.size(); ++left)
            {
                if (orders[left].target_kind != detail::EOrderTarget::SET)
                    continue;
                for (std::size_t right = left + 1;
                     right < orders.size(); ++right)
                {
                    if (orders[right].target_kind == detail::EOrderTarget::SET &&
                        conflicts(orders[left].set, orders[right].set))
                        return true;
                }
            }
            return false;
        }

        [[nodiscard]] std::optional<std::size_t> findNode(
            std::span<const detail::CandidateNode> nodes,
            detail::HandleKey key
        ) noexcept
        {
            for (std::size_t index{}; index < nodes.size(); ++index)
            {
                if (nodes[index].key == key)
                    return index;
            }
            return std::nullopt;
        }

        [[nodiscard]] lux::cxx::expected<void, ScheduleFailure> addEdge(
            std::vector<std::vector<std::size_t>>& edges,
            std::span<const detail::CandidateNode> nodes,
            std::size_t from,
            std::size_t to
        ) noexcept
        {
            if (from == to)
                return lux::cxx::unexpected(failure(
                    EScheduleError::INVALID_RELATION,
                    nodes[from].slot->type,
                    nodes[to].slot->type
                ));
            const auto from_phase = detail::phaseIndex(nodes[from].slot->phase);
            const auto to_phase = detail::phaseIndex(nodes[to].slot->phase);
            if (from_phase > to_phase)
            {
                return lux::cxx::unexpected(failure(
                    EScheduleError::PHASE_ORDER_CONTRADICTION,
                    nodes[from].slot->type,
                    nodes[to].slot->type
                ));
            }
            if (from_phase < to_phase)
                return {};
            auto& outgoing = edges[from];
            if (std::find(outgoing.begin(), outgoing.end(), to) == outgoing.end())
                outgoing.push_back(to);
            return {};
        }

        [[nodiscard]] lux::cxx::expected<detail::ExecutionPlan, ScheduleFailure>
        compilePlan(
            std::span<const detail::CandidateNode> nodes,
            std::span<const detail::SetMembership> memberships,
            std::span<const detail::OrderRelation> orders,
            std::span<const detail::Requirement> requirements
        ) noexcept
        {
            try
            {
                std::vector<std::vector<std::size_t>> edges(nodes.size());

                for (const detail::OrderRelation& relation : orders)
                {
                    const auto source = findNode(nodes, relation.source);
                    if (!source)
                        continue;

                    std::vector<std::size_t> targets;
                    if (relation.target_kind == detail::EOrderTarget::SYSTEM)
                    {
                        if (const auto target = findNode(nodes, relation.target))
                            targets.push_back(*target);
                    }
                    else
                    {
                        for (const detail::SetMembership& membership : memberships)
                        {
                            if (membership.set != relation.set)
                                continue;
                            if (const auto target = findNode(nodes, membership.system);
                                target && *target != *source)
                                targets.push_back(*target);
                        }
                    }

                    for (const std::size_t target : targets)
                    {
                        const std::size_t from = relation.before ? *source : target;
                        const std::size_t to = relation.before ? target : *source;
                        if (auto result = addEdge(edges, nodes, from, to); !result)
                            return lux::cxx::unexpected(result.error());
                    }
                }

                for (const detail::Requirement& requirement : requirements)
                {
                    const auto consumer = findNode(nodes, requirement.consumer);
                    const auto provider = findNode(nodes, requirement.provider);
                    if (!consumer || !provider)
                        return lux::cxx::unexpected(failure(
                            EScheduleError::INVALID_HANDLE
                        ));
                    if (auto result = addEdge(
                            edges, nodes, *provider, *consumer); !result)
                        return lux::cxx::unexpected(result.error());
                }

                detail::ExecutionPlan result;
                for (std::size_t phase{}; phase != 3; ++phase)
                {
                    std::vector<std::size_t> phase_nodes;
                    std::vector<std::size_t> indegree(nodes.size());
                    for (std::size_t index{}; index < nodes.size(); ++index)
                    {
                        if (detail::phaseIndex(nodes[index].slot->phase) == phase)
                            phase_nodes.push_back(index);
                        for (const std::size_t target : edges[index])
                            ++indegree[target];
                    }

                    std::vector<std::size_t> ready;
                    for (const std::size_t index : phase_nodes)
                    {
                        if (indegree[index] == 0)
                            ready.push_back(index);
                    }
                    const auto stable = [&](std::size_t a, std::size_t b) noexcept
                    {
                        return nodeLess(nodes[a], nodes[b]);
                    };
                    std::sort(ready.begin(), ready.end(), stable);

                    std::size_t emitted{};
                    while (!ready.empty())
                    {
                        std::vector<std::size_t> selected;
                        for (const std::size_t candidate : ready)
                        {
                            bool compatible = true;
                            for (const std::size_t existing : selected)
                            {
                                if (detail::accessConflict(
                                        *nodes[candidate].slot,
                                        *nodes[existing].slot))
                                {
                                    compatible = false;
                                    break;
                                }
                            }
                            if (compatible)
                                selected.push_back(candidate);
                        }
                        detail::require(!selected.empty());

                        std::vector<std::uint32_t> wave;
                        wave.reserve(selected.size());
                        for (const std::size_t index : selected)
                        {
                            wave.push_back(nodes[index].key.slot);
                            result.phase_order[phase].push_back(
                                nodes[index].key.slot
                            );
                        }
                        result.waves[phase].push_back(std::move(wave));
                        emitted += selected.size();

                        for (const std::size_t index : selected)
                        {
                            ready.erase(std::find(ready.begin(), ready.end(), index));
                        }
                        std::vector<std::size_t> newly_ready;
                        for (const std::size_t index : selected)
                        {
                            for (const std::size_t target : edges[index])
                            {
                                detail::require(indegree[target] != 0);
                                --indegree[target];
                                if (indegree[target] == 0)
                                    newly_ready.push_back(target);
                            }
                        }
                        for (const std::size_t index : newly_ready)
                        {
                            if (detail::phaseIndex(nodes[index].slot->phase) == phase &&
                                std::find(ready.begin(), ready.end(), index) == ready.end())
                                ready.push_back(index);
                        }
                        std::sort(ready.begin(), ready.end(), stable);
                    }

                    if (emitted != phase_nodes.size())
                    {
                        return lux::cxx::unexpected(failure(
                            EScheduleError::DEPENDENCY_CYCLE
                        ));
                    }
                }
                return result;
            }
            catch (...)
            {
                return lux::cxx::unexpected(failure(
                    EScheduleError::ALLOCATION_FAILURE
                ));
            }
        }
    } // namespace

    Schedule::Schedule(World& world) noexcept
        : impl_(std::make_unique<Impl>(world))
    {
        detail::require(std::this_thread::get_id() == world.owner_thread_);
        detail::require(world.state_ == detail::EWorldState::IDLE);
        detail::require(world.schedule_ == nullptr);
        world.schedule_ = this;
    }

    Schedule::~Schedule() noexcept
    {
        detail::require(std::this_thread::get_id() == impl_->owner_thread);
        detail::require(!impl_->executing && !impl_->edit_open);
        requestClose();
        for (std::size_t step{}; step <= impl_->slots.size(); ++step)
        {
            if (closeComplete())
                break;
            runCloseStep();
        }
        detail::require(closeComplete());
        for (auto& slot : impl_->slots)
        {
            if (!slot)
                continue;
            slot->commands->invalidate();
            slot.reset();
        }
        detail::require(impl_->world->schedule_ == this);
        impl_->world->schedule_ = nullptr;
    }

    lux::cxx::expected<ScheduleEdit, ScheduleFailure> Schedule::edit() noexcept
    {
        if (std::this_thread::get_id() != impl_->owner_thread)
            return lux::cxx::unexpected(failure(EScheduleError::EXECUTING));
        if (impl_->edit_open)
            return lux::cxx::unexpected(failure(EScheduleError::EDIT_IN_PROGRESS));
        if (impl_->executing)
            return lux::cxx::unexpected(failure(EScheduleError::EXECUTING));
        if (impl_->closing)
            return lux::cxx::unexpected(failure(EScheduleError::CLOSING));
        try
        {
            impl_->edit_open = true;
            return ScheduleEdit(*this);
        }
        catch (...)
        {
            impl_->edit_open = false;
            return lux::cxx::unexpected(failure(
                EScheduleError::ALLOCATION_FAILURE
            ));
        }
    }

    ScheduleEdit::ScheduleEdit(Schedule& schedule)
        : schedule_(&schedule), impl_(std::make_unique<Impl>())
    {
    }

    ScheduleEdit::ScheduleEdit(ScheduleEdit&& other) noexcept
        : schedule_(std::exchange(other.schedule_, nullptr)),
          impl_(std::move(other.impl_))
    {
    }

    ScheduleEdit& ScheduleEdit::operator=(ScheduleEdit&& other) noexcept
    {
        if (this != &other)
        {
            release();
            schedule_ = std::exchange(other.schedule_, nullptr);
            impl_ = std::move(other.impl_);
        }
        return *this;
    }

    ScheduleEdit::~ScheduleEdit() noexcept
    {
        release();
    }

    lux::cxx::expected<detail::StagedSystemHandle, ScheduleFailure>
    ScheduleEdit::stageAdd(
        lux::cxx::TypeToken type,
        std::unique_ptr<System> system,
        SystemPhase phase,
        ESystemExecutionAffinity affinity,
        bool (*affinity_validator)(const System&) noexcept
    ) noexcept
    {
        if (impl_->failure)
            return lux::cxx::unexpected(*impl_->failure);
        try
        {
            auto& owner = *schedule_->impl_;
            auto slot = std::make_unique<detail::ScheduleSlot>();
            impl_->additions.reserve(impl_->additions.size() + 1);

            std::uint32_t index{};
            for (; index < owner.slots.size(); ++index)
            {
                if (!owner.slots[index] && !owner.reserved[index])
                    break;
            }
            if (index == owner.slots.size())
            {
                const std::size_t required = owner.slots.size() + 1;
                owner.slots.reserve(required);
                owner.generations.reserve(required);
                owner.reserved.reserve(required);
                owner.slots.push_back(nullptr);
                owner.generations.push_back(1);
                owner.reserved.push_back(false);
            }
            owner.reserved[index] = true;

            slot->system = std::move(system);
            slot->type = type;
            slot->phase = phase;
            slot->affinity = affinity;
            slot->affinity_validator = affinity_validator;
            slot->sequence = owner.next_sequence++;

            const auto generation = owner.generations[index];
            impl_->additions.push_back(Impl::Addition{
                detail::HandleKey{owner.owner_id, index, generation},
                std::move(slot)});
            return detail::StagedSystemHandle{index, generation};
        }
        catch (...)
        {
            recordFailure(EScheduleError::ALLOCATION_FAILURE);
            return lux::cxx::unexpected(*impl_->failure);
        }
    }

    void ScheduleEdit::addToSet(
        AnySystemHandle system,
        SystemSetId set
    ) noexcept
    {
        if (impl_->failure)
            return;
        if (!system || !set.valid())
        {
            recordFailure(EScheduleError::INVALID_RELATION);
            return;
        }
        try
        {
            impl_->memberships.push_back(
                detail::SetMembership{detail::key(system), detail::own(set)}
            );
        }
        catch (...)
        {
            recordFailure(EScheduleError::ALLOCATION_FAILURE);
        }
    }

    void ScheduleEdit::before(
        AnySystemHandle system,
        AnySystemHandle other
    ) noexcept
    {
        if (impl_->failure)
            return;
        try
        {
            impl_->orders.push_back(detail::OrderRelation{
                detail::key(system), detail::EOrderTarget::SYSTEM,
                detail::key(other), {}, true});
        }
        catch (...)
        {
            recordFailure(EScheduleError::ALLOCATION_FAILURE);
        }
    }

    void ScheduleEdit::before(
        AnySystemHandle system,
        SystemSetId set
    ) noexcept
    {
        if (impl_->failure)
            return;
        try
        {
            impl_->orders.push_back(detail::OrderRelation{
                detail::key(system), detail::EOrderTarget::SET,
                {}, detail::own(set), true});
        }
        catch (...)
        {
            recordFailure(EScheduleError::ALLOCATION_FAILURE);
        }
    }

    void ScheduleEdit::after(
        AnySystemHandle system,
        AnySystemHandle other
    ) noexcept
    {
        if (impl_->failure)
            return;
        try
        {
            impl_->orders.push_back(detail::OrderRelation{
                detail::key(system), detail::EOrderTarget::SYSTEM,
                detail::key(other), {}, false});
        }
        catch (...)
        {
            recordFailure(EScheduleError::ALLOCATION_FAILURE);
        }
    }

    void ScheduleEdit::after(
        AnySystemHandle system,
        SystemSetId set
    ) noexcept
    {
        if (impl_->failure)
            return;
        try
        {
            impl_->orders.push_back(detail::OrderRelation{
                detail::key(system), detail::EOrderTarget::SET,
                {}, detail::own(set), false});
        }
        catch (...)
        {
            recordFailure(EScheduleError::ALLOCATION_FAILURE);
        }
    }

    void ScheduleEdit::require(
        AnySystemHandle consumer,
        AnySystemHandle provider
    ) noexcept
    {
        if (impl_->failure)
            return;
        try
        {
            impl_->requirements.push_back(
                detail::Requirement{detail::key(consumer), detail::key(provider)}
            );
        }
        catch (...)
        {
            recordFailure(EScheduleError::ALLOCATION_FAILURE);
        }
    }

    void ScheduleEdit::remove(AnySystemHandle value) noexcept
    {
        if (impl_->failure)
            return;
        try
        {
            impl_->removals.push_back(detail::key(value));
        }
        catch (...)
        {
            recordFailure(EScheduleError::ALLOCATION_FAILURE);
        }
    }

    void ScheduleEdit::recordFailure(EScheduleError error) noexcept
    {
        if (!impl_->failure)
            impl_->failure = failure(error);
    }

    std::uint64_t ScheduleEdit::ownerId() const noexcept
    {
        return schedule_ == nullptr ? 0 : schedule_->impl_->owner_id;
    }

    lux::cxx::expected<void, ScheduleFailure> ScheduleEdit::commit() noexcept
    {
        if (schedule_ == nullptr)
            return lux::cxx::unexpected(failure(EScheduleError::INVALID_HANDLE));
        if (impl_->failure)
            return lux::cxx::unexpected(*impl_->failure);

        auto& owner = *schedule_->impl_;
        try
        {
            for (const auto& removal : impl_->removals)
            {
                if (removal.owner != owner.owner_id ||
                    removal.slot >= owner.generations.size() ||
                    owner.generations[removal.slot] != removal.generation ||
                    (owner.find(removal) == nullptr &&
                     stagedSlot(*impl_, removal) == nullptr))
                    return lux::cxx::unexpected(failure(
                        EScheduleError::INVALID_HANDLE
                    ));
            }
            for (const auto& membership : impl_->memberships)
            {
                if (!candidateHandle(owner, *impl_, membership.system))
                    return lux::cxx::unexpected(failure(
                        EScheduleError::INVALID_HANDLE
                    ));
            }
            for (const auto& order : impl_->orders)
            {
                if (!candidateHandle(owner, *impl_, order.source) ||
                    (order.target_kind == detail::EOrderTarget::SYSTEM &&
                     !candidateHandle(owner, *impl_, order.target)))
                    return lux::cxx::unexpected(failure(
                        EScheduleError::INVALID_HANDLE
                    ));
            }
            for (const auto& requirement : impl_->requirements)
            {
                if (!candidateHandle(owner, *impl_, requirement.consumer) ||
                    !candidateHandle(owner, *impl_, requirement.provider) ||
                    requirement.consumer == requirement.provider)
                    return lux::cxx::unexpected(failure(
                        EScheduleError::INVALID_RELATION
                    ));
            }

            std::vector<detail::SetMembership> memberships = owner.memberships;
            memberships.insert(
                memberships.end(),
                impl_->memberships.begin(), impl_->memberships.end()
            );
            std::vector<detail::OrderRelation> orders = owner.orders;
            orders.insert(orders.end(), impl_->orders.begin(), impl_->orders.end());
            std::vector<detail::Requirement> requirements = owner.requirements;
            requirements.insert(
                requirements.end(),
                impl_->requirements.begin(), impl_->requirements.end()
            );

            const auto removed = [&](detail::HandleKey key) noexcept
            {
                return contains(impl_->removals, key);
            };
            for (const auto& requirement : requirements)
            {
                if (removed(requirement.provider) &&
                    !removed(requirement.consumer))
                    return lux::cxx::unexpected(failure(
                        EScheduleError::HARD_DEPENDENT_EXISTS
                    ));
            }
            std::erase_if(memberships, [&](const auto& value)
            {
                return removed(value.system);
            });
            std::erase_if(orders, [&](const auto& value)
            {
                return removed(value.source) ||
                    (value.target_kind == detail::EOrderTarget::SYSTEM &&
                     removed(value.target));
            });
            std::erase_if(requirements, [&](const auto& value)
            {
                return removed(value.consumer) || removed(value.provider);
            });

            if (setCollision(memberships, orders))
                return lux::cxx::unexpected(failure(
                    EScheduleError::SET_ID_COLLISION
                ));

            for (auto& addition : impl_->additions)
            {
                if (removed(addition.key))
                    continue;
                if (auto result = captureAccess(*addition.slot); !result)
                    return result;
            }

            std::vector<detail::CandidateNode> nodes;
            nodes.reserve(owner.slots.size() + impl_->additions.size());
            for (std::uint32_t index{}; index < owner.slots.size(); ++index)
            {
                if (!owner.slots[index])
                    continue;
                detail::HandleKey key{
                    owner.owner_id, index, owner.generations[index]};
                if (!removed(key))
                    nodes.push_back({key, owner.slots[index].get(), nodes.size()});
            }
            for (auto& addition : impl_->additions)
            {
                if (!removed(addition.key))
                    nodes.push_back({
                        addition.key, addition.slot.get(), nodes.size()});
            }
            std::sort(nodes.begin(), nodes.end(), nodeLess);
            for (std::size_t index{}; index < nodes.size(); ++index)
                nodes[index].candidate_index = index;

            for (std::size_t left{}; left < nodes.size(); ++left)
            {
                for (std::size_t right = left + 1; right < nodes.size(); ++right)
                {
                    if (detail::tokenCollision(
                            nodes[left].slot->type,
                            nodes[right].slot->type))
                    {
                        return lux::cxx::unexpected(failure(
                            EScheduleError::TYPE_TOKEN_COLLISION,
                            nodes[left].slot->type,
                            nodes[right].slot->type
                        ));
                    }
                    for (const ComponentAccess& a :
                         nodes[left].slot->component_access)
                    {
                        for (const ComponentAccess& b :
                             nodes[right].slot->component_access)
                        {
                            if (detail::tokenCollision(a.type, b.type))
                            {
                                return lux::cxx::unexpected(failure(
                                    EScheduleError::TYPE_TOKEN_COLLISION,
                                    a.type,
                                    b.type
                                ));
                            }
                        }
                    }
                    for (const ExternalAccess& a :
                         nodes[left].slot->external_access)
                    {
                        for (const ExternalAccess& b :
                             nodes[right].slot->external_access)
                        {
                            if (detail::tokenCollision(a.type, b.type))
                            {
                                return lux::cxx::unexpected(failure(
                                    EScheduleError::TYPE_TOKEN_COLLISION,
                                    a.type,
                                    b.type
                                ));
                            }
                        }
                    }
                }
            }

            for (const auto& node : nodes)
            {
                if (node.slot->affinity ==
                        ESystemExecutionAffinity::OWNER_THREAD &&
                    (node.slot->affinity_validator == nullptr ||
                     !node.slot->affinity_validator(*node.slot->system)))
                {
                    return lux::cxx::unexpected(failure(
                        EScheduleError::EXECUTION_AFFINITY_MISMATCH,
                        node.slot->type
                    ));
                }
            }

            auto plan = compilePlan(nodes, memberships, orders, requirements);
            if (!plan)
                return lux::cxx::unexpected(plan.error());

            for (const detail::HandleKey removal : impl_->removals)
            {
                if (auto* slot = owner.find(removal))
                {
                    if (!slot->stop_requested || !slot->system->stopped())
                        return lux::cxx::unexpected(failure(
                            EScheduleError::SYSTEM_NOT_STOPPED,
                            slot->type
                        ));
                }
            }

            SystemStart start{*owner.world};
            for (std::size_t phase{}; phase != 3; ++phase)
            {
                for (const auto& wave : plan->waves[phase])
                {
                    for (const std::uint32_t slot_index : wave)
                    {
                        auto found = std::find_if(
                            impl_->additions.begin(), impl_->additions.end(),
                            [&](const auto& value)
                            {
                                return value.key.slot == slot_index &&
                                    !removed(value.key);
                            }
                        );
                        if (found == impl_->additions.end())
                            continue;
                        if (auto result = found->slot->system->start(start); !result)
                        {
                            return lux::cxx::unexpected(failure(
                                EScheduleError::SYSTEM_START_FAILED,
                                found->slot->type
                            ));
                        }
                    }
                }
            }

            for (const detail::HandleKey removal : impl_->removals)
            {
                if (auto* slot = owner.find(removal))
                {
                    owner.retired_discarded += slot->commands->discarded();
                    owner.retired_command_allocations +=
                        slot->commands->allocationEvents();
                    slot->commands->invalidate();
                    owner.slots[removal.slot].reset();
                    ++owner.generations[removal.slot];
                    if (owner.generations[removal.slot] == 0)
                        ++owner.generations[removal.slot];
                }
            }
            for (auto& addition : impl_->additions)
            {
                owner.reserved[addition.key.slot] = false;
                if (removed(addition.key))
                {
                    ++owner.generations[addition.key.slot];
                    if (owner.generations[addition.key.slot] == 0)
                        ++owner.generations[addition.key.slot];
                    continue;
                }
                owner.slots[addition.key.slot] = std::move(addition.slot);
            }
            owner.memberships = std::move(memberships);
            owner.orders = std::move(orders);
            owner.requirements = std::move(requirements);
            owner.plan = std::move(*plan);
            impl_->committed = true;
            detail::require(owner.edit_open);
            owner.edit_open = false;
            schedule_ = nullptr;
            return {};
        }
        catch (...)
        {
            return lux::cxx::unexpected(failure(
                EScheduleError::ALLOCATION_FAILURE
            ));
        }
    }

    void ScheduleEdit::release() noexcept
    {
        if (schedule_ == nullptr)
            return;
        auto& owner = *schedule_->impl_;
        if (impl_ && !impl_->committed)
        {
            for (const auto& addition : impl_->additions)
            {
                if (addition.key.slot >= owner.reserved.size() ||
                    !owner.reserved[addition.key.slot])
                    continue;
                owner.reserved[addition.key.slot] = false;
                ++owner.generations[addition.key.slot];
                if (owner.generations[addition.key.slot] == 0)
                    ++owner.generations[addition.key.slot];
            }
        }
        detail::require(owner.edit_open);
        owner.edit_open = false;
        schedule_ = nullptr;
        impl_.reset();
    }

    void Schedule::run(float delta_seconds, std::uint64_t tick_index) noexcept
    {
        detail::require(std::this_thread::get_id() == impl_->owner_thread);
        detail::require(!impl_->executing && !impl_->edit_open && !impl_->closing);
        detail::require(impl_->world->state_ == detail::EWorldState::IDLE);
        impl_->executing = true;
        impl_->world->state_ = detail::EWorldState::EXECUTING;

        for (std::size_t phase{}; phase != 3; ++phase)
        {
            for (const auto& wave : impl_->plan.waves[phase])
            {
                for (const std::uint32_t index : wave)
                {
                    auto& slot = *impl_->slots[index];
#if !defined(NDEBUG) || defined(LUX_ECS_CONTRACT_CHECKS)
                    if (slot.affinity == ESystemExecutionAffinity::OWNER_THREAD)
                    {
                        detail::require(
                            slot.affinity_validator != nullptr &&
                            slot.affinity_validator(*slot.system)
                        );
                    }
#endif
                    slot.changes.begin();
                    WorldCommands commands = slot.commands->beginExecution();
                    SystemFrame frame(
                        *impl_->world,
                        commands,
                        delta_seconds,
                        tick_index,
                        slot.component_access,
                        slot.access_complete,
                        slot.changes.recorder()
                    );
                    slot.system->update(frame);
                    slot.commands->endExecution();
                }

                bool overflow{};
                for (const std::uint32_t index : wave)
                    overflow = overflow || impl_->slots[index]->changes.overflow;
                if (overflow)
                {
                    detail::establishWorldChangeBaseline(*impl_->world);
                }
                else
                {
                    for (const std::uint32_t index : wave)
                    {
                        for (const detail::ShardRecord& record :
                             impl_->slots[index]->changes.records)
                        {
                            detail::recordWorldComponentChange(
                                *impl_->world,
                                record.storage,
                                record.entity,
                                record.kind
                            );
                        }
                    }
                }
            }

            impl_->world->state_ = detail::EWorldState::APPLYING_COMMANDS;
            {
                WorldEdit edit(*impl_->world, false);
                for (const std::uint32_t index : impl_->plan.phase_order[phase])
                    impl_->slots[index]->commands->applyPending(edit);
            }
            impl_->world->state_ = phase == 2
                ? detail::EWorldState::IDLE
                : detail::EWorldState::EXECUTING;
        }
        impl_->executing = false;
    }

    lux::cxx::expected<void, ScheduleFailure> Schedule::requestStop(
        AnySystemHandle value
    ) noexcept
    {
        detail::require(std::this_thread::get_id() == impl_->owner_thread);
        if (impl_->executing || impl_->edit_open)
            return lux::cxx::unexpected(failure(EScheduleError::EXECUTING));
        const auto key = detail::key(value);
        auto* slot = impl_->find(key);
        if (slot == nullptr)
            return lux::cxx::unexpected(failure(EScheduleError::INVALID_HANDLE));
        if (!impl_->dependentStopped(key))
            return lux::cxx::unexpected(failure(
                EScheduleError::HARD_DEPENDENT_EXISTS,
                slot->type
            ));
        if (!slot->stop_requested)
        {
            slot->stop_requested = true;
            slot->system->requestStop();
        }
        return {};
    }

    bool Schedule::stopped(AnySystemHandle value) const noexcept
    {
        const auto* slot = impl_->find(detail::key(value));
        return slot != nullptr && slot->stop_requested && slot->system->stopped();
    }

    void Schedule::requestClose() noexcept
    {
        detail::require(std::this_thread::get_id() == impl_->owner_thread);
        detail::require(!impl_->executing && !impl_->edit_open);
        impl_->closing = true;
        runCloseStep();
    }

    void Schedule::runCloseStep() noexcept
    {
        detail::require(std::this_thread::get_id() == impl_->owner_thread);
        detail::require(impl_->closing && !impl_->executing && !impl_->edit_open);

        std::vector<std::pair<std::uint64_t, std::uint32_t>> candidates;
        try
        {
            candidates.reserve(impl_->slots.size());
            for (std::uint32_t index{}; index < impl_->slots.size(); ++index)
            {
                if (impl_->slots[index] && !impl_->slots[index]->stop_requested)
                    candidates.emplace_back(impl_->slots[index]->sequence, index);
            }
        }
        catch (...)
        {
            detail::contractFailure();
        }
        std::sort(candidates.begin(), candidates.end(), std::greater<>());
        for (const auto [_, index] : candidates)
        {
            detail::HandleKey key{
                impl_->owner_id, index, impl_->generations[index]};
            auto& slot = *impl_->slots[index];
            if (impl_->dependentStopped(key))
            {
                slot.stop_requested = true;
                slot.system->requestStop();
            }
        }
    }

    bool Schedule::closeComplete() const noexcept
    {
        for (const auto& slot : impl_->slots)
        {
            if (slot && (!slot->stop_requested || !slot->system->stopped()))
                return false;
        }
        return true;
    }

    detail::ExecutionPlanSnapshot detail::ScheduleTestAccess::snapshot(
        const Schedule& schedule
    )
    {
        ExecutionPlanSnapshot result;
        for (std::size_t phase{}; phase != 3; ++phase)
        {
            for (const auto& wave : schedule.impl_->plan.waves[phase])
            {
                result.batches.push_back(wave);
                for (const std::uint32_t index : wave)
                {
                    const auto& slot = *schedule.impl_->slots[index];
                    result.order.push_back(ExecutionPlanEntry{
                        slot.type,
                        index,
                        slot.affinity == ESystemExecutionAffinity::OWNER_THREAD});
                }
            }
        }
        return result;
    }

    std::size_t detail::ScheduleTestAccess::discardedCommands(
        const Schedule& schedule
    ) noexcept
    {
        std::size_t result = schedule.impl_->retired_discarded;
        for (const auto& slot : schedule.impl_->slots)
        {
            if (slot)
                result += slot->commands->discarded();
        }
        return result;
    }

    std::size_t detail::ScheduleTestAccess::commandAllocationEvents(
        const Schedule& schedule
    ) noexcept
    {
        std::size_t result = schedule.impl_->retired_command_allocations;
        for (const auto& slot : schedule.impl_->slots)
        {
            if (slot)
                result += slot->commands->allocationEvents();
        }
        return result;
    }
} // namespace lux::ecs
