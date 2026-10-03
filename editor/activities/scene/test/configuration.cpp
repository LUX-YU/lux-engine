#include <lux/engine/editor/scene/SceneConfigurationPreparation.hpp>
#include <lux/engine/project/PluginManager.hpp>
#include <lux/engine/project/PluginRendering.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <cassert>
#include <iostream>
#include <algorithm>

int main(int argc, char** argv)
{
    using namespace lux;
    using namespace lux::editor::scene;
    assert(argc == 2);
    lux::project::PluginCatalog catalog;
    const std::filesystem::path installation{argv[1]};
    assert(catalog.read(installation / "share/lux-engine/plugins/catalog.json", installation));
    const lux::project::MetadataIdentity selected{"lux.builtin.scene_render", 1};
    auto manager = lux::project::PluginManager::create(std::move(catalog), std::span(&selected, 1));
    assert(manager);
    auto registrations = lux::project::readSceneRegistrations({}, manager->libraries());
    assert(registrations);
    constexpr SceneProviderOption providers[]{
        {"lux.render.runtime", "main-window"}, {"lux.render.scene_bindings", "render-bindings"},
        {"lux.render.resources", "resources"}, {"lux.render.assets", "assets"},
        {"lux.world.loading", "world-storage"}
    };
    const SceneConfigurationRegistrations inputs{registrations->components,
        *registrations->simulation_systems, registrations->scene_systems, registrations->features, providers,
        registrations->render_bindings};
    const auto same_systems = [](auto left, auto right) {
        // SceneDescriptionBuilder canonically orders provider bindings by requirement.
        for (auto* values : {&left, &right})
            for (auto& row : *values)
                std::ranges::sort(row.providers, {}, &SceneProviderBinding::requirement);
        return left == right;
    };
    const auto id = asset::AssetId{*uuids::uuid::from_string("b3e1486c-3744-476d-acb8-e4f93a737a3b")};
    for (auto preset : {ESceneContentPreset::EMPTY, ESceneContentPreset::TWO_DIMENSIONAL,
         ESceneContentPreset::THREE_DIMENSIONAL})
    {
        auto draft = makeSceneConfigurationPreset(preset, "lux.spatial.builtin.single", 1, inputs);
        assert(draft);
        auto built = prepareSceneConfiguration(*draft, inputs);
        assert(built);
        auto package = lux::scene::createScenePackage(id, built->name, built->schemas, built->simulation, built->scene);
        assert(package);
        SceneConfiguration source{package->scene, package->world, package->simulation};
        auto reopened = captureSceneConfiguration(source, {}, draft->viewport);
        assert(reopened && same_systems(reopened->systems, draft->systems) && reopened->schemas == draft->schemas);
        auto edit = prepareSceneConfigurationEdit(*reopened, inputs);
        assert(edit && edit->world == source.world);
        auto again = captureSceneConfiguration(*edit, {}, draft->viewport);
        assert(again && same_systems(again->systems, draft->systems));
        if (preset == ESceneContentPreset::THREE_DIMENSIONAL)
        {
            auto opaque = *draft;
            auto& renderer = opaque.systems.back();
            const auto codec = lux::scene::renderSystemConfigurationCodec();
            lux::scene::RenderSystemConfiguration configuration;
            assert(codec.decode(renderer.configuration, &configuration));
            configuration.features.push_back({UINT64_MAX, {std::byte{0x42}}, "future.feature", 8});
            assert(codec.encode(&configuration, renderer.configuration));
            assert(!prepareSceneConfiguration(opaque, inputs)); // New unknown features cannot be fabricated.
            lux::scene::SceneDescriptionBuilder source_builder;
            for (const auto& row : opaque.systems)
            {
                assert(source_builder.addSystem(row.id, row.name, row.type, row.version,
                    row.configuration_schema, row.configuration_version, row.configuration));
                for (const auto& binding : row.providers)
                    assert(source_builder.bindRequirement(row.id, binding.requirement, binding.provider));
            }
            auto source_description = std::move(source_builder).buildResolved();
            assert(source_description);
            auto opaque_package = lux::scene::createScenePackage(id, built->name, built->schemas,
                built->simulation, *source_description);
            assert(opaque_package);
            auto opaque_draft = captureSceneConfiguration(
                {opaque_package->scene, opaque_package->world, opaque_package->simulation});
            assert(opaque_draft);
            auto opaque_edit = prepareSceneConfigurationEdit(*opaque_draft, inputs);
            assert(opaque_edit);
            auto opaque_roundtrip = captureSceneConfiguration(*opaque_edit);
            assert(opaque_roundtrip && opaque_roundtrip->systems == opaque_draft->systems);
            configuration.features.back().configuration.push_back(std::byte{0x43});
            assert(codec.encode(&configuration, opaque_draft->systems.back().configuration));
            assert(!prepareSceneConfigurationEdit(*opaque_draft, inputs));
        }
        reopened->partition = "lux.spatial.builtin.grid2d";
        assert(!prepareSceneConfigurationEdit(*reopened, inputs));
        if (!draft->systems.empty())
        {
            auto missing = *draft;
            for (auto& system : missing.systems)
                system.providers.clear();
            const auto rejected = prepareSceneConfiguration(missing, inputs);
            assert(!rejected && rejected.error().code == EScenePreparationError::MISSING_PROVIDER);
            draft->scene_dependencies = {{draft->systems.front().id, draft->systems.back().id},
                {draft->systems.back().id, draft->systems.front().id}};
            assert(!prepareSceneConfiguration(*draft, inputs));
        }
    }
    assert(!makeSceneConfigurationPreset(ESceneContentPreset::EMPTY, "lux.spatial.builtin.grid3d", 1, inputs));
    constexpr std::string_view worlds[]{"*"};
    const simulation::SimulationSystemDescription unknown{
        {"unknown.simulation", 8, "unknown.config", 7, {}, system::ESystemMultiplicity::MULTIPLE, worlds}, {}, {}
    };
    const std::byte payload[]{std::byte{0x10}, std::byte{0x20}};
    simulation::SimulationDescriptionBuilder sim;
    assert(sim.addSystem({900}, "opaque", unknown, payload));
    auto sim_value = std::move(sim).build();
    assert(sim_value);
    lux::scene::SceneDescriptionBuilder scene;
    assert(scene.addSystem({901}, "opaque-scene", system::systemTypeId("unknown.scene"), 4, "unknown.scene.config", 2,
        payload));
    assert(scene.bindRequirement({901}, "opaque.provider", "future-provider"));
    auto scene_value = std::move(scene).buildResolved();
    assert(scene_value);
    auto package = lux::scene::createScenePackage(id, "Opaque", {},
        std::make_shared<const simulation::SimulationDescription>(std::move(*sim_value)), *scene_value);
    assert(package);
    auto draft = captureSceneConfiguration({package->scene, package->world, package->simulation});
    assert(draft);
    auto rebuilt = prepareSceneConfigurationEdit(*draft, inputs);
    assert(rebuilt);
    auto retained = captureSceneConfiguration(*rebuilt);
    assert(retained && retained->systems == draft->systems);
    draft->systems.front().configuration.push_back(std::byte{0xff});
    assert(!prepareSceneConfigurationEdit(*draft, inputs)); // Missing plugin cannot validate new opaque changes.
    std::cout << "EC2 configuration: headless presets, exact providers, relationships, opaque contracts and structural refusal\n";
}
