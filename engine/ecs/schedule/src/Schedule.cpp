#include <lux/engine/ecs/Schedule.hpp>

#include <lux/engine/ecs/core/detail/CommandStorage.hpp>
#include <lux/engine/ecs/schedule/detail/ScheduleTestAccess.hpp>

#include <algorithm>
#include <atomic>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace lux::ecs
{
    namespace
    {
        struct OwnedSet final
        {
            std::uint64_t hash{};
            std::string name;
        };

        struct OwnedOrder final
        {
            ESystemOrder relation{ESystemOrder::AFTER};
            OwnedSet target;
            bool required{};
        };

        struct Slot final
        {
            std::unique_ptr<System> system;
            lux::cxx::TypeToken type;
            SystemPhase phase{SystemPhase::Update};
            std::vector<OwnedSet> sets;
            std::vector<OwnedOrder> orders;
            std::vector<lux::cxx::TypeToken> required;
            std::vector<ComponentAccess> component_access;
            std::vector<ExternalAccess> external_access;
            bool structural{};
            bool access_complete{};
            bool object_affine{};
            std::unique_ptr<detail::CommandShard> commands;
            std::vector<entt::scoped_connection> observers;
            std::uint32_t generation{1};
            std::uint64_t sequence{};
            bool close_requested{};
        };

        struct Candidate final
        {
            std::uint32_t slot{};
            Slot* descriptor{};
            lux::cxx::TypeToken type;
            SystemPhase phase{SystemPhase::Update};
            std::uint64_t sequence{};
        };

        [[nodiscard]] bool sameType(
            lux::cxx::TypeToken left,
            lux::cxx::TypeToken right
        ) noexcept
        {
            return left.hash() == right.hash() && left.name() == right.name();
        }

        [[nodiscard]] int phaseRank(SystemPhase phase) noexcept
        {
            return static_cast<int>(phase);
        }

        [[nodiscard]] bool accessCompatible(
            const Slot& left,
            const Slot& right
        ) noexcept
        {
            if (left.object_affine || right.object_affine ||
                !left.access_complete || !right.access_complete)
                return false;
            if (left.structural || right.structural)
                return false;

            const auto conflicts = [](const auto& a, const auto& b)
            {
                for (const auto& lhs : a)
                {
                    for (const auto& rhs : b)
                    {
                        if (sameType(lhs.type, rhs.type) &&
                            (lhs.mode == EAccessMode::WRITE ||
                             rhs.mode == EAccessMode::WRITE))
                        {
                            return true;
                        }
                    }
                }
                return false;
            };

            return !conflicts(left.component_access, right.component_access) &&
                   !conflicts(left.external_access, right.external_access);
        }
    } // namespace

    struct Schedule::Impl final
    {
        explicit Impl(World& value) noexcept
            : world(&value),
              id(next_id.fetch_add(1, std::memory_order_relaxed)),
              owner_thread(std::this_thread::get_id())
        {
            if (id == 0)
                id = next_id.fetch_add(1, std::memory_order_relaxed);
        }

        static std::atomic<std::uint64_t> next_id;

        World* world{};
        std::uint64_t id{};
        std::thread::id owner_thread;
        std::vector<std::unique_ptr<Slot>> slots;
        std::vector<std::uint32_t> free_slots;
        std::vector<std::uint32_t> compiled;
        std::vector<std::vector<std::uint32_t>> batches;
        std::vector<std::vector<std::uint32_t>> outgoing;
        std::uint64_t next_sequence{1};
        bool editing{};
        bool executing{};
        bool closing{};

        void requireOwnerThread() const noexcept
        {
            detail::require(std::this_thread::get_id() == owner_thread);
        }

        void clearObservers(Slot& slot) noexcept
        {
            detail::require(world->observer_relations_ >= slot.observers.size());
            world->observer_relations_ -= slot.observers.size();
            slot.observers.clear();
        }

        void applyCommands(std::span<const std::uint32_t> command_order) noexcept
        {
            world->state_ = detail::EWorldState::APPLYING_COMMANDS;
            WorldEdit edit(*world, false);

            for (const std::uint32_t index : command_order)
                slots[index]->commands->beginApply();
            for (const std::uint32_t index : command_order)
                slots[index]->commands->applyPending(edit);
            for (const std::uint32_t index : command_order)
                slots[index]->commands->endApply();

            world->state_ = detail::EWorldState::IDLE;
        }

        void applyCommands() noexcept
        {
            applyCommands(compiled);
        }

        void detachAll() noexcept
        {
            SystemDetach detach(*world);
            for (auto iterator = compiled.rbegin(); iterator != compiled.rend(); ++iterator)
            {
                Slot& slot = *slots[*iterator];
                clearObservers(slot);
                slot.system->onDetach(detach);
                slot.commands->invalidate();
                slot.system.reset();
            }
            compiled.clear();
            batches.clear();
            outgoing.clear();
        }
    };

    std::atomic<std::uint64_t> Schedule::Impl::next_id{1};

    struct ScheduleEdit::Impl final
    {
        struct Addition final
        {
            lux::cxx::TypeToken type;
            std::unique_ptr<System> system;
            SystemPhase phase{SystemPhase::Update};
            std::uint32_t slot{};
            std::uint32_t generation{};
            bool object_affine{};
        };

        std::vector<Addition> additions;
        std::vector<detail::AnySystemHandle> removals;
        std::optional<ScheduleFailure> failure;
        std::size_t free_slots_used{};
    };

    Schedule::Schedule(World& world) noexcept
        : impl_(std::make_unique<Impl>(world))
    {
        detail::require(world.schedule_ == nullptr);
        world.schedule_ = this;
    }

    Schedule::~Schedule() noexcept
    {
        impl_->requireOwnerThread();
        detail::require(!impl_->editing && !impl_->executing);
        requestClose();
        for (std::size_t pass{}; pass <= impl_->compiled.size(); ++pass)
            runCloseStep(0.0F, 0);
        detail::require(closeComplete());
        impl_->detachAll();
        detail::require(impl_->world->observer_relations_ == 0);
        impl_->world->schedule_ = nullptr;
    }

    lux::cxx::expected<ScheduleEdit, ScheduleFailure> Schedule::edit() noexcept
    {
        impl_->requireOwnerThread();
        if (impl_->editing)
        {
            return lux::cxx::unexpected(
                ScheduleFailure{EScheduleError::EDIT_IN_PROGRESS}
            );
        }
        if (impl_->executing)
        {
            return lux::cxx::unexpected(
                ScheduleFailure{EScheduleError::EXECUTING}
            );
        }
        if (impl_->closing)
        {
            return lux::cxx::unexpected(
                ScheduleFailure{EScheduleError::CLOSING}
            );
        }
        if (impl_->world->state_ != detail::EWorldState::IDLE)
        {
            return lux::cxx::unexpected(
                ScheduleFailure{EScheduleError::EXECUTING}
            );
        }

        impl_->editing = true;
        try
        {
            return ScheduleEdit(*this);
        }
        catch (...)
        {
            impl_->editing = false;
            return lux::cxx::unexpected(
                ScheduleFailure{EScheduleError::ALLOCATION_FAILURE}
            );
        }
    }

    void Schedule::run(float delta_seconds, std::uint64_t tick_index) noexcept
    {
        impl_->requireOwnerThread();
        detail::require(
            !impl_->editing && !impl_->executing && !impl_->closing &&
            impl_->world->state_ == detail::EWorldState::IDLE
        );

        impl_->executing = true;
        impl_->world->state_ = detail::EWorldState::EXECUTING;
        for (const std::uint32_t index : impl_->compiled)
        {
            Slot& slot = *impl_->slots[index];
            const SystemFrame frame(
                *impl_->world,
                slot.commands->writer(),
                delta_seconds,
                tick_index
            );
            slot.system->update(frame);
        }
        impl_->applyCommands();
        impl_->executing = false;
    }

    void Schedule::requestClose() noexcept
    {
        impl_->requireOwnerThread();
        impl_->closing = true;
        runCloseStep(0.0F, 0);
    }

    void Schedule::runCloseStep(float, std::uint64_t) noexcept
    {
        impl_->requireOwnerThread();
        if (!impl_->closing)
            return;

        for (auto iterator = impl_->compiled.rbegin(); iterator != impl_->compiled.rend(); ++iterator)
        {
            const std::uint32_t index = *iterator;
            Slot& slot = *impl_->slots[index];
            if (slot.close_requested)
                continue;

            bool dependents_complete = true;
            for (const std::uint32_t dependent : impl_->outgoing[index])
            {
                const Slot& consumer = *impl_->slots[dependent];
                if (!consumer.close_requested || !consumer.system->closeComplete())
                {
                    dependents_complete = false;
                    break;
                }
            }

            if (dependents_complete)
            {
                slot.system->requestClose();
                slot.close_requested = true;
            }
        }
    }

    bool Schedule::closeComplete() const noexcept
    {
        impl_->requireOwnerThread();
        if (!impl_->closing)
            return false;
        for (const std::uint32_t index : impl_->compiled)
        {
            const Slot& slot = *impl_->slots[index];
            if (!slot.close_requested || !slot.system->closeComplete())
                return false;
        }
        return true;
    }

    void* Schedule::getRaw(
        detail::AnySystemHandle handle,
        lux::cxx::TypeToken expected_type
    ) noexcept
    {
        impl_->requireOwnerThread();
        if (handle.owner != impl_->id || handle.slot >= impl_->slots.size())
            return nullptr;
        Slot* slot = impl_->slots[handle.slot].get();
        if (slot == nullptr || !slot->system || slot->generation != handle.generation ||
            !sameType(slot->type, expected_type))
        {
            return nullptr;
        }
        return slot->system.get();
    }

    detail::ExecutionPlanSnapshot detail::ScheduleTestAccess::snapshot(
        const Schedule& schedule
    )
    {
        ExecutionPlanSnapshot result;
        result.order.reserve(schedule.impl_->compiled.size());
        for (const std::uint32_t index : schedule.impl_->compiled)
        {
            const Slot& slot = *schedule.impl_->slots[index];
            result.order.push_back(
                ExecutionPlanEntry{
                    slot.type,
                    index,
                    slot.object_affine}
            );
        }
        result.batches = schedule.impl_->batches;
        return result;
    }

    std::size_t detail::ScheduleTestAccess::discardedCommands(
        const Schedule& schedule
    ) noexcept
    {
        std::size_t result{};
        for (const auto& slot : schedule.impl_->slots)
            if (slot && slot->commands)
                result += slot->commands->discarded();
        return result;
    }

    std::size_t detail::ScheduleTestAccess::commandAllocationEvents(
        const Schedule& schedule
    ) noexcept
    {
        std::size_t result{};
        for (const auto& slot : schedule.impl_->slots)
            if (slot && slot->commands)
                result += slot->commands->allocationEvents();
        return result;
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
        bool object_affine
    ) noexcept
    {
        if (schedule_ != nullptr)
            schedule_->impl_->requireOwnerThread();
        if (impl_->failure)
            return lux::cxx::unexpected(*impl_->failure);

        try
        {
            std::uint32_t slot{};
            std::uint32_t generation{1};
            auto& schedule_impl = *schedule_->impl_;
            if (impl_->free_slots_used < schedule_impl.free_slots.size())
            {
                slot = schedule_impl.free_slots[impl_->free_slots_used++];
                generation = schedule_impl.slots[slot]->generation;
            }
            else
            {
                slot = static_cast<std::uint32_t>(
                    schedule_impl.slots.size() +
                    impl_->additions.size() - impl_->free_slots_used
                );
            }

            impl_->additions.push_back(
                Impl::Addition{
                    type,
                    std::move(system),
                    phase,
                    slot,
                    generation,
                    object_affine}
            );
            return detail::StagedSystemHandle{slot, generation};
        }
        catch (...)
        {
            const ScheduleFailure failure{EScheduleError::ALLOCATION_FAILURE, type};
            impl_->failure = failure;
            return lux::cxx::unexpected(failure);
        }
    }

    void ScheduleEdit::stageRemove(detail::AnySystemHandle handle) noexcept
    {
        if (schedule_ != nullptr)
            schedule_->impl_->requireOwnerThread();
        if (schedule_ == nullptr || impl_->failure)
            return;
        try
        {
            impl_->removals.push_back(handle);
        }
        catch (...)
        {
            recordFailure(EScheduleError::ALLOCATION_FAILURE);
        }
    }

    void ScheduleEdit::recordFailure(EScheduleError error) noexcept
    {
        if (impl_ && !impl_->failure)
            impl_->failure = ScheduleFailure{error};
    }

    std::uint64_t ScheduleEdit::ownerId() const noexcept
    {
        return schedule_ ? schedule_->impl_->id : 0;
    }

    lux::cxx::expected<void, ScheduleFailure> ScheduleEdit::commit() noexcept
    {
        if (schedule_ == nullptr)
        {
            return lux::cxx::unexpected(
                ScheduleFailure{EScheduleError::INVALID_HANDLE}
            );
        }
        if (impl_->failure)
            return lux::cxx::unexpected(*impl_->failure);

        auto& owner = *schedule_->impl_;
        owner.requireOwnerThread();
        bool commit_complete{};
        const auto rollback = [&](void*) noexcept
        {
            if (!commit_complete && schedule_ != nullptr)
            {
                schedule_->impl_->editing = false;
                schedule_ = nullptr;
                impl_.reset();
            }
        };
        const std::unique_ptr<void, decltype(rollback)> rollback_guard(
            static_cast<void*>(this),
            rollback
        );
        bool publication_started{};
        try
        {
            std::unordered_set<std::uint32_t> removed_slots;
            removed_slots.reserve(impl_->removals.size());
            for (const detail::AnySystemHandle handle : impl_->removals)
            {
                if (handle.owner != owner.id || handle.slot >= owner.slots.size())
                    return lux::cxx::unexpected(ScheduleFailure{EScheduleError::INVALID_HANDLE});
                Slot* slot = owner.slots[handle.slot].get();
                if (slot == nullptr || !slot->system || slot->generation != handle.generation)
                    return lux::cxx::unexpected(ScheduleFailure{EScheduleError::INVALID_HANDLE});
                if (!slot->system->removable())
                    return lux::cxx::unexpected(ScheduleFailure{EScheduleError::SYSTEM_NOT_REMOVABLE, slot->type});
                if (!slot->system->closeComplete())
                    return lux::cxx::unexpected(ScheduleFailure{EScheduleError::SYSTEM_NOT_CLOSED, slot->type});
                removed_slots.insert(handle.slot);
            }

            std::vector<std::unique_ptr<Slot>> prepared_additions;
            prepared_additions.reserve(impl_->additions.size());
            for (std::size_t addition_index{};
                 addition_index < impl_->additions.size();
                 ++addition_index)
            {
                auto& addition = impl_->additions[addition_index];
                auto slot = std::make_unique<Slot>();
                slot->system = std::move(addition.system);
                slot->type = addition.type;
                slot->phase = addition.phase;
                slot->generation = addition.generation;
                slot->sequence = owner.next_sequence + addition_index;
                slot->object_affine = addition.object_affine;

                for (const SystemSetId set : slot->system->sets())
                    slot->sets.push_back(OwnedSet{set.id.hash(), std::string(set.id.name())});
                for (const SystemOrder value : slot->system->ordering())
                {
                    slot->orders.push_back(OwnedOrder{
                        value.relation,
                        OwnedSet{value.target.id.hash(), std::string(value.target.id.name())},
                        value.required});
                }
                slot->required.assign(
                    slot->system->requiredSystems().begin(),
                    slot->system->requiredSystems().end()
                );
                const SystemAccess access = slot->system->access();
                slot->component_access.assign(
                    access.components.begin(),
                    access.components.end()
                );
                slot->external_access.assign(
                    access.external.begin(),
                    access.external.end()
                );
                slot->structural = access.structural;
                slot->access_complete = access.complete;
                slot->commands = std::make_unique<detail::CommandShard>(slot->generation);
                slot->commands->reserve(64);
                slot->observers.reserve(8);
                prepared_additions.push_back(std::move(slot));
            }

            std::vector<Candidate> candidates;
            candidates.reserve(owner.compiled.size() + prepared_additions.size());
            for (const std::uint32_t index : owner.compiled)
            {
                if (removed_slots.contains(index))
                    continue;
                Slot& slot = *owner.slots[index];
                candidates.push_back(Candidate{
                    index,
                    &slot,
                    slot.type,
                    slot.phase,
                    slot.sequence});
            }
            for (std::size_t index{}; index < prepared_additions.size(); ++index)
            {
                Slot& slot = *prepared_additions[index];
                candidates.push_back(Candidate{
                    impl_->additions[index].slot,
                    &slot,
                    slot.type,
                    slot.phase,
                    slot.sequence});
            }

            for (std::size_t left{}; left < candidates.size(); ++left)
            {
                for (std::size_t right = left + 1; right < candidates.size(); ++right)
                {
                    if (candidates[left].type.hash() == candidates[right].type.hash())
                    {
                        const auto error = candidates[left].type.name() == candidates[right].type.name()
                            ? EScheduleError::DUPLICATE_SYSTEM
                            : EScheduleError::TYPE_TOKEN_COLLISION;
                        return lux::cxx::unexpected(ScheduleFailure{
                            error,
                            candidates[left].type,
                            candidates[right].type});
                    }
                }
            }

            std::unordered_map<std::uint64_t, std::string> set_names;
            for (const Candidate& candidate : candidates)
            {
                for (const OwnedSet& set : candidate.descriptor->sets)
                {
                    if (set.name.empty() ||
                        set.hash != lux::cxx::Fnv1a64::hash(set.name))
                    {
                        return lux::cxx::unexpected(ScheduleFailure{
                            EScheduleError::SET_ID_COLLISION,
                            candidate.type});
                    }
                    const auto [iterator, inserted] = set_names.emplace(set.hash, set.name);
                    if (!inserted && iterator->second != set.name)
                        return lux::cxx::unexpected(ScheduleFailure{EScheduleError::SET_ID_COLLISION, candidate.type});
                }
                for (const OwnedOrder& order : candidate.descriptor->orders)
                {
                    if (order.target.name.empty() ||
                        order.target.hash != lux::cxx::Fnv1a64::hash(order.target.name))
                    {
                        return lux::cxx::unexpected(ScheduleFailure{
                            EScheduleError::SET_ID_COLLISION,
                            candidate.type});
                    }
                    const auto [iterator, inserted] = set_names.emplace(
                        order.target.hash,
                        order.target.name
                    );
                    if (!inserted && iterator->second != order.target.name)
                        return lux::cxx::unexpected(ScheduleFailure{EScheduleError::SET_ID_COLLISION, candidate.type});
                }
            }

            std::vector<std::vector<std::size_t>> edges(candidates.size());
            std::vector<std::size_t> indegree(candidates.size());
            const auto addEdge = [&](std::size_t from, std::size_t to)
            {
                if (from == to)
                    return;
                auto& list = edges[from];
                if (std::find(list.begin(), list.end(), to) == list.end())
                {
                    list.push_back(to);
                    ++indegree[to];
                }
            };

            for (std::size_t left{}; left < candidates.size(); ++left)
            {
                for (std::size_t right{}; right < candidates.size(); ++right)
                {
                    if (phaseRank(candidates[left].phase) < phaseRank(candidates[right].phase))
                        addEdge(left, right);
                }
            }

            for (std::size_t index{}; index < candidates.size(); ++index)
            {
                const Candidate& candidate = candidates[index];
                for (const lux::cxx::TypeToken required : candidate.descriptor->required)
                {
                    auto iterator = std::find_if(
                        candidates.begin(), candidates.end(),
                        [&](const Candidate& other) { return sameType(other.type, required); }
                    );
                    if (iterator == candidates.end())
                    {
                        const bool removed_dependency = std::any_of(
                            removed_slots.begin(),
                            removed_slots.end(),
                            [&](std::uint32_t removed)
                            {
                                return sameType(owner.slots[removed]->type, required);
                            }
                        );
                        return lux::cxx::unexpected(ScheduleFailure{
                            removed_dependency
                                ? EScheduleError::HARD_DEPENDENT_EXISTS
                                : EScheduleError::MISSING_REQUIRED_SYSTEM,
                            candidate.type,
                            required
                        });
                    }
                    addEdge(static_cast<std::size_t>(iterator - candidates.begin()), index);
                }

                for (const OwnedOrder& order : candidate.descriptor->orders)
                {
                    bool found{};
                    for (std::size_t target{}; target < candidates.size(); ++target)
                    {
                        const auto& target_sets = candidates[target].descriptor->sets;
                        const bool member = std::any_of(
                            target_sets.begin(),
                            target_sets.end(),
                            [&](const OwnedSet& set)
                            {
                                return set.hash == order.target.hash &&
                                    set.name == order.target.name;
                            }
                        );
                        if (!member || target == index)
                            continue;
                        found = true;

                        const bool contradiction =
                            (order.relation == ESystemOrder::BEFORE &&
                             phaseRank(candidate.phase) > phaseRank(candidates[target].phase)) ||
                            (order.relation == ESystemOrder::AFTER &&
                             phaseRank(candidate.phase) < phaseRank(candidates[target].phase));
                        if (contradiction)
                            return lux::cxx::unexpected(ScheduleFailure{EScheduleError::PHASE_ORDER_CONTRADICTION, candidate.type, candidates[target].type});

                        if (order.relation == ESystemOrder::BEFORE)
                            addEdge(index, target);
                        else
                            addEdge(target, index);
                    }
                    if (!found && order.required)
                        return lux::cxx::unexpected(ScheduleFailure{EScheduleError::MISSING_REQUIRED_SET, candidate.type});
                }
            }

            std::vector<std::size_t> ready;
            for (std::size_t index{}; index < candidates.size(); ++index)
                if (indegree[index] == 0)
                    ready.push_back(index);

            std::vector<std::size_t> order;
            order.reserve(candidates.size());
            while (!ready.empty())
            {
                const auto iterator = std::min_element(
                    ready.begin(), ready.end(),
                    [&](std::size_t left, std::size_t right)
                    {
                        return candidates[left].sequence < candidates[right].sequence;
                    }
                );
                const std::size_t current = *iterator;
                ready.erase(iterator);
                order.push_back(current);
                for (const std::size_t target : edges[current])
                    if (--indegree[target] == 0)
                        ready.push_back(target);
            }
            if (order.size() != candidates.size())
                return lux::cxx::unexpected(ScheduleFailure{EScheduleError::DEPENDENCY_CYCLE});

            const std::size_t required_size = owner.slots.size() +
                (impl_->additions.size() - impl_->free_slots_used);
            std::vector<Slot*> descriptors(required_size, nullptr);
            for (const Candidate& candidate : candidates)
                descriptors[candidate.slot] = candidate.descriptor;

            std::vector<std::uint32_t> next_compiled;
            next_compiled.reserve(candidates.size());
            for (const std::size_t candidate_index : order)
                next_compiled.push_back(candidates[candidate_index].slot);

            std::vector<std::vector<std::uint32_t>> next_outgoing(required_size);
            for (std::size_t from{}; from < edges.size(); ++from)
            {
                auto& outgoing = next_outgoing[candidates[from].slot];
                outgoing.reserve(edges[from].size());
                for (const std::size_t to : edges[from])
                    outgoing.push_back(candidates[to].slot);
            }

            std::vector<std::vector<std::uint32_t>> next_batches;
            next_batches.reserve(next_compiled.size());
            for (const std::uint32_t index : next_compiled)
            {
                bool added{};
                for (auto& batch : next_batches)
                {
                    bool compatible = true;
                    for (const std::uint32_t member : batch)
                    {
                        if (!accessCompatible(*descriptors[index], *descriptors[member]))
                        {
                            compatible = false;
                            break;
                        }
                    }
                    if (compatible)
                    {
                        batch.push_back(index);
                        added = true;
                        break;
                    }
                }
                if (!added)
                    next_batches.push_back({index});
            }

            std::vector<std::uint8_t> is_addition(required_size, std::uint8_t{});
            for (const auto& addition : impl_->additions)
                is_addition[addition.slot] = 1;
            std::vector<std::uint32_t> attach_order;
            attach_order.reserve(impl_->additions.size());
            for (const std::uint32_t index : next_compiled)
                if (is_addition[index] != 0)
                    attach_order.push_back(index);

            owner.slots.reserve(required_size);
            owner.free_slots.reserve(owner.free_slots.size() + removed_slots.size());

            publication_started = true;
            for (const std::uint32_t index : removed_slots)
            {
                Slot& slot = *owner.slots[index];
                SystemDetach detach(*owner.world);
                owner.clearObservers(slot);
                slot.system->onDetach(detach);
                slot.commands->invalidate();
                slot.system.reset();
                ++slot.generation;
                if (slot.generation == 0)
                    ++slot.generation;
                owner.free_slots.push_back(index);
            }

            for (std::size_t index{}; index < impl_->additions.size(); ++index)
            {
                const auto slot_index = impl_->additions[index].slot;
                if (slot_index < owner.slots.size())
                {
                    owner.slots[slot_index] = std::move(prepared_additions[index]);
                    const auto free_iterator = std::find(
                        owner.free_slots.begin(),
                        owner.free_slots.end(),
                        slot_index
                    );
                    if (free_iterator != owner.free_slots.end())
                        owner.free_slots.erase(free_iterator);
                }
                else
                {
                    detail::require(slot_index == owner.slots.size());
                    owner.slots.push_back(std::move(prepared_additions[index]));
                }
            }
            owner.next_sequence += impl_->additions.size();

            for (const std::uint32_t index : attach_order)
            {
                Slot& slot = *owner.slots[index];
                SystemAttach attach(
                    *owner.world,
                    slot.commands->writer(),
                    slot.observers
                );
                slot.system->onAttach(attach);
            }
            owner.applyCommands(attach_order);

            owner.compiled = std::move(next_compiled);
            owner.outgoing = std::move(next_outgoing);
            owner.batches = std::move(next_batches);
        }
        catch (...)
        {
            if (publication_started)
                detail::contractFailure();
            return lux::cxx::unexpected(
                ScheduleFailure{EScheduleError::ALLOCATION_FAILURE}
            );
        }

        commit_complete = true;
        schedule_->impl_->editing = false;
        schedule_ = nullptr;
        impl_.reset();
        return {};
    }

    void ScheduleEdit::release() noexcept
    {
        if (schedule_ != nullptr)
        {
            schedule_->impl_->requireOwnerThread();
            schedule_->impl_->editing = false;
        }
        schedule_ = nullptr;
        impl_.reset();
    }
} // namespace lux::ecs
