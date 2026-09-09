#define NOMINMAX
#include "PluginApi.hpp"
#include <lux/engine/editor/ui/scene/SceneWorkspace.hpp>
#include <lux/engine/editor/sessions/scene/SceneOpenInfo.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <cassert>
#include <array>
#include <filesystem>
#include <cstdio>
#include <Windows.h>
template <class T> T identity(unsigned char tail)
{
    std::array<std::uint8_t, 16> bytes{};
    bytes.back() = tail;
    return T{uuids::uuid{bytes}};
}
lux::editor::sessions::SceneOpenInfo source(lux::simulation::ecs::ComponentSchema schema)
{
    auto components = lux::simulation::ecs::ComponentSchemaSet::build(std::vector{std::move(schema)});
    assert(components);
    auto metadata = lux::scene::SceneMetaManager::build({std::move(*components), {}, {}, {}, {}});
    assert(metadata);
    auto shared = std::make_shared<lux::scene::SceneMetaManager>(std::move(*metadata));
    lux::world::WorldDescriptionBuilder wb;
    assert(wb.setIdentity(identity<lux::world::WorldBundleId>(1),
        identity<lux::world::WorldBundleGeneration>(1), "Installed plugin Scene"));
    assert(wb.setPartitioner({lux::world::worldPartitionerId("consumer.none"), 1}, 0));
    auto world = std::move(wb).build();
    lux::simulation::SimulationDescriptionBuilder sb;
    auto simulation = std::move(sb).build();
    lux::scene::SceneDescriptionBuilder cb;
    cb.setWorld(identity<lux::asset::AssetId>(1));
    cb.setSimulation(identity<lux::asset::AssetId>(2));
    auto description = std::move(cb).build();
    assert(world && simulation && description);
    auto scene = lux::scene::Scene::create({
        std::make_shared<lux::scene::SceneDescription>(std::move(*description)),
        std::make_shared<lux::world::WorldDescription>(std::move(*world)),
        std::make_shared<lux::simulation::SimulationDescription>(std::move(*simulation)), *shared, {}});
    assert(scene && (*scene)->simulation().seal());
    lux::editor::sessions::SceneOpenInfo input;
    input.id = {7};
    input.scene = std::move(*scene);
    input.metadata = std::move(shared);
    return input;
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    const auto path = std::filesystem::absolute(argv[1]);
    unsigned unloads{};
    lux::meta::ReflectionRegistry::initRegistry();
    {
        const auto library = LoadLibraryW(path.c_str());
        assert(library);
        auto lifetime = std::shared_ptr<const void>(library, [&unloads](const void *module)
        {
            assert(FreeLibrary(static_cast<HMODULE>(const_cast<void *>(module))));
            ++unloads;
        });
        const auto open = reinterpret_cast<OpenPlugin>(GetProcAddress(library, "openSceneReaderPlugin"));
        assert(open);
        PluginApi plugin;
        open(&plugin);
        assert(plugin.reader.version == 3 && plugin.reader.canonical_schema == "consumer.RichComponent");
        plugin.schema.code_lifetime = lifetime;
        plugin.reader.code_lifetime = lifetime;
        auto input = source(std::move(plugin.schema));
        const auto entity = plugin.populate(input.scene->registry());
        input.initial_selection = entity;
        lux::ui::UISession ui;
        input.dispatcher = ui.dispatcherRef();
        auto session = lux::editor::sessions::SceneSession::openInspection(input);
        assert(session && !input.scene);
        input.metadata.reset();
        lifetime.reset();
        const lux::editor::sessions::SceneEntityRef ref{{7}, entity};
        assert((*session)->updateAtOwnerSafePoint({1, 0.01}));
        assert(plugin.verify(**session, ref) && unloads == 0);
        {
            lux::editor::ui::SceneInspector inspector(ui.dispatcherRef(), lux::ui::PaneId{"installed.inspector"}, **session);
            assert(inspector.installReaders({&plugin.reader, 1}));
            plugin.reader = {};
            auto registration = ui.registerPane(inspector);
            assert(registration);
            for (unsigned i = 0; i < 4; ++i)
            {
                auto frame = ui.beginFrame({{800, 600}, 1.0F / 60.0F, {1, 1}});
                frame.drawPanes();
                frame.finish();
                auto snapshot = ui.captureFrame();
                assert(snapshot && snapshot->valid());
            }
            assert(plugin.drawCalls() > 0 && plugin.verify(**session, ref));
            registration->reset();
        }
        assert(unloads == 0 && plugin.verify(**session, ref));
        assert((*session)->advanceScene({1, 0.01}));
        assert((*session)->beginClose());
        auto closed = (*session)->advanceClose();
        assert(closed && *closed == lux::editor::sessions::ECloseProgress::COMPLETE);
        session->reset();
        assert(unloads == 1 && GetModuleHandleW(path.filename().c_str()) == nullptr);
        std::puts("Installed generated reader: real DLL retained through Session/Pane use, actual draw and unload passed");
    }
    lux::meta::ReflectionRegistry::destroyRegistry();
}
