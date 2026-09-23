#pragma once
#include <lux/engine/scene/SceneInstanceId.hpp>

#include <lux/engine/scene/SceneSystem.hpp>
#include <lux/engine/scene/SceneSystemRegistration.hpp>
#include <lux/engine/scene/visibility.h>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>

#include <lux/cxx/core/move_only_function.hpp>

#include <concepts>
#include <memory>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>

namespace lux::scene
{
class SceneInstance;

class LUX_ENGINE_SCENE_PUBLIC SceneBuilder final
{
  public:
    [[nodiscard]] simulation::ecs::Registry &registry() noexcept;
    [[nodiscard]] SceneInstanceId sceneInstanceId() const noexcept;
    [[nodiscard]] simulation::Simulation &simulation() noexcept;
    [[nodiscard]] const simulation::ecs::ComponentSchemaSet &components() const noexcept;

    template <class Configuration>
    [[nodiscard]] lux::cxx::expected<Configuration, SceneSystemBuildFailure> decodeConfiguration(
        SceneSystemDescription description) noexcept
    {
        static_assert(std::is_nothrow_default_constructible_v<Configuration>);
        static_assert(std::is_nothrow_destructible_v<Configuration>);
        const auto *registration = currentRegistration();
        const bool invalid_registration = registration == nullptr || registration->description == nullptr ||
                                          !registration->configuration.valid() ||
                                          registration->configuration.type != lux::cxx::typeToken<Configuration>();
        if (invalid_registration)
        {
            return lux::cxx::unexpected(
                SceneSystemBuildFailure{ESceneSystemBuildError::INVALID_DESCRIPTION, description.instanceId()});
        }
        const auto &type = *registration->description;
        const bool invalid_description =
            !description || type.configuration_schema_name.empty() ||
            description.configurationSchemaName() != type.configuration_schema_name ||
            description.configurationSchemaHash() != lux::cxx::Fnv1a64::hash(type.configuration_schema_name) ||
            description.configurationSchemaVersion() != type.configuration_schema_version;
        if (invalid_description)
        {
            return lux::cxx::unexpected(
                SceneSystemBuildFailure{ESceneSystemBuildError::INVALID_DESCRIPTION, description.instanceId()});
        }
        Configuration value{};
        auto decoded = registration->configuration.decode(description.configurationPayload(), &value);
        if (!decoded)
        {
            return lux::cxx::unexpected(SceneSystemBuildFailure{ESceneSystemBuildError::CONFIGURATION_DECODE_FAILURE,
                                                                description.instanceId(),
                                                                {},
                                                                0U,
                                                                decoded.error()});
        }
        return value;
    }

    template <SceneSystem Type, class... Args>
    [[nodiscard]] lux::cxx::expected<Type *, SceneSystemBuildFailure> emplaceSystem(system::SystemInstanceId instance,
                                                                                    Args &&...args) noexcept
    {
        const auto *registration = currentRegistration();
        if (registration == nullptr || registration->cpp_type != lux::cxx::typeToken<Type>())
        {
            return lux::cxx::unexpected(SceneSystemBuildFailure{ESceneSystemBuildError::INVALID_DESCRIPTION, instance});
        }
        try
        {
            std::unique_ptr<Type> object{new Type(std::forward<Args>(args)...)};
            object::LuxObject *endpoint =
                registration->project_object != nullptr ? registration->project_object(object.get()) : nullptr;
            if (registration->project_object != nullptr && endpoint == nullptr)
            {
                return lux::cxx::unexpected(
                    SceneSystemBuildFailure{ESceneSystemBuildError::INVALID_DESCRIPTION, instance});
            }
            auto appended = appendSystem(
                instance, lux::cxx::typeToken<Type>(), registration->description, object.get(), endpoint,
                +[](void *value) noexcept { delete static_cast<Type *>(value); });
            if (!appended)
            {
                return lux::cxx::unexpected(appended.error());
            }
            object.release();
            return static_cast<Type *>(*appended);
        }
        catch (const std::bad_alloc &)
        {
            return lux::cxx::unexpected(SceneSystemBuildFailure{ESceneSystemBuildError::ALLOCATION_FAILURE, instance});
        }
        catch (...)
        {
            return lux::cxx::unexpected(
                SceneSystemBuildFailure{ESceneSystemBuildError::CONSTRUCTION_FAILURE, instance});
        }
    }

    template <SceneSystem Type> [[nodiscard]] Type *findSystem(system::SystemInstanceId instance) noexcept
    {
        return static_cast<Type *>(findErased(instance, lux::cxx::typeToken<Type>()));
    }

    // Only explicitly declared constructor predecessors are visible here.
    template <SceneSystem Type> [[nodiscard]] Type *findDependency() noexcept
    {
        return static_cast<Type *>(findDependencyErased(lux::cxx::typeToken<Type>()));
    }

    template <class RequirementType>
    [[nodiscard]] RequirementType *require(system::SystemInstanceId system, std::string_view requirement) noexcept
    {
        return static_cast<RequirementType *>(
            requireErased(system, requirement, lux::cxx::typeToken<RequirementType>()));
    }

    template <SceneSystem Type, class Callable>
    [[nodiscard]] lux::cxx::expected<void, SceneSystemBuildFailure> addStablePointTask(
        system::SystemInstanceId instance, Callable &&callable) noexcept
    {
        return addHook<Type>(instance, ESceneSystemPhase::STABLE, std::forward<Callable>(callable));
    }

    template <SceneSystem Type, class Callable>
    [[nodiscard]] lux::cxx::expected<void, SceneSystemBuildFailure> addMaintenanceTask(
        system::SystemInstanceId instance, Callable &&callable) noexcept
    {
        return addHook<Type>(instance, ESceneSystemPhase::MAINTENANCE, std::forward<Callable>(callable));
    }

    template <SceneSystem Type, class Callable>
    [[nodiscard]] lux::cxx::expected<void, SceneSystemBuildFailure> addPublicationTask(
        system::SystemInstanceId instance, Callable &&callable) noexcept
    {
        return addHook<Type>(instance, ESceneSystemPhase::PUBLICATION, std::forward<Callable>(callable));
    }

  private:
    struct Impl;
    explicit SceneBuilder(Impl &impl) noexcept : impl_(&impl)
    {
    }

    template <SceneSystem Type, class Callable>
    [[nodiscard]] lux::cxx::expected<void, SceneSystemBuildFailure> addHook(system::SystemInstanceId instance,
                                                                            ESceneSystemPhase phase,
                                                                            Callable &&callable) noexcept
    {
        using Function = std::decay_t<Callable>;
        static_assert(std::is_move_constructible_v<Function>);
        static_assert(std::is_nothrow_invocable_r_v<SceneStageResult, const Function &, Type &> ||
                      std::is_nothrow_invocable_r_v<SceneStageResult, const Function &, Type &, SceneStageContext &>);
        auto *system = findInstalledErased(instance, lux::cxx::typeToken<Type>());
        if (system == nullptr)
        {
            return lux::cxx::unexpected(SceneSystemBuildFailure{ESceneSystemBuildError::INVALID_DESCRIPTION, instance});
        }
        try
        {
            lux::cxx::move_only_function<SceneStageResult(SceneStageContext &)> invoke(
                [object = static_cast<Type *>(system), instance, function = Function(std::forward<Callable>(callable))](
                    SceneStageContext &context) noexcept -> SceneStageResult {
                    auto result = [&]() -> SceneStageResult {
                        if constexpr (std::is_nothrow_invocable_r_v<SceneStageResult, const Function &, Type &,
                                                                    SceneStageContext &>)
                        {
                            return function(*object, context);
                        }
                        else
                        {
                            return function(*object);
                        }
                    }();
                    if (!result && result.error().system.value == 0)
                    {
                        result.error().system = instance;
                    }
                    return result;
                });
            return addHookErased(instance, phase, std::move(invoke));
        }
        catch (const std::bad_alloc &)
        {
            return lux::cxx::unexpected(SceneSystemBuildFailure{ESceneSystemBuildError::ALLOCATION_FAILURE, instance});
        }
    }

    [[nodiscard]] const SceneSystemRegistration *currentRegistration() const noexcept;
    [[nodiscard]] lux::cxx::expected<void *, SceneSystemBuildFailure> appendSystem(
        system::SystemInstanceId instance, lux::cxx::TypeToken type, const system::SystemTypeDescription *description,
        void *object, object::LuxObject *endpoint, void (*destroy)(void *) noexcept) noexcept;
    [[nodiscard]] void *findErased(system::SystemInstanceId instance, lux::cxx::TypeToken type) noexcept;
    [[nodiscard]] void *findDependencyErased(lux::cxx::TypeToken) noexcept;
    [[nodiscard]] void *findInstalledErased(system::SystemInstanceId instance, lux::cxx::TypeToken type) noexcept;
    [[nodiscard]] void *requireErased(system::SystemInstanceId system, std::string_view requirement,
                                      lux::cxx::TypeToken type) noexcept;
    [[nodiscard]] lux::cxx::expected<void, SceneSystemBuildFailure> addHookErased(
        system::SystemInstanceId instance, ESceneSystemPhase phase,
        lux::cxx::move_only_function<SceneStageResult(SceneStageContext &)> invoke) noexcept;

    Impl *impl_{};
    friend class SceneInstance;
};
} // namespace lux::scene
