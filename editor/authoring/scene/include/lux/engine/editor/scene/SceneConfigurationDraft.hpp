#pragma once

#include <lux/engine/editor/scene/SceneSource.hpp>
#include <lux/engine/simulation/SimulationExecutionSpec.hpp>
#include <optional>

namespace lux::editor::scene
{
    enum class ESceneContentPreset : std::uint8_t { EMPTY, TWO_DIMENSIONAL, THREE_DIMENSIONAL };
    enum class EConfigurationSystemDomain : std::uint8_t { SIMULATION, SCENE };

    struct SceneProviderBinding final
    {
        std::string requirement;
        std::string provider;
        friend bool operator==(const SceneProviderBinding&, const SceneProviderBinding&) = default;
    };

    struct SceneSystemConfigurationDraft final
    {
        EConfigurationSystemDomain domain{};
        system::SystemInstanceId id;
        std::string name;
        system::SystemTypeId type;
        std::uint32_t version{};
        std::string configuration_schema;
        std::uint32_t configuration_version{};
        std::vector<std::byte> configuration;
        std::vector<SceneProviderBinding> providers;
        friend bool operator==(const SceneSystemConfigurationDraft&, const SceneSystemConfigurationDraft&) = default;
    };

    // Encoded values own their bytes. The optional baseline preserves unknown contracts/data;
    // the origin stamp belongs to this draft, never to a later display refresh.
    struct SceneConfigurationDraft final
    {
        sessions::ContentStamp based_on;
        std::optional<SceneConfiguration> base;
        std::string name;
        std::string partition;
        std::uint32_t partition_version{};
        std::vector<world::WorldDataSchemaId> schemas;
        std::vector<SceneSystemConfigurationDraft> systems;
        std::vector<std::pair<system::SystemInstanceId, system::SystemInstanceId>> construction;
        std::vector<std::pair<system::SystemInstanceId, system::SystemInstanceId>> scene_dependencies;
        std::vector<simulation::SimulationExecutionDependency> execution;
        std::vector<simulation::SimulationChannelProducer> producers;
        system::SystemInstanceId viewport;
    };

    struct SceneCreationConfiguration final
    {
        std::string name;
        std::vector<world::WorldDataSchemaId> schemas;
        std::shared_ptr<const simulation::SimulationDescription> simulation;
        lux::scene::SceneDescription scene;
        system::SystemInstanceId viewport;
    };

    [[nodiscard]] SceneEditResult<SceneConfigurationDraft> captureSceneConfiguration(
        const SceneConfiguration&, sessions::ContentStamp = {}, system::SystemInstanceId viewport = {}
    );
}
