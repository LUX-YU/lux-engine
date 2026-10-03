#include <lux/engine/editor/scene/SceneConfigurationDraft.hpp>
#include <algorithm>

namespace lux::editor::scene
{
    SceneEditResult<SceneConfigurationDraft> captureSceneConfiguration(
        const SceneConfiguration& value, sessions::ContentStamp based_on, system::SystemInstanceId viewport
    )
    {
        const bool is_incomplete = !value.scene || !value.world || !value.simulation;
        if (is_incomplete)
            return cxx::unexpected(SceneEditError{ESceneEditError::INVALID_SOURCE});
        SceneConfigurationDraft draft;
        draft.base = value;
        draft.based_on = based_on;
        const auto& name = value.scene->info().display_name;
        draft.name.assign(name.begin(), std::ranges::find(name, '\0'));
        const auto& world = value.world->data();
        draft.partition = world.partitioner().id.name;
        draft.partition_version = world.partitioner().version;
        draft.schemas.assign(world.schemas().begin(), world.schemas().end());
        const auto add = [&](auto source, EConfigurationSystemDomain domain) {
            SceneSystemConfigurationDraft item{
                domain, source.instanceId(), std::string(source.instanceName()), source.type(), source.version(),
                std::string(source.configurationSchemaName()), source.configurationSchemaVersion(),
                {source.configurationPayload().begin(), source.configurationPayload().end()}, {}
            };
            if constexpr (requires { source.requirementBindingCount(); })
                for (std::size_t index{}; index < source.requirementBindingCount(); ++index)
                {
                    const auto binding = source.requirementBindingAt(index);
                    item.providers.push_back({std::string(binding.requirement()), std::string(binding.provider())});
                }
            draft.systems.push_back(std::move(item));
        };
        const auto& simulation = value.simulation->data();
        const auto& scene = value.scene->data();
        for (std::size_t index{}; index < simulation.systemCount(); ++index)
            add(simulation.systemAt(index), EConfigurationSystemDomain::SIMULATION);
        for (std::size_t index{}; index < scene.systemCount(); ++index)
            add(scene.systemAt(index), EConfigurationSystemDomain::SCENE);
        for (std::size_t index{}; index < simulation.constructionDependencyCount(); ++index)
        {
            const auto edge = simulation.constructionDependencyAt(index);
            draft.construction.emplace_back(edge.before().instanceId(), edge.after().instanceId());
        }
        for (std::size_t index{}; index < scene.dependencyCount(); ++index)
        {
            const auto edge = scene.dependencyAt(index);
            draft.scene_dependencies.emplace_back(edge.before(), edge.after());
        }
        draft.execution.assign(simulation.executionDependencies().begin(), simulation.executionDependencies().end());
        draft.producers.assign(simulation.channelProducers().begin(), simulation.channelProducers().end());
        draft.viewport = viewport;
        return draft;
    }
}
