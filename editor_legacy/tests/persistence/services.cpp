#include <lux/engine/editor/material/MaterialCodec.hpp>
#include <lux/engine/editor/material/MaterialSaveSource.hpp>
#include <lux/engine/editor/material/MaterialSessionFactory.hpp>
#include <lux/engine/editor/sessions/SessionOpening.hpp>
#include <lux/engine/editor/sessions/SessionServices.hpp>
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
    class MaterialFile final : public asset::IAssetProvider
    {
    public:
        explicit MaterialFile(std::filesystem::path file) : file_(std::move(file)) {}
        std::optional<asset::AssetId> resolve(std::string_view path) const override
        {
            return path == "material.luxmaterial" ? std::optional{identity()} : std::nullopt;
        }
        bool contains(const asset::AssetId& id) const override { return id == identity(); }
        cxx::expected<asset::AssetBlob, asset::EAssetStorageError>
        open(const asset::AssetId& id, std::size_t max_bytes) const override
        {
            assert(std::this_thread::get_id() != owner_);
            if (!contains(id))
                return cxx::unexpected(asset::EAssetStorageError::NOT_FOUND);
            auto bytes = storage::readPublicationFile(file_, max_bytes);
            if (!bytes)
                return cxx::unexpected(asset::EAssetStorageError::IO_FAILURE);
            return asset::AssetBlob::fromShared(cxx::SharedBytes<>::copyOf(*bytes));
        }
        void enumerate(const std::function<void(const asset::ProviderEntry&)>& receiver) const override
        {
            receiver({identity(), 0, "material.luxmaterial"});
        }
        std::optional<std::string> pathOf(const asset::AssetId& id) const override
        {
            return contains(id) ? std::optional<std::string>{"material.luxmaterial"} : std::nullopt;
        }
    private:
        std::filesystem::path file_;
        std::thread::id owner_{std::this_thread::get_id()};
    };
    void runServices(const std::filesystem::path& root)
    {
        auto runtime = take(process::ExecutionRuntime::create(
            {1, 32, 32, {8}, process::BlockingSchedulerConfig{1, 32}}
        ));
        auto messages = take(object::ObjectMessageQueue::create(32));
        lux::services::ServiceRegistry registry{messages.dispatcherRef()};
        auto scope = take(registry.createScope());
        const auto code = object::CodeLease::builtin();
        auto roots = std::make_shared<const storage::PublicationRoots>(
            root / "project",
            root / "personal",
            root / "installation"
        );
        for (const auto& path : {roots->project, roots->user, roots->installation})
            std::filesystem::create_directories(path);
        assert(registry.publish(
            {lux::services::ServiceEntry::bind<p::kWriteCoordinatorService>(code),
             lux::services::ServiceEntry::bind<p::kSaveService>(code),
             lux::services::ServiceEntry::bind<p::kSaveExecutionService>(code),
             lux::services::ServiceEntry::bind<sessions::kSessionStoreService>(code),
             lux::services::ServiceEntry::bind<sessions::kSessionOpeningService>(code),
             lux::services::ServiceEntry::bind<storage::kPublicationFileStoreService>(code)}
        ));
        assert(scope.drained()); // Descriptors and file roots do not eagerly create providers.
        const auto missing = registry.get<p::SaveExecution>(scope);
        assert(!missing && missing.error().code == lux::services::EServiceError::NOT_FOUND);
        assert(scope.drained());
        assert(scope.provide(lux::services::ServiceNameView{"lux.process.execution"}, runtime));
        assert(scope.provide(lux::services::ServiceNameView{"lux.services.registry"}, registry));
        assert(scope.provide(lux::services::ServiceNameView{"lux.services.scope"}, scope));

        assert(scope.provide(lux::services::ServiceNameView{"lux.editor.publication.roots"}, roots));
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
        // Real asynchronous file opening through the public declaration, with no UI or project owner.
        auto opening = take(registry.get<sessions::SessionOpening>(scope));
        auto store = take(registry.get<sessions::SessionStore>(scope));
        using MaterialAccess = sessions::TSessionAccess<em::MaterialSession>;
        const auto absent_store = MaterialAccess::create({});
        assert(!absent_store && absent_store.error() == sessions::ESessionError::INVALID_ARGUMENT);
        std::optional<MaterialAccess> retained_access{take(MaterialAccess::create(store))};
        assert(take(registry.get<sessions::SessionOpening>(scope)) == opening);
        assert(take(registry.get<sessions::SessionStore>(scope)) == store);
        {
            asset::AssetVfs vfs;
            assert(vfs.mount({"/sources", std::make_shared<MaterialFile>(roots->project / "material.luxmaterial")}));
            const auto factories = take(sessions::SessionFactorySnapshot::create({em::makeMaterialSessionFactory()}));
            sessions::OpenAssetRequest request{
                17, {"lux.editor.material"},
                {vfs.view().capture(), identity(), sessions::BoundSource{identity(), "material.luxmaterial"},
                 take(files->resolve("material.luxmaterial"))}
            };
            const auto cancelled = take(opening->open(request, factories));
            const auto wanted = take(opening->open(request, factories));
            assert(cancelled != wanted && opening->cancel(cancelled));
            for (unsigned turn = 0; turn < 10000 && !opening->settled(); ++turn)
            {
                assert(runtime.collectCompletions());
                assert(scope.maintain());
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            const auto loaded = take(opening->status(wanted));
            assert(loaded.stage == sessions::EOpenAssetStage::PUBLISHED && store->size() == 1);
            assert(take(opening->status(cancelled)).cancellation_requested);
            const auto reused = take(opening->open(request, factories));
            assert(take(opening->status(reused)).reused && take(opening->status(reused)).session == loaded.session);
            auto model = take(store->share(take(store->key<em::MaterialSession>(loaded.session))));
            const auto initial = model->describe();
            assert(!initial.dirty);
            em::MaterialEditBatch edit{initial.current, "Opened edit", {}};
            edit.edits.emplace_back(em::MaterialRename{"Opened and saved"});
            assert(model->apply(std::move(edit)));
            const auto saved = take(saves->requestSave({loaded.session}));
            for (unsigned turn = 0; turn < 10000 && !take(saves->status(saved)).outcome; ++turn)
            {
                assert(runtime.collectCompletions());
                assert(scope.maintain());
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            assert(take(saves->status(saved)).outcome->adoption == p::EAdoption::APPLIED && !model->describe().dirty);
            assert(saves->acknowledge(saved));
            assert(opening->find(loaded.session)->close(model->describe().current));
            assert(!store->describe(loaded.session) && store->size() == 0);
            const auto stale = retained_access->key(loaded.session);
            assert(!stale && stale.error() == sessions::ESessionError::STALE_SESSION);
            assert(opening->acknowledge(cancelled) && opening->acknowledge(wanted) && opening->acknowledge(reused));
            assert(scope.maintain());
            opening->requestStop();
            assert(opening->settled());
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
        std::weak_ptr<sessions::SessionStore> weak_store = store;
        assert(scope.release());
        saves.reset();
        writes.reset();
        files.reset();
        store.reset();
        assert(!weak_saves.expired() && !weak_writes.expired() && !weak_files.expired());
        assert(!scope.drained());
        std::thread worker([owned = std::move(execution), reading = std::move(opening)]() mutable
        {
            reading.reset();
            owned.reset();
        });
        worker.join();
        // The public allocation retires on its original owner; its dependency pins survive the worker release.
        assert(!weak_saves.expired() && !weak_writes.expired() && !weak_files.expired());
        assert(!weak_store.expired());
        for (unsigned turn = 0; turn < 16 && !registry.drained(); ++turn)
            (void)messages.collectRetired();
        assert(weak_saves.expired() && weak_writes.expired() && weak_files.expired());
        // A typed window/role access retains only this exact Store allocation, not a closed SessionId.
        assert(!weak_store.expired() && !scope.drained());
        retained_access.reset();
        for (unsigned turn = 0; turn < 16 && !registry.drained(); ++turn)
            (void)messages.collectRetired();
        assert(weak_store.expired());
        assert(scope.drained() && registry.drained());
        // Invalid root input is rejected on demand without constructing a fallback backend.
        assert(registry.publish({lux::services::ServiceEntry::bind<storage::kPublicationFileStoreService>(code)}));
        auto invalid_scope = take(registry.createScope());
        auto invalid_roots = std::make_shared<const storage::PublicationRoots>();
        assert(invalid_scope.provide(lux::services::ServiceNameView{"lux.editor.publication.roots"}, invalid_roots));
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
