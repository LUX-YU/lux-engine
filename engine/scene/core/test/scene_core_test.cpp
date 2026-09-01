#include <lux/engine/scene/Scene.hpp>
#include <lux/engine/scene/SceneBuilder.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace
{
    template <class Type>
    [[nodiscard]] Type worldId(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes[15] = tail;
        return Type{uuids::uuid(bytes)};
    }

    [[nodiscard]] lux::asset::AssetId assetId(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes[15] = tail;
        return lux::asset::AssetId(bytes);
    }

    struct ProbeRuntime final
    {
        int stable{};
        int presentation{};
    };

    struct ProbeSceneSystem final
    {
        inline static constexpr std::array Capabilities{std::string_view{"lux.scene.probe"}};
        inline static constexpr lux::system::SystemTypeDescription Description{
            .canonical_name = "lux.scene.test.probe",
            .version = 1U,
            .capabilities = Capabilities
        };

        explicit ProbeSceneSystem(ProbeRuntime& runtime) noexcept : runtime_(&runtime)
        {
        }
        ~ProbeSceneSystem() noexcept = default;

        [[nodiscard]] bool stable() noexcept
        {
            ++runtime_->stable;
            return true;
        }

        void presentation() noexcept
        {
            ++runtime_->presentation;
        }

        ProbeRuntime* runtime_{};
    };

    constexpr std::array Requirements{
        lux::scene::SceneSystemRequirementSpec{
            "runtime",
            "lux.test.runtime",
            lux::cxx::typeToken<ProbeRuntime>(),
            false
        }
    };

    lux::cxx::expected<void, lux::scene::SceneSystemBuildFailure> installProbe(
        lux::scene::SceneBuilder& builder,
        lux::scene::SceneSystemView description
    ) noexcept
    {
        auto* runtime = builder.require<ProbeRuntime>(description.instanceId(), "runtime");
        if (runtime == nullptr)
        {
            return lux::cxx::unexpected(lux::scene::SceneSystemBuildFailure{
                lux::scene::ESceneSystemBuildError::MISSING_REQUIREMENT,
                description.instanceId()
            });
        }
        auto system = builder.emplaceSystem<ProbeSceneSystem>(description.instanceId(), *runtime);
        if (!system) return lux::cxx::unexpected(system.error());
        auto stable = builder.addStablePointTask<ProbeSceneSystem>(
            description.instanceId(),
            [](ProbeSceneSystem& value) noexcept { return value.stable(); }
        );
        if (!stable) return stable;
        return builder.addPresentationTask<ProbeSceneSystem>(
            description.instanceId(),
            [](ProbeSceneSystem& value) noexcept { value.presentation(); }
        );
    }
}

int main()
{
    using namespace lux;

    static_assert(!std::is_copy_constructible_v<scene::Scene>);
    static_assert(!std::is_move_constructible_v<scene::Scene>);
    static_assert(scene::SceneSystem<ProbeSceneSystem>);

    world::WorldDescriptionBuilder world_builder;
    assert(world_builder.setIdentity(
        worldId<world::WorldBundleId>(1U),
        worldId<world::WorldBundleGeneration>(2U),
        "scene-test"
    ));
    assert(world_builder.setPartitioner({world::worldPartitionerId("test.none"), 1U}, 0U));
    auto world = std::move(world_builder).build();
    assert(world);
    auto world_owner = std::make_shared<world::WorldDescription>(std::move(*world));

    simulation::SimulationDescriptionBuilder simulation_builder;
    auto simulation = std::move(simulation_builder).build();
    assert(simulation);
    auto simulation_owner = std::make_shared<simulation::SimulationDescription>(std::move(*simulation));

    constexpr system::SystemInstanceId ProbeInstance{1U};
    scene::SceneDescriptionBuilder scene_builder;
    scene_builder.setWorld(assetId(1U));
    scene_builder.setSimulation(assetId(2U));
    const auto probe_type = system::systemTypeId(ProbeSceneSystem::Description.canonical_name);
    assert(scene_builder.addSystem(ProbeInstance, "probe", probe_type, 1U, {}, 0U));
    assert(scene_builder.bindRequirement(ProbeInstance, "runtime", "host.runtime"));
    auto description = std::move(scene_builder).build();
    assert(description);
    auto scene_description = std::make_shared<scene::SceneDescription>(std::move(*description));

    meta::ReflectionRegistry::initRegistry();
    scene::SceneSystemRegistration registration{
        .type = probe_type,
        .cpp_type = cxx::typeToken<ProbeSceneSystem>(),
        .description = &ProbeSceneSystem::Description,
        .configuration = serialization::noPortableValueCodec(),
        .requirements = Requirements,
        .project_object = scene::sceneSystemObjectProjection<ProbeSceneSystem>(),
        .install = &installProbe
    };
    auto components = simulation::ecs::ComponentSchemaSet::build({});
    assert(components);
    auto manager = scene::SceneMetaManager::build({
        std::move(*components),
        simulation::SimulationSystemRegistry{},
        {registration}
    });
    assert(manager);

    auto missing = scene::Scene::create({
        scene_description,
        world_owner,
        simulation_owner,
        *manager,
        {}
    });
    assert(!missing);
    assert(missing.error().scene_system.code == scene::ESceneSystemBuildError::INVALID_REQUIREMENT_BINDING);

    ProbeRuntime runtime;
    const auto provider = scene::makeSceneCapabilityProvider<ProbeRuntime>(
        "host.runtime",
        "lux.test.runtime",
        runtime
    );
    auto created = scene::Scene::create({
        scene_description,
        world_owner,
        simulation_owner,
        *manager,
        std::span(&provider, 1U)
    });
    assert(created);
    assert((*created)->description().findSystem("probe"));
    assert((*created)->findSceneSystem<ProbeSceneSystem>() != nullptr);
    assert((*created)->hasCapability("lux.scene.probe"));
    assert((*created)->executeStablePoint());
    assert((*created)->executePresentation());
    assert(runtime.stable == 1 && runtime.presentation == 1);
    assert(!(*created)->stopToken().stop_requested());
    (*created)->requestStop();
    assert((*created)->stopToken().stop_requested());
    (*created)->requestStop();
    created->reset();
    meta::ReflectionRegistry::destroyRegistry();
    return 0;
}
