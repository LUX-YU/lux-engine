#include <lux/engine/editor/storage/ProjectPublicationOperation.hpp>
#include <lux/engine/editor/assets/AssetImporter.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <cassert>
#include <fstream>
#include <iostream>
#include <thread>

namespace p = lux::editor::persistence;
namespace
{
    // Real file publication followed by an injected uncertain transport result.
    struct LostReceipt final : p::IArtifactStore
    {
        lux::editor::storage::FileArtifactStore file;
        bool lose{};
        explicit LostReceipt(std::filesystem::path root) : file(std::move(root)) {}
        p::PersistenceResult<p::WriteTarget> resolve(std::string_view address) override
        {
            return file.resolve(address);
        }
        p::VPublicationOutcome publish(const p::PublicationQuery& work, std::stop_token stop) override
        {
            auto result = file.publish(work, stop);
            if (std::exchange(lose, false) && std::holds_alternative<p::CommitReceipt>(result))
                return p::PublicationUnknown{{p::EPersistenceError::IO, "receipt transport"}, "published"};
            return result;
        }
        p::Reconciliation reconcile(const p::PublicationQuery& work) override
        {
            return file.reconcile(work);
        }
    };
    auto bytes(std::string text)
    {
        auto owner = std::make_shared<const std::string>(std::move(text));
        return lux::cxx::SharedBytes<>::fromOwner(owner, std::as_bytes(std::span(*owner)));
    }
}
int main(int argc, char** argv)
{
    using namespace lux;
    using namespace lux::editor;
    assert(argc == 2);
    const auto root = std::filesystem::absolute(argv[1]) /
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root);
    const auto id = asset::AssetId{*uuids::uuid::from_string("57279371-1b9c-40d6-b55d-a1a12065d932")};
    const auto path = root / "Project.luxproject";
    {
        std::ofstream file(path);
        file << *encodeProjectManifest({id, "Coordinated project", {}, {}});
    }
    auto runtime = process::ExecutionRuntime::create({1, 64, 64, {32}, process::BlockingSchedulerConfig{1, 64}});
    assert(runtime);
    auto messages = object::ObjectMessageQueue::create(32);
    assert(messages);
    process::TaskScope tasks{*runtime};
    auto source = readProjectOpenData(path);
    assert(source);
    asset::AssetVfs assets;
    auto opened = ProjectStorage::open(*source, assets, *runtime->blocking(), tasks, messages->dispatcherRef());
    assert(opened);
    auto project = std::move(*opened);
    LostReceipt files{root};
    p::WriteCoordinator writes;
    p::SaveService saves{writes};
    p::SaveExecution execution{*runtime, saves, writes, files};
    const auto advance = [&](ProjectPublicationOperation& operation, auto done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done())
        {
            assert(std::chrono::steady_clock::now() < deadline);
            assert(runtime->collectCompletions());
            operation.update();
            assert(execution.submitReady());
            std::this_thread::yield();
        }
    };
    const std::vector<ProjectPluginEntry> selection{{"runtime.example", 1, "Plugins/Runtime.json"}};
    {
        ProjectUpdate update;
        update.plugins = selection;
        update.files.push_back({"Content/Package/source-v1.txt", "missing", bytes("immutable source"), true});
        auto publication = project->preparePublication(update);
        assert(publication);
        ProjectPublicationOperation operation{*project, *runtime, writes, files, execution, std::move(*publication)};
        ProjectUpdate competitor;
        assert(!project->preparePublication(competitor));
        advance(operation, [&] { return operation.terminal(); });
        assert(std::holds_alternative<PublicationSucceeded>(operation.status()));
        assert(project->manifest().plugins == selection && writes.size() == 0);
        assert(
            *projectFileDigest(root / "Content/Package/source-v1.txt") ==
            projectContentDigest(bytes("immutable source").view())
        );
    }
    const auto before = *projectFileDigest(path);
    {
        ProjectUpdate update;
        update.plugins.emplace();
        auto publication = project->preparePublication(update);
        assert(publication);
        ProjectPublicationOperation operation{*project, *runtime, writes, files, execution, std::move(*publication)};
        operation.abandon();
        advance(operation, [&] { return operation.terminal(); });
        assert(std::holds_alternative<PublicationAbandoned>(operation.status()));
        assert(*projectFileDigest(path) == before && project->manifest().plugins == selection);
    }
    {
        ProjectUpdate update;
        update.plugins.emplace();
        auto publication = project->preparePublication(update);
        assert(publication);
        ProjectPublicationOperation operation{*project, *runtime, writes, files, execution, std::move(*publication)};
        files.lose = true;
        advance(operation, [&] { return std::holds_alternative<EditorFailure>(operation.status()); });
        assert(!operation.terminal() && operation.ticket());
        assert(writes.status(*operation.ticket())->stage == p::EWriteStage::UNKNOWN);
        assert(project->manifest().plugins == selection); // Disk changed; live adoption has not happened.
        auto target = files.resolve("Project.luxproject");
        assert(target);
        auto later = writes.reserve(*target, {});
        assert(later && writes.provideEncoded(*later, {bytes("must not overtake unknown")}));
        const auto blocked = writes.takeReady();
        assert(blocked && !*blocked);
        assert(writes.cancelBeforePublish(*later, {p::EPersistenceError::CANCELLED}));
        assert(writes.acknowledge(*later));
        assert(operation.retry());
        advance(operation, [&] { return operation.terminal(); });
        assert(std::holds_alternative<PublicationSucceeded>(operation.status()));
        assert(project->manifest().plugins.empty() && writes.size() == 0);
    }
    {
        ProjectUpdate update;
        update.files.push_back(
            {"Content/Package/source-v1.txt",
             projectContentDigest(bytes("immutable source").view()),
             bytes("invalid overwrite"),
             false}
        );
        auto publication = project->preparePublication(update);
        assert(publication);
        ProjectPublicationOperation operation{*project, *runtime, writes, files, execution, std::move(*publication)};
        assert(std::holds_alternative<EditorFailure>(operation.status()));
        assert(!operation.retry());
        operation.update();
        assert(!operation.ticket() && writes.size() == 0);
        assert(
            *projectFileDigest(root / "Content/Package/source-v1.txt") ==
            projectContentDigest(bytes("immutable source").view())
        );
        operation.abandon();
        operation.update();
        assert(operation.terminal());
    }
    {
        assets::AssetImporter importer{*project, *runtime, writes, files, execution};
        const auto input = root / "triangle.obj";
        {
            std::ofstream obj(input);
            obj << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        }
        auto importing = importer.requestModel({id, input, "Content/Beginner/Triangle", {}});
        assert(importing);
        const auto await_import = [&](assets::AssetImportId request) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            for (;;)
            {
                assert(std::chrono::steady_clock::now() < deadline);
                assert(runtime->collectCompletions());
                importer.update();
                assert(execution.submitReady());
                auto state = importer.status(request);
                assert(state);
                if (auto* error = std::get_if<EditorFailure>(&*state))
                {
                    std::cerr << error->domain << ": " << error->message << '\n';
                    if (const auto* outcome = std::any_cast<p::VPublicationOutcome>(&error->cause))
                        std::visit(
                            [](const auto& value) {
                                if constexpr (requires { value.failure; })
                                    std::cerr << "Publication error " << unsigned(value.failure.code) << " native "
                                              << value.failure.native_code << ": " << value.failure.detail << '\n';
                            },
                            *outcome
                        );
                }
                assert(!std::holds_alternative<EditorFailure>(*state));
                if (std::holds_alternative<assets::AssetImportSucceeded>(*state))
                    break;
                std::this_thread::yield();
            }
        };
        await_import(*importing);
        assert(project->asset(id));
        const auto first = *project->asset(id);
        const auto first_recipe = *projectFileDigest(root / first.source_path);
        const auto first_cooked = *projectFileDigest(root / first.cooked_path);
        assert(importer.acknowledge(*importing));
        {
            std::ofstream obj(input);
            obj << "v 0 0 0\nv 2 0 0\nv 0 2 0\nf 1 2 3\n";
        }
        auto replacing = importer.reimportModel(id, input);
        assert(replacing);
        await_import(*replacing);
        const auto second = *project->asset(id);
        assert(second.source_path != first.source_path && second.cooked_path != first.cooked_path);
        assert(*projectFileDigest(root / first.source_path) == first_recipe);
        assert(*projectFileDigest(root / first.cooked_path) == first_cooked);
        assert(importer.acknowledge(*replacing));
        auto captured = importer.reimportModel(id);
        assert(captured);
        await_import(*captured);
        assert(project->asset(id)->source_path == second.source_path);
        assert(importer.acknowledge(*captured));
        importer.requestClose();
        assert(importer.closeStatus().state == assets::EAssetImportCloseState::CLOSED && writes.size() == 0);
    }
    {
        ProjectUpdate update;
        update.plugins = selection;
        auto publication = project->preparePublication(update);
        assert(publication);
        {
            std::ofstream external(path, std::ios::app);
            external << "\n# external conflict\n";
        }
        ProjectPublicationOperation operation{*project, *runtime, writes, files, execution, std::move(*publication)};
        advance(operation, [&] { return std::holds_alternative<EditorFailure>(operation.status()); });
        assert(!operation.terminal() && project->manifest().plugins.empty());
        assert(operation.retry());
        advance(operation, [&] { return std::holds_alternative<EditorFailure>(operation.status()); });
        assert(project->manifest().plugins.empty()); // Retry retains the captured precondition.
        operation.abandon();
        advance(operation, [&] { return operation.terminal(); });
        assert(writes.size() == 0);
    }
    project->requestClose();
    const auto closed = project->advanceClose();
    assert(closed && *closed);
    std::cout << "Real project files: same coordinator, manifest-last adoption, cancellation, Unknown lane and "
                 "retained conflict PASS\n";
}
