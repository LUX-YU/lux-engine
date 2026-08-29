#pragma once

#include <lux/engine/simulation/SystemConcept.hpp>
#include <lux/engine/simulation/SystemRegistry.hpp>
#include <lux/engine/simulation/core/visibility.h>
#include <lux/engine/simulation/ecs/EcsCommandBuffer.hpp>
#include <lux/engine/simulation/ecs/SystemTaskResources.hpp>
#include <lux/engine/task/TaskCallable.hpp>

#include <concepts>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace lux::simulation
{
    class Simulation;

    class LUX_ENGINE_SIMULATION_CORE_PUBLIC SimulationBuilder final
    {
    public:
        [[nodiscard]] ecs::Registry& registry() noexcept;

        template <System Type, class... Args>
        [[nodiscard]] lux::cxx::expected<Type*, SystemBuildFailure>
        emplaceSystem(SystemInstanceId instance, Args&&... args) noexcept
        {
            try
            {
                auto object = std::make_unique<Type>(std::forward<Args>(args)...);
                auto inserted = emplaceErased(
                    instance,
                    lux::cxx::typeToken<Type>(),
                    object.get(),
                    [](void* value) noexcept { delete static_cast<Type*>(value); }
                );
                if (!inserted)
                    return lux::cxx::unexpected(inserted.error());
                object.release();
                return static_cast<Type*>(*inserted);
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(
                    SystemBuildFailure{ESystemBuildError::ALLOCATION_FAILURE, instance}
                );
            }
            catch (...)
            {
                return lux::cxx::unexpected(
                    SystemBuildFailure{ESystemBuildError::CONSTRUCTION_FAILURE, instance}
                );
            }
        }

        template <System Type>
        [[nodiscard]] Type* findSystem(SystemInstanceId instance) noexcept
        {
            return static_cast<Type*>(findErased(instance, lux::cxx::typeToken<Type>()));
        }

        template <System Type, class Callable>
        [[nodiscard]] lux::cxx::expected<void, SystemBuildFailure>
        addSystemTask(SystemInstanceId instance, Callable&& callable) noexcept
        {
            using Function = std::decay_t<Callable>;
            static_assert(std::is_move_constructible_v<Function>);
            static_assert(std::is_nothrow_invocable_v<const Function&, Type&>);
            using Result = std::invoke_result_t<const Function&, Type&>;
            static_assert(std::same_as<Result, void> || std::same_as<Result, bool>);

            Type* object = findInstalledExact<Type>(instance);
            if (object == nullptr)
            {
                return lux::cxx::unexpected(
                    SystemBuildFailure{ESystemBuildError::INVALID_DESCRIPTION, instance}
                );
            }

            try
            {
                const FailureReporter reporter = failureReporter();
                task::TaskCallable task_callable(
                    [object, function = Function(std::forward<Callable>(callable)), reporter, instance]() noexcept {
                        if constexpr (std::same_as<Result, bool>)
                        {
                            if (!function(*object))
                                reporter.report(instance);
                        }
                        else
                            function(*object);
                    }
                );
                return addPrimaryTask(
                    instance,
                    ecs::systemTaskResources<Type>(),
                    std::move(task_callable)
                );
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(
                    SystemBuildFailure{ESystemBuildError::ALLOCATION_FAILURE, instance}
                );
            }
        }

        template <System Type, class Callable>
        [[nodiscard]] lux::cxx::expected<void, SystemBuildFailure> addSystemCommandTask(
            SystemInstanceId instance,
            ecs::EcsCommandProducerCapacity capacity,
            Callable&& callable
        ) noexcept
        {
            using Function = std::decay_t<Callable>;
            static_assert(std::is_move_constructible_v<Function>);
            static_assert(std::is_nothrow_invocable_v<const Function&, Type&, ecs::EcsCommandWriter&>);
            using Result = std::invoke_result_t<const Function&, Type&, ecs::EcsCommandWriter&>;
            static_assert(std::same_as<Result, void> || std::same_as<Result, bool>);

            Type* object = findInstalledExact<Type>(instance);
            if (object == nullptr)
            {
                return lux::cxx::unexpected(
                    SystemBuildFailure{ESystemBuildError::INVALID_DESCRIPTION, instance}
                );
            }

            auto command = allocateCommandProducer(instance, capacity);
            if (!command)
                return lux::cxx::unexpected(command.error());

            try
            {
                const CommandBinding binding = *command;
                task::TaskCallable task_callable(
                    [object, function = Function(std::forward<Callable>(callable)), binding, instance]() noexcept {
                        auto begun = binding.commands->begin(binding.producer);
                        if (!begun)
                        {
                            binding.reporter.report(instance);
                            return;
                        }
                        auto writer = std::move(*begun);
                        if constexpr (std::same_as<Result, bool>)
                        {
                            if (!function(*object, writer))
                                binding.reporter.report(instance);
                        }
                        else
                            function(*object, writer);
                    }
                );
                return addPrimaryTask(
                    instance,
                    ecs::systemTaskResources<Type>(),
                    std::move(task_callable)
                );
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(
                    SystemBuildFailure{ESystemBuildError::ALLOCATION_FAILURE, instance}
                );
            }
        }

    private:
        struct Impl;

        struct FailureReporter final
        {
            void* state{};
            void (*report_failure)(void*, SystemInstanceId) noexcept{};

            void report(SystemInstanceId system) const noexcept
            {
                report_failure(state, system);
            }
        };

        struct CommandBinding final
        {
            ecs::EcsCommandBuffer* commands{};
            std::size_t producer{};
            FailureReporter reporter;
        };

        explicit SimulationBuilder(Impl& impl) noexcept : impl_(&impl)
        {
        }

        [[nodiscard]] lux::cxx::expected<void*, SystemBuildFailure> emplaceErased(
            SystemInstanceId instance,
            lux::cxx::TypeToken type,
            void* object,
            void (*destroy)(void*) noexcept
        ) noexcept;

        [[nodiscard]] void* findErased(SystemInstanceId instance, lux::cxx::TypeToken type) noexcept;
        [[nodiscard]] void* findInstalledErased(SystemInstanceId instance, lux::cxx::TypeToken type) noexcept;

        template <System Type>
        [[nodiscard]] Type* findInstalledExact(SystemInstanceId instance) noexcept
        {
            return static_cast<Type*>(findInstalledErased(instance, lux::cxx::typeToken<Type>()));
        }

        [[nodiscard]] FailureReporter failureReporter() noexcept;

        [[nodiscard]] lux::cxx::expected<CommandBinding, SystemBuildFailure>
        allocateCommandProducer(
            SystemInstanceId instance,
            ecs::EcsCommandProducerCapacity capacity
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<void, SystemBuildFailure> addPrimaryTask(
            SystemInstanceId instance,
            task::TaskResources resources,
            task::TaskCallable callable
        ) noexcept;

        Impl* impl_{};

        friend class Simulation;
    };
} // namespace lux::simulation
