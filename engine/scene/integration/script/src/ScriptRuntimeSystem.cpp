#include <lux/engine/scene/ScriptRuntimeSystem.hpp>

#include <lux/engine/scene/SceneBuilder.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/simulation/abilities/DelayAbility.hpp>

#include <exec/materialize.hpp>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <mutex>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace lux::scene
{
    struct ScriptRealDelayProvider::Impl final
    {
        enum class EState : std::uint8_t
        {
            ACTIVE,
            STOPPING,
            JOINED,
        };

        enum class ETerminal : std::uint8_t
        {
            PENDING,
            READY,
            FAILED,
            STOPPED,
        };

        struct Request final
        {
            lux::script::ScriptAbilityCompletion<void> completion;
            std::atomic<ETerminal> terminal{ETerminal::PENDING};
            std::atomic<std::int32_t> status{};
        };

        Impl(process::TimerClient timer_value, std::size_t capacity_value) noexcept
            : timer(std::move(timer_value)), capacity(capacity_value)
        {
        }

        ~Impl() noexcept
        {
            if (!tasks.closed())
            {
                tasks.requestStop();
                if (!stdexec::sync_wait(tasks.close()))
                    std::terminate();
            }
        }

        process::TimerClient timer;
        process::TaskScope tasks;
        std::mutex mutex;
        std::vector<std::shared_ptr<Request>> requests;
        std::size_t capacity{};
        EState state{EState::ACTIVE};
    };

    namespace
    {
        [[nodiscard]] std::int32_t timerStatus(process::ETimerError error) noexcept
        {
            using simulation::script::EScriptDelayStatus;
            switch (error)
            {
            case process::ETimerError::INVALID_ARGUMENT:
                return static_cast<std::int32_t>(EScriptDelayStatus::INVALID_DURATION);
            case process::ETimerError::CAPACITY_EXCEEDED:
                return static_cast<std::int32_t>(EScriptDelayStatus::CAPACITY_EXCEEDED);
            case process::ETimerError::STOPPING:
                return static_cast<std::int32_t>(EScriptDelayStatus::STOPPING);
            case process::ETimerError::ALLOCATION_FAILURE:
                return static_cast<std::int32_t>(EScriptDelayStatus::ALLOCATION_FAILURE);
            case process::ETimerError::WORKER_CREATION_FAILURE:
            case process::ETimerError::BACKEND_FAILURE:
                return static_cast<std::int32_t>(EScriptDelayStatus::TIMER_FAILURE);
            }
            return static_cast<std::int32_t>(EScriptDelayStatus::TIMER_FAILURE);
        }

        constexpr std::array Requirements{
            SceneSystemRequirementSpec{
                .name = "script_runtime_host",
                .capability = "lux.script.runtime.host",
                .expected_type = lux::cxx::typeToken<ScriptRuntimeHost>(),
                .optional = false
            },
            SceneSystemRequirementSpec{
                .name = "timer",
                .capability = "lux.process.timer",
                .expected_type = lux::cxx::typeToken<process::TimerClient>(),
                .optional = false
            }
        };

        [[nodiscard]] SceneSystemBuildFailure failure(
            ESceneSystemBuildError code,
            system::SystemInstanceId system,
            std::uint64_t subject = 0U
        ) noexcept
        {
            return {code, system, {}, subject};
        }

        [[nodiscard]] lux::cxx::expected<void, SceneSystemBuildFailure> installScriptRuntimeSystem(
            SceneBuilder& builder,
            SceneSystemView description
        ) noexcept
        {
            auto* host = builder.require<ScriptRuntimeHost>(description.instanceId(), "script_runtime_host");
            if (host == nullptr)
            {
                return lux::cxx::unexpected(failure(
                    ESceneSystemBuildError::MISSING_REQUIREMENT,
                    description.instanceId()
                ));
            }
            auto* timer = builder.require<process::TimerClient>(description.instanceId(), "timer");
            if (timer == nullptr)
            {
                return lux::cxx::unexpected(failure(
                    ESceneSystemBuildError::MISSING_REQUIREMENT,
                    description.instanceId()
                ));
            }

            const auto data = builder.simulation().description().findData(
                simulation::script::scriptSystemDataSchemaId()
            );
            if (!data)
            {
                return lux::cxx::unexpected(failure(
                    ESceneSystemBuildError::INVALID_DESCRIPTION,
                    description.instanceId(),
                    simulation::script::scriptSystemDataSchemaId().hash
                ));
            }

            auto decoded = simulation::script::decodeScriptSystemDescription(
                data.payload(),
                builder.simulation().description(),
                host->codec_limits
            );
            if (!decoded)
            {
                return lux::cxx::unexpected(failure(
                    ESceneSystemBuildError::INVALID_DESCRIPTION,
                    description.instanceId(),
                    static_cast<std::uint64_t>(decoded.error())
                ));
            }

            try
            {
                auto real_delay = ScriptRealDelayProvider::create(*timer, host->real_delay_capacity);
                if (!real_delay)
                {
                    return lux::cxx::unexpected(failure(
                        ESceneSystemBuildError::CONSTRUCTION_FAILURE,
                        description.instanceId(),
                        static_cast<std::uint64_t>(real_delay.error())
                    ));
                }
                auto owned_description = std::make_unique<simulation::script::ScriptSystemDescription>(
                    std::move(*decoded)
                );
                auto created = simulation::script::ScriptSystem::create(
                    builder.simulation().description(),
                    *owned_description,
                    builder.registry(),
                    builder.simulation().clock(),
                    host->limits,
                    host->artifacts,
                    host->world,
                    builder.simulation().scriptApiCapabilities(),
                    host->backends,
                    builder.simulation().scriptHookEndpoints(),
                    builder.simulation().scriptEventEndpoints(),
                    host->host,
                    (*real_delay)->endpoint()
                );
                if (!created)
                {
                    return lux::cxx::unexpected(failure(
                        ESceneSystemBuildError::CONSTRUCTION_FAILURE,
                        description.instanceId(),
                        static_cast<std::uint64_t>(created.error())
                    ));
                }
                auto prepared = created->prepare();
                if (!prepared)
                {
                    return lux::cxx::unexpected(failure(
                        ESceneSystemBuildError::CONSTRUCTION_FAILURE,
                        description.instanceId(),
                        static_cast<std::uint64_t>(prepared.error())
                    ));
                }

                auto installed = builder.emplaceSystem<ScriptRuntimeSystem>(
                    description.instanceId(),
                    std::move(*real_delay),
                    std::move(owned_description),
                    std::move(*created)
                );
                if (!installed)
                    return lux::cxx::unexpected(installed.error());

                if (!(*installed)->bindSimulation(builder.simulation()))
                    return lux::cxx::unexpected(failure(
                        ESceneSystemBuildError::CONSTRUCTION_FAILURE, description.instanceId()));
                return {};
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(failure(
                    ESceneSystemBuildError::ALLOCATION_FAILURE,
                    description.instanceId()
                ));
            }
        }
    } // namespace

    ScriptRealDelayProvider::ScriptRealDelayProvider(std::unique_ptr<Impl> impl) noexcept
        : impl_(std::move(impl))
    {
    }

    ScriptRealDelayProvider::CreateResult ScriptRealDelayProvider::create(
        process::TimerClient timer,
        std::size_t capacity
    ) noexcept
    {
        if (!timer || capacity == 0U)
            return lux::cxx::unexpected(EScriptRealDelayProviderError::INVALID_ARGUMENT);
        try
        {
            auto impl = std::make_unique<Impl>(std::move(timer), capacity);
            impl->requests.reserve(capacity);
            return std::unique_ptr<ScriptRealDelayProvider>(new ScriptRealDelayProvider(std::move(impl)));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EScriptRealDelayProviderError::ALLOCATION_FAILURE);
        }
    }

    ScriptRealDelayProvider::~ScriptRealDelayProvider() noexcept
    {
        if (impl_->state != Impl::EState::JOINED)
        {
            requestStop();
            if (!join())
                std::terminate();
        }
    }

    lux::script::ScriptAbilityStartResult ScriptRealDelayProvider::start(
        std::chrono::nanoseconds duration,
        lux::script::ScriptAbilityCompletion<void> completion
    ) noexcept
    {
        using simulation::script::EScriptDelayStatus;
        if (duration <= std::chrono::nanoseconds::zero() || !completion.active())
        {
            return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{
                static_cast<std::int32_t>(EScriptDelayStatus::INVALID_DURATION)
            });
        }

        std::shared_ptr<Impl::Request> request;
        try
        {
            request = std::make_shared<Impl::Request>();
            request->completion = std::move(completion);
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{
                static_cast<std::int32_t>(EScriptDelayStatus::ALLOCATION_FAILURE)
            });
        }

        {
            std::lock_guard lock{impl_->mutex};
            if (impl_->state != Impl::EState::ACTIVE)
            {
                return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{
                    static_cast<std::int32_t>(EScriptDelayStatus::STOPPING)
                });
            }
            if (impl_->requests.size() >= impl_->capacity)
            {
                return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{
                    static_cast<std::int32_t>(EScriptDelayStatus::CAPACITY_EXCEEDED)
                });
            }
            impl_->requests.push_back(request);
        }

        try
        {
            auto terminal = exec::materialize(impl_->timer.after(duration)) |
                stdexec::then([request]<class Tag, class... Values>(Tag, Values&&... values) noexcept {
                    if constexpr (std::same_as<Tag, stdexec::set_value_t>)
                    {
                        request->terminal.store(Impl::ETerminal::READY, std::memory_order_release);
                    }
                    else if constexpr (std::same_as<Tag, stdexec::set_error_t>)
                    {
                        const auto error = (std::forward<Values>(values), ...);
                        request->status.store(timerStatus(error), std::memory_order_release);
                        request->terminal.store(Impl::ETerminal::FAILED, std::memory_order_release);
                    }
                    else
                    {
                        request->status.store(
                            static_cast<std::int32_t>(EScriptDelayStatus::STOPPING),
                            std::memory_order_release
                        );
                        request->terminal.store(Impl::ETerminal::STOPPED, std::memory_order_release);
                    }
                });
            auto started = impl_->tasks.start(std::move(terminal));
            if (started)
                return {};

            std::lock_guard lock{impl_->mutex};
            std::erase(impl_->requests, request);
            const auto status = started.error() == process::ETaskStartError::STOPPING
                ? EScriptDelayStatus::STOPPING
                : EScriptDelayStatus::ALLOCATION_FAILURE;
            return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{
                static_cast<std::int32_t>(status)
            });
        }
        catch (const std::bad_alloc&)
        {
            std::lock_guard lock{impl_->mutex};
            std::erase(impl_->requests, request);
            return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{
                static_cast<std::int32_t>(EScriptDelayStatus::ALLOCATION_FAILURE)
            });
        }
        catch (...)
        {
            std::lock_guard lock{impl_->mutex};
            std::erase(impl_->requests, request);
            return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{
                static_cast<std::int32_t>(EScriptDelayStatus::TIMER_FAILURE)
            });
        }
    }

    simulation::script::ScriptRealDelayEndpoint ScriptRealDelayProvider::endpoint() noexcept
    {
        return {
            this,
            +[](void* context,
                std::chrono::nanoseconds duration,
                lux::script::ScriptAbilityCompletion<void> completion) noexcept {
                return static_cast<ScriptRealDelayProvider*>(context)->start(duration, std::move(completion));
            }
        };
    }

    bool ScriptRealDelayProvider::drainCompletions() noexcept
    {
        for (;;)
        {
            std::shared_ptr<Impl::Request> request;
            Impl::ETerminal terminal{Impl::ETerminal::PENDING};
            {
                std::lock_guard lock{impl_->mutex};
                const auto found = std::find_if(impl_->requests.begin(), impl_->requests.end(), [](const auto& value) {
                    return value->terminal.load(std::memory_order_acquire) != Impl::ETerminal::PENDING;
                });
                if (found == impl_->requests.end())
                    return true;
                request = *found;
                terminal = request->terminal.load(std::memory_order_acquire);
            }

            const auto completed = terminal == Impl::ETerminal::READY
                ? request->completion.success()
                : request->completion.fail({request->status.load(std::memory_order_acquire)});
            if (!completed && completed.error() == lux::script::EScriptAbilityCompletionError::BACKPRESSURE)
                return true;

            {
                std::lock_guard lock{impl_->mutex};
                std::erase(impl_->requests, request);
            }
            if (!completed && completed.error() != lux::script::EScriptAbilityCompletionError::STALE &&
                completed.error() != lux::script::EScriptAbilityCompletionError::STOPPING &&
                completed.error() != lux::script::EScriptAbilityCompletionError::ALREADY_COMPLETED)
            {
                return false;
            }
        }
    }

    void ScriptRealDelayProvider::requestStop() noexcept
    {
        {
            std::lock_guard lock{impl_->mutex};
            if (impl_->state != Impl::EState::ACTIVE)
                return;
            impl_->state = Impl::EState::STOPPING;
        }
        impl_->tasks.requestStop();
    }

    lux::cxx::expected<void, EScriptRealDelayProviderError> ScriptRealDelayProvider::join() noexcept
    {
        {
            std::lock_guard lock{impl_->mutex};
            if (impl_->state != Impl::EState::STOPPING)
                return lux::cxx::unexpected(EScriptRealDelayProviderError::INVALID_STATE);
        }
        if (!stdexec::sync_wait(impl_->tasks.close()))
            return lux::cxx::unexpected(EScriptRealDelayProviderError::INVALID_STATE);
        {
            std::lock_guard lock{impl_->mutex};
            impl_->requests.clear();
            impl_->state = Impl::EState::JOINED;
        }
        return {};
    }

    ScriptRuntimeSystem::ScriptRuntimeSystem(
        std::unique_ptr<ScriptRealDelayProvider> real_delay,
        std::unique_ptr<simulation::script::ScriptSystemDescription> description,
        simulation::script::ScriptSystem system
    ) noexcept
        : real_delay_(std::move(real_delay)), description_(std::move(description)), system_(std::move(system))
    {
    }

    ScriptRuntimeSystem::~ScriptRuntimeSystem() noexcept
    {
        hook_connection_.reset();
        real_delay_->requestStop();
        if (!system_.shutdown())
            std::terminate();
        if (!real_delay_->join())
            std::terminate();
    }

    bool ScriptRuntimeSystem::bindSimulation(simulation::Simulation& simulation) noexcept
    {
        auto connection = simulation.bindHookCallbacks({this,
            [](void* context, const simulation::SimulationClockSnapshot&, bool stable) noexcept {
                auto& runtime = *static_cast<ScriptRuntimeSystem*>(context);
                auto& system = runtime.system_;
                if (stable)
                {
                    if (!runtime.real_delay_->drainCompletions())
                        return false;
                    system.beginStableAdmission();
                }
                const auto result = system.processLifecycle();
                return result || result.error() == simulation::script::EScriptSystemError::WORLD_OBJECT_NOT_RESOLVED;
            },
            [](void* context, const simulation::SimulationClockSnapshot&, bool stable_resume) noexcept {
                if (!stable_resume)
                    return true;
                auto& runtime = *static_cast<ScriptRuntimeSystem*>(context);
                return static_cast<bool>(runtime.system_.executeStablePoint());
            },
            [](void* context, const simulation::SimulationClockSnapshot&) noexcept {
                auto& runtime = *static_cast<ScriptRuntimeSystem*>(context);
                const auto result = runtime.system_.processLifecycle();
                runtime.stats_exchange_.write() = runtime.system_.stats();
                runtime.stats_exchange_.publish();
                return result || result.error() == simulation::script::EScriptSystemError::WORLD_OBJECT_NOT_RESOLVED;
            },
            [](void* context, const simulation::SimulationClockSnapshot&) noexcept {
                auto& runtime = *static_cast<ScriptRuntimeSystem*>(context);
                static_cast<void>(runtime.system_.processLifecycle(
                    simulation::script::EScriptLifecycleAdmission::RETIRE_ONLY));
                runtime.stats_exchange_.write() = runtime.system_.stats();
                runtime.stats_exchange_.publish();
            }});
        if (!connection)
            return false;
        hook_connection_ = std::move(*connection);
        return static_cast<bool>(simulation.seal());
    }

    const simulation::script::ScriptSystem& ScriptRuntimeSystem::scriptSystem() const noexcept
    {
        return system_;
    }

    bool ScriptRuntimeSystem::acquireStats(simulation::script::ScriptRuntimeStats& output) noexcept
    {
        const bool updated = stats_exchange_.acquireLatest();
        output = stats_exchange_.read();
        return updated;
    }

    SceneSystemRegistration builtinScriptRuntimeSystemRegistration() noexcept
    {
        return SceneSystemRegistration{
            .type = system::systemTypeId(ScriptRuntimeSystem::Description.canonical_name),
            .cpp_type = lux::cxx::typeToken<ScriptRuntimeSystem>(),
            .description = &ScriptRuntimeSystem::Description,
            .configuration = {},
            .observations = {},
            .requirements = Requirements,
            .connections = {},
            .project_object = sceneSystemObjectProjection<ScriptRuntimeSystem>(),
            .install = &installScriptRuntimeSystem
        };
    }

    std::span<const SceneSystemRegistration> builtinScriptRuntimeSystemRegistrations() noexcept
    {
        static const std::array registrations{builtinScriptRuntimeSystemRegistration()};
        return registrations;
    }
} // namespace lux::scene
