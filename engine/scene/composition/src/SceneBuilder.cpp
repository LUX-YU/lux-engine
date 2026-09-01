#include <lux/engine/scene/SceneBuilder.hpp>
#include <lux/engine/scene/detail/SceneBuilderImpl.hpp>

#include <algorithm>
#include <new>
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
    }

    simulation::ecs::Registry& SceneBuilder::registry() noexcept
    {
        return *impl_->registry;
    }

    simulation::Simulation& SceneBuilder::simulation() noexcept
    {
        return *impl_->simulation;
    }

    const SceneMetaManager& SceneBuilder::meta() const noexcept
    {
        return *impl_->meta;
    }

    const SceneSystemRegistration* SceneBuilder::currentRegistration() const noexcept
    {
        return impl_->current_registration;
    }

    lux::cxx::expected<void*, SceneSystemBuildFailure> SceneBuilder::appendSystem(
        system::SystemInstanceId instance,
        lux::cxx::TypeToken type,
        const system::SystemTypeDescription* description,
        void* object,
        object::LuxObject* endpoint,
        void (*destroy)(void*) noexcept
    ) noexcept
    {
        const auto current = impl_->description->systemAt(impl_->current_ordinal);
        const bool invalid = !current || current.instanceId() != instance || impl_->current_registration == nullptr ||
            impl_->current_registration->cpp_type != type || description != impl_->current_registration->description ||
            object == nullptr || destroy == nullptr;
        if (invalid)
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
        try
        {
            impl_->systems->push_back({instance, type, description, object, endpoint, destroy});
            return object;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(ESceneSystemBuildError::ALLOCATION_FAILURE, instance));
        }
    }

    void* SceneBuilder::findInstalledErased(system::SystemInstanceId instance, lux::cxx::TypeToken type) noexcept
    {
        const auto found = std::find_if(impl_->systems->begin(), impl_->systems->end(), [instance](const auto& record) {
            return record.instance == instance;
        });
        return found != impl_->systems->end() && found->type == type ? found->object : nullptr;
    }

    void* SceneBuilder::findErased(system::SystemInstanceId instance, lux::cxx::TypeToken type) noexcept
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
        const bool is_predecessor = std::ranges::any_of(
            impl_->predecessors[impl_->current_ordinal],
            [&](std::size_t ordinal) noexcept { return impl_->description->systemAt(ordinal).instanceId() == instance; }
        );
        if (!is_self && !is_predecessor)
        {
            impl_->pending_failure = failure(
                ESceneSystemBuildError::UNDECLARED_CONSTRUCTOR_DEPENDENCY,
                current.instanceId(),
                instance
            );
            return nullptr;
        }
        return findInstalledErased(instance, type);
    }

    void* SceneBuilder::requireErased(
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
        const auto resolved = std::find_if(
            impl_->requirements.begin(),
            impl_->requirements.end(),
            [&](const auto& value) noexcept { return value.system == system && value.name == requirement; }
        );
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

    lux::cxx::expected<void, SceneSystemBuildFailure> SceneBuilder::addHookErased(
        system::SystemInstanceId instance,
        bool stable,
        lux::cxx::move_only_function<bool()> invoke
    ) noexcept
    {
        auto& hooks = stable ? *impl_->stable_hooks : *impl_->presentation_hooks;
        if (std::ranges::any_of(hooks, [instance](const auto& hook) noexcept { return hook.system == instance; }))
        {
            return lux::cxx::unexpected(failure(
                stable ? ESceneSystemBuildError::DUPLICATE_STABLE_POINT_TASK
                       : ESceneSystemBuildError::DUPLICATE_PRESENTATION_TASK,
                instance
            ));
        }
        try
        {
            hooks.push_back({instance, std::move(invoke)});
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(ESceneSystemBuildError::ALLOCATION_FAILURE, instance));
        }
    }
} // namespace lux::scene
