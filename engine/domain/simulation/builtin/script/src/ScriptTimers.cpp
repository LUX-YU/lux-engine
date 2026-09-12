#include <lux/engine/simulation/script/ScriptTimers.hpp>
#include <lux/engine/simulation/script/ScriptExecution.hpp>
#include <lux/engine/simulation/scripting/ScriptAbilityInvocation.hpp>
#include "DelayAbility.ability.generated.hpp"
#include <cmath>
#include <limits>
#include <type_traits>
#include <tuple>

namespace lux::simulation::script::detail
{
    namespace
    {
        // A double representation of INT64_MAX rounds up on MSVC. Use the exact
        // exclusive power-of-two limit and check the rounded value before casting.
        template<class Duration>
        [[nodiscard]] std::optional<Duration> checkedDuration(double seconds) noexcept
        {
            using Rep = typename Duration::rep;
            static_assert(std::is_integral_v<Rep> && std::is_signed_v<Rep>);
            const auto count = std::ceil(static_cast<long double>(seconds) * 1'000'000'000.0L);
            static_assert(std::numeric_limits<Rep>::digits < 64);
            constexpr auto exclusive_limit = static_cast<long double>(
                std::uint64_t{1U} << std::numeric_limits<Rep>::digits);
            if (count >= exclusive_limit) return std::nullopt;
            return Duration{static_cast<Rep>(count)};
        }
    }

    void ScriptTimers::prepare(const SimulationClock& clock, ScriptRuntimeLimits limits,
        ScriptRealDelayEndpoint real_delay, ScriptExecution& execution, std::size_t instance_capacity)
    {
        clock_ = &clock;
        execution_ = &execution;
        real_delay_ = real_delay;
        next_capacity_ = limits.next_step_wait_capacity;
        delay_capacity_ = limits.simulation_delay_capacity;
        // One Timer source per owned awaitable bounds simultaneous physical storage. Keep both
        // logical limits unchanged, and avoid imposing a new combined-capacity input restriction.
        const auto next_slots = (std::min)(next_capacity_, limits.awaitable_capacity);
        const auto delay_slots = (std::min)(delay_capacity_, limits.awaitable_capacity);
        const auto slots = next_slots > limits.awaitable_capacity - delay_slots ? limits.awaitable_capacity :
            next_slots + delay_slots;
        waits_.reserve(slots);
        external_.reserve(slots);
        heap_.reserve(delay_slots);
        instances_.resize(instance_capacity);
    }

    void ScriptTimers::beginInstance(ScriptInstanceId instance) noexcept
    {
        auto& index = instances_[instance.slot - 1U];
        if (index.first.valid())
            std::terminate();
        index = {instance, {}};
    }

    ScriptTimers::StartResult ScriptTimers::error(EScriptDelayStatus status) noexcept
    {
        return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{static_cast<std::int32_t>(status)});
    }

    PreparedLocalAsyncCatalog ScriptTimers::localCatalog(const ScriptApiCapabilityPublication& publication) noexcept
    {
        const auto binding = lux::script::bindScriptAbility<DelayAbility>(*this);
        const auto expected = publishScriptAbility(binding);
        const bool invalid = publication.context != this || publication.dispatch != expected.dispatch ||
            publication.contract != expected.contract || publication.schema_hash != expected.schema_hash ||
            publication.schema_version != expected.schema_version ||
            publication.methods.data() != expected.methods.data();
        if (invalid) return {};
        return {this, expected.dispatch, &resolveLocal};
    }

    PreparedLocalAsyncStart ScriptTimers::resolveLocal(
        void* context, lux::script::ScriptApiMethodIdView method
    ) noexcept
    {
        if (method == lux::script::ScriptApiMethodIdView{"lux.simulation.delay.next_step"})
            return {context, &startLocal<ETimerKind::NEXT_STEP>, 0U};
        if (method == lux::script::ScriptApiMethodIdView{"lux.simulation.delay.seconds"} ||
            method == lux::script::ScriptApiMethodIdView{"lux.simulation.delay.simulation_seconds"})
            return {context, &startLocal<ETimerKind::SIMULATION_DELAY>, 1U};
        return {};
    }

    ScriptTimers::StartResult ScriptTimers::planWait(ETimerKind kind, double duration, Schedule& result) const noexcept
    {
        if (kind == ETimerKind::NEXT_STEP)
        {
            const auto current = clock_->snapshot();
            if (current.step_index == std::numeric_limits<std::uint64_t>::max())
                return error(EScriptDelayStatus::DURATION_OVERFLOW);
            if (next_count_ >= next_capacity_) return error(EScriptDelayStatus::CAPACITY_EXCEEDED);
            result = {{}, current.step_index + 1U};
            return {};
        }
        if (!std::isfinite(duration) || duration < 0.0) return error(EScriptDelayStatus::INVALID_DURATION);
        const auto converted = checkedDuration<SimulationDuration>(duration);
        if (!converted) return error(EScriptDelayStatus::DURATION_OVERFLOW);
        const auto count = converted->count();
        const auto current = clock_->snapshot();
        const bool step_overflow = current.step_index == std::numeric_limits<std::uint64_t>::max();
        const bool deadline_overflow = count > 0 &&
            current.elapsed.count() > std::numeric_limits<SimulationDuration::rep>::max() - count;
        if (step_overflow || deadline_overflow) return error(EScriptDelayStatus::DURATION_OVERFLOW);
        if (heap_.size() >= delay_capacity_) return error(EScriptDelayStatus::CAPACITY_EXCEEDED);
        if (sequence_ == std::numeric_limits<std::uint64_t>::max()) return error(EScriptDelayStatus::DURATION_OVERFLOW);
        result = {current.elapsed + SimulationDuration{count}, current.step_index + 1U};
        return {};
    }

    template<ScriptTimers::ETimerKind Kind>
    ScriptStepResult ScriptTimers::startLocal(void* context, ScriptStepContext& step,
        std::span<const lux::script::ScriptAbilityInputSlot> arguments) noexcept
    {
        auto& self = *static_cast<ScriptTimers*>(context);
        double duration{};
        if constexpr (Kind == ETimerKind::NEXT_STEP)
        {
            if (!arguments.empty()) return ScriptStepResult::failed(-1);
        }
        else
        {
            if (arguments.size() != 1U || !lux::script::scriptAbilityInputMatches<double>(arguments.front()))
                return ScriptStepResult::failed(-1);
            std::memcpy(&duration, arguments.front().data, sizeof(duration));
        }
        const auto admitted = self.execution_->reserveLocalTimer(step);
        if (!admitted) return ScriptStepResult::failed(awaitableCreateStatus(admitted.error()));
        Schedule schedule;
        auto planned = self.stopping_ ? error(EScriptDelayStatus::STOPPING) : self.planWait(Kind, duration, schedule);
        if (planned)
            planned = self.registerWait(Kind, *admitted, {}, schedule.deadline, schedule.step);
        if (!planned)
        {
            self.execution_->discardLocalTimer(admitted->association());
            return ScriptStepResult::failed(planned.error().status);
        }
        return ScriptStepResult::suspended(admitted->association().awaitable);
    }

    ScriptTimers::StartResult ScriptTimers::nextStep(Completion completion) noexcept
    {
        if (stopping_ || !completion.active()) return error(EScriptDelayStatus::STOPPING);
        Schedule schedule;
        if (auto planned = planWait(ETimerKind::NEXT_STEP, 0.0, schedule); !planned) return planned;
        const auto admission = execution_->timerAssociation(completion);
        if (!admission) return error(EScriptDelayStatus::STOPPING);
        return registerWait(ETimerKind::NEXT_STEP, *admission, std::move(completion), schedule.deadline, schedule.step);
    }

    ScriptTimers::StartResult ScriptTimers::seconds(double duration, Completion completion) noexcept
    {
        return simulationSeconds(duration, std::move(completion));
    }

    ScriptTimers::StartResult ScriptTimers::simulationSeconds(double duration, Completion completion) noexcept
    {
        if (stopping_ || !completion.active()) return error(EScriptDelayStatus::STOPPING);
        Schedule schedule;
        if (auto planned = planWait(ETimerKind::SIMULATION_DELAY, duration, schedule); !planned) return planned;
        const auto admission = execution_->timerAssociation(completion);
        if (!admission) return error(EScriptDelayStatus::STOPPING);
        return registerWait(
            ETimerKind::SIMULATION_DELAY, *admission, std::move(completion), schedule.deadline, schedule.step
        );
    }

    ScriptTimers::StartResult ScriptTimers::realSeconds(double duration, Completion completion) noexcept
    {
        if (stopping_ || !completion.active() || !real_delay_)
            return error(EScriptDelayStatus::STOPPING);
        if (!std::isfinite(duration) || duration < 0.0)
            return error(EScriptDelayStatus::INVALID_DURATION);
        if (duration == 0.0)
            return nextStep(std::move(completion));
        const auto converted = checkedDuration<std::chrono::nanoseconds>(duration);
        if (!converted) return error(EScriptDelayStatus::DURATION_OVERFLOW);
        return real_delay_.invoke(*converted, std::move(completion));
    }

    ScriptTimers::StartResult ScriptTimers::registerWait(ETimerKind kind, const ScriptTimerAdmission& admission,
        Completion completion, SimulationDuration deadline, std::uint64_t step) noexcept
    {
        const auto association = admission.association();
        auto& owner = instances_[association.instance.slot - 1U];
        if (owner.id != association.instance)
            return error(EScriptDelayStatus::STOPPING);
        auto external = ExternalStorage::key_type::invalid();
        const auto route = static_cast<bool>(completion) ? ETimerRoute::EXTERNAL_CAPABILITY : ETimerRoute::OWNER_LOCAL;
        if (route == ETimerRoute::EXTERNAL_CAPABILITY)
        {
            const auto reserved = external_.tryEmplace(std::move(completion));
            if (!reserved) return error(EScriptDelayStatus::ALLOCATION_FAILURE);
            external = *reserved;
        }
        const auto inserted = waits_.tryEmplace(Wait{
            {}, association, external, route, kind, deadline, step,
            kind == ETimerKind::SIMULATION_DELAY ? sequence_++ : 0U, 0U, {}, {}, {}, owner.first
        });
        if (!inserted)
        {
            if (route == ETimerRoute::EXTERNAL_CAPABILITY) static_cast<void>(external_.erase(external));
            return error(EScriptDelayStatus::ALLOCATION_FAILURE);
        }
        const ScriptSourceId id{inserted->index + 1U, inserted->gen};
        auto& wait = waits_[*inserted];
        wait.id = id;
        if (owner.first.valid())
            waits_[key(owner.first)].instance_previous = id;
        owner.first = id;
        if (kind == ETimerKind::NEXT_STEP)
        {
            wait.previous = next_last_;
            if (next_last_.valid())
                waits_[key(next_last_)].next = id;
            else
                next_first_ = id;
            next_last_ = id;
            ++next_count_;
        }
        else
        {
            wait.heap_index = heap_.size();
            heap_.push_back(id); // Prepared capacity; admission checks the component's logical limit.
            siftUp(wait.heap_index);
        }
        execution_->attachTimer(admission, id);
        return {};
    }

    bool ScriptTimers::earlier(ScriptSourceId left, ScriptSourceId right) const noexcept
    {
        const auto& a = waits_[key(left)];
        const auto& b = waits_[key(right)];
        return a.deadline < b.deadline || (a.deadline == b.deadline && a.sequence < b.sequence);
    }

    void ScriptTimers::swapHeap(std::size_t left, std::size_t right) noexcept
    {
        std::swap(heap_[left], heap_[right]);
        waits_[key(heap_[left])].heap_index = left;
        waits_[key(heap_[right])].heap_index = right;
    }

    void ScriptTimers::siftUp(std::size_t index) noexcept
    {
        while (index != 0U && earlier(heap_[index], heap_[(index - 1U) / 2U]))
        {
            const auto parent = (index - 1U) / 2U;
            swapHeap(index, parent);
            index = parent;
        }
    }

    void ScriptTimers::siftDown(std::size_t index) noexcept
    {
        for (;;)
        {
            auto child = index * 2U + 1U;
            if (child >= heap_.size())
                return;
            if (child + 1U < heap_.size() && earlier(heap_[child + 1U], heap_[child]))
                ++child;
            if (!earlier(heap_[child], heap_[index]))
                return;
            swapHeap(index, child);
            index = child;
        }
    }

    void ScriptTimers::eraseHeap(std::size_t index) noexcept
    {
        const auto last = heap_.size() - 1U;
        if (index != last)
            swapHeap(index, last);
        heap_.pop_back();
        if (index >= heap_.size())
            return;
        if (index != 0U && earlier(heap_[index], heap_[(index - 1U) / 2U]))
            siftUp(index);
        else
            siftDown(index);
    }

    std::optional<ScriptSourceCancellation> ScriptTimers::cancel(ScriptSourceId id) noexcept
    {
        auto* wait = waits_.find(key(id));
        if (wait == nullptr)
            return std::nullopt;
        const auto association = wait->association;
        if (wait->kind == ETimerKind::NEXT_STEP)
        {
            if (wait->previous.valid())
                waits_[key(wait->previous)].next = wait->next;
            else
                next_first_ = wait->next;
            if (wait->next.valid())
                waits_[key(wait->next)].previous = wait->previous;
            else
                next_last_ = wait->previous;
            --next_count_;
        }
        else
            eraseHeap(wait->heap_index);
        if (wait->instance_previous.valid())
            waits_[key(wait->instance_previous)].instance_next = wait->instance_next;
        else
            instances_[association.instance.slot - 1U].first = wait->instance_next;
        if (wait->instance_next.valid())
            waits_[key(wait->instance_next)].instance_previous = wait->instance_previous;
        if (wait->route == ETimerRoute::EXTERNAL_CAPABILITY)
            static_cast<void>(external_.erase(wait->external));
        static_cast<void>(waits_.erase(key(id)));
        return ScriptSourceCancellation{association.instance, association.awaitable, {id, EScriptWaitSource::TIMER}};
    }

    std::optional<ScriptSourceCancellation> ScriptTimers::cancelNext(ScriptInstanceId instance) noexcept
    {
        if (!instance.valid() || instance.slot > instances_.size())
            return std::nullopt;
        const auto& owner = instances_[instance.slot - 1U];
        return owner.id == instance && owner.first.valid() ? cancel(owner.first) : std::nullopt;
    }

    bool ScriptTimers::completeDue(ScriptSourceId id, bool& backpressured) noexcept
    {
        // Completion can detach its source synchronously. Keep the lease, never a SlotMap record borrow.
        const auto [association, route, external] = [&]() noexcept {
            const auto& due = waits_[key(id)];
            return std::tuple{due.association, due.route, due.external};
        }();
        const auto completed = [&]() noexcept {
            if (route == ETimerRoute::OWNER_LOCAL) return execution_->completeLocalTimer(association, id);
            const auto completion = external_[external];
            return lux::script::detail::ScriptAbilityOwnerCompletionAccess::success(completion);
        }();
        backpressured = !completed && completed.error() == lux::script::EScriptAbilityCompletionError::BACKPRESSURE;
        if (backpressured)
            return true;
        if (const auto removed = cancel(id))
            execution_->detachSource(*removed);
        if (completed)
            return true;
        return completed.error() == lux::script::EScriptAbilityCompletionError::STALE ||
            completed.error() == lux::script::EScriptAbilityCompletionError::STOPPING ||
            completed.error() == lux::script::EScriptAbilityCompletionError::ALREADY_COMPLETED;
    }

    bool ScriptTimers::promoteNextStep() noexcept
    {
        const auto current = clock_->snapshot();
        while (next_first_.valid())
        {
            if (waits_[key(next_first_)].minimum_step > current.step_index)
                return true;
            bool backpressured{};
            if (!completeDue(next_first_, backpressured))
                return false;
            if (backpressured)
                return true;
        }
        return true;
    }

    bool ScriptTimers::promoteSimulationDelay() noexcept
    {
        const auto current = clock_->snapshot();
        while (!heap_.empty())
        {
            const auto& wait = waits_[key(heap_.front())];
            if (wait.deadline > current.elapsed || wait.minimum_step > current.step_index)
                return true;
            bool backpressured{};
            if (!completeDue(heap_.front(), backpressured))
                return false;
            if (backpressured)
                return true;
        }
        return true;
    }

    void ScriptTimers::shutdown() noexcept
    {
        if (!waits_.empty() || !external_.empty())
            std::terminate();
        heap_.clear();
        instances_.clear();
        next_first_ = {};
        next_last_ = {};
        next_count_ = 0U;
    }

    void ScriptTimers::writeStats(ScriptRuntimeStats& result) const noexcept
    {
        result.next_step_waits = next_count_;
        result.simulation_delay_waits = heap_.size();
    }
}
