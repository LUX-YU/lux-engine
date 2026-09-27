#include <lux/engine/scene/SceneSystemInstaller.hpp>
#include <lux/engine/scene/detail/SceneSystemInstallerImpl.hpp>

#include <algorithm>
#include <utility>

namespace lux::scene
{
    namespace
    {
        [[nodiscard]] SceneSystemBuildFailure failure(
            ESceneSystemBuildError code,
            system::SystemInstanceId system = {},
            system::SystemInstanceId related = {}
        ) noexcept
        {
            return SceneSystemBuildFailure{code, system, related};
        }
    } // namespace

    simulation::ecs::Registry& SceneSystemInstaller::registry() noexcept
    {
        return *impl_->registry;
    }

    SceneInstanceId SceneSystemInstaller::sceneInstanceId() const noexcept
    {
        return impl_->instance;
    }

    simulation::Simulation& SceneSystemInstaller::simulation() noexcept
    {
        return *impl_->simulation;
    }

    const simulation::ecs::ComponentSchemaSet& SceneSystemInstaller::components() const noexcept
    {
        return *impl_->components;
    }

    const SceneSystemRegistration* SceneSystemInstaller::currentRegistration() const noexcept
    {
        return impl_->current_registration;
    }

    lux::cxx::expected<void*, SceneSystemBuildFailure> SceneSystemInstaller::appendSystem(
        system::SystemInstanceId instance,
        lux::cxx::TypeToken type,
        const system::SystemTypeDescription* description,
        void* object,
        void (*destroy)(void*) noexcept
    ) noexcept
    {
        const auto current = impl_->description->systemAt(impl_->current_ordinal);
        const bool is_invalid_system =
            !current || current.instanceId() != instance || impl_->current_registration == nullptr ||
            impl_->current_registration->cpp_type != type || description != impl_->current_registration->description ||
            object == nullptr || destroy == nullptr;
        if (is_invalid_system)
        {
            return lux::cxx::unexpected(failure(ESceneSystemBuildError::INVALID_DESCRIPTION, instance));
        }
        const bool duplicate = std::ranges::any_of(*impl_->systems, [instance](const auto& record) noexcept {
            return record.instance == instance;
        });
        if (duplicate)
        {
            return lux::cxx::unexpected(failure(ESceneSystemBuildError::DUPLICATE_SYSTEM, instance));
        }
        impl_->systems->push_back(
            {instance, type, description, object, destroy, impl_->current_registration->projections}
        );
        return object;
    }

    void* SceneSystemInstaller::findInstalledErased(
        system::SystemInstanceId instance,
        lux::cxx::TypeToken type
    ) noexcept
    {
        const auto found = std::find_if(impl_->systems->begin(), impl_->systems->end(), [instance](const auto& record) {
            return record.instance == instance;
        });
        return found != impl_->systems->end() ? found->project(type) : nullptr;
    }

    void* SceneSystemInstaller::findDependencyErased(lux::cxx::TypeToken type) noexcept
    {
        void* selected{};
        for (const auto ordinal : impl_->predecessors[impl_->current_ordinal])
        {
            const auto instance = impl_->description->systemAt(ordinal).instanceId();
            auto* candidate = findInstalledErased(instance, type);
            if (!candidate)
            {
                continue;
            }
            if (selected)
            {
                impl_->pending_failure = failure(
                    ESceneSystemBuildError::AMBIGUOUS_REQUIREMENT,
                    impl_->description->systemAt(impl_->current_ordinal).instanceId(),
                    instance
                );
                return nullptr;
            }
            selected = candidate;
        }
        return selected;
    }

    void* SceneSystemInstaller::findErased(system::SystemInstanceId instance, lux::cxx::TypeToken type) noexcept
    {
        const auto current = impl_->description->systemAt(impl_->current_ordinal);
        const auto requested = impl_->description->findSystem(instance);
        if (!current || !requested)
        {
            impl_->pending_failure = failure(
                ESceneSystemBuildError::INVALID_DESCRIPTION,
                current ? current.instanceId() : system::SystemInstanceId{},
                instance
            );
            return nullptr;
        }
        const bool is_self = current.instanceId() == instance;
        const bool is_predecessor =
            std::ranges::any_of(impl_->predecessors[impl_->current_ordinal], [&](std::size_t ordinal) noexcept {
                return impl_->description->systemAt(ordinal).instanceId() == instance;
            });
        if (!is_self && !is_predecessor)
        {
            impl_->pending_failure =
                failure(ESceneSystemBuildError::UNDECLARED_CONSTRUCTOR_DEPENDENCY, current.instanceId(), instance);
            return nullptr;
        }
        return findInstalledErased(instance, type);
    }

    void* SceneSystemInstaller::requireErased(
        system::SystemInstanceId system,
        std::string_view requirement,
        lux::cxx::TypeToken type
    ) noexcept
    {
        const auto current = impl_->description->systemAt(impl_->current_ordinal);
        if (!current || current.instanceId() != system || impl_->current_registration == nullptr)
        {
            impl_->pending_failure = failure(ESceneSystemBuildError::INVALID_DESCRIPTION, system);
            return nullptr;
        }
        const auto declared = std::find_if(
            impl_->current_registration->requirements.begin(),
            impl_->current_registration->requirements.end(),
            [requirement](const auto& value) noexcept { return value.name == requirement; }
        );
        if (declared == impl_->current_registration->requirements.end() || declared->expected_type != type)
        {
            impl_->pending_failure = failure(ESceneSystemBuildError::REQUIREMENT_TYPE_MISMATCH, system);
            return nullptr;
        }
        const auto resolved =
            std::find_if(impl_->requirements.begin(), impl_->requirements.end(), [&](const auto& value) noexcept {
                return value.system == system && value.name == requirement;
            });
        if (resolved == impl_->requirements.end())
        {
            if (!declared->optional)
            {
                impl_->pending_failure = failure(ESceneSystemBuildError::MISSING_REQUIREMENT, system);
            }
            return nullptr;
        }
        if (resolved->type != type)
        {
            impl_->pending_failure = failure(ESceneSystemBuildError::REQUIREMENT_TYPE_MISMATCH, system);
            return nullptr;
        }
        return resolved->value;
    }

    lux::cxx::expected<void, SceneSystemBuildFailure> SceneSystemInstaller::addHookErased(
        system::SystemInstanceId instance,
        ESceneSystemPhase phase,
        lux::cxx::move_only_function<SceneStageResult(SceneStageContext&)> invoke
    ) noexcept
    {
        auto& hooks = phase == ESceneSystemPhase::SYNCHRONIZATION ? *impl_->synchronization_hooks
                      : phase == ESceneSystemPhase::MAINTENANCE   ? *impl_->maintenance_hooks
                      : phase == ESceneSystemPhase::PUBLICATION   ? *impl_->publication_hooks
                                                                  : *impl_->stable_hooks;
        if (std::ranges::any_of(hooks, [instance](const auto& hook) noexcept { return hook.system == instance; }))
        {
            return lux::cxx::unexpected(failure(ESceneSystemBuildError::DUPLICATE_STABLE_POINT_TASK, instance));
        }
        hooks.push_back({instance, std::move(invoke)});
        return {};
    }
} // namespace lux::scene
