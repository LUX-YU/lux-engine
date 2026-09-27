#include <lux/engine/scene/detail/SceneDriver.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/scene/detail/SceneInstanceImpl.hpp>
#include <lux/engine/process/CompletionWork.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/cxx/container/SlotMap.hpp>

#include <atomic>
#include <new>
#include <stdexcept>
#include <thread>

namespace lux::scene
{
    namespace
    {
        using SteadyClock = std::chrono::steady_clock;

        struct BusyScope final
        {
            bool& active;
            explicit BusyScope(bool& value) noexcept : active(value)
            {
                active = true;
            }
            ~BusyScope() noexcept
            {
                active = false;
            }
        };

        template <class Error> auto rejected(Error error, SceneInstanceId id = {}) noexcept
        {
            return lux::cxx::unexpected(SceneRuntimeFailure{id, std::move(error)});
        }
    }

    struct SceneRuntime::Impl final
    {
        struct Record final
        {
            VSimulationClock clock;
            SceneDriver driver;
            SceneInstance scene;
            std::optional<EClockError> clock_error;
            bool enabled{true};

            Record(VSimulationClock value, task::TaskExecutor& executor, std::unique_ptr<SceneInstance::Impl> instance)
                : clock(std::move(value)), driver(executor), scene(std::move(instance))
            {
                scene.registry().ctx().emplace<std::reference_wrapper<const SceneDriveSnapshot>>(
                    std::cref(scene.progress())
                );
            }
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

        using Records = lux::cxx::SlotMap<std::unique_ptr<Record>>;
        using TimerOperation = stdexec::connect_result_t<process::TimerSender, TimerReceiver>;

        Impl(process::ExecutionRuntime& execution, task::TaskExecutor executor, std::uint64_t domain)
            : execution_(execution), executor_(std::move(executor)), domain_(domain),
              wake_(execution, this, +[](void*) noexcept {})
        {}

        ~Impl() noexcept
        {
            if (std::this_thread::get_id() != owner_ || busy_)
                std::terminate();
            if (timer_active_)
            {
                timer_stop_->request_stop();
                const auto joined = execution_.waitUntil([this]() noexcept {
                    return timer_completed_.load(std::memory_order_acquire);
                });
                if (!joined)
                    std::terminate();
                reclaimTimer();
            }
            wake_.cancel();
            // Revocation precedes destruction; system destructors cannot observe a live ID.
            while (!records_.empty())
            {
                auto record = std::move(records_.values().back());
                const auto id = record->scene.id();
                records_.erase({id.slot, id.generation});
            }
        }

        [[nodiscard]] SceneRuntimeResult<void> access() const noexcept
        {
            if (std::this_thread::get_id() != owner_)
                return rejected(ESceneRuntimeError::WRONG_THREAD);
            if (busy_)
                return rejected(ESceneRuntimeError::BUSY);
            return {};
        }

        [[nodiscard]] SceneRuntimeResult<Record*> find(SceneInstanceId id) const noexcept
        {
            const auto allowed = access();
            if (!allowed)
                return lux::cxx::unexpected(allowed.error());
            if (!id.valid())
                return rejected(ESceneRuntimeError::INVALID_ID, id);
            if (id.domain != domain_)
                return rejected(ESceneRuntimeError::WRONG_DOMAIN, id);
            const auto* found = records_.find({id.slot, id.generation});
            if (!found || !*found)
                return rejected(ESceneRuntimeError::INVALID_ID, id);
            return found->get();
        }

        [[nodiscard]] SceneRuntimeResult<SceneInstanceId> build(const Builder& input) noexcept
        {
            const auto allowed = access();
            if (!allowed)
                return lux::cxx::unexpected(allowed.error());
            if (!input.components_ || !input.simulation_systems_)
                return rejected(ESceneRuntimeError::INVALID_INPUT);
            const auto initial_time = std::visit([](const auto& clock) { return clock.snapshot(); }, input.clock_);
            const bool has_elapsed = initial_time.elapsed != simulation::SimulationDuration{};
            const bool has_delta = initial_time.delta != simulation::SimulationDuration{};
            const bool has_step = initial_time.step_index != 0;
            if (has_elapsed || has_delta || has_step)
                return rejected(ESceneRuntimeError::INVALID_INPUT);
            BusyScope building(busy_);
            Records::key_type key;
            try
            {
                key = records_.emplace(nullptr);
            }
            catch (const std::length_error&)
            {
                // SlotMap reports identity-space exhaustion at this cold allocation boundary.
                return rejected(ESceneRuntimeError::IDENTITY_EXHAUSTED);
            }
            const SceneInstanceId id{domain_, key.index, key.gen};
            const auto fail = [&](auto error) -> SceneRuntimeResult<SceneInstanceId> {
                records_.erase(key);
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
                return fail(prepared.error());
            const auto sealed = (*prepared)->simulation->seal();
            if (!sealed)
            {
                SceneBuildFailure error{ESceneBuildError::SIMULATION_BUILD_FAILURE};
                error.simulation = sealed.error();
                return fail(error);
            }
            const auto reserved = executor_.reserve((*prepared)->simulation->taskCount());
            if (!reserved)
                return fail(reserved.error());
            failures_.reserve(records_.size());
            *records_.find(key) = std::make_unique<Record>(input.clock_, executor_, std::move(*prepared));
            execution_.wake();
            return id;
        }

        [[nodiscard]] SceneRuntimeResult<void> setEnabled(SceneInstanceId id, bool enabled) noexcept
        {
            const auto found = find(id);
            if (!found)
                return lux::cxx::unexpected(found.error());
            auto& record = **found;
            if (enabled)
            {
                if (record.clock_error)
                    return rejected(*record.clock_error, id);
                if (!record.scene.progress().result)
                    return rejected(record.scene.progress().result.error(), id);
                if (record.scene.stopToken().stop_requested())
                    return rejected(ESceneRuntimeError::STOPPED, id);
            }
            if (record.enabled != enabled)
            {
                record.enabled = enabled;
                if (enabled)
                    std::visit([](auto& clock) noexcept { clock.rebase(SteadyClock::now()); }, record.clock);
                execution_.wake();
            }
            return {};
        }

        [[nodiscard]] SceneRuntimeResult<void> destroy(SceneInstanceId id) noexcept
        {
            const auto allowed = access();
            if (!allowed)
                return allowed;
            if (!id.valid())
                return {};
            if (id.domain != domain_)
                return rejected(ESceneRuntimeError::WRONG_DOMAIN, id);
            auto* found = records_.find({id.slot, id.generation});
            if (!found)
                return {};
            BusyScope destroying(busy_);
            auto record = std::move(*found);
            records_.erase({id.slot, id.generation});
            failures_.clear();
            execution_.wake();
            return {};
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
                reclaimTimer();
            if (timer_active_)
            {
                if (deadline != armed_deadline_)
                    timer_stop_->request_stop();
                return {}; // Cancellation completion wakes the owner; never reuse an in-flight operation.
            }
            if (timer_error_)
                return rejected(*timer_error_);
            if (!deadline)
                return {};
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
                return rejected(*timer_error_);
            return {};
        }

        [[nodiscard]] TickResult tick() noexcept
        {
            const auto allowed = access();
            if (!allowed)
                return lux::cxx::unexpected(allowed.error());
            BusyScope ticking(busy_);
            const auto now = SteadyClock::now();
            failures_.clear();
            for (auto& record : records_)
                static_cast<void>(record->driver.maintain(record->scene));
            for (auto& record : records_)
            {
                const bool may_tick = record->enabled && !record->clock_error && record->scene.canTick();
                if (!may_tick)
                    continue;
                std::visit(
                    [&](auto& clock) noexcept {
                        const auto sampled = clock.sample(now);
                        if (!sampled)
                        {
                            record->clock_error = sampled.error();
                            return;
                        }
                        if (!*sampled)
                            return;
                        const auto before = record->scene.simulation().time().step_index;
                        const auto executed = record->driver.tick(record->scene, **sampled);
                        const auto actual = record->scene.simulation().time();
                        if (actual.step_index != before)
                            clock.adopt(now, actual);
                        (void)executed; // The original execution failure remains in SceneDriveSnapshot.
                    },
                    record->clock
                );
            }
            std::optional<SteadyClock::time_point> deadline;
            for (auto& record : records_)
            {
                static_cast<void>(record->driver.publish(record->scene));
                if (!record->scene.progress().result)
                    failures_.push_back({record->scene.id(), record->scene.progress().result.error()});
                else if (record->clock_error)
                    failures_.push_back({record->scene.id(), *record->clock_error});
                const bool may_tick = record->enabled && !record->clock_error && record->scene.canTick();
                if (may_tick)
                {
                    const auto next =
                        std::visit([&](const auto& clock) { return clock.deadline().value_or(now); }, record->clock);
                    if (!deadline || next < *deadline)
                        deadline = next;
                }
            }
            const auto armed = armTimer(deadline);
            if (!armed)
                return lux::cxx::unexpected(armed.error());
            return std::span<const SceneRuntimeFailure>{failures_};
        }

        SceneRuntimeResult<std::reference_wrapper<simulation::ecs::Registry>> getSceneRegistry(SceneInstanceId id
        ) noexcept
        {
            const auto record = find(id);
            if (!record)
                return lux::cxx::unexpected(record.error());
            if (!(**record).scene.atSafePoint())
                return rejected(ESceneRuntimeError::BUSY, id);
            return std::ref((**record).scene.registry());
        }
        SceneRuntimeResult<std::reference_wrapper<const simulation::ecs::Registry>> getSceneRegistry(SceneInstanceId id
        ) const noexcept
        {
            const auto record = find(id);
            if (!record)
                return lux::cxx::unexpected(record.error());
            return std::cref((**record).scene.registry());
        }
        SceneRuntimeResult<std::reference_wrapper<const VSimulationClock>> getClock(SceneInstanceId id) const noexcept
        {
            const auto record = find(id);
            if (!record)
                return lux::cxx::unexpected(record.error());
            return std::cref((**record).clock);
        }
        process::ExecutionRuntime& execution_;
        task::TaskExecutor executor_;
        std::uint64_t domain_;
        std::thread::id owner_{std::this_thread::get_id()};
        Records records_;
        std::vector<SceneRuntimeFailure> failures_;
        bool busy_{};
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
        if (!execution.timer())
            return rejected(process::ETimerError::STOPPING);
        const auto domain = SceneInstance::allocateDomain();
        if (!domain)
            return rejected(ESceneRuntimeError::IDENTITY_EXHAUSTED);
        auto executor = task::TaskExecutor::create(config);
        if (!executor)
            return rejected(executor.error());
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
    SceneRuntimeResult<SceneInstanceId> SceneRuntime::Builder::build() noexcept
    {
        return runtime_.build(*this);
    }
    SceneRuntimeResult<SceneInstanceId> SceneRuntime::build(const Builder& input) noexcept
    {
        return impl_->build(input);
    }

    SceneRuntimeResult<std::reference_wrapper<simulation::ecs::Registry>> SceneRuntime::getSceneRegistry(
        SceneInstanceId id
    ) noexcept
    {
        return impl_->getSceneRegistry(id);
    }
    SceneRuntimeResult<std::reference_wrapper<const simulation::ecs::Registry>> SceneRuntime::getSceneRegistry(
        SceneInstanceId id
    ) const noexcept
    {
        return std::as_const(*impl_).getSceneRegistry(id);
    }
    SceneRuntimeResult<std::reference_wrapper<const VSimulationClock>> SceneRuntime::getClock(SceneInstanceId id
    ) const noexcept
    {
        return impl_->getClock(id);
    }
    SceneRuntimeResult<void> SceneRuntime::invalid(SceneInstanceId id) noexcept
    {
        return impl_->setEnabled(id, false);
    }
    SceneRuntimeResult<void> SceneRuntime::valid(SceneInstanceId id) noexcept
    {
        return impl_->setEnabled(id, true);
    }
    SceneRuntimeResult<void> SceneRuntime::destroy(SceneInstanceId id) noexcept
    {
        return impl_->destroy(id);
    }
    SceneRuntime::TickResult SceneRuntime::tick() noexcept
    {
        return impl_->tick();
    }
}
