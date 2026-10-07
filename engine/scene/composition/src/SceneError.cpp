#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/scene/SceneError.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>

namespace lux::scene
{
    namespace
    {
        namespace Errors
        {
            constexpr error::ErrorDescriptor SceneSystemBuildDescriptor{
                "lux.scene.system.build",
                "Scene system code {0}, instance {1}, subject {2}",
                error::ERecovery::NEEDS_INPUT,
                {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::HEX}
            };
            constexpr error::ErrorId SceneSystemBuild = error::errorId(SceneSystemBuildDescriptor.name);
            constexpr error::ErrorDescriptor SceneBuildDescriptor{
                "lux.scene.build",
                "Scene build code {0}, simulation code {1}, subject {2}",
                error::ERecovery::NEEDS_INPUT,
                {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::HEX}
            };
            constexpr error::ErrorId SceneBuild = error::errorId(SceneBuildDescriptor.name);
            constexpr error::ErrorDescriptor SceneExecutionDescriptor{
                "lux.scene.execution",
                "Scene execution code {0}, system {1}, phase {2}",
                error::ERecovery::PERMANENT,
                {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
            };
            constexpr error::ErrorId SceneExecution = error::errorId(SceneExecutionDescriptor.name);
            constexpr error::ErrorDescriptor SimulationCommandsDescriptor{
                "lux.simulation.commands",
                "ECS command code {0}, producer {1}, command {2}",
                error::ERecovery::PERMANENT,
                {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
            };
            constexpr error::ErrorId SimulationCommands = error::errorId(SimulationCommandsDescriptor.name);
            constexpr error::ErrorDescriptor SimulationExecutionDescriptor{
                "lux.simulation.execution",
                "Simulation code {0}, system {1}, executor code {2}",
                error::ERecovery::PERMANENT,
                {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
            };
            constexpr error::ErrorId SimulationExecution = error::errorId(SimulationExecutionDescriptor.name);
            constexpr error::ErrorDescriptor SceneDriveDescriptor{
                "lux.scene.drive",
                "Scene drive code {0}, phase {1}",
                error::ERecovery::PERMANENT,
                {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
            };
            constexpr error::ErrorId SceneDrive = error::errorId(SceneDriveDescriptor.name);
            constexpr error::ErrorDescriptor SceneClockDescriptor{
                "lux.scene.clock",
                "Clock code {0}",
                error::ERecovery::PERMANENT,
                {error::EArgument::UNSIGNED}
            };
            constexpr error::ErrorId SceneClock = error::errorId(SceneClockDescriptor.name);
            constexpr error::ErrorDescriptor SceneExecutorDescriptor{
                "lux.scene.executor",
                "Executor code {0}",
                error::ERecovery::PERMANENT,
                {error::EArgument::UNSIGNED}
            };
            constexpr error::ErrorId SceneExecutor = error::errorId(SceneExecutorDescriptor.name);
            constexpr error::ErrorDescriptor SceneTimerDescriptor{
                "lux.scene.timer",
                "Timer code {0}",
                error::ERecovery::PERMANENT,
                {error::EArgument::UNSIGNED}
            };
            constexpr error::ErrorId SceneTimer = error::errorId(SceneTimerDescriptor.name);
        } // namespace Errors
        constexpr error::ErrorDescriptor ErrorDescriptors[]{
            Errors::SceneSystemBuildDescriptor,
            Errors::SceneBuildDescriptor,
            Errors::SceneExecutionDescriptor,
            Errors::SimulationCommandsDescriptor,
            Errors::SimulationExecutionDescriptor,
            Errors::SceneDriveDescriptor,
            Errors::SceneClockDescriptor,
            Errors::SceneExecutorDescriptor,
            Errors::SceneTimerDescriptor
        };
    } // namespace

    namespace
    {
        constexpr error::ErrorDescriptor RuntimeDescriptors[]{
            {"lux.scene.runtime.invalid_id",
             "Scene Runtime code {0}",
             error::ERecovery::PERMANENT,
             {error::EArgument::UNSIGNED}},
            {"lux.scene.runtime.wrong_domain",
             "Scene Runtime code {0}",
             error::ERecovery::PERMANENT,
             {error::EArgument::UNSIGNED}},
            {"lux.scene.runtime.wrong_thread",
             "Scene Runtime code {0}",
             error::ERecovery::PERMANENT,
             {error::EArgument::UNSIGNED}},
            {"lux.scene.runtime.busy",
             "Scene Runtime code {0}",
             error::ERecovery::RETRYABLE,
             {error::EArgument::UNSIGNED}},
            {"lux.scene.runtime.stopped",
             "Scene Runtime code {0}",
             error::ERecovery::PERMANENT,
             {error::EArgument::UNSIGNED}},
            {"lux.scene.runtime.invalid_input",
             "Scene Runtime code {0}",
             error::ERecovery::PERMANENT,
             {error::EArgument::UNSIGNED}},
            {"lux.scene.runtime.identity_exhausted",
             "Scene Runtime code {0}",
             error::ERecovery::PERMANENT,
             {error::EArgument::UNSIGNED}},
            {"lux.scene.runtime.capacity",
             "Scene Runtime code {0}",
             error::ERecovery::RETRYABLE,
             {error::EArgument::UNSIGNED}},
            {"lux.scene.runtime.unknown",
             "Scene Runtime code {0}",
             error::ERecovery::PERMANENT,
             {error::EArgument::UNSIGNED}}
        };
        constexpr auto RuntimeDescriptorsIds = []
        {
            std::array<error::ErrorId, std::size(RuntimeDescriptors)> result{};
            for (std::size_t i{}; i < result.size(); ++i)
            {
                result[i] = error::errorId(RuntimeDescriptors[i].name);
            }
            return result;
        }();
        error::Error runtimeError(ESceneRuntimeError value) noexcept
        {
            const auto code = static_cast<std::size_t>(value);
            const auto index = code < std::size(RuntimeDescriptors) - 1 ? code : std::size(RuntimeDescriptors) - 1;
            return {RuntimeDescriptorsIds[index], {code}};
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
                return error::Error{
                    Errors::SceneSystemBuild,
                    {static_cast<std::uint64_t>(system.code), system.system.value, system.subject_hash}
                };
            }
            return error::Error{
                Errors::SceneBuild,
                {static_cast<std::uint64_t>(failure.code),
                 static_cast<std::uint64_t>(failure.simulation.code),
                 failure.subject_hash}
            };
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
                        return error::Error{
                            Errors::SceneExecution,
                            {static_cast<std::uint64_t>(cause.code),
                             cause.system.value,
                             static_cast<std::uint64_t>(failure.phase)}
                        };
                    }
                    else if constexpr (std::is_same_v<T, simulation::SimulationExecutionFailure>)
                    {
                        if (cause.code == simulation::ESimulationExecutionError::ECS_COMMAND_FAILURE)
                        {
                            return error::Error{
                                Errors::SimulationCommands,
                                {static_cast<std::uint64_t>(cause.ecs_command.code),
                                 cause.ecs_command.producer,
                                 cause.ecs_command.command}
                            };
                        }
                        return error::Error{
                            Errors::SimulationExecution,
                            {static_cast<std::uint64_t>(cause.code),
                             cause.system.value,
                             static_cast<std::uint64_t>(cause.task_executor.code)}
                        };
                    }
                    else
                    {
                        return error::Error{
                            Errors::SceneDrive,
                            {static_cast<std::uint64_t>(cause), static_cast<std::uint64_t>(failure.phase)}
                        };
                    }
                },
                failure.cause
            );
        }
    } // namespace
    cxx::expected<void, error::Error> registerSceneErrors() noexcept
    {
        auto& registry = error::ErrorRegistry::instance();
        if (auto result = registry.registerTypes(ErrorDescriptors); !result)
        {
            return result;
        }
        return registry.registerTypes(RuntimeDescriptors);
    }
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
                    return error::Error{Errors::SceneClock, {static_cast<std::uint64_t>(cause)}};
                }
                else if constexpr (std::is_same_v<T, task::TaskExecutorFailure>)
                {
                    return error::Error{Errors::SceneExecutor, {static_cast<std::uint64_t>(cause.code)}};
                }
                else
                {
                    static_assert(std::is_same_v<T, process::ETimerError>);
                    return error::Error{Errors::SceneTimer, {static_cast<std::uint64_t>(cause)}};
                }
            },
            failure.cause
        );
    }
} // namespace lux::scene
