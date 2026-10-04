#include "../../test-support/ObjectQueue.hpp"
#include "Probe.hpp"
#include <lux/engine/editor/extensions/EditorExtension.hpp>
#include <lux/engine/editor/scene/SceneConfigurationElement.hpp>
#include <lux/engine/editor/material/MaterialSessionFactory.hpp>
#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
#include <lux/engine/editor/desktop/ViewHost.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <fstream>
#include <cassert>
#include <iostream>
#include <thread>

using namespace lux;
using namespace lux::editor;
namespace
{
    template <class T> auto take(T result)
    {
        if (!result)
        {
            if constexpr (requires { result.error().code; })
                std::cerr << "error=" << int(result.error().code) << '\n';
            if constexpr (requires { result.error().subject; })
                std::cerr << result.error().subject << '\n';
            if constexpr (requires { result.error().detail; })
                std::cerr << result.error().detail << '\n';
            std::abort();
        }
        return std::move(*result);
    }
    asset::AssetId assetId()
    {
        return asset::AssetId{*uuids::uuid::from_string("01234567-89ab-cdef-0123-456789abcdef")};
    }
    class Files final : public asset::IAssetProvider
    {
    public:
        explicit Files(std::filesystem::path file) : file_(std::move(file)) {}
        std::optional<asset::AssetId> resolve(std::string_view path) const override
        {
            return path == "input.luxmaterial" ? std::optional{assetId()} : std::nullopt;
        }
        bool contains(const asset::AssetId& id) const override
        {
            return id == assetId();
        }
        cxx::expected<asset::AssetBlob, asset::EAssetStorageError>
        open(const asset::AssetId& id, std::size_t max_bytes) const override
        {
            assert(std::this_thread::get_id() != owner_);
            if (!contains(id))
                return cxx::unexpected(asset::EAssetStorageError::NOT_FOUND);
            if (std::filesystem::file_size(file_) > max_bytes)
                return cxx::unexpected(asset::EAssetStorageError::LIMIT_EXCEEDED);
            std::ifstream file(file_, std::ios::binary);
            std::string bytes{std::istreambuf_iterator<char>(file), {}};
            return asset::AssetBlob::fromShared(cxx::SharedBytes<>::copyOf(std::as_bytes(std::span{bytes})));
        }
        void enumerate(const std::function<void(const asset::ProviderEntry&)>& callback) const override
        {
            callback({assetId(), 0, "input.luxmaterial"});
        }
        std::optional<std::string> pathOf(const asset::AssetId& id) const override
        {
            return contains(id) ? std::optional<std::string>{"input.luxmaterial"} : std::nullopt;
        }

    private:
        std::filesystem::path file_;
        std::thread::id owner_{std::this_thread::get_id()};
    };
}
int main(int argc, char** argv)
{
    assert(argc == 9);
    const bool with_configuration = std::string_view{argv[8]} == "with-configuration";
    const auto root = std::filesystem::path(argv[1]).parent_path();
    lux::project::PluginLibraryDescription library;
    library.path = std::filesystem::path(argv[1]).filename();
    library.sdk_abi = lux::project::pluginSdkAbi();
    library.build_id = "p11-installed";
    library.declaration_digest = "p11-declarations";
    library.exports = {lux::project::EPluginExport::COMPONENTS};
    lux::project::PluginDescription description;
    description.identity = {"qualification.p11", 1};
    description.root = root;
    description.runtime_library = library;
    const auto& schema = simulation::ecs::transformComponentSchemas().front();
    description.components.push_back({{std::string(schema.id.name), schema.version}});
    auto runtime_plugin = take(lux::project::PluginLibrary::load(description));
    assert(runtime_plugin->components().size() == 1);
    assert(runtime_plugin->components().front().operations.valid());
    library.exports = {lux::project::EPluginExport::EDITOR};
    for (int index = 3; index != 8; ++index)
    {
        library.path = std::filesystem::path(argv[index]).filename();
        description.editor_library = library;
        auto rejected = extensions::EditorExtension::load(description, *runtime_plugin);
        assert(!rejected);
        const auto expected = index == 3   ? lux::project::EPluginError::MISSING_EXPORT
                              : index == 6 ? lux::project::EPluginError::ABI_MISMATCH
                                           : lux::project::EPluginError::INVALID_EXPORT;
        assert(rejected.error().code == expected);
    }
    library.path = std::filesystem::path(argv[2]).filename();
    description.editor_library = library;
    probe::Facts facts;
    std::weak_ptr<const void> weak_library;
    std::weak_ptr<commands::CommandEntry> retired_entry;
    auto messages = take(object::ObjectMessageQueue::create(32));
    auto ui_root = take(ui::Root::create(messages.dispatcherRef()));
    desktop::ViewHost host{*ui_root};
    lux::test::ObjectQueue store_messages;
    sessions::SessionStore store{store_messages.dispatcherRef(), 4};
    facts.sessions = &store;
    persistence::WriteCoordinator writes;
    persistence::SaveService saves{writes};
    storage::FileArtifactStore disk{root};
    auto execution = take(process::ExecutionRuntime::create(
        {.cpu_concurrency = 2,
         .cpu_queue_capacity = 16,
         .timer = {16},
         .blocking = process::BlockingSchedulerConfig{1, 8}}
    ));
    process::TaskScope tasks{execution};
    commands::CommandRegistry commands;
    extensions::ContributionRegistry catalog{messages.dispatcherRef(), commands};
    std::optional<sessions::InstalledSession> installed;
    views::ViewId view_id;
    persistence::SaveId save_id;
    ui::Pane configuration_window{
        messages.dispatcherRef(),
        ui::PaneId{"configuration"},
        ui::PaneTypeId{"configuration"},
        "Configuration"
    };
    ui::Layout configuration_layout{configuration_window, ui::ElementId{"content"}, ui::ELayoutType::VERTICAL};
    assert(configuration_window.setContent(configuration_layout));
    std::optional<lux::editor::scene::ConfigurationControl> configuration;
    {
        auto extension = take(extensions::EditorExtension::load(description, *runtime_plugin));
        weak_library = extension.code();
        auto probe_library = take(lux::project::loadPluginLibrary(description, *description.editor_library));
        auto set_probe = probe_library->get_symbol<void(probe::Facts*) noexcept>("p11_probe");
        assert(set_probe);
        set_probe(&facts);
        auto bad_draft = take(extension.contributions());
        bad_draft.configurations[0].value.reflection = [](meta::ReflectionRegistry&) noexcept -> const meta::RefClass* {
            return nullptr;
        };
        auto bad = take(extensions::ContributionSnapshot::prepare(std::move(bad_draft)));
        assert(catalog.enqueue(bad));
        const auto rejected = catalog.applyPending();
        assert(!rejected && rejected.error().domain == "configuration.reflection" && catalog.revision() == 0);
        assert(!catalog.snapshot().valid());
        auto draft = take(extension.contributions());
        auto missing = extension.activate({});
        assert(!missing && missing.error().code == extensions::EContributionError::UNAVAILABLE);
        assert(facts.activations == 0);
        const extensions::SessionActivities session_activities{store, saves};
        const extensions::WorkbenchAccess workbench{messages.dispatcherRef(), host, commands};
        std::jthread foreign([&] {
            auto refused = extension.activate({&session_activities, {}, &workbench});
            assert(!refused && refused.error().code == extensions::EContributionError::WRONG_THREAD);
        });
        foreign.join();
        assert(facts.activations == 0);
        auto activated = take(extension.activate({&session_activities, {}, &workbench}));
        assert(facts.activations == 1 && activated.commands.size() == 1 && activated.views.size() == 1);
        for (auto& pin : activated.code)
            draft.code.push_back(std::move(pin));
        for (auto& entry : activated.commands)
            draft.commands.push_back(std::move(entry));
        for (auto& entry : activated.views)
            draft.views.push_back(std::move(entry));
        if (!with_configuration)
        {
            draft.configurations.clear();
            draft.reflection.clear();
        }
        auto snapshot = take(extensions::ContributionSnapshot::prepare(std::move(draft)));
        assert(catalog.enqueue(snapshot));
        assert(catalog.applyPending());
        const auto current = catalog.snapshot();
        retired_entry = current.commands().entries().front();
        assert(current.configurations().size() == (with_configuration ? 1 : 0));
        if (with_configuration)
            configuration.emplace(take(lux::editor::scene::makeConfigurationControl(
                current.configurations()[0],
                configuration_layout,
                ui::ElementId{"fields"},
                {}
            )));
        lux::material::MaterialSource material{assetId(), "external", {}};
        assert(material.graph.addNode(std::make_unique<lux::material::ConstantNode>()).valid());
        const auto bytes = take(lux::material::encodeMaterialSource(material));
        const auto file = root / "input.luxmaterial";
        {
            std::ofstream output(file, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            assert(output);
        }
        asset::AssetVfs vfs;
        assert(vfs.mount({"/source", std::make_shared<Files>(file)}));
        auto factory = take(current.sessions().find({"lux.editor.material"}));
        sessions::SessionLoadInput input{
            vfs.view().capture(),
            assetId(),
            sessions::BoundSource{assetId(), "input.luxmaterial"},
            take(disk.resolve("input.luxmaterial"))
        };
        std::optional<sessions::SessionPreparation> completed;
        const auto owner = std::this_thread::get_id();
        assert(tasks.submit(
            {.name = "External decode"},
            [scheduler = take(execution.blocking()),
             job =
                 sessions::SessionLoadJob{factory, std::move(input)}](process::TaskReporter reporter) mutable noexcept {
                return stdexec::then(
                    stdexec::schedule(scheduler),
                    [job = std::move(job), stop = reporter.stopToken()]() mutable { return std::move(job).run(stop); }
                );
            },
            [&](process::TTaskResult<sessions::SessionPreparation, sessions::SessionFactoryFailure>&& value) noexcept {
                assert(std::this_thread::get_id() == owner && value);
                completed.emplace(std::move(*value));
            }
        ));
        assert(tasks.join() && completed);
        auto candidate = take(std::move(*completed).prepare(store, saves));
        assert(store.size() == 0 && !saves.requestSave({candidate.id()}));
        installed.emplace(take(candidate.publish()));
        auto model = store.access<lux::editor::material::MaterialSession>();
        auto key = take(store.key<lux::editor::material::MaterialSession>(installed->id()));
        auto& session = take(model.edit(key)).get();
        lux::editor::material::MaterialEditBatch batch{session.describe().current, "rename", {}};
        batch.edits.emplace_back(lux::editor::material::MaterialRename{"external edited"});
        assert(session.apply(std::move(batch)) && installed->undo() && installed->redo());
        auto handle = take(current.commands().find(commands::CommandIdView{"qualification.inspect"}));
        commands::CommandInvocation invocation{commands::SessionTarget{installed->id()}};
        assert(commands.execute(handle, invocation));
        assert(facts.queries == 1 && facts.executions == 1);
        auto active_command = take(current.commands().find(commands::CommandIdView{"qualification.activated"}));
        assert(commands.execute(active_command, invocation));
        assert(facts.activation_queries == 1 && facts.activation_executions == 1);
        views::ViewFactoryInput free_input{
            messages.dispatcherRef(),
            ui::PaneId{"free"},
            lux::object::CodeLease::builtin(),
            cxx::typeToken<std::monostate>(),
            std::make_shared<const std::monostate>()
        };
        auto free_window = take(current.views().prepare(views::ViewTypeId{"qualification.free"}, free_input));
        assert(!free_window.pane()->attachedRoot());
        const auto free_id = take(host.adopt(free_window, views::ViewRestoreKey{"free"})).id;
        assert(take(host.describe(free_id)).type == views::ViewTypeId{"qualification.free"});
        assert(host.focus(free_id) && host.close(free_id));
        take(host.drain());
        assert(store.describe(installed->id()));
        facts.fail_query = true;
        auto contained = commands.query(handle, invocation.query());
        assert(!contained && contained.error().domain == "plugin.command.query");
        facts.fail_query = false;
        views::ViewFactoryInput view_input{
            messages.dispatcherRef(),
            ui::PaneId{"external"},
            lux::object::CodeLease::builtin(),
            cxx::typeToken<probe::Binding>(),
            std::make_shared<const probe::Binding>(probe::Binding{&facts})
        };
        auto detached = take(current.views().prepare(views::ViewTypeId{"qualification.window"}, view_input));
        assert(!detached.pane()->attachedRoot());
        view_id = take(host.adopt(detached, views::ViewRestoreKey{"external"})).id;
        save_id = take(saves.requestSave({installed->id()}));
        auto empty = take(extensions::ContributionSnapshot::prepare({}));
        assert(catalog.enqueue(empty) && catalog.applyPending());
    }
    assert(!weak_library.expired() && facts.unloaded == 0);
    assert(facts.activations_destroyed == 1);
    assert(host.close(view_id));
    take(host.drain());
    assert(facts.panes_destroyed == 1);
    assert(store.describe(installed->id())); // Closing a window never closes its content.
    assert(installed->close(take(store.describe(installed->id())).current));
    assert(!weak_library.expired()); // Frozen job/operation still pins the external code.
    persistence::SaveExecution saving{execution, saves, writes, disk};
    for (unsigned turn{}; turn != 10000; ++turn)
    {
        assert(saving.submitReady() && execution.collectCompletions());
        saves.adoptCompletions();
        if (take(saves.status(save_id)).stage == persistence::ESaveStage::TERMINAL)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const auto outcome = take(saves.status(save_id));
    assert(outcome.outcome && outcome.outcome->adoption != persistence::EAdoption::APPLIED);
    std::ifstream file(root / "input.luxmaterial", std::ios::binary);
    std::string encoded{std::istreambuf_iterator<char>(file), {}};
    auto decoded = take(lux::editor::material::MaterialCodec::decode(std::as_bytes(std::span{encoded})));
    assert(decoded.source.name == "external edited");
    assert(saves.acknowledge(save_id) && saving.tasks().join());
    if (with_configuration)
    {
        assert(
            !weak_library.expired() && facts.unloaded == 0
        ); // Configuration outlives the catalogs and file operation.
        std::vector<std::byte> configuration_bytes;
        assert(configuration->encode(configuration_bytes) && !configuration_bytes.empty());
        configuration.reset();
    }
    assert(weak_library.expired() && facts.unloaded == 1);
    assert(retired_entry.expired());
    retired_entry.reset(); // Its receiver-owned control block is independent of the unloaded DLL.
    assert(runtime_plugin->components().front().operations.valid());
    std::cout
        << "PASS actual installed runtime/editor DLLs, ABI rejection, factory, commands, view, late IO and unload\n";
}
