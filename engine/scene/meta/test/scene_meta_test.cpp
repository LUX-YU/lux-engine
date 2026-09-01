#include <lux/engine/function/render/client/core/RenderFeatureMetaModule.hpp>
#include <lux/engine/scene/SceneMetaManager.hpp>
#include <lux/engine/simulation/SimulationSystemRegistry.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/engine/simulation/ecs/VisualSchema.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    struct UnknownComponent final
    {
    };

    struct SimulationProbe final
    {
        inline static constexpr auto Access = lux::simulation::makeSystemAccessSpec<
            lux::simulation::ComponentRead<lux::simulation::ecs::Mesh3D>>();
        inline static constexpr lux::simulation::SimulationSystemDescription Description{
            .type = {.canonical_name = "lux.simulation.meta.probe", .version = 1U}
        };
    };

    struct UnknownSimulationProbe final
    {
        inline static constexpr auto Access =
            lux::simulation::makeSystemAccessSpec<lux::simulation::ComponentRead<UnknownComponent>>();
        inline static constexpr lux::simulation::SimulationSystemDescription Description{
            .type = {.canonical_name = "lux.simulation.meta.unknown", .version = 1U}
        };
    };

    struct SceneProbe final
    {
        inline static constexpr std::array Capabilities{std::string_view{"lux.scene.probe"}};
        inline static constexpr lux::system::SystemTypeDescription Description{
            .canonical_name = "lux.scene.probe",
            .version = 1U,
            .capabilities = Capabilities
        };
    };

    lux::cxx::expected<void, lux::simulation::SimulationSystemBuildFailure> installSimulationProbe(
        lux::simulation::SimulationBuilder&,
        lux::simulation::SimulationSystemView
    ) noexcept
    {
        return {};
    }

    lux::cxx::expected<void, lux::scene::SceneSystemBuildFailure> installSceneProbe(
        lux::scene::SceneBuilder&,
        lux::scene::SceneSystemView
    ) noexcept
    {
        return {};
    }

    template <class System>
    [[nodiscard]] lux::simulation::SimulationSystemRegistry simulationRegistry()
    {
        lux::simulation::SimulationSystemRegistry registry;
        const lux::simulation::SimulationSystemRegistration registration{
            .type = lux::system::systemTypeId(System::Description.type.canonical_name),
            .cpp_type = lux::cxx::typeToken<System>(),
            .description = &System::Description,
            .access = System::Access.spec(),
            .configuration = lux::serialization::noPortableValueCodec(),
            .install = &installSimulationProbe
        };
        assert(registry.add(registration));
        return registry;
    }

    [[nodiscard]] lux::scene::SceneSystemRegistration sceneRegistration(
        std::span<const lux::scene::ComponentObservationSpec> observations = {}
    ) noexcept
    {
        return lux::scene::SceneSystemRegistration{
            .type = lux::system::systemTypeId(SceneProbe::Description.canonical_name),
            .cpp_type = lux::cxx::typeToken<SceneProbe>(),
            .description = &SceneProbe::Description,
            .configuration = lux::serialization::noPortableValueCodec(),
            .observations = observations,
            .install = &installSceneProbe
        };
    }

    [[nodiscard]] lux::simulation::ecs::ComponentSchemaSet components(bool include_visual)
    {
        std::vector<lux::simulation::ecs::ComponentSchema> schemas;
        if (include_visual)
        {
            const auto visual = lux::simulation::ecs::visualComponentSchemas();
            schemas.assign(visual.begin(), visual.end());
        }
        auto result = lux::simulation::ecs::ComponentSchemaSet::build(std::move(schemas));
        assert(result);
        return std::move(*result);
    }

    [[nodiscard]] lux::render::RenderFeatureRegistration renderFeature()
    {
        const auto registrations = lux::render::builtinRenderFeatureRegistrations();
        const auto found = std::ranges::find_if(registrations, [](const auto& value) noexcept {
            return value.stable_name == "lux.render.tonemap.v1";
        });
        assert(found != registrations.end());
        return *found;
    }

    [[nodiscard]] lux::scene::RenderFeatureSceneBinding renderBinding(
        std::span<const lux::scene::ComponentObservationSpec> observations = {}
    ) noexcept
    {
        return lux::scene::RenderFeatureSceneBinding{
            lux::system::systemTypeId(SceneProbe::Description.canonical_name),
            lux::render::featureId("lux.render.tonemap.v1"),
            observations,
            nullptr
        };
    }
}

int main()
{
    using namespace lux;

    render::initializeBuiltinRenderFeatureMeta();
    meta::ReflectionRegistry::initRegistry();

    constexpr std::uint8_t ObserveAll =
        static_cast<std::uint8_t>(scene::EComponentObservation::CONSTRUCT) |
        static_cast<std::uint8_t>(scene::EComponentObservation::UPDATE) |
        static_cast<std::uint8_t>(scene::EComponentObservation::DESTROY);
    const std::array known_observation{
        scene::ComponentObservationSpec{cxx::typeToken<simulation::ecs::Mesh3D>(), ObserveAll}
    };
    const std::array unknown_observation{
        scene::ComponentObservationSpec{cxx::typeToken<UnknownComponent>(), ObserveAll}
    };

    {
        auto unknown_simulation = scene::SceneMetaManager::build({
            components(false),
            simulationRegistry<UnknownSimulationProbe>()
        });
        assert(!unknown_simulation);
        assert(unknown_simulation.error().code == scene::ESceneMetaError::UNKNOWN_COMPONENT_SCHEMA);
        assert(unknown_simulation.error().subject_hash == cxx::typeToken<UnknownComponent>().hash());

        auto unknown_scene = scene::SceneMetaManager::build({
            components(false),
            simulation::SimulationSystemRegistry{},
            {sceneRegistration(unknown_observation)}
        });
        assert(!unknown_scene);
        assert(unknown_scene.error().code == scene::ESceneMetaError::UNKNOWN_COMPONENT_SCHEMA);
        assert(unknown_scene.error().subject_hash == cxx::typeToken<UnknownComponent>().hash());

        auto unknown_render = scene::SceneMetaManager::build({
            components(false),
            simulation::SimulationSystemRegistry{},
            {sceneRegistration()},
            {renderFeature()},
            {renderBinding(unknown_observation)}
        });
        assert(!unknown_render);
        assert(unknown_render.error().code == scene::ESceneMetaError::UNKNOWN_COMPONENT_SCHEMA);
        assert(unknown_render.error().subject_hash == cxx::typeToken<UnknownComponent>().hash());

        auto duplicate_empty_binding = scene::SceneMetaManager::build({
            components(false),
            simulation::SimulationSystemRegistry{},
            {sceneRegistration()},
            {renderFeature()},
            {renderBinding(), renderBinding()}
        });
        assert(!duplicate_empty_binding);
        assert(
            duplicate_empty_binding.error().code == scene::ESceneMetaError::DUPLICATE_RENDER_FEATURE_BINDING
        );

        auto manager = scene::SceneMetaManager::build({
            components(true),
            simulationRegistry<SimulationProbe>(),
            {sceneRegistration(known_observation)},
            {renderFeature()},
            {renderBinding(known_observation)}
        });
        assert(manager);
        const auto* mesh = manager->getComponentMeta(cxx::typeToken<simulation::ecs::Mesh3D>());
        assert(mesh != nullptr);
        const auto usages = manager->systemsUsingComponent(mesh->id);
        assert(usages.size() == 3U);
        assert(std::ranges::any_of(usages, [](const auto& usage) noexcept {
            return usage.domain == scene::ESystemDomain::SIMULATION && usage.simulation_access.has_value();
        }));
        assert(std::ranges::any_of(usages, [](const auto& usage) noexcept {
            return usage.domain == scene::ESystemDomain::SCENE &&
                usage.via_render_feature == render::kInvalidFeatureTypeId;
        }));
        assert(std::ranges::any_of(usages, [](const auto& usage) noexcept {
            return usage.domain == scene::ESystemDomain::SCENE &&
                usage.via_render_feature == render::featureId("lux.render.tonemap.v1");
        }));
        assert(manager->getSystemMeta(SceneProbe::Description.canonical_name));
        assert(manager->systemsProvidingCapability("lux.scene.probe").size() == 1U);
    }

    meta::ReflectionRegistry::destroyRegistry();
    return 0;
}
