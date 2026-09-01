#pragma once

#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/scene/SceneSystemRegistration.hpp>
#include <lux/engine/scene/RenderFeatureMeta.hpp>
#include <lux/engine/scene/meta/visibility.h>
#include <lux/engine/simulation/SimulationSystemRegistry.hpp>
#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace lux::scene
{
    enum class ESceneMetaError : std::uint8_t
    {
        INVALID_REFLECTION_REGISTRY,
        INVALID_COMPONENT_SCHEMA,
        UNKNOWN_COMPONENT_SCHEMA,
        INVALID_SIMULATION_SYSTEM,
        INVALID_SCENE_SYSTEM,
        DUPLICATE_SYSTEM_TYPE,
        SYSTEM_TYPE_COLLISION,
        CONFIGURATION_REFLECTION_NOT_FOUND,
        CONFIGURATION_DEFAULT_ENCODING_FAILURE,
        INVALID_REQUIREMENT,
        INVALID_CONNECTION,
        INVALID_RENDER_FEATURE,
        DUPLICATE_RENDER_FEATURE,
        RENDER_FEATURE_TYPE_COLLISION,
        INVALID_RENDER_FEATURE_BINDING,
        DUPLICATE_RENDER_FEATURE_BINDING,
        UNKNOWN_RENDER_FEATURE_BINDING,
        UNKNOWN_SCENE_SYSTEM_BINDING,
        ALLOCATION_FAILURE,
    };

    struct SceneMetaFailure final
    {
        ESceneMetaError code{ESceneMetaError::ALLOCATION_FAILURE};
        std::uint64_t subject_hash{};
        lux::serialization::SerializationFailure configuration{};
    };

    enum class ESystemDomain : std::uint8_t
    {
        SIMULATION,
        SCENE,
    };

    struct SystemMetaView final
    {
        ESystemDomain domain{};
        system::SystemTypeId type;
        lux::cxx::TypeToken cpp_type;
        const system::SystemTypeDescription* description{};
        lux::serialization::PortableValueCodec configuration{};
        const meta::RefClass* configuration_reflection{};
        std::span<const std::byte> default_configuration{};
    };

    struct ComponentSystemUsage final
    {
        system::SystemTypeId system;
        ESystemDomain domain{};
        std::optional<simulation::ESystemAccessMode> simulation_access{};
        std::uint8_t scene_observations{};
        render::FeatureTypeId via_render_feature{render::kInvalidFeatureTypeId};
    };

    class LUX_ENGINE_SCENE_META_PUBLIC SceneMetaManager final
    {
    public:
        struct BuildInfo final
        {
            simulation::ecs::ComponentSchemaSet components;
            simulation::SimulationSystemRegistry simulation_systems;
            std::vector<SceneSystemRegistration> scene_systems;
            std::vector<render::RenderFeatureRegistration> render_features;
            std::vector<RenderFeatureSceneBinding> render_scene_bindings;
        };

        [[nodiscard]] static lux::cxx::expected<SceneMetaManager, SceneMetaFailure>
        build(BuildInfo info) noexcept;

        SceneMetaManager(SceneMetaManager&&) noexcept;
        SceneMetaManager& operator=(SceneMetaManager&&) noexcept;
        ~SceneMetaManager();
        SceneMetaManager(const SceneMetaManager&) = delete;
        SceneMetaManager& operator=(const SceneMetaManager&) = delete;

        [[nodiscard]] const simulation::ecs::ComponentSchemaSet& components() const noexcept;
        [[nodiscard]] const simulation::SimulationSystemRegistry& simulationSystems() const noexcept;
        [[nodiscard]] const simulation::ecs::ComponentSchema* getComponentMeta(
            const simulation::ecs::ComponentSchemaId& id
        ) const noexcept;
        [[nodiscard]] const simulation::ecs::ComponentSchema* getComponentMeta(lux::cxx::TypeToken type) const noexcept;
        [[nodiscard]] std::optional<SystemMetaView> getSystemMeta(const system::SystemTypeId& type) const noexcept;
        [[nodiscard]] std::optional<SystemMetaView> getSystemMeta(std::string_view canonical_name) const noexcept;
        [[nodiscard]] const simulation::SimulationSystemRegistration* getSimulationSystemMeta(
            const system::SystemTypeId& type
        ) const noexcept;
        [[nodiscard]] const SceneSystemRegistration* getSceneSystemMeta(const system::SystemTypeId& type) const noexcept;
        [[nodiscard]] const RenderFeatureMeta* getRenderFeatureMeta(render::FeatureTypeId type) const noexcept;
        [[nodiscard]] const RenderFeatureMeta* getRenderFeatureMeta(std::string_view stable_name) const noexcept;
        [[nodiscard]] std::span<const ComponentSystemUsage> systemsUsingComponent(
            const simulation::ecs::ComponentSchemaId& component
        ) const noexcept;
        [[nodiscard]] std::span<const system::SystemTypeId> systemsProvidingCapability(
            std::string_view capability
        ) const noexcept;
        [[nodiscard]] std::span<const SystemMetaView> allSystems() const noexcept;
        [[nodiscard]] std::span<const SceneSystemRegistration> sceneSystems() const noexcept;
        [[nodiscard]] std::span<const RenderFeatureMeta> allRenderFeatures() const noexcept;

    private:
        struct Impl;
        explicit SceneMetaManager(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::scene
