#pragma once

#include <lux/engine/scene/SceneBuilder.hpp>

#include <algorithm>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace lux::scene::detail
{
    struct SceneSystemObjectRecord final
    {
        system::SystemInstanceId instance{};
        lux::cxx::TypeToken type{};
        const system::SystemTypeDescription* description{};
        void* object{};
        object::LuxObject* object_endpoint{};
        void (*destroy)(void*) noexcept{};
    };

    struct SceneHookRecord final
    {
        system::SystemInstanceId system{};
        lux::cxx::move_only_function<bool()> invoke;
    };

    struct ResolvedSceneRequirement final
    {
        system::SystemInstanceId system{};
        std::string_view name;
        lux::cxx::TypeToken type{};
        void* value{};
        object::LuxObject* object{};
    };
} // namespace lux::scene::detail

struct lux::scene::SceneBuilder::Impl final
{
    simulation::ecs::Registry* registry{};
    simulation::Simulation* simulation{};
    const SceneMetaManager* meta{};
    std::vector<detail::SceneSystemObjectRecord>* systems{};
    std::vector<detail::SceneHookRecord>* stable_hooks{};
    std::vector<detail::SceneHookRecord>* presentation_hooks{};
    const SceneDescription* description{};
    std::span<const SceneSystemRegistration> registrations;
    std::vector<std::vector<std::size_t>> predecessors;
    std::vector<detail::ResolvedSceneRequirement> requirements;
    const SceneSystemRegistration* current_registration{};
    std::size_t current_ordinal{};
    std::optional<SceneSystemBuildFailure> pending_failure;
};
