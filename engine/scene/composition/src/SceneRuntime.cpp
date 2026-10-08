#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/process/CompletionWork.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/scene/SceneError.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/scene/detail/SceneDriver.hpp>
#include <lux/engine/scene/detail/SceneInstanceImpl.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>

namespace lux::scene
{
    namespace detail
    {
        struct InstanceLifetime final
        {
            struct Step final
            {
                SceneStepTicket ticket;
                SceneStepStatus status;
            };

            // Only code pins and bounded results survive the heavyweight instance.
            std::shared_ptr<const void> code_lifetime;
            std::array<Step, 32> steps;
            std::uint64_t next_step{1};
            SceneInstanceId id;
            process::CompletionWork::Request wake;
            std::atomic_bool requested{}, completed{};

            void request() noexcept
            {
                if (!requested.exchange(true, std::memory_order_acq_rel))
                {
                    wake.request();
                }
            }
        };
    } // namespace detail

    InstanceRetirement::InstanceRetirement(std::shared_ptr<detail::InstanceLifetime> value) noexcept
        : lifetime_(std::move(value))
    {
    }

    bool InstanceRetirement::complete() const noexcept
    {
        return !lifetime_ || lifetime_->completed.load(std::memory_order_acquire);
    }

    SceneInstanceId InstanceRetirement::id() const noexcept
    {
        return lifetime_ ? lifetime_->id : SceneInstanceId{};
    }

    SceneInstanceLease::SceneInstanceLease(std::shared_ptr<detail::InstanceLifetime> value) noexcept
        : lifetime_(std::move(value))
    {
    }

    SceneInstanceLease::~SceneInstanceLease() noexcept
    {
        if (lifetime_)
        {
            lifetime_->request();
        }
    }

    SceneInstanceLease::SceneInstanceLease(SceneInstanceLease&&) noexcept = default;

    SceneInstanceLease& SceneInstanceLease::operator=(SceneInstanceLease&& other) noexcept
    {
        if (this != &other)
        {
            if (lifetime_)
            {
                lifetime_->request();
            }
            lifetime_ = std::move(other.lifetime_);
        }
        return *this;
    }

    SceneInstanceId SceneInstanceLease::id() const noexcept
    {
        return lifetime_ ? lifetime_->id : SceneInstanceId{};
    }

    SceneInstanceLease::operator bool() const noexcept
    {
        return bool(lifetime_);
    }

    InstanceRetirement SceneInstanceLease::retire() noexcept
    {
        if (lifetime_)
        {
            lifetime_->request();
        }
        return InstanceRetirement(std::move(lifetime_));
    }

    namespace
    {
        using SteadyClock = std::chrono::steady_clock;

        struct BusyScope final
        {
            bool& active;
            bool previous;

            explicit BusyScope(bool& value) noexcept : active(value), previous(value)
            {
                active = true;
            }

            ~BusyScope() noexcept
            {
                active = previous;
            }
        };

        template <class Error> auto rejected(Error error, SceneInstanceId id = {}) noexcept
        {
            return lux::cxx::unexpected(SceneRuntimeFailure{id, std::move(error)});
        }
    } // namespace

    struct SceneRuntime::Impl final
    {
        struct ActiveRecord final
        {
            VSimulationClock clock;
            SceneDriver driver;
            std::unique_ptr<SceneInstance> scene;
            std::optional<EClockError> clock_error;
            std::shared_ptr<detail::InstanceLifetime> lifetime;
            bool enabled{true};

            detail::InstanceLifetime::Step* pendingStep() noexcept
            {
                detail::InstanceLifetime::Step* first{};
                for (auto& step : lifetime->steps)
                {
                    const bool pending = step.ticket.serial && (step.status.state == ESceneStepState::QUEUED ||
                                                                step.status.state == ESceneStepState::EXECUTING);
                    if (pending && (!first || step.ticket.serial < first->ticket.serial))
                    {
                        first = &step;
                    }
                }
                return first;
            }

            ActiveRecord(
                VSimulationClock value,
                task::TaskExecutor& executor,
                std::unique_ptr<SceneInstance::Impl> instance,
                std::shared_ptr<detail::InstanceLifetime> retirement
            )
                : clock(std::move(value)), driver(executor), scene(new SceneInstance(std::move(instance))),
                  lifetime(std::move(retirement))
            {
                scene->registry().ctx().emplace<std::reference_wrapper<const SceneDriveSnapshot>>(
                    std::cref(scene->progress())
                );
            }
        };

        struct RetiredRecord final
        {
            explicit RetiredRecord(ActiveRecord& record) noexcept
                : lifetime(std::move(record.lifetime)), scene(std::move(record.scene)), clock_error(record.clock_error)
            {
            }

            RetiredRecord(const RetiredRecord&) = delete;
            RetiredRecord& operator=(const RetiredRecord&) = delete;
            RetiredRecord(RetiredRecord&&) noexcept = default;
            RetiredRecord& operator=(RetiredRecord&&) noexcept = default;

            // Scene/native teardown finishes while its code and result endpoint live.
            std::shared_ptr<detail::InstanceLifetime> lifetime;
            std::unique_ptr<SceneInstance> scene;
            std::optional<EClockError> clock_error;
            bool maintenance_complete{};
        };

        struct TimerReceiver final
        {
            using receiver_concept = stdexec::receiver_t;
            stdexec::inplace_stop_token stop;
            process::CompletionWork::Request wake;
            std::atomic_bool* completed{};
            std::optional<process::ETimerError>* error{};

            struct Environment final
            {
                stdexec::inplace_stop_token stop;

                [[nodiscard]] stdexec::inplace_stop_token query(stdexec::get_stop_token_t) const noexcept
                {
                    return stop;
                }
            };

            [[nodiscard]] Environment get_env() const noexcept
            {
                return {stop};
            }

            void finish(std::optional<process::ETimerError> failure) noexcept
            {
                // Once completion is published the owner may reclaim this receiver/operation.
                auto notification = wake;
                *error = failure;
                completed->store(true, std::memory_order_release);
                notification.request();
            }

            void set_value() && noexcept
            {
                finish({});
            }

            void set_stopped() && noexcept
            {
                finish({});
            }

            void set_error(process::ETimerError failure) && noexcept
            {
                finish(failure);
            }
        };

        using ActiveRecords = lux::cxx::SlotMap<std::unique_ptr<ActiveRecord>>;
        using TimerOperation = stdexec::connect_result_t<process::TimerSender, TimerReceiver>;

        Impl(process::ExecutionRuntime& execution, task::TaskExecutor executor, std::uint64_t domain)
            : execution_(execution), executor_(std::move(executor)), domain_(domain),
              wake_(execution, this, +[](void*) noexcept {})
        {
        }

        ~Impl() noexcept
        {
            if (std::this_thread::get_id() != owner_ || busy_)
            {
                std::terminate();
            }
            if (timer_active_)
            {
                timer_stop_->request_stop();
                const auto joined = execution_.waitUntil([this]() noexcept
                                                         { return timer_completed_.load(std::memory_order_acquire); });
                if (!joined)
                {
                    std::terminate();
                }
                reclaimTimer();
            }
            wake_.cancel();
            closing_ = true;
            for (auto& record : active_records_)
            {
                record->lifetime->request();
            }
            const auto retired = execution_.waitUntil(
                [this]() noexcept
                {
                    BusyScope draining(busy_);
                    beginRetirements();
                    maintainRetired();
                    collectRetired();
                    return active_records_.empty() && retiring_records_.empty();
                }
            );
            if (!retired)
            {
                std::terminate(); // Runtime teardown requires its owner outside any completion callback.
            }
        }

        [[nodiscard]] SceneRuntimeResult<void> access() const noexcept
        {
            if (std::this_thread::get_id() != owner_)
            {
                return rejected(ESceneRuntimeError::WRONG_THREAD);
            }
            if (closing_)
            {
                return rejected(ESceneRuntimeError::STOPPED);
            }
            if (busy_)
            {
                return rejected(ESceneRuntimeError::BUSY);
            }
            return {};
        }

        [[nodiscard]] SceneRuntimeResult<ActiveRecord*> find(SceneInstanceId id) const noexcept
        {
            const auto allowed = access();
            if (!allowed)
            {
                return lux::cxx::unexpected(allowed.error());
            }
            if (!id.valid())
            {
                return rejected(ESceneRuntimeError::INVALID_ID, id);
            }
            if (id.domain != domain_)
            {
                return rejected(ESceneRuntimeError::WRONG_DOMAIN, id);
            }
            const auto* found = active_records_.find({id.slot, id.generation});
            if (!found || !*found)
            {
                return rejected(ESceneRuntimeError::INVALID_ID, id);
            }
            return found->get();
        }

        [[nodiscard]] SceneRuntimeResult<SceneInstanceLease> build(const Builder& input) noexcept
        {
            const auto allowed = access();
            if (!allowed)
            {
                return lux::cxx::unexpected(allowed.error());
            }
            if (!input.components_ || !input.simulation_systems_)
            {
                return rejected(ESceneRuntimeError::INVALID_INPUT);
            }
            const auto initial_time = std::visit([](const auto& clock) { return clock.snapshot(); }, input.clock_);
            const bool has_elapsed = initial_time.elapsed != simulation::SimulationDuration{};
            const bool has_delta = initial_time.delta != simulation::SimulationDuration{};
            const bool has_step = initial_time.step_index != 0;
            if (has_elapsed || has_delta || has_step)
            {
                return rejected(ESceneRuntimeError::INVALID_INPUT);
            }
            BusyScope building(busy_);
            ActiveRecords::key_type key;
            try
            {
                key = active_records_.emplace(nullptr);
            }
            catch (const std::length_error&)
            {
                // SlotMap reports identity-space exhaustion at this cold allocation boundary.
                return rejected(ESceneRuntimeError::IDENTITY_EXHAUSTED);
            }
            const SceneInstanceId id{domain_, key.index, key.gen};
            const auto fail = [&](auto error) -> SceneRuntimeResult<SceneInstanceLease>
            {
                active_records_.erase(key);
                return rejected(std::move(error), id);
            };
            auto prepared = SceneInstance::prepare(
                {input.description_,
                 input.world_,
                 input.simulation_,
                 *input.components_,
                 *input.simulation_systems_,
                 input.scene_systems_,
                 input.providers_},
                id
            );
            if (!prepared)
            {
                return fail(prepared.error());
            }
            const auto sealed = (*prepared)->simulation->seal();
            if (!sealed)
            {
                SceneBuildFailure error{ESceneBuildError::SIMULATION_BUILD_FAILURE};
                error.simulation = sealed.error();
                return fail(error);
            }
            const auto reserved = executor_.reserve((*prepared)->simulation->taskCount());
            if (!reserved)
            {
                return fail(reserved.error());
            }
            const auto record_count = active_records_.size() + retiring_records_.size();
            failures_.reserve(record_count);
            failure_code_owners_.reserve(record_count);
            retiring_records_.reserve(record_count);
            auto lifetime = std::make_shared<detail::InstanceLifetime>();
            lifetime->id = id;
            lifetime->wake = wake_.requester();
            auto code = std::make_shared<std::vector<std::shared_ptr<const void>>>((*prepared)->code_owners);
            for (const auto& component : input.components_->all())
            {
                if (component.code_lifetime)
                {
                    code->push_back(component.code_lifetime);
                }
            }
            for (std::size_t i{}; i < input.simulation_->systemCount(); ++i)
            {
                const auto* registration = input.simulation_systems_->find(input.simulation_->systemAt(i).type());
                if (registration && registration->code_lifetime)
                {
                    code->push_back(registration->code_lifetime);
                }
            }
            lifetime->code_lifetime = std::move(code);
            *active_records_.find(key) =
                std::make_unique<ActiveRecord>(input.clock_, executor_, std::move(*prepared), lifetime);
            execution_.wake();
            return SceneInstanceLease(std::move(lifetime));
        }

        [[nodiscard]] SceneRuntimeResult<void> setEnabled(SceneInstanceId id, bool enabled) noexcept
        {
            const auto found = find(id);
            if (!found)
            {
                return lux::cxx::unexpected(found.error());
            }
            auto& record = **found;
            if (record.lifetime->requested.load(std::memory_order_acquire))
            {
                return rejected(ESceneRuntimeError::STOPPED, id);
            }
            if (enabled && record.pendingStep())
            {
                return rejected(ESceneRuntimeError::BUSY, id);
            }
            if (enabled)
            {
                if (record.clock_error)
                {
                    return rejected(*record.clock_error, id);
                }
                if (!record.scene->progress().result)
                {
                    return rejected(record.scene->progress().result.error(), id);
                }
                if (record.scene->stopToken().stop_requested())
                {
                    return rejected(ESceneRuntimeError::STOPPED, id);
                }
            }
            if (record.enabled != enabled)
            {
                record.enabled = enabled;
                if (enabled)
                {
                    std::visit([](auto& clock) noexcept { clock.rebase(SteadyClock::now()); }, record.clock);
                }
                execution_.wake();
            }
            return {};
        }

        [[nodiscard]] SceneRuntimeResult<InstanceRetirement> retireInstance(SceneInstanceId id) noexcept
        {
            // Leaf completion/intent admission is legal under the outer driving guard.
            // Do not clear that guard or invoke stop callbacks here.
            if (std::this_thread::get_id() != owner_)
            {
                return rejected(ESceneRuntimeError::WRONG_THREAD, id);
            }
            if (id.domain != domain_)
            {
                return rejected(ESceneRuntimeError::WRONG_DOMAIN, id);
            }
            const auto* found = active_records_.find({id.slot, id.generation});
            if (!found || !*found)
            {
                return rejected(ESceneRuntimeError::INVALID_ID, id);
            }
            auto lifetime = (*found)->lifetime;
            lifetime->request();
            return InstanceRetirement(std::move(lifetime));
        }

        [[nodiscard]] SceneRuntimeResult<SceneStepTicket> requestStep(SceneInstanceId id) noexcept
        {
            const auto found = find(id);
            if (!found)
            {
                return lux::cxx::unexpected(found.error());
            }
            auto& record = **found;
            const bool stopped = record.lifetime->requested.load(std::memory_order_acquire) ||
                                 record.scene->stopToken().stop_requested();
            if (stopped)
            {
                return rejected(ESceneRuntimeError::STOPPED, id);
            }
            if (record.enabled)
            {
                return rejected(ESceneRuntimeError::BUSY, id);
            }
            if (!record.scene->progress().result)
            {
                return rejected(record.scene->progress().result.error(), id);
            }
            if (record.clock_error)
            {
                return rejected(*record.clock_error, id);
            }
            auto empty =
                std::ranges::find_if(record.lifetime->steps, [](const auto& step) { return !step.ticket.serial; });
            if (empty == record.lifetime->steps.end())
            {
                return rejected(ESceneRuntimeError::CAPACITY, id);
            }
            auto target = record.scene->progress().simulation_completed;
            for (const auto& step : record.lifetime->steps)
            {
                target = std::max(target, step.ticket.simulation_completed);
            }
            if (target == UINT64_MAX || record.lifetime->next_step == UINT64_MAX)
            {
                return rejected(ESceneRuntimeError::IDENTITY_EXHAUSTED, id);
            }
            empty->ticket = {id, record.lifetime->next_step++, target + 1};
            empty->status = {};
            execution_.wake();
            return empty->ticket;
        }

        [[nodiscard]] SceneRuntimeResult<detail::InstanceLifetime::Step*> findStep(
            SceneStepTicket ticket,
            const std::shared_ptr<detail::InstanceLifetime>& retained
        ) const noexcept
        {
            const auto allowed = access();
            if (!allowed)
            {
                return lux::cxx::unexpected(allowed.error());
            }
            if (ticket.scene.domain != domain_)
            {
                return rejected(ESceneRuntimeError::WRONG_DOMAIN, ticket.scene);
            }
            auto* lifetime = retained.get();
            if (lifetime)
            {
                if (lifetime->id != ticket.scene)
                {
                    return rejected(ESceneRuntimeError::INVALID_ID, ticket.scene);
                }
            }
            else
            {
                const auto found = find(ticket.scene);
                if (!found)
                {
                    return lux::cxx::unexpected(found.error());
                }
                lifetime = (*found)->lifetime.get();
            }
            for (auto& step : lifetime->steps)
            {
                if (ticket.serial && step.ticket == ticket)
                {
                    return &step;
                }
            }
            return rejected(ESceneRuntimeError::INVALID_ID, ticket.scene);
        }

        [[nodiscard]] TimerOperation* timerOperation() noexcept
        {
            return std::launder(reinterpret_cast<TimerOperation*>(timer_storage_));
        }

        void reclaimTimer() noexcept
        {
            std::destroy_at(timerOperation());
            timer_stop_.reset();
            timer_active_ = false;
            armed_deadline_.reset();
        }

        [[nodiscard]] SceneRuntimeResult<void> armTimer(std::optional<SteadyClock::time_point> deadline) noexcept
        {
            if (timer_active_ && timer_completed_.load(std::memory_order_acquire))
            {
                reclaimTimer();
            }
            if (timer_active_)
            {
                if (deadline != armed_deadline_)
                {
                    timer_stop_->request_stop();
                }
                return {}; // Cancellation completion wakes the owner; never reuse an in-flight operation.
            }
            if (timer_error_)
            {
                return rejected(*timer_error_);
            }
            if (!deadline)
            {
                return {};
            }
            timer_stop_.emplace();
            timer_completed_.store(false, std::memory_order_relaxed);
            timer_active_ = true;
            armed_deadline_ = deadline;
            const auto delay = *deadline - SteadyClock::now();
            ::new (static_cast<void*>(timer_storage_)) TimerOperation(stdexec::connect(
                execution_.timer().after(delay),
                TimerReceiver{timer_stop_->get_token(), wake_.requester(), &timer_completed_, &timer_error_}
            ));
            stdexec::start(*timerOperation());
            if (timer_completed_.load(std::memory_order_acquire) && timer_error_)
            {
                return rejected(*timer_error_);
            }
            return {};
        }

        void beginRetirements() noexcept
        {
            // A request made by a later maintenance/publication callback belongs
            // to the next safe point. No active iterator crosses stop callbacks.
            for (std::size_t index = active_records_.size(); index > 0; --index)
            {
                auto& value = active_records_.values()[index - 1];
                if (!value->lifetime->requested.load(std::memory_order_acquire))
                {
                    continue;
                }
                auto active = std::move(value);
                const auto id = active->scene->id();
                active_records_.erase({id.slot, id.generation});
                auto& retired = retiring_records_.emplace_back(*active);
                // The old clock/playback owner ends here; only physical backing
                // and the original bounded result endpoint enter retirement.
                active.reset();
                retired.scene->requestStop();
                for (auto& step : retired.lifetime->steps)
                {
                    const bool pending = step.ticket.serial && (step.status.state == ESceneStepState::QUEUED ||
                                                                step.status.state == ESceneStepState::EXECUTING);
                    if (pending)
                    {
                        step.status = {ESceneStepState::CANCELLED, rejected(ESceneRuntimeError::STOPPED, id)};
                    }
                }
            }
        }

        void maintainRetired() noexcept
        {
            SceneDriver driver(executor_);
            for (auto& record : retiring_records_)
            {
                record.maintenance_complete = driver.maintain(*record.scene) == ESceneProgress::COMPLETE;
            }
        }

        void collectRetired() noexcept
        {
            // All traversals/callbacks have returned. Extract the owner before
            // rearranging the container so plugin teardown never runs in erase.
            for (std::size_t index = retiring_records_.size(); index > 0; --index)
            {
                auto& value = retiring_records_[index - 1];
                if (!value.maintenance_complete)
                {
                    continue;
                }
                auto record = std::move(value);
                if (index != retiring_records_.size())
                {
                    value = std::move(retiring_records_.back());
                }
                retiring_records_.pop_back();
                record.scene.reset();
                record.lifetime->code_lifetime.reset();
                record.lifetime->completed.store(true, std::memory_order_release);
                execution_.wake();
            }
        }

        [[nodiscard]] DriveResult driveFrame() noexcept
        {
            const auto allowed = access();
            if (!allowed)
            {
                return lux::cxx::unexpected(allowed.error());
            }
            BusyScope ticking(busy_);
            const auto now = SteadyClock::now();
            failures_.clear();
            failure_code_owners_.clear();
            beginRetirements();
            for (auto& record : active_records_)
            {
                static_cast<void>(record->driver.maintain(*record->scene));
            }
            maintainRetired();
            for (auto& record : active_records_)
            {
                auto* step = record->pendingStep();
                const bool step_ready = step && step->status.state == ESceneStepState::QUEUED;
                const bool may_tick = !record->lifetime->requested.load(std::memory_order_acquire) &&
                                      (record->enabled || step_ready) && !record->clock_error &&
                                      record->scene->canTick();
                if (!may_tick)
                {
                    continue;
                }
                std::visit(
                    [&](auto& clock) noexcept
                    {
                        if (step_ready)
                        {
                            clock.rebase(now);
                        }
                        const auto sampled = clock.sample(now);
                        if (!sampled)
                        {
                            record->clock_error = sampled.error();
                            return;
                        }
                        if (!*sampled)
                        {
                            return;
                        }
                        const auto before = record->scene->simulation().time().step_index;
                        const auto executed = record->driver.tick(*record->scene, **sampled);
                        const auto actual = record->scene->simulation().time();
                        if (actual.step_index != before)
                        {
                            clock.adopt(now, actual);
                            if (step)
                            {
                                step->status.state = ESceneStepState::EXECUTING;
                            }
                        }
                        (void)executed; // The original execution failure remains in SceneDriveSnapshot.
                    },
                    record->clock
                );
            }
            std::optional<SteadyClock::time_point> deadline;
            for (auto& record : active_records_)
            {
                static_cast<void>(record->driver.publish(*record->scene));
                for (auto& step : record->lifetime->steps)
                {
                    const bool pending = step.ticket.serial && (step.status.state == ESceneStepState::QUEUED ||
                                                                step.status.state == ESceneStepState::EXECUTING);
                    if (!pending)
                    {
                        continue;
                    }
                    const auto& progress = record->scene->progress();
                    if (!progress.result)
                    {
                        step.status = {
                            ESceneStepState::FAILED,
                            rejected(progress.result.error(), record->scene->id()),
                            record->lifetime->code_lifetime
                        };
                    }
                    else if (record->clock_error)
                    {
                        step.status = {ESceneStepState::FAILED, rejected(*record->clock_error, record->scene->id())};
                    }
                    else if (step.status.state == ESceneStepState::EXECUTING &&
                             progress.publication_completed >= step.ticket.simulation_completed)
                    {
                        step.status.state = ESceneStepState::COMPLETED;
                    }
                }
                if (!record->scene->progress().result)
                {
                    failure_code_owners_.push_back(record->lifetime->code_lifetime);
                    failures_.push_back({record->scene->id(), record->scene->progress().result.error()});
                }
                else if (record->clock_error)
                {
                    failures_.push_back({record->scene->id(), *record->clock_error});
                }
                const bool may_tick = !record->lifetime->requested.load(std::memory_order_acquire) &&
                                      (record->enabled || record->pendingStep()) && !record->clock_error &&
                                      record->scene->canTick();
                if (may_tick)
                {
                    const auto next =
                        std::visit([&](const auto& clock) { return clock.deadline().value_or(now); }, record->clock);
                    if (!deadline || next < *deadline)
                    {
                        deadline = next;
                    }
                }
            }
            // Retiring systems still receive their original completion/publish
            // stages, but no clock, simulation tick or ordinary ID lookup.
            SceneDriver retirement_driver(executor_);
            for (auto& record : retiring_records_)
            {
                static_cast<void>(retirement_driver.publish(*record.scene));
                if (!record.scene->progress().result)
                {
                    failure_code_owners_.push_back(record.lifetime->code_lifetime);
                    failures_.push_back({record.scene->id(), record.scene->progress().result.error()});
                }
                else if (record.clock_error)
                {
                    failures_.push_back({record.scene->id(), *record.clock_error});
                }
            }
            collectRetired();
            const auto armed = armTimer(deadline);
            if (!armed)
            {
                return lux::cxx::unexpected(armed.error());
            }
            return std::span<const SceneRuntimeFailure>{failures_};
        }

        SceneRuntimeResult<std::reference_wrapper<simulation::ecs::Registry>> borrowInstance(SceneInstanceId id
        ) noexcept
        {
            const auto record = find(id);
            if (!record)
            {
                return lux::cxx::unexpected(record.error());
            }
            if ((**record).lifetime->requested.load(std::memory_order_acquire))
            {
                return rejected(ESceneRuntimeError::STOPPED, id);
            }
            if (!(**record).scene->atSafePoint())
            {
                return rejected(ESceneRuntimeError::BUSY, id);
            }
            return std::ref((**record).scene->registry());
        }

        SceneRuntimeResult<std::reference_wrapper<const simulation::ecs::Registry>> borrowInstance(SceneInstanceId id
        ) const noexcept
        {
            const auto record = find(id);
            if (!record)
            {
                return lux::cxx::unexpected(record.error());
            }
            if ((**record).lifetime->requested.load(std::memory_order_acquire))
            {
                return rejected(ESceneRuntimeError::STOPPED, id);
            }
            return std::cref((**record).scene->registry());
        }

        SceneRuntimeResult<std::reference_wrapper<const VSimulationClock>> borrowClock(SceneInstanceId id
        ) const noexcept
        {
            const auto record = find(id);
            if (!record)
            {
                return lux::cxx::unexpected(record.error());
            }
            return std::cref((**record).clock);
        }

        process::ExecutionRuntime& execution_;
        task::TaskExecutor executor_;
        std::uint64_t domain_;
        std::thread::id owner_{std::this_thread::get_id()};
        ActiveRecords active_records_;
        std::vector<RetiredRecord> retiring_records_;
        // The existing borrowed drive-failure span ends at the next frame, after payload cleanup.
        std::vector<std::shared_ptr<const void>> failure_code_owners_;
        std::vector<SceneRuntimeFailure> failures_;
        bool busy_{}, closing_{};
        process::CompletionWork wake_;
        std::optional<stdexec::inplace_stop_source> timer_stop_;
        std::atomic_bool timer_completed_{false};
        std::optional<process::ETimerError> timer_error_;
        std::optional<SteadyClock::time_point> armed_deadline_;
        alignas(TimerOperation) std::byte timer_storage_[sizeof(TimerOperation)];
        bool timer_active_{};
    };

    SceneRuntime::SceneRuntime(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    SceneRuntime::~SceneRuntime() noexcept = default;

    SceneRuntime::CreateResult SceneRuntime::create(
        process::ExecutionRuntime& execution,
        task::TaskExecutorConfig config
    ) noexcept
    {
        if (auto registered = registerSceneErrors(); !registered)
        {
            return rejected(SceneBuildFailure{
                .code = ESceneBuildError::SCENE_SYSTEM_BUILD_FAILURE,
                .scene_system = {.code = ESceneSystemBuildError::CONSTRUCTION_FAILURE, .cause = registered.error()}
            });
        }
        if (!execution.timer())
        {
            return rejected(process::ETimerError::STOPPING);
        }
        const auto domain = SceneInstance::allocateDomain();
        if (!domain)
        {
            return rejected(ESceneRuntimeError::IDENTITY_EXHAUSTED);
        }
        auto executor = task::TaskExecutor::create(config);
        if (!executor)
        {
            return rejected(executor.error());
        }
        return std::unique_ptr<SceneRuntime>(
            new SceneRuntime(std::make_unique<Impl>(execution, std::move(*executor), domain))
        );
    }

    SceneRuntime::Builder& SceneRuntime::Builder::setDescription(std::shared_ptr<const SceneDescription> value) noexcept
    {
        description_ = std::move(value);
        return *this;
    }

    SceneRuntime::Builder& SceneRuntime::Builder::setWorld(std::shared_ptr<const world::WorldDescription> value
    ) noexcept
    {
        world_ = std::move(value);
        return *this;
    }

    SceneRuntime::Builder& SceneRuntime::Builder::setSimulation(
        std::shared_ptr<const simulation::SimulationDescription> value
    ) noexcept
    {
        simulation_ = std::move(value);
        return *this;
    }

    SceneRuntime::Builder& SceneRuntime::Builder::setRegistrations(
        const simulation::ecs::ComponentSchemaSet& components,
        const simulation::SimulationSystemRegistry& simulation_systems,
        std::span<const SceneSystemRegistration> scene_systems
    ) noexcept
    {
        components_ = &components;
        simulation_systems_ = &simulation_systems;
        scene_systems_ = scene_systems;
        return *this;
    }

    SceneRuntime::Builder& SceneRuntime::Builder::setProviders(std::span<const SceneCapabilityProvider> providers
    ) noexcept
    {
        providers_ = providers;
        return *this;
    }

    SceneRuntimeResult<SceneInstanceLease> SceneRuntime::Builder::build() noexcept
    {
        return runtime_.build(*this);
    }

    SceneRuntimeResult<SceneInstanceLease> SceneRuntime::build(const Builder& input) noexcept
    {
        return impl_->build(input);
    }

    SceneRuntimeResult<std::reference_wrapper<simulation::ecs::Registry>> SceneRuntime::borrowInstance(
        SceneInstanceId id
    ) noexcept
    {
        return impl_->borrowInstance(id);
    }

    SceneRuntimeResult<std::reference_wrapper<const simulation::ecs::Registry>> SceneRuntime::borrowInstance(
        SceneInstanceId id
    ) const noexcept
    {
        return std::as_const(*impl_).borrowInstance(id);
    }

    SceneRuntimeResult<std::reference_wrapper<const VSimulationClock>> SceneRuntime::borrowClock(SceneInstanceId id
    ) const noexcept
    {
        return impl_->borrowClock(id);
    }

    SceneRuntimeResult<void> SceneRuntime::pauseSimulation(SceneInstanceId id) noexcept
    {
        return impl_->setEnabled(id, false);
    }

    SceneRuntimeResult<void> SceneRuntime::resumeSimulation(SceneInstanceId id) noexcept
    {
        return impl_->setEnabled(id, true);
    }

    SceneRuntimeResult<InstanceRetirement> SceneRuntime::retireInstance(SceneInstanceId id) noexcept
    {
        return impl_->retireInstance(id);
    }

    SceneRuntimeResult<SceneStepTicket> SceneRuntime::requestStep(SceneInstanceId id) noexcept
    {
        return impl_->requestStep(id);
    }

    SceneRuntimeResult<SceneStepStatus> SceneRuntime::stepStatus(
        SceneStepTicket ticket,
        const InstanceRetirement& retirement
    ) const noexcept
    {
        const auto found = impl_->findStep(ticket, retirement.lifetime_);
        if (!found)
        {
            return lux::cxx::unexpected(found.error());
        }
        BusyScope reading(impl_->busy_);
        return (*found)->status;
    }

    SceneRuntimeResult<void> SceneRuntime::acknowledgeStep(
        SceneStepTicket ticket,
        const InstanceRetirement& retirement
    ) noexcept
    {
        const auto found = impl_->findStep(ticket, retirement.lifetime_);
        if (!found)
        {
            return lux::cxx::unexpected(found.error());
        }
        const auto state = (*found)->status.state;
        if (state == ESceneStepState::QUEUED || state == ESceneStepState::EXECUTING)
        {
            return rejected(ESceneRuntimeError::BUSY, ticket.scene);
        }
        BusyScope acknowledging(impl_->busy_);
        **found = {};
        return {};
    }

    std::size_t SceneRuntime::instanceCount() const noexcept
    {
        if (std::this_thread::get_id() != impl_->owner_)
        {
            std::terminate();
        }
        return impl_->active_records_.size();
    }

    SceneRuntime::DriveResult SceneRuntime::driveFrame() noexcept
    {
        return impl_->driveFrame();
    }
} // namespace lux::scene
