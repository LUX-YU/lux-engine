#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/scene/SceneError.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>

namespace lux::scene
{
    namespace
    {
        error::Error runtimeError(ESceneRuntimeError value) noexcept
        {
            constexpr std::string_view names[]{
                "invalid_id",
                "wrong_domain",
                "wrong_thread",
                "busy",
                "stopped",
                "invalid_input",
                "identity_exhausted",
                "capacity"
            };
            const auto code = static_cast<std::size_t>(value);
            const bool retryable = value == ESceneRuntimeError::BUSY || value == ESceneRuntimeError::CAPACITY;
            const std::string name =
                "lux.scene.runtime." + (code < std::size(names) ? std::string(names[code]) : "unknown");
            return error::makeError(
                {name,
                 "Scene Runtime code {0}",
                 retryable ? error::ERecovery::RETRYABLE : error::ERecovery::PERMANENT,
                 {error::EArgument::UNSIGNED}},
                {code}
            );
        }
        error::Error buildError(const SceneBuildFailure& failure) noexcept
        {
            if (failure.code == ESceneBuildError::SCENE_SYSTEM_BUILD_FAILURE)
            {
                const auto& system = failure.scene_system;
                if (system.cause.type)
                {
                    return system.cause;
                }
                return error::makeError(
                    {"lux.scene.system.build",
                     "Scene system code {0}, instance {1}, subject {2}",
                     error::ERecovery::NEEDS_INPUT,
                     {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::HEX}},
                    {static_cast<std::uint64_t>(system.code), system.system.value, system.subject_hash}
                );
            }
            return error::makeError(
                {"lux.scene.build",
                 "Scene build code {0}, simulation code {1}, subject {2}",
                 error::ERecovery::NEEDS_INPUT,
                 {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::HEX}},
                {static_cast<std::uint64_t>(failure.code),
                 static_cast<std::uint64_t>(failure.simulation.code),
                 failure.subject_hash}
            );
        }
        error::Error driveError(const SceneDriveFailure& failure) noexcept
        {
            return std::visit(
                [&](const auto& cause) noexcept -> error::Error
                {
                    using T = std::decay_t<decltype(cause)>;
                    if constexpr (std::is_same_v<T, SceneExecutionFailure>)
                    {
                        if (cause.cause.type)
                        {
                            return cause.cause;
                        }
                        return error::makeError(
                            {"lux.scene.execution",
                             "Scene execution code {0}, system {1}, phase {2}",
                             error::ERecovery::PERMANENT,
                             {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}},
                            {static_cast<std::uint64_t>(cause.code),
                             cause.system.value,
                             static_cast<std::uint64_t>(failure.phase)}
                        );
                    }
                    else if constexpr (std::is_same_v<T, simulation::SimulationExecutionFailure>)
                    {
                        if (cause.code == simulation::ESimulationExecutionError::ECS_COMMAND_FAILURE)
                        {
                            return error::makeError(
                                {"lux.simulation.commands",
                                 "ECS command code {0}, producer {1}, command {2}",
                                 error::ERecovery::PERMANENT,
                                 {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}},
                                {static_cast<std::uint64_t>(cause.ecs_command.code),
                                 cause.ecs_command.producer,
                                 cause.ecs_command.command}
                            );
                        }
                        return error::makeError(
                            {"lux.simulation.execution",
                             "Simulation code {0}, system {1}, executor code {2}",
                             error::ERecovery::PERMANENT,
                             {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}},
                            {static_cast<std::uint64_t>(cause.code),
                             cause.system.value,
                             static_cast<std::uint64_t>(cause.task_executor.code)}
                        );
                    }
                    else
                    {
                        return error::makeError(
                            {"lux.scene.drive",
                             "Scene drive code {0}, phase {1}",
                             error::ERecovery::PERMANENT,
                             {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}},
                            {static_cast<std::uint64_t>(cause), static_cast<std::uint64_t>(failure.phase)}
                        );
                    }
                },
                failure.cause
            );
        }
    } // namespace
    error::Error toError(const SceneRuntimeFailure& failure) noexcept
    {
        return std::visit(
            [](const auto& cause) noexcept -> error::Error
            {
                using T = std::decay_t<decltype(cause)>;
                if constexpr (std::is_same_v<T, ESceneRuntimeError>)
                {
                    return runtimeError(cause);
                }
                else if constexpr (std::is_same_v<T, SceneBuildFailure>)
                {
                    return buildError(cause);
                }
                else if constexpr (std::is_same_v<T, SceneDriveFailure>)
                {
                    return driveError(cause);
                }
                else if constexpr (std::is_same_v<T, EClockError>)
                {
                    return error::makeError(
                        {"lux.scene.clock", "Clock code {0}", error::ERecovery::PERMANENT, {error::EArgument::UNSIGNED}
                        },
                        {static_cast<std::uint64_t>(cause)}
                    );
                }
                else if constexpr (std::is_same_v<T, task::TaskExecutorFailure>)
                {
                    return error::makeError(
                        {"lux.scene.executor",
                         "Executor code {0}",
                         error::ERecovery::PERMANENT,
                         {error::EArgument::UNSIGNED}},
                        {static_cast<std::uint64_t>(cause.code)}
                    );
                }
                else
                {
                    static_assert(std::is_same_v<T, process::ETimerError>);
                    return error::makeError(
                        {"lux.scene.timer", "Timer code {0}", error::ERecovery::PERMANENT, {error::EArgument::UNSIGNED}
                        },
                        {static_cast<std::uint64_t>(cause)}
                    );
                }
            },
            failure.cause
        );
    }
} // namespace lux::scene
