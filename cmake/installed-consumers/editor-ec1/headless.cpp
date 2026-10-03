#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <lux/engine/editor/material/MaterialSessionFactory.hpp>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
#include <lux/engine/editor/editing/EditExecutor.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <fstream>
#include <cassert>
#include <thread>
#include <iostream>

using namespace lux;
using namespace lux::editor;
namespace
{
    template <class T> auto take(T value)
    {
        assert(value);
        return std::move(*value);
    }
    void write(const std::filesystem::path& path, std::string_view bytes)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        assert(file.write(bytes.data(), static_cast<std::streamsize>(bytes.size())));
    }
}
int main(int argc, char** argv)
{
    assert(argc == 2);
    const auto root = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(root / "Content/Beginner");
    const auto id = asset::AssetId{*uuids::uuid::from_string("8278afad-6d18-4135-af0a-5421e870d7af")};
    lux::material::MaterialSource original{id, "before", {}};
    const auto node = original.graph.addNode(std::make_unique<lux::material::ConstantNode>());
    assert(node.valid());
    const auto bytes = take(lux::material::encodeMaterialSource(original));
    const std::string path = "Content/Beginner/material.luxmaterial";
    write(root / path, std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
    ProjectManifest manifest{id, "Primitives"};
    manifest.assets.push_back({id, "lux.material.source", path});
    write(root / "Project.luxproject", take(encodeProjectManifest(manifest)));
    auto runtime = take(process::ExecutionRuntime::create({
        .cpu_concurrency = 2,
        .cpu_queue_capacity = 16,
        .timer = {16},
        .blocking = process::BlockingSchedulerConfig{1, 8}
    }));
    process::TaskScope tasks{runtime};
    auto messages = take(object::ObjectMessageQueue::create(32));
    asset::AssetVfs vfs;
    auto data = take(prepareProjectOpen(root / "Project.luxproject"));
    auto project = take(ProjectStorage::open(data, vfs, take(runtime.blocking()), tasks, messages.dispatcherRef()));
    const auto catalog = take(project->catalogModel().snapshot());
    const auto* entry = catalog.find(id);
    assert(entry && catalog.assets().size() == 1 && catalog.find(id) == entry);
    persistence::WriteCoordinator writes;
    persistence::SaveService saves{writes};
    storage::FileArtifactStore disk{root};
    const auto target = take(disk.resolve(path));
    auto input = take(project->captureSource(id, 1024 * 1024, target.expected_version));
    sessions::SessionLoadJob job{
        lux::editor::material::makeMaterialSessionFactory(),
        {std::move(input), id, sessions::BoundSource{id, target.key.value}, target}
    };
    const auto owner = std::this_thread::get_id();
    std::optional<sessions::SessionPreparation> ready;
    assert(tasks.submit(
        {.name = "Read frozen author source"},
        [scheduler = take(runtime.blocking()), job = std::move(job), owner](process::TaskReporter reporter) mutable noexcept {
            return stdexec::then(stdexec::schedule(scheduler), [job = std::move(job), owner,
                stop = reporter.stopToken()]() mutable {
                assert(std::this_thread::get_id() != owner);
                return std::move(job).run(stop);
            });
        },
        [&](process::TTaskResult<sessions::SessionPreparation, sessions::SessionFactoryFailure>&& completed) noexcept {
            assert(std::this_thread::get_id() == owner && completed);
            ready.emplace(std::move(*completed));
        }
    ));
    assert(tasks.join() && ready);
    sessions::SessionStore sessions{4};
    auto preparation = take(std::move(*ready).prepare(sessions, saves));
    auto installed = take(preparation.publish());
    const auto key = take(sessions.key<lux::editor::material::MaterialSession>(installed.id()));
    auto& model = take(sessions.access<lux::editor::material::MaterialSession>().edit(key)).get();
    lux::editor::material::MaterialEditBatch batch{model.describe().current, "rename", {}};
    batch.edits.emplace_back(lux::editor::material::MaterialRename{"after"});
    assert(model.apply(std::move(batch)) && installed.undo() && installed.redo());
    const auto save = take(saves.requestSave({installed.id()}));
    persistence::SaveExecution execution{runtime, saves, writes, disk};
    for (unsigned i{}; i != 10000; ++i)
    {
        assert(execution.submitReady() && runtime.collectCompletions());
        saves.adoptCompletions();
        if (take(saves.status(save)).stage == persistence::ESaveStage::TERMINAL)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const auto saved = take(saves.status(save));
    assert(saved.outcome && saved.outcome->adoption == persistence::EAdoption::APPLIED);
    assert(!take(sessions.describe(installed.id())).dirty);
    std::ifstream file(root / path, std::ios::binary);
    const std::string encoded{std::istreambuf_iterator<char>(file), {}};
    const auto decoded = take(lux::editor::material::MaterialCodec::decode(std::as_bytes(std::span{encoded})));
    assert(decoded.source.name == "after" && decoded.source.graph.node(node));
    assert(saves.acknowledge(save) && execution.tasks().join());
    assert(installed.close(take(sessions.describe(installed.id())).current));
    project->requestClose();
    assert(take(project->advanceClose()));
    std::cout << "PASS installed shared catalog/read/Process/session/history/save providers without UI\n";
}
