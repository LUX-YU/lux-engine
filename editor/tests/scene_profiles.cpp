#include "api_contract.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/editor/ScenePackageFile.hpp>
#include <lux/engine/editor/SceneProfile3D.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>
#include <lux/engine/project/PluginManager.hpp>
#include <lux/engine/project/PluginRendering.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/TransformSystem.hpp>
#include <lux/engine/scene/WorldLoadingConfiguration.hpp>
#include <thread>

using namespace lux;
using namespace lux::editor;
namespace
{
    uuids::uuid_name_generator identity{*uuids::uuid::from_string("55677517-5438-46d1-967e-2977ad66961b")};
    SceneCreateResult create2D(const SceneCreateInfo& info) noexcept
    {
        return scene::createScenePackage(
            info.id,
            info.name,
            {},
            std::make_shared<const simulation::SimulationDescription>(),
            {}
        );
    }
    void inspect3D(const scene::ScenePackage& package, asset::AssetId id)
    {
        assert(package.scene->id() == id && package.scene->data().world() == package.world->id());
        assert(package.scene->data().simulation() == package.simulation->id());
        assert(package.simulation->data().systemCount() == 0);
        assert(package.world->data().partitioner().id.name == "lux.spatial.builtin.single");
        assert(package.world->data().partitionCount() == 1 && package.partitions.size() == 1);
        assert(package.partitions[0]->objectCount() == 0); // No authored editor camera/overlay.
        assert(package.volumes.size() == 1 && !package.volumes[0].empty());
        const auto schemas = package.world->data().schemas();
        assert(schemas.size() == 5);
        for (const auto name :
             {"lux.ecs.Parent", "lux.ecs.Transform3D", "lux.ecs.Mesh3D", "lux.ecs.Light3D", "lux.scene.Camera"})
        {
            assert(std::ranges::any_of(schemas, [&](const auto& schema) { return schema.name == name; }));
        }
        const auto& description = package.scene->data();
        assert(description.systemCount() == 3);
        const auto transform = description.findSystem("transform");
        assert(transform && transform.type().name == "lux.scene.transform");
        scene::TransformSystemConfiguration transform_config;
        auto transform_codec = scene::transformSystemRegistration().configuration;
        assert(transform_codec.decode(transform.configurationPayload(), &transform_config));
        assert(transform_config.entity_capacity == 4096 && transform_config.max_commands == 32768);
        const auto loading = description.findSystem("world_loading");
        scene::WorldLoadingConfiguration world_config;
        assert(
            loading && scene::worldLoadingConfigurationCodec().decode(loading.configurationPayload(), &world_config)
        );
        assert(world_config.bootstrap.size() == 1 && world_config.bootstrap[0].value == 0);
        const auto render = description.findSystem("render");
        scene::RenderSystemConfiguration render_config;
        assert(render && scene::renderSystemConfigurationCodec().decode(render.configurationPayload(), &render_config));
        assert(render_config.features.size() == 5);
        assert(render_config.features[0].type == render::featureId("lux.render.material.v1"));
        assert(render_config.features[1].type == render::featureId("lux.render.mesh_stack.v1"));
        assert(render_config.features[4].type == render::featureId("lux.render.forward_mesh.v1"));
    }
} // namespace
int main(int argc, char** argv)
{
    assert(argc == 4);
    static_assert(!api_contract::PublicFreeze<SceneProfileRegistrar>);
    auto& object_runtime = object::ObjectRuntime::instance();
    assert(object_runtime.isCurrent());
    auto engine = engine::EngineContext::create({1, 64, 64, {32}}, {0, 64});
    assert(engine && (*engine)->sceneRuntime().instanceCount() == 0);
    std::weak_ptr<int> provider;
    auto assembly = [&](EditorContext& context) noexcept -> FrameworkResult<void>
    {
        auto& profiles = context.sceneProfiles();
        assert(!profiles.find("lux.editor.scene.3d"));
        auto profile = sceneProfile3D();
        profile.id = "Bad Profile";
        assert(!profiles.registerProfile(std::move(profile)));
        profile = sceneProfile3D();
        profile.capabilities.push_back(profile.capabilities.front());
        assert(!profiles.registerProfile(std::move(profile)));
        assert(profiles.registerProfile(sceneProfile3D()));
        assert(!profiles.registerProfile(sceneProfile3D()));
        auto pin = std::make_shared<int>(42);
        provider = pin;
        return profiles.registerProfile({pin, "test.editor.scene.2d", "Test 2D", {"spatial.2d"}, &create2D});
    };
    ProjectManifest manifest{1, identity("project"), "Profiles"};
    auto context =
        EditorContext::create(**engine, {manifest.name, std::filesystem::current_path()}, manifest, assembly);
    assert(context);
    assert((*context)->sceneProfiles().profiles().size() == 2);
    assert(!(*context)->sceneProfiles().registerProfile(sceneProfile3D()));
    assert(!(*context)->sceneProfiles().find("test.missing"));
    auto three = (*context)->sceneProfiles().find("lux.editor.scene.3d")->get();
    auto two = (*context)->sceneProfiles().find("test.editor.scene.2d")->get();
    const auto scene_id = asset::AssetId{identity("scene")};
    project::SceneRegistrations registrations;
    {
        project::PluginCatalog catalog;
        assert(catalog.read(argv[1], argv[2]));
        const project::MetadataIdentity selected{"lux.builtin.scene_render", 1};
        auto plugins = project::PluginManager::create(std::move(catalog), std::span{&selected, 1});
        assert(plugins);
        auto read = project::readSceneRegistrations({}, plugins->libraries());
        assert(read);
        registrations = std::move(*read);
    } // Only the real registration leases now keep plugin codec callbacks alive.
    context->reset();
    assert(!provider.expired()); // A copied profile safely owns its provider code.
    auto two_package = two.create({scene_id, "2D", registrations, {}});
    assert(two_package && two_package->scene->data().systemCount() == 0);
    two = {};
    assert(provider.expired());

    SceneCreateResult package = cxx::unexpected(scene::ScenePackageFailure{});
    std::thread worker([&] { package = three.create({scene_id, "3D", registrations, {}}); });
    worker.join();
    if (!package)
    {
        std::fprintf(stderr, "Profile failed: %u %s\n", unsigned(package.error().code), package.error().stage.c_str());
    }
    assert(package);
    inspect3D(*package, scene_id);
    assert((*engine)->sceneRuntime().instanceCount() == 0 && !(*engine)->renderContext());

    project::SceneRegistrations missing;
    auto rejected = three.create({scene_id, "3D", missing, {}});
    assert(!rejected && rejected.error().stage == "3d profile schema");
    auto incomplete = registrations;
    incomplete.features.pop_back();
    const auto mesh = render::featureId("lux.render.mesh_stack.v1");
    std::erase_if(incomplete.features, [&](const auto& entry) { return entry.factory.descriptor.type == mesh; });
    rejected = three.create({scene_id, "3D", incomplete, {}});
    assert(!rejected && rejected.error().stage == "3d profile feature");
    incomplete = registrations;
    incomplete.scene_systems.clear();
    rejected = three.create({scene_id, "3D", incomplete, {}});
    assert(!rejected && rejected.error().stage == "3d profile system");
    incomplete = registrations;
    incomplete.render_bindings.clear();
    rejected = three.create({scene_id, "3D", incomplete, {}});
    assert(!rejected && rejected.error().stage == "3d profile render binding");
    std::stop_source stopped;
    stopped.request_stop();
    rejected = three.create({scene_id, "3D", registrations, stopped.get_token()});
    assert(!rejected && rejected.error().code == scene::EScenePackageError::CANCELLED);

    // An unknown Pak member must remain byte-identical across actual scene file IO.
    auto opaque = std::make_shared<const std::vector<std::byte>>(
        std::initializer_list<std::byte>{std::byte{0x42}, std::byte{0x19}}
    );
    const auto opaque_id = asset::AssetId{identity("opaque")};
    package->package.entries.push_back(
        {{opaque_id, 0x55555555U, "Unknown"}, cxx::SharedBytes<>::fromOwner(opaque, *opaque)}
    );
    auto directory = std::filesystem::absolute(argv[3]);
    std::filesystem::create_directories(directory);
    const auto file = directory / "profile.scene";
    std::filesystem::remove(file);
    constexpr std::size_t limit = 16U * 1024U * 1024U;
    auto written = writeScenePackageAtomic(file, *package, EProjectWrite::CREATE, limit);
    if (!written)
    {
        if (auto* error = std::get_if<ProjectFailure>(&written.error()))
        {
            std::fprintf(stderr, "Scene file IO: %u %s\n", unsigned(error->code), error->system.message().c_str());
        }
        else
        {
            const auto& encoding = std::get<scene::ScenePackageFailure>(written.error());
            const auto* message = std::get_if<std::string>(&encoding.cause);
            std::fprintf(
                stderr,
                "Scene encoding: %u %s %s\n",
                unsigned(encoding.code),
                encoding.stage.c_str(),
                message ? message->c_str() : ""
            );
        }
    }
    assert(written);
    assert(!writeScenePackageAtomic(file, *package, EProjectWrite::CREATE, limit));
    auto loaded = readScenePackageFile(file, limit);
    assert(loaded);
    inspect3D(*loaded, scene_id);
    auto roundtrip = scene::encodeScenePackage(*loaded, limit);
    assert(roundtrip);
    const auto found = std::ranges::find_if(
        loaded->package.entries,
        [&](const auto& entry) { return entry.metadata.id == opaque_id; }
    );
    assert(found != loaded->package.entries.end() && std::ranges::equal(found->bytes.view(), *opaque));
    assert(!writeScenePackageAtomic(file, *two_package, EProjectWrite::REPLACE, limit, stopped.get_token()));
    assert(!writeScenePackageAtomic(file, *two_package, EProjectWrite::REPLACE, 1));
    loaded = readScenePackageFile(file, limit);
    assert(loaded && scene::encodeScenePackage(*loaded, limit).value() == *roundtrip);
    assert(!readScenePackageFile(file, 1));
    assert(!readScenePackageFile(file, limit, stopped.get_token()));
    assert(writeScenePackageAtomic(file, *two_package, EProjectWrite::REPLACE, limit));
    loaded = readScenePackageFile(file, limit);
    assert(loaded && loaded->scene->data().systemCount() == 0);
    std::filesystem::remove(file);
    assert((*engine)->sceneRuntime().instanceCount() == 0);
    std::puts("PASS frozen profiles, independent 2D extension, real plugin 3D package/codecs/IO, no runtime Scene");
}
