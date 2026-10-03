#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <lux/engine/editor/persistence/DerivedArtifact.hpp>
#include <lux/engine/editor/storage/ArtifactPublicationOperation.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
#include <lux/engine/editor/material/MaterialSaveSource.hpp>
#include <lux/engine/editor/material/PublishCompiledMaterial.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <cassert>
#include <fstream>
#include <iostream>
#include <source_location>
#include <thread>

using namespace lux;
using namespace lux::editor;
namespace p = lux::editor::persistence;
namespace em = lux::editor::material;
namespace
{
    template<class T> auto take(T result)
    {
        assert(result);
        return std::move(*result);
    }
    struct Faults final : p::IArtifactStore
    {
        storage::FileArtifactStore files;
        p::WriteTargetKey manifest_key;
        bool lose_package{}, fail_manifest{};
        std::size_t package_writes{};
        explicit Faults(const std::filesystem::path& root)
            : files(root), manifest_key(take(files.resolve("Project.luxproject")).key) {}
        p::PersistenceResult<p::WriteTarget> resolve(std::string_view path) override { return files.resolve(path); }
        p::Reconciliation reconcile(const p::PublicationQuery& query) override { return files.reconcile(query); }
        p::VPublicationOutcome publish(const p::PublicationQuery& query, std::stop_token stop) override
        {
            const bool manifest = query.target.key == manifest_key;
            if (manifest && std::exchange(fail_manifest, false))
                return p::NotPublished{{p::EPersistenceError::IO, "injected manifest failure"}};
            auto result = files.publish(query, stop);
            if (query.target.key.value.ends_with(".pak"))
            {
                ++package_writes;
                if (std::exchange(lose_package, false) && std::holds_alternative<p::CommitReceipt>(result))
                    return p::PublicationUnknown{{p::EPersistenceError::IO, "lost transport receipt"}, "actual disk"};
            }
            return result;
        }
    };
}
int main(int argc, char** argv)
{
    assert(argc == 3);
    const auto root = std::filesystem::absolute(argv[1]) /
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root);
    const asset::AssetId id{*uuids::uuid::from_string("12345678-1234-1234-1234-123456789abc")};
    lux::material::MaterialSource source{id, "S0", {}};
    auto constant = std::make_unique<lux::material::ConstantNode>();
    constant->setType(lux::material::EValueType::VEC3);
    const auto node = source.graph.addNode(std::move(constant));
    const auto output = source.graph.addNode(std::make_unique<lux::material::OutputSurfaceNode>());
    assert(source.graph.connect(node, 0, output, 0));
    const auto initial = take(lux::material::encodeMaterialSource(source));
    const auto original_digest = projectContentDigest(std::as_bytes(std::span(initial)));
    ProjectAssetEntry asset{id, "lux.material.source", "author.material", {}, original_digest};
    {
        std::ofstream file(root / "author.material", std::ios::binary);
        file << initial;
        std::ofstream manifest(root / "Project.luxproject", std::ios::binary);
        manifest << take(encodeProjectManifest({id, "Artifacts", {}, {asset}}));
    }
    auto runtime = take(process::ExecutionRuntime::create({1, 64, 64, {32}, process::BlockingSchedulerConfig{1, 64}}));
    auto messages = take(object::ObjectMessageQueue::create(32));
    process::TaskScope tasks{runtime};
    asset::AssetVfs vfs;
    auto prepared = take(prepareProjectOpen(root / "Project.luxproject"));
    auto project = take(ProjectStorage::open(prepared, vfs, *runtime.blocking(), tasks, messages.dispatcherRef()));
    Faults files{root};
    p::WriteCoordinator writes;
    p::SaveService saves{writes};
    p::SaveExecution execution{runtime, saves, writes, files};
    sessions::SessionStore authors{2};
    auto reserved = take(authors.reserve<em::MaterialSession>({"lux.editor.material"}, contracts::CodeLease::builtin()));
    auto candidate = take(em::MaterialSession::create(reserved.id(), sessions::BoundSource{id, "author.material"},
        std::move(source)));
    auto* model = candidate.get();
    assert(authors.prepare(reserved, candidate));
    const auto session = take(authors.publish(reserved));
    const auto wait = [&](auto predicate, std::source_location location = std::source_location::current()) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!predicate())
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                std::cerr << "Timed out at line " << location.line() << '\n';
                std::abort();
            }
            assert(runtime.collectCompletions());
            assert(runtime.dispatchTaskEvents());
            assert(execution.submitReady());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    auto library = std::make_shared<engine::platform::DynamicLibrary>(argv[2]);
    assert(library->is_loaded());
    using Factory = p::IArtifactSource* (*)(const char*, std::size_t);
    auto factory = reinterpret_cast<Factory>(library->get_symbol("makeSource"));
    assert(factory);
    const auto stamp = model->describe().current;
    auto foreign = std::shared_ptr<const p::IArtifactSource>(factory(initial.data(), initial.size()));
    auto cooked = std::make_shared<const std::string>("external arbitrary cooked bytes");
    const auto make_artifact = [&] {
        return p::DerivedArtifact{contracts::CodeLease::plugin(library),
            {stamp, id, "example.ec2.foreign.result", 1, 0x45433258},
            cxx::SharedBytes<>::fromOwner(cooked, std::as_bytes(std::span(*cooked))), foreign};
    };
    auto artifact = make_artifact();
    auto operation = take(ArtifactPublicationOperation::create(artifact, authors, *project, runtime, writes,
        files, execution));
    em::MaterialEditBatch edit{model->describe().current, "Later source", {}};
    edit.edits.emplace_back(em::MaterialRename{"S1"});
    assert(model->apply(std::move(edit)));
    auto stale = make_artifact();
    const auto retained_bytes = stale.bytes();
    auto refused = ArtifactPublicationOperation::create(stale, authors, *project, runtime, writes, files, execution);
    assert(!refused && stale.valid() && stale.bytes().view().data() == retained_bytes.view().data());
    em::MaterialSaveSource save_source{authors.access<em::MaterialSession>(), take(authors.key<em::MaterialSession>(session)),
        take(files.resolve("author.material")), take(em::MaterialPersistenceAccess::inspect(*model)).revision};
    std::optional registration{take(saves.registerSource(save_source))};
    const auto save = take(saves.requestSave({session}));
    wait([&] { saves.adoptCompletions(); return take(saves.status(save)).stage == p::ESaveStage::TERMINAL; });
    const auto saved = take(saves.status(save));
    assert(saved.outcome && saved.outcome->adoption == p::EAdoption::APPLIED);
    const auto new_digest = std::get<p::CommitReceipt>(saved.outcome->publication).version;
    assert(new_digest != original_digest);
    ProjectUpdate update;
    asset.source_digest = new_digest;
    update.assets.push_back(asset);
    {
        ProjectPublicationOperation source_catalog{*project, runtime, writes, files, execution,
            take(project->preparePublication(update))};
        wait([&] { source_catalog.update(); return source_catalog.terminal(); });
        assert(std::holds_alternative<PublicationSucceeded>(source_catalog.status()));
    }
    assert(saves.acknowledge(save));
    const auto author_state = model->describe();
    const auto author_bytes = take(take(model->read()).encode());
    files.lose_package = true;
    files.fail_manifest = true;
    wait([&] { operation->update(); return std::holds_alternative<EditorFailure>(operation->status()); });
    assert(operation->ticket() && take(writes.status(*operation->ticket())).stage == p::EWriteStage::UNKNOWN);
    assert(project->asset(id)->cooked_path.empty());
    assert(std::filesystem::exists(root / operation->path()));
    assert(operation->retry());
    wait([&] { operation->update(); return std::holds_alternative<EditorFailure>(operation->status()); });
    assert(!operation->terminal() && !operation->ticket());
    assert(files.package_writes == 1 && project->asset(id)->source_digest == new_digest);
    assert(model->describe().current == author_state.current && model->describe().dirty == author_state.dirty);
    assert(take(take(model->read()).encode()) == author_bytes);
    registration.reset();
    auto close = take(authors.prepareClose(author_state.current));
    assert(authors.close(close)); // Late publication uses only the accepted immutable source.
    assert(operation->retry());
    wait([&] { operation->update(); return operation->terminal(); });
    assert(std::holds_alternative<PublicationSucceeded>(operation->status()));
    assert(project->asset(id)->source_digest == new_digest && project->asset(id)->compiled_source_digest == original_digest);
    assert(project->catalogAsset(id) && files.package_writes == 1 && writes.size() == 0);
    std::cout << "EC2 external artifact: actual DLL encoder, arbitrary cooked type, source save, Unknown, manifest failure, fixed retry, closed author\n";
}
