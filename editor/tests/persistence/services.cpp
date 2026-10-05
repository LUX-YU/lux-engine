#include <lux/engine/editor/material/MaterialCodec.hpp>
#include <lux/engine/editor/material/MaterialSaveSource.hpp>
#include <lux/engine/editor/persistence/PersistenceServices.hpp>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
#include <lux/engine/editor/storage/PublicationFileStore.hpp>
#include <lux/engine/editor/storage/FilePublication.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>
#include <cassert>
#include <chrono>
#include <iostream>
#include <source_location>
#include <thread>

namespace
{
    using namespace lux;
    using namespace lux::editor;
    namespace p = persistence;
    namespace em = lux::editor::material;

    template <class T> auto take(T result, std::source_location location = std::source_location::current())
    {
        if (!result)
        {
            std::cerr << "Unexpected failure at " << location.line();
            if constexpr (requires { result.error().code; })
                std::cerr << " code=" << static_cast<unsigned>(result.error().code);
            if constexpr (requires { result.error().detail; })
                std::cerr << " detail=" << result.error().detail;
            if constexpr (requires { result.error().domain; })
                std::cerr << " domain=" << result.error().domain;
            std::cerr << '\n';
        }
        assert(result);
        return std::move(*result);
    }
    asset::AssetId identity()
    {
        return asset::AssetId{*uuids::uuid::from_string("8b7f75de-3d9a-45a0-9c6e-ffefeea203b5")};
    }
    void runServices(const std::filesystem::path& root)
    {
        auto runtime = take(process::ExecutionRuntime::create(
            {1, 32, 32, {8}, process::BlockingSchedulerConfig{1, 32}}
        ));
        auto messages = take(object::ObjectMessageQueue::create(32));
        lux::services::ServiceRegistry registry{messages.dispatcherRef()};
        auto scope = take(registry.createScope());
        const auto code = object::CodeLease::builtin();
        const auto roots = std::make_shared<const storage::PublicationRoots>(
            root / "project", root / "personal", root / "installation"
        );
        for (const auto& path : {roots->project, roots->user, roots->installation})
            std::filesystem::create_directories(path);
        assert(registry.publish({
            lux::services::ServiceEntry::bind<p::kWriteCoordinatorService>(code),
            lux::services::ServiceEntry::bind<p::kSaveService>(code),
            lux::services::ServiceEntry::bind<p::kSaveExecutionService>(code),
            lux::services::ServiceEntry::bind<storage::kPublicationFileStoreService>(code, roots)
        }));
        assert(scope.drained()); // Descriptors and file roots do not eagerly create providers.
        const auto missing = registry.get<p::SaveExecution>(scope);
        assert(!missing && missing.error().code == lux::services::EServiceError::NOT_FOUND);
        assert(scope.drained());
        assert(scope.provide(lux::services::ServiceNameView{"lux.process.execution"}, runtime));

        auto execution = take(registry.get<p::SaveExecution>(scope));
        auto saves = take(registry.get<p::SaveService>(scope));
        auto writes = take(registry.get<p::WriteCoordinator>(scope));
        auto files = take(registry.get<p::IArtifactStore>(scope));
        assert(take(registry.get<p::SaveExecution>(scope)) == execution);
        assert(take(registry.get<p::SaveService>(scope)) == saves);
        assert(take(registry.get<p::WriteCoordinator>(scope)) == writes);
        assert(take(registry.get<p::IArtifactStore>(scope)) == files);

        {
            sessions::SessionStore store{messages.dispatcherRef(), 4};
            auto reserved = take(store.reserve<em::MaterialSession>({"lux.editor.material"}, code));
            const auto id = reserved.id();
            lux::material::MaterialSource input{identity(), "Captured", {}};
            assert(input.graph.addNode(std::make_unique<lux::material::ConstantNode>()).valid());
            auto candidate = take(em::MaterialSession::create(
                id, sessions::BoundSource{identity(), "material.luxmaterial"}, std::move(input)
            ));
            auto& model = *candidate;
            assert(store.prepare(reserved, candidate));
            assert(store.publish(reserved));
            em::MaterialSaveSource source{
                store.access<em::MaterialSession>(), take(store.key<em::MaterialSession>(id)),
                take(files->resolve("material.luxmaterial")), sessions::BindingRevision{1}
            };
            auto registration = take(saves->registerSource(source));
            const auto captured = model.describe().current;
            const auto operation = take(saves->requestSave({id}));
            const auto ticket = take(saves->status(operation)).ticket;
            assert(take(writes->status(ticket)).stage == p::EWriteStage::RESERVED);
            em::MaterialEditBatch edit{captured, "Continue editing", {}};
            edit.edits.emplace_back(em::MaterialRename{"Live"});
            assert(model.apply(std::move(edit)));
            for (unsigned turn = 0; turn < 10000 && !take(saves->status(operation)).outcome; ++turn)
            {
                assert(runtime.collectCompletions());
                assert(scope.maintain());
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            const auto completed = take(saves->status(operation));
            assert(completed.outcome && completed.outcome->adoption == p::EAdoption::APPLIED);
            assert(std::holds_alternative<p::CommitReceipt>(completed.outcome->publication));
            const auto encoded = take(storage::readPublicationFile(roots->project / "material.luxmaterial", 1 << 20));
            const auto decoded = take(em::MaterialCodec::decode(encoded));
            assert(decoded.source.name == "Captured");
            assert(model.describe().dirty && model.describe().current != captured);
            assert(model.undo());
            assert(model.describe().current == captured && !model.describe().dirty);
            assert(model.redo() && model.describe().dirty);
            assert(saves->acknowledge(operation));
            assert(writes->size() == 0);
        }
        // The very same execution adapter publishes another admitted producer's ticket.
        const auto bytes = std::as_bytes(std::span("frozen", 6));
        const auto target = take(files->resolve((roots->user / "derived").generic_string()));
        const auto ticket = take(p::publishEncodedArtifact(
            *writes, target, p::EncodedArtifact{std::vector<std::byte>(bytes.begin(), bytes.end())}
        ));
        for (unsigned turn = 0; turn < 10000 && take(writes->status(ticket)).stage != p::EWriteStage::TERMINAL; ++turn)
        {
            assert(runtime.collectCompletions());
            assert(scope.maintain());
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        const auto publication = take(writes->status(ticket));
        assert(publication.outcome && std::holds_alternative<p::CommitReceipt>(*publication.outcome));
        assert(take(storage::readPublicationFile(roots->user / "derived", 64)) ==
               std::vector<std::byte>(bytes.begin(), bytes.end()));
        assert(writes->acknowledge(ticket));
        std::weak_ptr<p::SaveService> weak_saves = saves;
        std::weak_ptr<p::WriteCoordinator> weak_writes = writes;
        std::weak_ptr<p::IArtifactStore> weak_files = files;
        assert(scope.release());
        saves.reset();
        writes.reset();
        files.reset();
        assert(!weak_saves.expired() && !weak_writes.expired() && !weak_files.expired());
        assert(!scope.drained());
        std::thread worker([owned = std::move(execution)]() mutable { owned.reset(); });
        worker.join();
        // The public allocation retires on its original owner; its dependency pins survive the worker release.
        assert(!weak_saves.expired() && !weak_writes.expired() && !weak_files.expired());
        for (unsigned turn = 0; turn < 16 && !registry.drained(); ++turn)
            (void)messages.collectRetired();
        assert(weak_saves.expired() && weak_writes.expired() && weak_files.expired());
        assert(scope.drained() && registry.drained());
        // Invalid definition input is rejected on demand without constructing a fallback backend.
        assert(registry.publish({lux::services::ServiceEntry::bind<storage::kPublicationFileStoreService>(
            code, std::make_shared<const storage::PublicationRoots>()
        )}));
        auto invalid_scope = take(registry.createScope());
        assert(invalid_scope.drained());
        const auto invalid = registry.get<p::IArtifactStore>(invalid_scope);
        assert(!invalid && invalid.error().code == lux::services::EServiceError::INVALID_CONFIGURATION);
        assert(invalid_scope.drained());
        assert(invalid_scope.release());
        assert(registry.drained());
        std::cout << "PASS declared save services: lazy, shared coordinator, real Material/IO, continued edits, "
                     "derived publication, owner retirement and retained dependencies\n";
    }
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    const auto root = std::filesystem::absolute(argv[1]) /
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    runServices(root);
}
