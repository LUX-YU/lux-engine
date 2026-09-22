#include <lux/engine/function/render/features/BuiltinFeatures.hpp>
#include <lux/engine/function/render/features/genops/MaterialOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>
#include <lux/engine/scene/Builtin3DRenderIntegration.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/RenderSystemMetadata.hpp>
#include <lux/engine/scene/ResolvedMeshResources.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <lux/engine/scene/SceneRenderSchema.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/engine/simulation/ecs/VisualSchema.hpp>

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

namespace
{
class HeldReads final : public lux::process::asset_loading::AssetReadPort::Endpoint
{
    using Read = lux::process::asset_loading::ReadAssetImage;
    struct Pending final
    {
        Read read;
        void *receiver;
        void (*complete)(void *, Outcome &&) noexcept;
        lux::async::SubmitOptions options;
    };
    lux::process::asset_loading::AssetReadPort port_;
    std::vector<Pending> pending_;

  public:
    explicit HeldReads(lux::process::asset_loading::AssetReadPort port) : port_(std::move(port))
    {
    }
    bool held{};
    std::size_t submissions{};
    lux::async::SubmitResult submit(Read read, void *receiver, void (*complete)(void *, Outcome &&) noexcept,
                                    lux::async::SubmitOptions options) noexcept override
    {
        ++submissions;
        if (held)
        {
            pending_.push_back({read, receiver, complete, options});
            return {};
        }
        return port_.submit(read, receiver, complete, options);
    }
    std::size_t pending() const noexcept
    {
        return pending_.size();
    }
    void resume()
    {
        held = false;
        for (const auto &item : pending_)
        {
            auto submitted = port_.submit(item.read, item.receiver, item.complete, item.options);
            if (!submitted)
            {
                item.complete(item.receiver, lux::cxx::unexpected(
                                                 lux::async::OperationFailure<lux::asset::EAssetStorageError>::runtime(
                                                     submitted.error())));
            }
        }
        pending_.clear();
    }
};
} // namespace

int main(int argc, char **argv)
{
    using namespace lux;
    using namespace std::chrono_literals;
    namespace ecs = simulation::ecs;
    assert(argc == 2);
    auto pak = asset::PakAssetProvider::loadFromFile(argv[1]);
    assert(pak);
    asset::AssetVfs vfs;
    const auto mount = vfs.mount({"/Seed", *pak});
    assert(mount);
    const auto mesh = vfs.view().resolve("/Seed/Meshes/Cube");
    const auto ground = vfs.view().resolve("/Seed/Meshes/Ground");
    const auto material = vfs.view().resolve("/Seed/Materials/Material-0");
    const auto other_material = vfs.view().resolve("/Seed/Materials/Material-1");
    assert(!mesh.isNull() && !ground.isNull() && !material.isNull() && !other_material.isNull());
    auto process = process::ExecutionRuntime::create({2, 64, 64, {64}, process::BlockingSchedulerConfig{2, 64}});
    assert(process && process->blocking());
    process::TaskScope tasks;
    auto endpoint =
        process::asset_loading::VfsAssetReadEndpoint::create(vfs.view().capture(), *process->blocking(), tasks, {32});
    assert(endpoint);
    vfs.unmount(mount);
    assert(vfs.view().resolve("/Seed/Meshes/Cube").isNull()); // Captured source remains readable.
    meta::ReflectionRegistry::initRegistry();
    scene::initializeBuiltinRenderSystemMeta();
    render::initializeBuiltinRenderFeatureMeta();
    std::vector<ecs::ComponentSchema> components;
    for (const auto schemas :
         {scene::sceneRenderComponentSchemas(), ecs::transformComponentSchemas(), ecs::visualComponentSchemas()})
    {
        components.insert(components.end(), schemas.begin(), schemas.end());
    }
    auto schemas = ecs::ComponentSchemaSet::build(std::move(components));
    assert(schemas);
    auto metadata = scene::SceneMetaManager::build(
        {.components = *schemas, .scene_systems = {scene::builtinRenderSystemRegistration()}});
    assert(metadata);
    std::vector<scene::RenderFeatureSceneBinding> bindings;
    for (const auto &binding : scene::builtinRenderFeatureSceneBindings())
    {
        if (binding.feature == render::kMeshStackDescriptor.type)
        {
            bindings.push_back(binding);
        }
    }
    auto render_meta = scene::RenderSystemMetadata::build(
        *metadata, {render::kMeshStackRenderFeatureRegistration, render::kMaterialRenderFeatureRegistration}, bindings);
    assert(render_meta);
    auto shared_meta = std::make_shared<const scene::RenderSystemMetadata>(std::move(*render_meta));
    render::RendererConfig config;
    config.validation = true;
    config.feature_factories = {render::kMeshStackFeatureFactory, render::kMaterialFeatureFactory};
    auto made_runtime = render::RenderRuntime::create(std::move(config));
    assert(made_runtime);
    auto runtime = std::move(*made_runtime);
    auto reads = std::make_shared<HeldReads>((*endpoint)->port());
    auto assets =
        std::make_shared<scene::RenderAssetSource>(*runtime, tasks, process::asset_loading::AssetReadPort{reads}, 1);
    scene::RenderSystemConfiguration render_config;
    render_config.features = {{render::kMeshStackDescriptor.type, {}}, {render::kMaterialDescriptor.type, {}}};
    std::vector<std::byte> bytes;
    assert(scene::builtinRenderSystemRegistration().configuration.encode(&render_config, bytes));
    scene::SceneDescriptionBuilder description;
    assert(description.addSystem({1}, "render", scene::builtinRenderSystemRegistration().type, 1,
                                 scene::RenderSystem::Description.configuration_schema_name, 1, bytes));
    auto resolved = std::move(description).buildResolved();
    assert(resolved);
    std::array providers{
        scene::makeSceneCapabilityProvider<render::RenderRuntime>("runtime", "lux.render.runtime", *runtime),
        scene::makeSceneCapabilityProvider<std::shared_ptr<const scene::RenderSystemMetadata>>(
            "metadata", "lux.render.metadata", shared_meta),
        scene::makeSceneCapabilityProvider<std::shared_ptr<scene::RenderAssetSource>>("assets", "lux.render.assets",
                                                                                      assets)};
    scene::SceneCreateInfo create{std::make_shared<const scene::SceneDescription>(std::move(*resolved)),
                                  std::make_shared<const world::WorldDescription>(),
                                  std::make_shared<const simulation::SimulationDescription>(),
                                  *metadata,
                                  providers,
                                  simulation::ESimulationMode::DERIVATION};
    auto made_author = scene::SceneInstance::create(create), made_run = scene::SceneInstance::create(create);
    assert(made_author && made_run);
    auto author = std::move(*made_author), run = std::move(*made_run);
    assert(author->simulation().seal() && run->simulation().seal());
    auto *system = author->findSceneSystem<scene::RenderSystem>();
    auto *running = run->findSceneSystem<scene::RenderSystem>();
    assert(system && running);
    auto executor = task::TaskExecutor::create({0, 1024});
    assert(executor);
    scene::SceneDriver driver(*executor);
    const auto pump = [&] {
        assert(process->drainMain(8));
        std::size_t controls = 8, programs = 4;
        assert(runtime->poll(32, controls, programs));
        scene::SceneAdvanceBudget budget;
        for (auto *instance : {author.get(), run.get()})
        {
            if (instance)
            {
                static_cast<void>(driver.advance(*instance, std::chrono::steady_clock::now(), budget));
                assert(instance->progress().result);
            }
        }
    };
    const auto until = [&](auto condition) {
        const auto deadline = std::chrono::steady_clock::now() + 20s;
        while (std::chrono::steady_clock::now() < deadline)
        {
            pump();
            if (condition())
            {
                return;
            }
            std::this_thread::sleep_for(1ms);
        }
        assert(false && "real render resource completion did not arrive");
    };
    const auto add = [&](scene::SceneInstance &instance, asset::AssetId mesh_id, asset::AssetId material_id) {
        auto &registry = instance.registry();
        const auto entity = registry.create();
        auto &visual = registry.emplace<ecs::Mesh3D>(entity);
        visual.value.mesh = mesh_id;
        visual.value.material = material_id;
        registry.emplace<ecs::WorldTransform3D>(entity);
        return entity;
    };
    const auto first = add(*author, mesh, material), second = add(*run, mesh, material);
    // Zero means no read admission, even when component observers see new work.
    // A single shared visit cannot be re-used by the second instance.
    const auto ready_deadline = std::chrono::steady_clock::now() + 20s;
    while (system->resourceStatus().state != render::ESceneResourceState::READY ||
           running->resourceStatus().state != render::ESceneResourceState::READY)
    {
        assert(std::chrono::steady_clock::now() < ready_deadline);
        std::size_t controls = 8, programs = 4;
        assert(runtime->poll(32, controls, programs));
        scene::SceneAdvanceBudget budget{.resource_steps = 0};
        static_cast<void>(driver.advance(*author, std::chrono::steady_clock::now(), budget));
        static_cast<void>(driver.advance(*run, std::chrono::steady_clock::now(), budget));
        assert(reads->submissions == 0 && budget.resource_steps == 0);
        std::this_thread::sleep_for(1ms);
    }
    scene::SceneAdvanceBudget single{.resource_steps = 1};
    static_cast<void>(driver.advance(*author, std::chrono::steady_clock::now(), single));
    assert(single.resource_steps == 0 && reads->submissions == 2);
    static_cast<void>(driver.advance(*run, std::chrono::steady_clock::now(), single));
    assert(single.resource_steps == 0 && reads->submissions == 2);
    assert(!run->registry().all_of<scene::ResolvedMeshResources>(second));
    until([&] {
        return author->registry().all_of<scene::ResolvedMeshResources>(first) &&
               run->registry().all_of<scene::ResolvedMeshResources>(second);
    });
    const auto original_mesh = author->registry().get<scene::ResolvedMeshResources>(first).mesh;
    const auto original_material = author->registry().get<scene::ResolvedMeshResources>(first).material;
    assert(original_mesh == run->registry().get<scene::ResolvedMeshResources>(second).mesh);
    assert(original_material == run->registry().get<scene::ResolvedMeshResources>(second).material);
    assert(reads->submissions == 2); // One mesh and one material for two instances.
    const auto unreferenced = add(*run, mesh, {});
    until([&] { return running->assetStatus().size() == 2; });
    assert(run->registry().get<scene::ResolvedMeshResources>(second).mesh == original_mesh);
    run->registry().destroy(unreferenced);
    assert(system->assetStatus().size() == 1 && system->assetStatus()[0].key.instance == author->id());
    assert(running->assetStatus()[0].key.instance == run->id());

    // A genuinely new source is admitted after initial readiness, not read from
    // a startup-frozen handle list. Its Entity uses the complete generation.
    const auto dynamic = add(*run, ground, other_material);
    until([&] { return run->registry().all_of<scene::ResolvedMeshResources>(dynamic); });
    const auto ground_handle = run->registry().get<scene::ResolvedMeshResources>(dynamic).mesh;
    const auto other_handle = run->registry().get<scene::ResolvedMeshResources>(dynamic).material;
    const auto alternate_material = add(*run, mesh, other_material);
    const auto alternate_mesh = add(*run, ground, material);
    until([&] {
        return run->registry().all_of<scene::ResolvedMeshResources>(alternate_material) &&
               run->registry().all_of<scene::ResolvedMeshResources>(alternate_mesh);
    });
    assert(run->registry().get<scene::ResolvedMeshResources>(alternate_material).mesh == original_mesh);
    assert(run->registry().get<scene::ResolvedMeshResources>(alternate_material).material == other_handle);
    assert(run->registry().get<scene::ResolvedMeshResources>(alternate_mesh).mesh == ground_handle);
    assert(run->registry().get<scene::ResolvedMeshResources>(alternate_mesh).material == original_material);
    assert(reads->submissions == 4); // Four combinations, two Mesh + two Material reads/uploads.
    run->registry().destroy(alternate_material);
    run->registry().destroy(alternate_mesh);
    const auto removed_key = running->assetStatus()[0].key;
    run->registry().destroy(dynamic);
    pump();
    const auto reused = add(*run, ground, other_material);
    assert(reused != dynamic);
    until([&] { return run->registry().all_of<scene::ResolvedMeshResources>(reused); });
    if (removed_key.entity == dynamic)
    {
        assert(!running->retryAsset(removed_key));
    }

    // A missing material is preserved as that dependency's storage failure;
    // successful Mesh work cannot make the aggregate request READY.
    std::array<std::uint8_t, 16> missing_bytes{};
    missing_bytes.back() = 254;
    const asset::AssetId missing{missing_bytes};
    const auto bad = add(*run, mesh, missing);
    until([&] {
        const auto rows = running->assetStatus();
        const auto row = std::ranges::find(rows, bad, [](const auto &r) { return r.key.entity; });
        return row != rows.end() && row->state == scene::ERenderAssetState::FAILED;
    });
    auto rows = running->assetStatus();
    const auto failure = *std::ranges::find(rows, bad, [](const auto &r) { return r.key.entity; });
    assert(failure.failed_dependency == missing);
    assert(std::get<process::asset_loading::AssetLoadFailure>(failure.failure).storage_error ==
           asset::EAssetStorageError::NOT_FOUND);
    assert(!run->registry().all_of<scene::ResolvedMeshResources>(bad));
    assert(running->retryAsset(failure.key));
    assert(!running->retryAsset(failure.key)); // A retry has a new request identity.

    auto view = system->openView({.extent = {96, 64}});
    assert(view);
    until([&] { return (*view)->status().state == render::EViewState::READY; });
    auto receipt = system->resourceReceipt();
    author.reset();
    for (int turn = 0; turn < 12; ++turn)
    {
        pump();
    }
    assert(receipt.status().state != render::ESceneResourceState::RETIRED);
    assert(run->registry().get<scene::ResolvedMeshResources>(second).mesh == original_mesh);
    assert(run->registry().get<scene::ResolvedMeshResources>(second).material == original_material);
    view->reset();
    until([&] { return receipt.status().state == render::ESceneResourceState::RETIRED; });
    // Close another system while its fresh read is still pending. Completion
    // belongs to the root scope and cannot re-enter the destroyed Registry.
    reads->held = true;
    missing_bytes.back() = 253;
    add(*run, mesh, asset::AssetId{missing_bytes});
    until([&] { return reads->pending() > 0; });
    auto run_receipt = running->resourceReceipt();
    run.reset();
    assets.reset();
    pump(); // Revoke the result destination before its real disk read starts.
    reads->resume();
    until([&] {
        return run_receipt.status().state == render::ESceneResourceState::RETIRED &&
               runtime->control()->get().pendingResourceReleases() == 0;
    });
    assert(runtime->statistics().validation_errors == 0);
    (*endpoint)->requestStop();
    assert(stdexec::sync_wait(tasks.close()));
    assert((*endpoint)->join());
    assert(runtime->beginClose());
    until([&] {
        std::size_t replies = 32, controls = 8, programs = 4;
        auto result = runtime->advanceClose(replies, controls, programs);
        assert(result);
        return *result == render::ERenderClose::COMPLETE;
    });
    assert(runtime->joinStopped());
    process->requestStop();
    assert(process->join());
    std::cout << "PASS Process asset reads, real GPU resource creation, shared author/Run handles, "
                 "dynamic Entity source, exact material failure/retry, late completion and View retention; "
                 "no rendered pixel assertion\n";
}
