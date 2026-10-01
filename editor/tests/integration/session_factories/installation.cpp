#include <lux/engine/editor/scene/SceneSessionFactory.hpp>
#include <lux/engine/editor/material/MaterialSessionFactory.hpp>
#include <lux/engine/editor/material/MaterialSaveSource.hpp>
#include <lux/engine/editor/flowforge/FlowSessionFactory.hpp>
#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
#include <lux/engine/editor/extensions/BuiltinContributions.hpp>
#include <lux/engine/editor/desktop/CommandMenu.hpp>
#include <lux/engine/editor/desktop/ViewHost.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <cassert>
#include <fstream>
#include <iostream>
#include <thread>
#include <array>
using namespace lux;
using namespace lux::editor;
using namespace lux::editor::sessions;
using namespace lux::editor::persistence;
namespace es = lux::editor::scene;
namespace em = lux::editor::material;
namespace ef = lux::editor::flowforge;
namespace
{
    class CommandRoot final : public ui::Root
    {
    public:
        explicit CommandRoot(object::ObjectDispatcherRef dispatcher) : Root(dispatcher)
        {
            assert(initialize({}));
        }
        desktop::CommandMenu* menu{};

    private:
        void event(object::EventView& event) noexcept override
        {
            if (auto* request = event.getIf<ui::MenuRequest>(); request && menu)
            {
                menu->receive(*request);
                event.accept();
            }
        }
    };
    template <class T> auto take(T value)
    {
        if (!value)
        {
            if constexpr (requires { value.error().code; })
                std::cerr << "error=" << int(value.error().code) << '\n';
            if constexpr (requires { value.error().detail; })
                std::cerr << value.error().detail << '\n';
            std::abort();
        }
        return std::move(*value);
    }
    asset::AssetId identity(std::string_view value)
    {
        return asset::AssetId{
            uuids::uuid_name_generator(*uuids::uuid::from_string("12345678-1234-1234-1234-123456789abc"))(value)
        };
    }
    void write(const std::filesystem::path& path, std::span<const std::byte> bytes)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        file.close();
        assert(file);
    }
    std::vector<std::byte> read(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        std::string text{std::istreambuf_iterator<char>(file), {}};
        auto bytes = std::as_bytes(std::span{text});
        return {bytes.begin(), bytes.end()};
    }
    class SourceFiles final : public asset::IAssetProvider
    {
    public:
        explicit SourceFiles(std::filesystem::path root) : root_(std::move(root)) {}
        std::optional<asset::AssetId> resolve(std::string_view path) const override
        {
            for (const auto name : names)
                if (path == name)
                    return identity(name);
            return {};
        }
        bool contains(const asset::AssetId& id) const override
        {
            return pathOf(id).has_value();
        }
        cxx::expected<asset::AssetBlob, asset::EAssetStorageError> open(const asset::AssetId& id) const override
        {
            assert(std::this_thread::get_id() != owner_);
            const auto path = pathOf(id);
            if (!path)
                return cxx::unexpected(asset::EAssetStorageError::NOT_FOUND);
            return asset::AssetBlob::fromShared(cxx::SharedBytes<>::copyOf(read(root_ / *path)));
        }
        void enumerate(const std::function<void(const asset::ProviderEntry&)>& fn) const override
        {
            for (const auto name : names)
                fn({identity(name), 0, std::string(name)});
        }
        std::optional<std::string> pathOf(const asset::AssetId& id) const override
        {
            for (const auto name : names)
                if (id == identity(name))
                    return std::string(name);
            return {};
        }
        static constexpr std::array names{"scene.pak", "material.luxmaterial", "flow.luxflow"};

    private:
        std::thread::id owner_{std::this_thread::get_id()};
        std::filesystem::path root_;
    };
    class ReentrantSource final : public ISaveSource
    {
    public:
        ReentrantSource(ISaveSource& source, SessionStore& store, SaveService& saves, PreparedSessionData& ready)
            : source_(source), store_(store), saves_(saves), ready_(ready)
        {}
        PersistenceResult<SaveSourceInfo> describe() const override
        {
            const auto refused = std::move(ready_).prepare(store_, saves_);
            assert(!refused && refused.error().code == ESessionFactoryError::BUSY);
            assert(!saves_.canPrepareSource()); // Inner preflight did not release outer dispatch.
            return source_.describe();
        }
        PersistenceResult<FrozenSave> captureForSave(
            const SaveSourceInfo& info,
            const SaveRequest& request,
            std::size_t bytes
        ) override
        {
            return source_.captureForSave(info, request, bytes);
        }
        EAdoption accept(SaveReceipt&& receipt) noexcept override
        {
            return source_.accept(std::move(receipt));
        }

    private:
        ISaveSource& source_;
        SessionStore& store_;
        SaveService& saves_;
        PreparedSessionData& ready_;
    };
    // Real material roles exercise each public preparation boundary; no alternate author/history model.
    class MaterialHistory final : public HistoryActions
    {
    public:
        MaterialHistory(TSessionAccess<em::MaterialSession> access, TSessionKey<em::MaterialSession> key)
            : access_(access), key_(key)
        {}
        SessionFactoryResult<HistoryActionsInfo> query() const override
        {
            auto model = access_.read(key_);
            if (!model)
                return cxx::unexpected(factoryFailure(model.error()));
            auto history = take(model->get().historyView());
            return HistoryActionsInfo{{key_.id(), history.snapshot.current}, history.can_undo, history.can_redo};
        }
        SessionFactoryResult<ContentStamp> undo() override
        {
            auto& model = take(access_.edit(key_)).get();
            assert(model.undo());
            return model.describe().current;
        }
        SessionFactoryResult<ContentStamp> redo() override
        {
            auto& model = take(access_.edit(key_)).get();
            assert(model.redo());
            return model.describe().current;
        }

    private:
        TSessionAccess<em::MaterialSession> access_;
        TSessionKey<em::MaterialSession> key_;
    };
    void fixedCommandPayload(SessionStore& store, em::MaterialSession& model, lux::material::NodeId node)
    {
        using namespace commands;
        struct Selection final
        {
            std::vector<lux::material::NodeId> nodes;
        };
        CommandRegistry registry;
        CommandDispatcher dispatcher{registry};
        auto action = std::make_shared<CommandEntry>(
            contracts::CodeLease::builtin(),
            CommandDescriptor{
                CommandId{"test.delete"},
                "Delete",
                "Edit",
                "",
                ECommandScope::SESSION,
                1,
                cxx::typeToken<Selection>()
            },
            [&store](const CommandQuery& input) -> CommandResult<CommandState> {
                const auto& target = std::get<SessionTarget>(input.target);
                auto state = store.describe(target.id);
                if (!state)
                    return cxx::unexpected(CommandFailure{ECommandError::STALE_TARGET, "session"});
                if (state->admission != EEditAdmission::AVAILABLE)
                    return cxx::unexpected(CommandFailure{ECommandError::BUSY, "session"});
                if (target.based_on && *target.based_on != state->current)
                    return cxx::unexpected(CommandFailure{ECommandError::STALE_CONTENT, "material"});
                return CommandState{true};
            },
            [&model](const CommandInvocation& input) -> CommandResult<DispatchReceipt> {
                const auto& target = std::get<SessionTarget>(input.target());
                em::MaterialEditBatch batch{target.based_on.value_or(model.describe().current), "delete", {}};
                for (auto id : static_cast<const Selection*>(input.arguments().data())->nodes)
                    batch.edits.emplace_back(em::MaterialEraseNode{id});
                auto result = model.apply(std::move(batch));
                if (!result)
                    return cxx::unexpected(CommandFailure{
                        ECommandError::DOMAIN_FAILURE,
                        "material",
                        static_cast<std::uint64_t>(result.error().code)
                    });
                return DispatchReceipt{ImmediateCompletion{}};
            }
        );
        auto catalog = take(CommandRegistrySnapshot::create({action}));
        assert(registry.publish(catalog));
        auto handle = take(catalog.find(CommandIdView{"test.delete"}));
        std::vector<lux::material::NodeId> selection{node};
        const auto original = model.describe().current;
        auto owned = std::make_shared<const Selection>(Selection{selection});
        CommandArguments args{contracts::CodeLease::builtin(), cxx::typeToken<Selection>(), owned};
        CommandInvocation strict{SessionTarget{model.describe().id, original}, args};
        assert(dispatcher.enqueue(handle, strict));
        owned.reset();
        args = {};
        strict = CommandInvocation{};
        selection.clear();
        em::MaterialEditBatch later{original, "later author edit", {}};
        later.edits.emplace_back(em::MaterialRename{"later"});
        assert(model.apply(std::move(later)));
        const auto before = model.describe();
        const auto encoded = take(take(model.read()).encode());
        const auto history = take(model.historyView()).snapshot;
        auto refused = take(dispatcher.drain());
        assert(refused.size() == 1 && !refused[0].result);
        assert(refused[0].result.error().code == ECommandError::STALE_CONTENT);
        assert(encoded == take(take(model.read()).encode()));
        const auto after = model.describe();
        assert(after.current == before.current && after.observed == before.observed && after.dirty == before.dirty);
        assert(after.binding == before.binding && take(model.historyView()).snapshot.revision == history.revision);
        // An identity-only delete uses the captured set, never the current selection.
        owned = std::make_shared<const Selection>(Selection{{node}});
        CommandInvocation erase{
            SessionTarget{model.describe().id},
            CommandArguments{contracts::CodeLease::builtin(), cxx::typeToken<Selection>(), owned}
        };
        assert(dispatcher.enqueue(handle, erase));
        owned.reset();
        erase = CommandInvocation{};
        selection.push_back(lux::material::NodeId{});
        auto completed = take(dispatcher.drain());
        assert(completed.size() == 1 && completed[0].result);
        assert(take(model.capture()).source().graph.nodes().empty());
        assert(model.undo() && model.undo() && model.describe().current == original);
        std::cout << "PASS real model strict source conflict and owned original selection\n";
    }
    void installationStages(std::span<const std::byte> bytes)
    {
        SessionStore store{1};
        WriteCoordinator writes;
        SaveService saves{writes};
        for (unsigned boundary{}; boundary != 8; ++boundary)
        {
            SessionId id;
            unsigned released{};
            const auto exercise = [&] {
                auto owner = std::shared_ptr<const void>(new int{}, [&](const void* p) {
                    ++released;
                    delete static_cast<const int*>(p);
                });
                auto code = contracts::CodeLease::plugin(owner);
                auto decoded = take(em::MaterialCodec::decode(bytes));
                if (boundary == 0)
                    return;
                auto reservation = take(store.reserve<em::MaterialSession>({"lux.editor.material"}, code));
                id = reservation.id();
                assert(!store.describe(id) && !saves.requestSave({id}));
                if (boundary == 1)
                    return;
                auto model = take(std::move(decoded).createSession(id, {}, code));
                assert(model->historyView());
                if (boundary == 2)
                    return;
                assert(store.prepare(reservation, model));
                assert(!model && !store.describe(id));
                if (boundary == 3)
                    return;
                auto key = take(store.key<em::MaterialSession>(reservation));
                auto history = std::make_unique<MaterialHistory>(store.access<em::MaterialSession>(), key);
                assert(!history->query());
                if (boundary == 4)
                    return;
                auto source = std::make_unique<em::MaterialSaveSource>(
                    store.access<em::MaterialSession>(),
                    key,
                    std::nullopt,
                    BindingRevision{1}
                );
                assert(!source->describe());
                if (boundary == 5)
                    return;
                auto prepared = take(PreparedSessionInstallation::prepare(
                    store,
                    saves,
                    std::move(reservation),
                    code,
                    std::move(history),
                    std::move(source)
                ));
                assert(!store.describe(id) && !saves.requestSave({id}));
                if (boundary == 6)
                    return;
                auto installed = take(prepared.publish());
                assert(store.size() == 1 && store.describe(id));
                auto messages = take(object::ObjectMessageQueue::create(4));
                struct Notice final : object::LuxObject
                {
                    using LuxObject::LuxObject;
                    using LuxObject::emit;
                    object::TSignal<> installed{*this};
                } notice{messages.dispatcherRef()};
                object::LuxObject receiver{messages.dispatcherRef()};
                auto connection = take(object::LuxObject::connect(
                    &notice,
                    &Notice::installed,
                    &receiver,
                    []() noexcept { std::abort(); },
                    object::EDelivery::QUEUED
                ));
                messages.close();
                const auto notification = notice.emit(notice.installed);
                assert(notification.closed == 1 && store.describe(id) && installed.queryHistory());
                assert(installed.close(take(store.describe(id)).current));
            };
            exercise();
            assert(released == 1 && store.size() == 0 && !store.describe(id) && !saves.requestSave({id}));
            assert(store.canReserve());
        }
        std::cout << "PASS eight real decode/installation/role/publication boundaries and closed notification\n";
    }
    SessionFactoryResult<PreparedSessionData> load(
        process::ExecutionRuntime& runtime,
        std::shared_ptr<SessionFactoryEntry> factory,
        SessionLoadInput input
    )
    {
        const auto owner = std::this_thread::get_id();
        std::optional<SessionFactoryResult<PreparedSessionData>> result;
        process::TaskScope tasks{runtime};
        auto admitted = tasks.submit(
            {.name = "Read/decode session source"},
            [scheduler = take(runtime.blocking()),
             job = SessionLoadJob{std::move(factory), std::move(input)}](process::TaskReporter reporter
            ) mutable noexcept {
                return stdexec::then(
                    stdexec::schedule(scheduler),
                    [job = std::move(job), stop = reporter.stopToken()]() mutable { return std::move(job).run(stop); }
                );
            },
            [&](process::TTaskResult<PreparedSessionData, SessionFactoryFailure>&& completed) noexcept {
                assert(std::this_thread::get_id() == owner);
                if (completed)
                    result.emplace(std::move(*completed));
                else if (auto* error = completed.error().domainFailure())
                    result.emplace(cxx::unexpected(std::move(*error)));
                else
                    std::abort();
            }
        );
        assert(admitted && tasks.join() && result);
        return std::move(*result);
    }
}
int main(int argc, char** argv)
{
    assert(argc == 2);
    const auto root =
        std::filesystem::path(argv[1]) / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root);
    auto simulation = take(std::move(simulation::SimulationDescriptionBuilder{}).build());
    auto description = take(std::move(lux::scene::SceneDescriptionBuilder{}).buildResolved());
    const auto schema = world::worldDataSchemaId("test.unknown");
    auto package = take(lux::scene::createScenePackage(
        identity(SourceFiles::names[0]),
        "scene",
        std::span{&schema, 1},
        std::make_shared<const simulation::SimulationDescription>(std::move(simulation)),
        description
    ));
    write(root / SourceFiles::names[0], take(lux::scene::encodeScenePackage(package, 64 * 1024 * 1024)));
    lux::material::MaterialSource material{identity(SourceFiles::names[1]), "material", {}};
    const auto original_node = material.graph.addNode(std::make_unique<lux::material::ConstantNode>());
    assert(original_node.valid());
    const auto material_bytes = take(lux::material::encodeMaterialSource(material));
    write(root / SourceFiles::names[1], std::as_bytes(std::span{material_bytes}));
    installationStages(std::as_bytes(std::span{material_bytes}));
    lux::flowforge::FlowGraph graph;
    graph.addNodes(std::make_unique<lux::flowforge::BranchNode>());
    auto source = take(lux::flowforge::captureFlowSource(identity(SourceFiles::names[2]), "flow", graph));
    const auto flow_bytes = take(lux::flowforge::encodeFlowSource(source));
    write(root / SourceFiles::names[2], std::as_bytes(std::span{flow_bytes}));
    auto schemas = take(simulation::ecs::ComponentSchemaSet::build({}));
    auto factories = take(SessionFactorySnapshot::create(extensions::builtinSessionFactories(schemas, {})));
    std::array<SessionKindId, 3> kinds{{{"lux.editor.scene"}, {"lux.editor.material"}, {"lux.editor.flowforge"}}};
    SessionStore store{8};
    WriteCoordinator writes;
    SaveService saves{writes};
    storage::FileArtifactStore disk{root};
    auto runtime = take(process::ExecutionRuntime::create(
        {.cpu_concurrency = 2,
         .cpu_queue_capacity = 16,
         .timer = {16},
         .blocking = process::BlockingSchedulerConfig{2, 16}}
    ));
    asset::AssetVfs vfs;
    assert(vfs.mount({"/sources", std::make_shared<SourceFiles>(root)}));
    std::vector<InstalledSession> installed;
    for (std::size_t i{}; i < 3; ++i)
    {
        SessionLoadInput input{
            vfs.view().capture(),
            identity(SourceFiles::names[i]),
            BoundSource{identity(SourceFiles::names[i]), SourceFiles::names[i]},
            take(disk.resolve(SourceFiles::names[i]))
        };
        auto factory = take(factories.find(kinds[i]));
        auto missing = input;
        missing.asset = identity("absent");
        assert(!load(runtime, factory, missing));
        auto oversized = input;
        oversized.max_bytes = 1;
        assert(!load(runtime, factory, oversized));
        const auto original_bytes = read(root / SourceFiles::names[i]);
        write(root / SourceFiles::names[i], std::span<const std::byte>{});
        assert(!load(runtime, factory, input));
        write(root / SourceFiles::names[i], original_bytes);
        auto decoded = take(load(runtime, factory, input));
        auto hidden = take(std::move(decoded).prepare(store, saves));
        assert(store.size() == i && !store.describe(hidden.id()));
        auto refused = saves.requestSave({hidden.id()});
        assert(!refused && refused.error().code == EPersistenceError::STALE_SOURCE);
        auto current = take(hidden.publish());
        assert(store.size() == i + 1 && !take(store.describe(current.id())).dirty);
        installed.push_back(std::move(current));
        // Abandon a fully prepared actual model: no visible role or content, and the slot can be reused.
        {
            auto rejected = take(load(runtime, factory, input));
            auto prepared = take(std::move(rejected).prepare(store, saves));
            assert(!store.describe(prepared.id()));
            assert(!saves.requestSave({prepared.id()}));
        }
        assert(store.size() == i + 1);
        // Bad binding fails after decode/reservation/construction admission without publishing anything.
        input.binding->asset = identity("mismatch");
        auto invalid = take(load(runtime, factory, input));
        assert(!std::move(invalid).prepare(store, saves));
        assert(store.size() == i + 1);
    }
    auto scene_key = take(store.key<es::SceneSession>(installed[0].id()));
    {
        const auto material_key = take(store.key<em::MaterialSession>(installed[1].id()));
        em::MaterialSaveSource original{
            store.access<em::MaterialSession>(),
            material_key,
            take(disk.resolve(SourceFiles::names[1])),
            BindingRevision{1}
        };
        SessionLoadInput input{
            vfs.view().capture(),
            identity(SourceFiles::names[1]),
            BoundSource{identity(SourceFiles::names[1]), SourceFiles::names[1]},
            take(disk.resolve(SourceFiles::names[1]))
        };
        auto ready = take(load(runtime, take(factories.find(kinds[1])), input));
        ReentrantSource recursive{original, store, saves, ready};
        const auto duplicate = saves.registerSource(recursive);
        assert(!duplicate); // Existing material role remains unique; describe still exercised reentry.
        assert(store.size() == 3);
        auto candidate = take(std::move(ready).prepare(store, saves)); // Same completion, no second read/decode.
        auto extra = take(candidate.publish());
        assert(store.size() == 4);
        assert(extra.close(take(store.describe(extra.id())).current));
        assert(store.size() == 3);
    }
    auto& scene = take(store.access<es::SceneSession>().edit(scene_key)).get();
    es::SceneEditBatch scene_edit{scene.describe().current, "insert", {}};
    scene_edit.edits.emplace_back(es::SceneCreateObject{
        {{identity("object").uuid()}, {0}, {{simulation::ecs::componentSchemaId("test.unknown"), 1, {std::byte{7}}}}}
    });
    assert(scene.apply(std::move(scene_edit)));
    auto material_key = take(store.key<em::MaterialSession>(installed[1].id()));
    auto& mat = take(store.access<em::MaterialSession>().edit(material_key)).get();
    em::MaterialEditBatch material_edit{mat.describe().current, "rename", {}};
    material_edit.edits.emplace_back(em::MaterialRename{"edited material"});
    assert(mat.apply(std::move(material_edit)));
    fixedCommandPayload(store, mat, original_node);
    auto flow_key = take(store.key<ef::FlowSession>(installed[2].id()));
    auto& flow = take(store.access<ef::FlowSession>().edit(flow_key)).get();
    ef::FlowEditBatch flow_edit{flow.describe().current, "rename", {}};
    flow_edit.edits.emplace_back(ef::FlowRename{"edited flow"});
    assert(flow.apply(std::move(flow_edit)));
    std::vector<SaveId> requests;
    for (auto& entry : installed)
    {
        assert(take(entry.queryHistory()).can_undo);
        const auto content = take(store.describe(entry.id())).current;
        assert(entry.undo());
        assert(entry.redo());
        assert(take(store.describe(entry.id())).current == content);
        requests.push_back(take(saves.requestSave({entry.id()})));
    }
    {
        using namespace commands;
        CommandRegistry registry;
        CommandDispatcher dispatcher{registry, 4};
        auto find = [&](SessionId id) -> InstalledSession* {
            for (auto& entry : installed)
                if (entry.id() == id)
                    return &entry;
            return nullptr;
        };
        auto snapshot = take(CommandRegistrySnapshot::create(extensions::builtinSessionCommands(store, saves, find)));
        assert(registry.publish(snapshot));
        auto messages = take(object::ObjectMessageQueue::create(64));
        CommandRoot root{messages.dispatcherRef()};
        auto selected = installed[0].id();
        desktop::CommandMenu menu{
            root,
            registry,
            dispatcher,
            [&](const CommandDescriptor&, const ui::Pane*, const ui::Element*) -> CommandResult<CommandInvocation> {
                return CommandInvocation{SessionTarget{selected}};
            }
        };
        root.menu = &menu;
        desktop::ViewHost host{root};
        views::DetachedView window{
            contracts::CodeLease::builtin(),
            std::make_unique<ui::Pane>(
                messages.dispatcherRef(),
                ui::PaneId{"commands"},
                ui::PaneTypeId{"commands"},
                "Commands"
            )
        };
        const auto window_id = take(host.adopt(window, views::ViewRestoreKey{"commands"})).id;
        assert(host.focus(window_id));
        take(host.drain());
        assert(menu.update());
        const auto scene_before = take(store.describe(installed[0].id())).current;
        const auto material_before = take(store.describe(installed[1].id())).current;
        ui::MenuRequest opened;
        assert(object::sendEvent(root, opened));
        selected = installed[1].id(); // The menu's captured session must stay A.
        ui::MenuRequest
            undo{ui::EMenuAction::COMMAND, {}, {}, {ui::CommandIdView{"lux.editor.undo"}, ui::ECommandPhase::EXECUTE}};
        assert(object::sendEvent(root, undo));
        assert(undo.command.result == ui::ECommandDispatchResult::EXECUTED && dispatcher.pending() == 1);
        assert(registry.publish({})); // Pinned queue input survives catalog replacement.
        assert(menu.update());
        auto completed = menu.takeCompletions();
        assert(completed.size() == 1 && completed[0].result);
        assert(take(store.describe(installed[0].id())).current != scene_before);
        assert(take(store.describe(installed[1].id())).current == material_before);
        assert(installed[0].redo());
        assert(take(store.describe(installed[0].id())).current == scene_before);
        ui::MenuRequest closed{ui::EMenuAction::CLOSE};
        assert(object::sendEvent(root, closed));
        assert(registry.publish(snapshot));
        assert(menu.update());
        // Actual Root keyboard routing, not a fabricated command invoke.
        selected = installed[1].id();
        ui::DrawData drawing;
        assert(root.update({{640, 480}, 1.0f / 60.0f}, &drawing));
        assert(root.feedInput(ui::Key{ui::EKey::LEFT_CONTROL, true}));
        assert(root.feedInput(ui::Key{ui::EKey::Z, true}));
        for (int turn{}; turn < 4 && dispatcher.pending() == 0; ++turn)
            assert(root.update({{640, 480}, 1.0f / 60.0f}, &drawing));
        assert(dispatcher.pending() == 1);
        assert(menu.update());
        completed = menu.takeCompletions();
        assert(completed.size() == 1 && completed[0].result);
        assert(take(store.describe(installed[1].id())).current != material_before);
        assert(installed[1].redo());
        assert(root.feedInput(ui::Key{ui::EKey::Z, false}));
        assert(root.feedInput(ui::Key{ui::EKey::LEFT_CONTROL, false}));
        assert(root.update({{640, 480}, 1.0f / 60.0f}, &drawing));
        // Save fixes identity, but captures the content current when the queued request is admitted.
        selected = installed[2].id();
        assert(object::sendEvent(root, opened));
        ui::MenuRequest
            save{ui::EMenuAction::COMMAND, {}, {}, {ui::CommandIdView{"lux.editor.save"}, ui::ECommandPhase::EXECUTE}};
        assert(object::sendEvent(root, save));
        ef::FlowEditBatch late{flow.describe().current, "late", {}};
        late.edits.emplace_back(ef::FlowRename{"latest flow"});
        assert(flow.apply(std::move(late)));
        assert(menu.update());
        completed = menu.takeCompletions();
        assert(completed.size() == 1 && completed[0].result);
        const auto accepted = std::get<AcceptedOperation>(*completed[0].result);
        const SaveId latest{accepted.value};
        assert(take(saves.status(latest)).content == flow.describe().current);
        requests.push_back(latest);
        // A queued command keeps its original identity after closure and actual slot reuse.
        auto factory = take(factories.find(kinds[1]));
        SessionLoadInput input{
            vfs.view().capture(),
            identity(SourceFiles::names[1]),
            BoundSource{identity(SourceFiles::names[1]), SourceFiles::names[1]},
            take(disk.resolve(SourceFiles::names[1]))
        };
        auto open_extra = [&]() {
            auto decoded = take(load(runtime, factory, input));
            auto prepared = take(std::move(decoded).prepare(store, saves));
            return take(prepared.publish());
        };
        installed.push_back(open_extra());
        selected = installed.back().id();
        const auto abandoned_id = selected;
        CommandInvocation abandoned{SessionTarget{selected}};
        assert(dispatcher.enqueue(take(snapshot.find(CommandIdView{"lux.editor.save"})), abandoned));
        assert(installed.back().close(take(store.describe(selected)).current));
        installed.pop_back();
        auto replacement = open_extra();
        const auto replacement_before = take(store.describe(replacement.id()));
        assert(replacement.id() != abandoned_id);
        assert(menu.update());
        completed = menu.takeCompletions();
        assert(
            completed.size() == 1 && !completed[0].result &&
            completed[0].result.error().code == ECommandError::STALE_TARGET
        );
        assert(take(store.describe(replacement.id())).current == replacement_before.current);
        assert(replacement.close(replacement_before.current));
        root.menu = nullptr;
    }
    SaveExecution execution{runtime, saves, writes, disk};
    // Freeze is independent of later author edits. The coordinator retains actual publication ownership.
    for (int turn{}; turn < 10000; ++turn)
    {
        assert(execution.submitReady());
        assert(runtime.collectCompletions());
        saves.adoptCompletions();
        bool done = true;
        for (auto id : requests)
            done &= take(saves.status(id)).stage == ESaveStage::TERMINAL;
        if (done)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    for (auto id : requests)
    {
        assert(take(saves.status(id)).outcome->adoption == EAdoption::APPLIED);
        assert(saves.acknowledge(id));
    }
    assert(take(es::SceneCodec::decode(read(root / SourceFiles::names[0]))).source.partitions[0]->objectCount() == 1);
    assert(take(em::MaterialCodec::decode(read(root / SourceFiles::names[1]))).source.name == "edited material");
    assert(take(ef::FlowCodec::decode(read(root / SourceFiles::names[2]))).source.name == "latest flow");
    // All three factory-installed roles use the existing Save As/rebind and Export Copy algorithms.
    for (std::size_t index{}; index < installed.size(); ++index)
    {
        auto& entry = installed[index];
        const auto original = take(store.describe(entry.id()));
        const auto copy_path = "copy-" + std::to_string(index);
        const auto new_path = "renamed-" + std::to_string(index);
        const auto copy = take(
            saves.requestSave({entry.id(), ESaveMode::EXPORT_COPY, take(disk.resolve(copy_path)), identity(copy_path)})
        );
        const auto drain_save = [&](SaveId id) {
            for (unsigned turn{}; turn < 10000; ++turn)
            {
                assert(execution.submitReady() && runtime.collectCompletions());
                saves.adoptCompletions();
                if (take(saves.status(id)).stage == ESaveStage::TERMINAL)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            auto result = take(saves.status(id));
            assert(result.outcome);
            assert(saves.acknowledge(id));
            return *result.outcome;
        };
        drain_save(copy);
        const auto after_copy = take(store.describe(entry.id()));
        assert(
            after_copy.current == original.current && after_copy.binding == original.binding &&
            after_copy.dirty == original.dirty
        );
        assert(std::filesystem::exists(root / copy_path));
        const auto as =
            take(saves.requestSave({entry.id(), ESaveMode::SAVE_AS, take(disk.resolve(new_path)), identity(new_path)}));
        assert(drain_save(as).adoption == EAdoption::APPLIED);
        const auto after_as = take(store.describe(entry.id()));
        assert(after_as.current == original.current && !after_as.dirty && after_as.binding != original.binding);
        assert(take(entry.queryHistory()).can_undo && entry.undo() && entry.redo());
        assert(
            take(store.describe(entry.id())).current == original.current && std::filesystem::exists(root / new_path)
        );
    }
    for (auto& entry : installed)
    {
        auto info = take(store.describe(entry.id()));
        assert(!info.dirty);
        assert(entry.close(info.current));
    }
    assert(store.size() == 0 && execution.tasks().join());
    std::cout
        << "PASS real three-model worker read/decode, owner installation, hidden rollback, undo/redo, IO and close\n";
}
