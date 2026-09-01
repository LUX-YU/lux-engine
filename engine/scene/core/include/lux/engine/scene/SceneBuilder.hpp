#pragma once

#include <lux/engine/scene/SceneSystem.hpp>
#include <lux/engine/scene/SceneSystemRegistration.hpp>
#include <lux/engine/scene/visibility.h>
#include <lux/engine/simulation/Simulation.hpp>

#include <lux/cxx/core/move_only_function.hpp>

#include <concepts>
#include <memory>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>

namespace lux::scene
{
    class Scene;
    class SceneMetaManager;

    class LUX_ENGINE_SCENE_PUBLIC SceneBuilder final
    {
    public:
        [[nodiscard]] simulation::ecs::Registry& registry() noexcept;
        [[nodiscard]] simulation::Simulation& simulation() noexcept;
        [[nodiscard]] const SceneMetaManager& meta() const noexcept;

        template <class Configuration>
        [[nodiscard]] lux::cxx::expected<Configuration, SceneSystemBuildFailure>
        decodeConfiguration(SceneSystemView description) noexcept
        {
            static_assert(std::is_nothrow_default_constructible_v<Configuration>);
            static_assert(std::is_nothrow_destructible_v<Configuration>);
            const auto* registration = currentRegistration();
            const bool invalid_registration = registration == nullptr || registration->description == nullptr ||
                !registration->configuration.valid() ||
                registration->configuration.type != lux::cxx::typeToken<Configuration>();
            if (invalid_registration)
            {
                return lux::cxx::unexpected(SceneSystemBuildFailure{
                    ESceneSystemBuildError::INVALID_DESCRIPTION,
                    description.instanceId()
                });
            }
            const auto& type = *registration->description;
            const bool invalid_description = !description || type.configuration_schema_name.empty() ||
                description.configurationSchemaName() != type.configuration_schema_name ||
                description.configurationSchemaHash() != lux::cxx::Fnv1a64::hash(type.configuration_schema_name) ||
                description.configurationSchemaVersion() != type.configuration_schema_version;
            if (invalid_description)
            {
                return lux::cxx::unexpected(SceneSystemBuildFailure{
                    ESceneSystemBuildError::INVALID_DESCRIPTION,
                    description.instanceId()
                });
            }
            Configuration value{};
            auto decoded = registration->configuration.decode(description.configurationPayload(), &value);
            if (!decoded)
            {
                return lux::cxx::unexpected(SceneSystemBuildFailure{
                    ESceneSystemBuildError::CONFIGURATION_DECODE_FAILURE,
                    description.instanceId(),
                    {},
                    0U,
                    decoded.error()
                });
            }
            return value;
        }

        template <SceneSystem Type, class... Args>
        [[nodiscard]] lux::cxx::expected<Type*, SceneSystemBuildFailure> emplaceSystem(
            system::SystemInstanceId instance,
            Args&&... args
        ) noexcept
        {
            const auto* registration = currentRegistration();
            if (registration == nullptr || registration->cpp_type != lux::cxx::typeToken<Type>())
            {
                return lux::cxx::unexpected(SceneSystemBuildFailure{
                    ESceneSystemBuildError::INVALID_DESCRIPTION,
                    instance
                });
            }
            try
            {
                auto object = std::make_unique<Type>(std::forward<Args>(args)...);
                object::LuxObject* endpoint = registration->project_object != nullptr
                    ? registration->project_object(object.get())
                    : nullptr;
                if (registration->project_object != nullptr && endpoint == nullptr)
                {
                    return lux::cxx::unexpected(SceneSystemBuildFailure{
                        ESceneSystemBuildError::INVALID_DESCRIPTION,
                        instance
                    });
                }
                auto appended = appendSystem(
                    instance,
                    lux::cxx::typeToken<Type>(),
                    registration->description,
                    object.get(),
                    endpoint,
                    +[](void* value) noexcept { delete static_cast<Type*>(value); }
                );
                if (!appended)
                {
                    return lux::cxx::unexpected(appended.error());
                }
                object.release();
                return static_cast<Type*>(*appended);
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(SceneSystemBuildFailure{
                    ESceneSystemBuildError::ALLOCATION_FAILURE,
                    instance
                });
            }
            catch (...)
            {
                return lux::cxx::unexpected(SceneSystemBuildFailure{
                    ESceneSystemBuildError::CONSTRUCTION_FAILURE,
                    instance
                });
            }
        }

        template <SceneSystem Type>
        [[nodiscard]] Type* findSystem(system::SystemInstanceId instance) noexcept
        {
            return static_cast<Type*>(findErased(instance, lux::cxx::typeToken<Type>()));
        }

        template <class RequirementType>
        [[nodiscard]] RequirementType* require(
            system::SystemInstanceId system,
            std::string_view requirement
        ) noexcept
        {
            return static_cast<RequirementType*>(
                requireErased(system, requirement, lux::cxx::typeToken<RequirementType>())
            );
        }

        template <SceneSystem Type, class Callable>
        [[nodiscard]] lux::cxx::expected<void, SceneSystemBuildFailure> addStablePointTask(
            system::SystemInstanceId instance,
            Callable&& callable
        ) noexcept
        {
            return addHook<Type>(instance, true, std::forward<Callable>(callable));
        }

        template <SceneSystem Type, class Callable>
        [[nodiscard]] lux::cxx::expected<void, SceneSystemBuildFailure> addPresentationTask(
            system::SystemInstanceId instance,
            Callable&& callable
        ) noexcept
        {
            return addHook<Type>(instance, false, std::forward<Callable>(callable));
        }

    private:
        struct Impl;
        explicit SceneBuilder(Impl& impl) noexcept : impl_(&impl)
        {
        }

        template <SceneSystem Type, class Callable>
        [[nodiscard]] lux::cxx::expected<void, SceneSystemBuildFailure> addHook(
            system::SystemInstanceId instance,
            bool stable,
            Callable&& callable
        ) noexcept
        {
            using Function = std::decay_t<Callable>;
            static_assert(std::is_move_constructible_v<Function>);
            static_assert(std::is_nothrow_invocable_v<const Function&, Type&>);
            using Result = std::invoke_result_t<const Function&, Type&>;
            static_assert(std::same_as<Result, void> || std::same_as<Result, bool>);
            auto* system = findInstalledErased(instance, lux::cxx::typeToken<Type>());
            if (system == nullptr)
            {
                return lux::cxx::unexpected(SceneSystemBuildFailure{
                    ESceneSystemBuildError::INVALID_DESCRIPTION,
                    instance
                });
            }
            try
            {
                lux::cxx::move_only_function<bool()> invoke(
                    [object = static_cast<Type*>(system), function = Function(std::forward<Callable>(callable))]() noexcept {
                        if constexpr (std::same_as<Result, bool>)
                        {
                            return function(*object);
                        }
                        function(*object);
                        return true;
                    }
                );
                return addHookErased(instance, stable, std::move(invoke));
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(SceneSystemBuildFailure{
                    ESceneSystemBuildError::ALLOCATION_FAILURE,
                    instance
                });
            }
        }

        [[nodiscard]] const SceneSystemRegistration* currentRegistration() const noexcept;
        [[nodiscard]] lux::cxx::expected<void*, SceneSystemBuildFailure> appendSystem(
            system::SystemInstanceId instance,
            lux::cxx::TypeToken type,
            const system::SystemTypeDescription* description,
            void* object,
            object::LuxObject* endpoint,
            void (*destroy)(void*) noexcept
        ) noexcept;
        [[nodiscard]] void* findErased(system::SystemInstanceId instance, lux::cxx::TypeToken type) noexcept;
        [[nodiscard]] void* findInstalledErased(system::SystemInstanceId instance, lux::cxx::TypeToken type) noexcept;
        [[nodiscard]] void* requireErased(
            system::SystemInstanceId system,
            std::string_view requirement,
            lux::cxx::TypeToken type
        ) noexcept;
        [[nodiscard]] lux::cxx::expected<void, SceneSystemBuildFailure> addHookErased(
            system::SystemInstanceId instance,
            bool stable,
            lux::cxx::move_only_function<bool()> invoke
        ) noexcept;

        Impl* impl_{};
        friend class Scene;
    };
} // namespace lux::scene
