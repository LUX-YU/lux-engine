#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
#include <lux/engine/editor/scene/ModelPlacementService.hpp>
#include <lux/engine/editor/scene/ProjectSceneEnvironment.hpp>
#include <lux/engine/editor/scene/SceneProjection.hpp>
#include <lux/engine/editor/sessions/SessionServices.hpp>
#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <lux/engine/editor/storage/ProjectPublicationOperation.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/material/Cooker.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ecs/HierarchySchema.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/simulation/ecs/VisualSchema.hpp>
#include <source_location>
#include <thread>

using namespace lux;
using namespace lux::editor;
using namespace lux::editor::scene;
namespace ecs = simulation::ecs;
namespace
{
    template <class Result> auto take(Result result, std::source_location location = std::source_location::current())
    {
        if (!result)
        {
            std::cerr << "Unexpected failure at " << location.file_name() << ':' << location.line() << '\n';
            std::abort();
        }
        return std::move(*result);
    }
    uuids::uuid uuid(std::string_view name)
    {
        return uuids::uuid_name_generator(*uuids::uuid::from_string("01234567-89ab-cdef-0123-456789abcdef"))(name);
    }
} // namespace
int main(int argc, char** argv)
{
    assert(argc == 2);
    const auto root = std::filesystem::absolute(argv[1]) /
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root / "Content");
    const asset::AssetId model_id{uuid("placement-model")};
    const asset::AssetId mesh_id{uuid("placement-mesh")}, material_id{uuid("placement-material")};
    auto mesh_data = std::make_shared<rdesc::Mesh>();
    mesh_data->vertices.resize(3);
    mesh_data->vertices[0].position = {-1, 0, 0};
    mesh_data->vertices[1].position = {1, 0, 0};
    mesh_data->vertices[2].position = {0, 1, 0};
    for (auto& vertex : mesh_data->vertices)
    {
        vertex.normal = {0, 0, 1};
        vertex.tangent = {1, 0, 0};
        vertex.bitangent = {0, 1, 0};
        vertex.uv = {0, 0};
        for (auto& bone : vertex.bone.bone_ids)
        {
            bone = -1;
        }
    }
    mesh_data->indices = {0, 1, 2};
    mesh_data->bounds = math::AABB{{-1, 0, 0}, {1, 1, 0}};
    auto mesh = take(asset::MeshAsset::create({mesh_id, asset::MeshAsset::asset_type}, mesh_data));
    auto material = take(material::cookImportedMaterial(
        {material_id, asset::MaterialAsset::asset_type},
        material::ImportedMaterialDescription{}
    ));
    auto model_description = std::make_shared<rdesc::ModelDescription>();
    model_description->primitives.push_back({mesh_id, material_id});
    model_description->nodes.resize(2);
    model_description->nodes[1].primitives.push_back(0);
    model_description->nodes[0].children.push_back(1);
    auto model = take(asset::ModelAsset::create({model_id, asset::ModelAsset::asset_type}, model_description));
    std::vector<asset::PakWriteEntry> entries;
    const auto append = [&]<class Asset>(const Asset& value, std::string path)
    {
        auto bytes = std::make_shared<const std::vector<std::byte>>(
            take(asset::TAssetSerDeser<Asset>::encode(value, asset::AssetEncodeLimits{1024 * 1024}))
        );
        entries.push_back(
            {value.id(), Asset::primary_magic, std::move(path), {}, cxx::SharedBytes<>::fromOwner(bytes, *bytes)}
        );
    };
    append(*model, "Content/Model");
    append(*mesh, "Content/Model/Mesh");
    append(*material, "Content/Model/Material");
    assert(asset::writePakFile(root / "Content/Model.pak", std::move(entries), "/Project"));
    {
        std::ofstream source(root / "Content/Model.recipe");
        source << "model source";
        std::ofstream manifest(root / "Project.luxproject");
        manifest << take(encodeProjectManifest(
            {asset::AssetId{uuid("placement-project")},
             "Placement",
             {},
             {{model_id, "lux.model.source", "Content/Model.recipe", "Content/Model.pak", {}, {}, "Content/Model"}},
             {}}
        ));
    }
    auto execution =
        take(process::ExecutionRuntime::create({1, 64, 64, {32}, process::BlockingSchedulerConfig{1, 64}}));
    auto messages = take(object::ObjectMessageQueue::create(64));
    process::TaskScope tasks{execution};
    asset::AssetVfs assets;
    auto source = take(prepareProjectOpen(root / "Project.luxproject"));
    auto project = take(ProjectStorage::open(source, assets, *execution.blocking(), tasks, messages.dispatcherRef()));
    services::ServiceRegistry registry{messages.dispatcherRef()};
    auto scope = take(registry.createScope());
    assert(registry.publish(
        {services::ServiceEntry::bind<sessions::kSessionStoreService>(object::CodeLease::builtin()),
         services::ServiceEntry::bind<kModelPlacementService>(object::CodeLease::builtin())}
    ));
    auto sessions_owner = take(registry.get<sessions::SessionStore>(scope));
    auto& sessions = *sessions_owner;
    std::vector<ecs::ComponentSchema> types;
    for (auto group :
         {ecs::transformComponentSchemas(), ecs::hierarchyComponentSchemas(), ecs::visualComponentSchemas()})
    {
        for (auto schema : group)
        {
            if (schema.snapshot == ecs::EComponentSnapshotPolicy::COPY)
            {
                types.push_back(std::move(schema));
            }
        }
    }
    auto schemas = take(ecs::ComponentSchemaSet::build(std::move(types)));
    std::vector<world::WorldDataSchemaId> ids;
    for (const auto& schema : schemas.all())
    {
        ids.push_back(world::worldDataSchemaId(schema.id.name));
    }
    auto simulation = take(std::move(simulation::SimulationDescriptionBuilder{}).build());
    auto description = take(std::move(lux::scene::SceneDescriptionBuilder{}).buildResolved());
    auto package = take(lux::scene::createScenePackage(
        asset::AssetId{uuid("placement-scene")},
        "CPU scene",
        ids,
        std::make_shared<const simulation::SimulationDescription>(std::move(simulation)),
        description
    ));
    auto reservation = take(sessions.reserve<SceneSession>({"lux.editor.scene"}, object::CodeLease::builtin()));
    auto author = take(SceneSession::create(
        reservation.id(),
        sessions::BoundSource{package.scene->id(), "scene.lux"},
        take(SceneSource::create(package, schemas))
    ));
    auto* session = author.get();
    assert(sessions.prepare(reservation, author));
    const auto key = take(sessions.key<SceneSession>(take(sessions.publish(reservation))));

    assert(scope.provide(services::ServiceNameView{"lux.process.execution"}, execution));
    assert(scope.provide(services::ServiceNameView{"lux.editor.project.storage"}, *project));
    assert(scope.provide(services::ServiceNameView{"lux.simulation.components"}, schemas));
    assert(scope.maintain() && take(scope.settled())); // Registration does not start a task.
    auto service = take(registry.get<ModelPlacementService>(scope));
    auto other_window = take(registry.get<ModelPlacementService>(scope));
    assert(service == other_window && service->reports().empty());
    const auto input = [&]
    { return ModelPlacement{key, session->describe().current, project->reference(model_id), {2, 3, 4}, {0}}; };
    const auto before = session->describe();
    const auto id = take(service->request(input()));
    assert(!service->settled() && !service->reports().front().active());
    assert(!service->acknowledge(id));
    other_window.reset(); // No UI owner is needed to finish the accepted operation.
    assert(scope.maintain() && service->reports().front().active());
    const auto until = [&](auto condition)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!condition())
        {
            assert(std::chrono::steady_clock::now() < deadline);
            assert(execution.collectCompletions() && execution.dispatchTaskEvents());
            assert(scope.maintain());
            std::this_thread::yield();
        }
    };
    auto read = take(session->read());
    assert(read.withRead(
        [&](const SceneReadView&) -> SceneEditResult<void>
        {
            until(
                [&]
                {
                    const auto tasks = execution.taskInfos();
                    return !tasks.empty() &&
                           std::ranges::all_of(tasks, [](const auto& task) { return task.finished.has_value(); });
                }
            );
            assert(execution.collectCompletions() && execution.dispatchTaskEvents());
            assert(scope.maintain());
            assert(!service->reports().front().result && service->reports().front().active());
            assert(service->reports().front().placement.based_on == before.current);
            assert(session->describe().current == before.current && session->describe().dirty == before.dirty);
            return {};
        }
    ));
    until([&] { return service->settled(); });
    assert(service->reports().front().result && *service->reports().front().result);
    assert(!service->reports().front().active() && take(scope.settled()));
    assert(take(session->capture()).objects().size() == 2);
    assert(session->undo() && session->describe().current == before.current);
    assert(session->redo());
    assert(service->acknowledge(id) && service->reports().empty());

    const auto stale = take(service->request(input()));
    assert(session->undo());
    const auto unmodified = session->describe();
    until([&] { return service->settled(); });
    assert(service->reports().front().result && !*service->reports().front().result);
    assert(session->describe().current == unmodified.current && session->describe().observed == unmodified.observed);
    assert(service->acknowledge(stale));
    assert(service->request(input()) && scope.maintain());
    assert(service->reports().front().active());
    for (unsigned i{1}; i != 32; ++i)
    {
        assert(service->request(input()));
    }
    const auto full = service->request(input());
    assert(!full && full.error().code == EEditorError::CAPACITY);
    std::thread worker(
        [&]
        {
            const auto wrong = service->update();
            assert(!wrong && wrong.error().code == EEditorError::INVALID_STATE);
        }
    );
    worker.join();
    assert(service->reports().size() == 32 && service->reports().front().active());
    assert(service->requestClose() && !service->request(input()));
    assert(!service->settled());
    assert(scope.maintain() && !service->settled()); // Cancellation is not transport completion.
    until([&] { return service->settled(); });
    for (const auto& report : service->reports())
    {
        assert(report.result && !*report.result && !report.active());
        assert(std::holds_alternative<process::TaskCancelled>(report.result->error().cause));
    }
    while (!service->reports().empty())
    {
        assert(service->acknowledge(service->reports().front().id));
    }
    assert(session->describe().current == unmodified.current && session->describe().observed == unmodified.observed);
    {
        services::ServiceRegistry environments{messages.dispatcherRef()};
        auto environment_scope = take(environments.createScope());
        assert(environments.publish({
            services::ServiceEntry::bind<kProjectSceneEnvironment>(object::CodeLease::builtin())
        }));
        assert(environment_scope.maintain() && environments.drained()); // Declarations do not capture project assets.
        const auto missing = environments.get<ProjectionEnvironment>(environment_scope);
        assert(!missing && missing.error().code == services::EServiceError::NOT_FOUND);
        assert(environments.drained());
        std::shared_ptr<const simulation::SimulationSystemRegistry> simulations =
            std::make_shared<simulation::SimulationSystemRegistry>();
        std::vector<lux::scene::SceneSystemRegistration> systems;
        std::vector<lux::scene::RenderFeatureSceneBinding> bindings;
        assert(environment_scope.provide(services::ServiceNameView{"lux.editor.project.storage"}, *project));
        assert(environment_scope.provide(services::ServiceNameView{"lux.simulation.components"}, schemas));
        assert(environment_scope.provide(services::ServiceNameView{"lux.simulation.systems"}, simulations));
        assert(environment_scope.provide(services::ServiceNameView{"lux.scene.systems"}, systems));
        assert(environment_scope.provide(services::ServiceNameView{"lux.render.scene.bindings"}, bindings));
        auto first = take(environments.get<ProjectionEnvironment>(environment_scope));
        auto second = take(environments.get<ProjectionEnvironment>(environment_scope));
        assert(first == second && !first->renderer && !first->resources);
        assert(first->assets.source[0] == project->catalogModel().reference({}).project_instance);
        assert(first->version == project->catalogRevision() && first->assets.version == first->version);
        auto fixed = *first; // A Run/compilation captures values, never a live mutable environment.
        for (unsigned i{}; i != 1000; ++i)
        {
            assert(environment_scope.maintain());
            assert(first == second && first->version == fixed.version);
        }
        persistence::WriteCoordinator writes;
        persistence::SaveService saves{writes};
        storage::FileArtifactStore files{root};
        persistence::SaveExecution delivery{execution, saves, writes, files};
        ProjectUpdate update;
        auto prepared = take(project->preparePublication(update));
        ProjectPublicationOperation publication(*project, execution, writes, files, delivery, std::move(prepared));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!publication.terminal())
        {
            assert(std::chrono::steady_clock::now() < deadline);
            assert(execution.collectCompletions());
            publication.update();
            assert(delivery.submitReady());
            std::this_thread::yield();
        }
        assert(std::holds_alternative<PublicationSucceeded>(publication.status()));
        assert(project->catalogRevision() != fixed.version);
        assert(environment_scope.maintain());
        assert(first == second && first->version == project->catalogRevision());
        assert(first->assets.version == first->version && fixed.assets.version == fixed.version);
        assert(first->version != fixed.version && writes.size() == 0);
        std::thread foreign([&]
        {
            const auto refused = environment_scope.maintain();
            assert(!refused && refused.error().code == services::EServiceError::WRONG_THREAD);
        });
        foreign.join();
        assert(first->version == project->catalogRevision());
        assert(environment_scope.beginClose() && environment_scope.release());
        assert(!environment_scope.drained()); // Both views still retain the genuine allocation.
        first.reset();
        assert(second->version == project->catalogRevision());
        second.reset();
        while (!environment_scope.drained())
        {
            assert(messages.collectRetired());
        }
    }
    assert(scope.beginClose() && scope.release());
    service.reset();
    sessions_owner.reset();
    while (!scope.drained())
    {
        assert(messages.collectRetired());
    }
    project->requestClose();
    while (!take(project->advanceClose()))
    {
        assert(execution.collectCompletions());
    }
    std::cout << "Scoped model placement: real pak/Process, one history batch, fixed source, BUSY, no window, capacity "
                 "and cancellation PASS\n";
}
