#include <lux/engine/editor/workspace/WorkspaceStore.hpp>
#include <lux/engine/editor/io/ProjectArtifactStore.hpp>
#include <lux/engine/editor/storage/FilePublication.hpp>
#include <array>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <limits>
#include <type_traits>

namespace w = lux::editor::workspace;
namespace p = lux::editor::persistence;
namespace v = lux::editor::views;
namespace fs = std::filesystem;
namespace io = lux::editor::io;
namespace file = lux::editor::storage;
static_assert(!std::is_default_constructible_v<w::ValidatedLayout>);
static_assert(!std::is_copy_constructible_v<w::WorkspaceStore>);
static_assert(!std::is_copy_assignable_v<w::WorkspaceStore>);
static_assert(!std::is_move_constructible_v<w::WorkspaceStore>);
static_assert(!std::is_move_assignable_v<w::WorkspaceStore>);
namespace
{
    template <class T> auto take(T result)
    {
        if (!result)
        {
            if constexpr (requires { result.error().detail; })
                std::fprintf(
                    stderr,
                    "unexpected: %u %s\n",
                    unsigned(result.error().code),
                    result.error().detail.c_str()
                );
            std::abort();
        }
        return std::move(*result);
    }
    std::vector<std::byte> bytes(std::string_view value)
    {
        const auto data = std::as_bytes(std::span(value));
        return {data.begin(), data.end()};
    }
    w::DockLayout layout()
    {
        w::DockLayout value;
        value.id = {"1234567890abcdef1234567890abcdef"};
        value.label = "Material tools";
        value.slots = {
            {{1}, v::ViewRestoreKey{"material:one"}, v::ViewTypeId{"test.material"}, true, {1, bytes("state")}}
        };
        value.dock.nodes = {{1, w::EDockSplit::LEAF, 0, 0, 0.5, {{1}}}};
        value.dock.roots = {{1}};
        return value;
    }
    struct Fixture final
    {
        fs::path root;
        p::WriteCoordinator coordinator;
        io::ProjectArtifactStore backend;
        w::WorkspaceStore store;
        explicit Fixture(fs::path path) : root(std::move(path)), backend(root), store(root, coordinator, backend)
        {
            fs::create_directories(root);
        }
        p::VPublicationOutcome publish(p::WriteTicket ticket)
        {
            auto work = take(coordinator.takeReady());
            assert(work && work->ticket == ticket);
            auto outcome = backend.publish(*work);
            assert(coordinator.complete(ticket, outcome));
            return outcome;
        }
        void success(p::WriteTicket ticket)
        {
            assert(std::holds_alternative<p::CommitReceipt>(publish(ticket)));
            assert(coordinator.acknowledge(ticket));
        }
        p::WriteTicket seed(const w::DockLayout& value)
        {
            auto ticket = take(store.saveLayout(value, "missing"));
            success(ticket);
            return ticket;
        }
    };
    void writeRaw(const fs::path& path, std::string_view text)
    {
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        assert(out.write(text.data(), text.size()));
    }
    std::string raw(const fs::path& path)
    {
        const auto value = take(file::readPublicationFile(path, 32 * 1024 * 1024));
        return {reinterpret_cast<const char*>(value.data()), value.size()};
    }
    void validation()
    {
        auto base = layout();
        assert(w::ValidatedLayout::validate(base));
        auto bad = base;
        bad.dock.nodes[0].first = 3;
        assert(!w::ValidatedLayout::validate(bad));
        bad = base;
        bad.slots.push_back(bad.slots[0]);
        assert(!w::ValidatedLayout::validate(bad));
        bad = base;
        bad.dock.nodes.push_back(bad.dock.nodes[0]);
        assert(!w::ValidatedLayout::validate(bad));
        bad = base;
        bad.dock.nodes[0] = {1, w::EDockSplit::HORIZONTAL, 1, 2};
        bad.dock.nodes.push_back({2, w::EDockSplit::LEAF, 0, 0, 0.5, {{1}}});
        assert(!w::ValidatedLayout::validate(bad));
        bad.dock.nodes[0].first = 2;
        assert(!w::ValidatedLayout::validate(bad));
        bad.dock.nodes[0].ratio = std::numeric_limits<double>::quiet_NaN();
        assert(!w::ValidatedLayout::validate(bad));
        bad = base;
        bad.dock.roots[0].width = -1;
        assert(!w::ValidatedLayout::validate(bad));
        bad = base;
        bad.dock.nodes[0].slots.clear();
        assert(!w::ValidatedLayout::validate(bad));
        bad = base;
        bad.id.value = "../escape";
        assert(!w::encodeLayout(bad));
        bad = base;
        bad.schema = 8;
        assert(w::encodeLayout(bad).error().code == w::EWorkspaceError::UNSUPPORTED_VERSION);
        std::puts(
            "X09-01 complete invalid-tree validation: duplicate/cyclic/shared/missing/ratio/geometry/schema rejected"
        );
    }
    void opaque()
    {
        auto value = layout();
        value.slots[0].type = v::ViewTypeId{"future.plugin"};
        value.slots[0].state = {99, {std::byte{0}, std::byte{255}, std::byte{13}, std::byte{10}}};
        value.opaque = {{"unloaded.extension", 47, {std::byte{0}, std::byte{250}}}};
        auto encoded = take(w::encodeLayout(value));
        auto decoded = take(w::decodeLayout(encoded));
        assert(decoded.opaque == value.opaque && decoded.slots[0].state == value.slots[0].state);
        assert(take(w::encodeLayout(decoded)) == encoded);
        auto plan = take(w::LayoutPlanner::resolve(take(w::ValidatedLayout::validate(decoded)), {}, {}));
        assert(plan.views[0].resolution == w::ELayoutResolution::MISSING_PROVIDER);
        assert(plan.views[0].slot.state == value.slots[0].state);
        const std::array providers{w::ViewProviderInfo{v::ViewTypeId{"future.plugin"}, 1, 2}};
        plan = take(w::LayoutPlanner::resolve(take(w::ValidatedLayout::validate(decoded)), {}, providers));
        assert(plan.views[0].resolution == w::ELayoutResolution::UNSUPPORTED_STATE);
        auto text = std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
        text = "future_setting = 42\n" + text;
        decoded = take(w::decodeLayout(bytes(text)));
        assert(decoded.opaque.back().bytes == bytes(text));
        auto again = take(w::decodeLayout(take(w::encodeLayout(decoded))));
        assert(again.opaque == decoded.opaque);
        w::UserPreferences preferences{1, value.id, value.opaque};
        assert(take(w::decodePreferences(take(w::encodePreferences(preferences)))).opaque == value.opaque);
        w::RecoveryManifest recovery{
            1,
            {{v::ViewRestoreKey{"x"}, v::ViewTypeId{"future.plugin"}, "asset:stable", true}},
            value.opaque
        };
        auto restored = take(w::decodeRecovery(take(w::encodeRecovery(recovery))));
        assert(restored.entries[0].unpersisted_changes && restored.opaque == value.opaque);
        w::WorkspaceLimits limits;
        limits.opaque_bytes = 3;
        assert(!w::encodeLayout(value, limits) && !w::decodeLayout(encoded, limits));
        limits = {};
        limits.file_bytes = encoded.size() - 1;
        assert(!w::decodeLayout(encoded, limits));
        std::puts("X09-04 exact unknown bytes/schema/fields roundtrip; budget errors preserve input, no truncation");
    }
    void rename(Fixture& f)
    {
        auto value = layout();
        f.seed(value);
        auto before = take(f.store.readLayout(value.id));
        f.success(take(f.store.writePreferences({1, value.id}, "missing")));
        auto ticket = take(f.store.renameLayout(value.id, "Renamed 中文"));
        auto outcome = f.publish(ticket);
        assert(std::holds_alternative<p::CommitReceipt>(outcome));
        auto prefs_before = take(f.store.readPreferences());
        auto preferences = take(f.store.writePreferences({}, prefs_before.target.expected_version));
        const auto prefs_path = f.root / ".lux/workspace/preferences.toml";
#if defined(_WIN32)
        // The real Windows replace path must reject a read-only target; assert the observed native error.
        fs::permissions(prefs_path, fs::perms::owner_read, fs::perm_options::replace);
#else
        fs::rename(prefs_path, f.root / ".lux/workspace/preferences.saved");
        fs::create_directory(prefs_path);
#endif
        auto denied = f.publish(preferences);
        assert(std::holds_alternative<p::NotPublished>(denied));
        const auto failure = std::get<p::NotPublished>(denied).failure;
        std::printf(
            "preferences REAL rejection native_code=%llu\n",
            static_cast<unsigned long long>(failure.native_code)
        );
#if defined(_WIN32)
        assert(failure.native_code == 5); // ERROR_ACCESS_DENIED, not an injected publication result.
        fs::permissions(prefs_path, fs::perms::owner_all, fs::perm_options::replace);
#endif
        auto preference_result = take(f.store.preferenceResult(preferences));
        assert(std::holds_alternative<p::NotPublished>(*preference_result.publication.outcome));
        assert(f.coordinator.acknowledge(preferences));
        auto receipt = take(f.store.layoutResult(ticket));
        assert(receipt.catalog && receipt.catalog->complete());
        auto after = take(f.store.readLayout(value.id));
        assert(before.target.key == after.target.key && after.value.label == "Renamed 中文");
        assert(take(f.store.chooseLayout({1, value.id})).layout->id == value.id);
        assert(f.coordinator.acknowledge(ticket));
        std::puts("X09-02 REAL preference publication rejected; label published, same filename/ID remains selectable");
    }
    void remove(Fixture& f)
    {
        auto value = layout();
        f.seed(value);
        f.success(take(f.store.writePreferences({1, value.id}, "missing")));
        f.success(take(f.store.removeLayout(value.id)));
        w::WorkspaceStore restarted(f.root, f.coordinator, f.backend);
        auto prefs = take(restarted.readPreferences());
        assert(prefs.value.selected_layout == value.id);
        auto choice = take(restarted.chooseLayout(prefs.value));
        assert(!choice.layout && choice.fallback_reason->code == w::EWorkspaceError::NOT_FOUND);
        assert(!fs::exists(f.root / ".lux/workspace/layouts" / (value.id.value + ".layout")));
        assert(take(restarted.listLayouts()).layouts.empty());
        std::puts("X09-03 physical delete with stale preference; restarted reader diagnoses fallback without rewriting "
                  "preference");
    }
    void ordering(Fixture& f, bool unknown)
    {
        auto value = layout();
        const auto save = take(f.store.saveLayout(value, "missing"));
        const auto work = take(f.coordinator.takeReady());
        assert(work && work->ticket == save);
        const auto remove = take(f.store.removeLayout(value.id));
        assert(!take(f.coordinator.takeReady()));
        if (unknown)
        {
            assert(f.coordinator.complete(save, p::PublicationUnknown{{p::EPersistenceError::IO}, "active"}));
            assert(!take(f.coordinator.takeReady()));
            assert(!f.coordinator.acknowledge(save));
            struct Backend final : p::IArtifactStore
            {
                io::ProjectArtifactStore& real;
                bool retired{};
                explicit Backend(io::ProjectArtifactStore& store) : real(store) {}
                p::PersistenceResult<p::WriteTarget> resolve(std::string_view name) override
                {
                    return real.resolve(name);
                }
                p::VPublicationOutcome publish(const p::PublicationQuery& q, std::stop_token stop) override
                {
                    return real.publish(q, stop);
                }
                p::Reconciliation reconcile(const p::PublicationQuery& q) override
                {
                    if (!retired)
                        return {false, p::PublicationUnknown{{p::EPersistenceError::IO}, "active"}};
                    return real.reconcile(q);
                }
            } backend{f.backend};
            assert(!f.coordinator.reconcile(save, backend));
            assert(!take(f.coordinator.takeReady()));
            assert(std::holds_alternative<p::CommitReceipt>(f.backend.publish(*work)));
            backend.retired = true;
            assert(f.coordinator.reconcile(save, backend));
        }
        else
            assert(f.coordinator.complete(save, f.backend.publish(*work)));
        assert(f.coordinator.acknowledge(save));
        const auto deletion = take(f.coordinator.takeReady());
        assert(deletion && deletion->ticket == remove && deletion->action == p::EPublicationAction::REMOVE);
        assert(deletion->target.expected_version != "missing");
        const auto removed = f.backend.publish(*deletion);
        assert(std::holds_alternative<p::CommitReceipt>(removed));
        if (unknown)
        {
            assert(f.coordinator.complete(remove, p::PublicationUnknown{{p::EPersistenceError::IO}, "delete-receipt"}));
            assert(!f.coordinator.acknowledge(remove));
            assert(f.coordinator.reconcile(remove, f.backend));
        }
        else
            assert(f.coordinator.complete(remove, removed));
        assert(std::holds_alternative<p::CommitReceipt>(*take(f.coordinator.status(remove)).outcome));
        assert(f.coordinator.acknowledge(remove));
        assert(!fs::exists(f.root / ".lux/workspace/layouts" / (value.id.value + ".layout")));
        // An external write is never silently inherited by a removal.
        f.seed(value);
        auto external_delete = take(f.store.removeLayout(value.id));
        writeRaw(f.root / ".lux/workspace/layouts" / (value.id.value + ".layout"), "external");
        auto outcome = f.publish(external_delete);
        assert(std::get<p::NotPublished>(outcome).failure.code == p::EPersistenceError::CONFLICT);
        assert(f.coordinator.acknowledge(external_delete));
        std::puts("same coordinator pending write/remove FIFO, verified version edge, Unknown retirement, external "
                  "conflict PASS");
    }
    void aliases(Fixture& f)
    {
        auto value = layout();
        auto first = take(f.store.saveLayout(value, "missing"));
        fs::create_directories(f.root / ".lux");
        io::ProjectArtifactStore other(f.root / ".lux/..");
        auto a = take(f.backend.resolve(".lux/workspace/layouts/" + value.id.value + ".layout"));
        auto b = take(other.resolve(".lux/workspace/layouts/../layouts/" + value.id.value + ".layout"));
        assert(a.key == b.key);
        auto second = take(f.coordinator.reserve(b, {}));
        assert(f.coordinator.provideRemoval(second));
        auto one = take(f.coordinator.takeReady());
        assert(one && one->ticket == first && !take(f.coordinator.takeReady()));
        assert(f.coordinator.complete(first, f.backend.publish(*one)));
        assert(f.coordinator.acknowledge(first));
        f.success(second);
        assert(!f.backend.resolve("../outside"));
        std::puts("physical alias namespaces share the original lane; no root escape PASS");
    }
    void reads(Fixture& f)
    {
        auto value = layout();
        f.seed(value);
        struct Busy final : p::IArtifactStore
        {
            p::PersistenceResult<p::WriteTarget> resolve(std::string_view) override
            {
                return lux::cxx::unexpected(p::PersistenceFailure{p::EPersistenceError::BUSY});
            }
            p::VPublicationOutcome publish(const p::PublicationQuery&, std::stop_token) override
            {
                std::abort();
            }
            p::Reconciliation reconcile(const p::PublicationQuery&) override
            {
                std::abort();
            }
        } busy;
        w::WorkspaceStore store(f.root, f.coordinator, busy);
        assert(store.readLayout(value.id).error().code == w::EWorkspaceError::BUSY);
        assert(store.chooseLayout({1, value.id}).error().code == w::EWorkspaceError::BUSY);
        auto catalog = take(store.listLayouts());
        assert(!catalog.complete() && catalog.diagnostics[0].failure.code == w::EWorkspaceError::BUSY);
        assert(f.coordinator.size() == 0);
        const auto path = f.root / ".lux/workspace/layouts" / (value.id.value + ".layout");
        const auto before = raw(path);
        auto invalid = f.store.readLayout({"../../escape"});
        assert(!invalid);
        assert(raw(path) == before);
        std::puts("BUSY/read errors retained; no default publication and incomplete catalog explicitly diagnosed PASS");
    }
    void catalog(Fixture& f)
    {
        auto value = layout();
        f.seed(value);
        writeRaw(f.root / ".lux/workspace/layouts/00000000000000000000000000000001.layout", "broken");
        auto partial = take(f.store.listLayouts());
        assert(partial.layouts.size() == 1 && partial.diagnostics.size() == 1 && !partial.complete());
        auto rename = take(f.store.renameLayout(value.id, "changed"));
        assert(std::holds_alternative<p::CommitReceipt>(f.publish(rename)));
        // Published fact survives a later catalog error.
        fs::rename(f.root / ".lux/workspace/layouts", f.root / ".lux/workspace/hidden");
        writeRaw(f.root / ".lux/workspace/layouts", "not a directory");
        auto receipt = take(f.store.layoutResult(rename));
        assert(std::holds_alternative<p::CommitReceipt>(*receipt.publication.outcome));
        assert(!receipt.catalog && receipt.catalog.error().code == w::EWorkspaceError::IO);
        assert(f.coordinator.acknowledge(rename));
        std::puts(
            "individual corruption vs failed enumeration distinguished; committed layout survives failed refresh PASS"
        );
    }
    void legacy(Fixture& f, bool collision)
    {
        const auto old = f.root / ".lux/editor/layouts/Beginner.toml";
        const std::string text = R"(version = 1
future = 'retained'
dock = '''[Window][Material###material-1]
Pos=0,0
Size=800,600
DockId=0x2,0
[Docking][Data]
DockSpace ID=0x1 Pos=0,0 Size=1200,600 Split=X
  DockNode ID=0x2 Parent=0x1 SizeRef=800,600
  DockNode ID=0x3 Parent=0x1 SizeRef=400,600
'''
[[panes]]
id = 'material-1'
type = 'lux.editor.material.v1'
visible = true
payload = 'v1:12345678-1234-1234-1234-123456789abc'
[[panes]]
id = 'unknown-2'
type = 'future.plugin'
visible = false
payload = 'raw unknown'
[[windows]]
id = 'material-1/child'
visible = false
)";
        writeRaw(old, text);
        writeRaw(f.root / ".lux/editor/settings.toml", "version=1\nselected='Beginner'\n");
        auto plan = take(f.store.prepareLegacyMigration());
        assert(plan.layouts.size() == 1 && plan.recovery.entries.size() == 1);
        assert(plan.layouts[0].slots[0].state.bytes.empty());
        assert(plan.layouts[0].slots[1].state.bytes == bytes("raw unknown"));
        assert(plan.layouts[0].opaque[0].bytes == bytes(text));
        assert(plan.layouts[0].dock.nodes[0].ratio == 2.0 / 3.0);
        const auto id = plan.layouts[0].id;
        if (collision)
        {
            auto impostor = plan.layouts[0];
            impostor.legacy_origin.reset();
            f.seed(impostor);
            assert(f.store.continueMigration(plan).error().code == w::EWorkspaceError::CONFLICT);
            assert(raw(old) == text);
            return;
        }
        auto first = take(f.store.continueMigration(plan));
        assert(first);
        f.success(*first);
        // Crash boundary: persisted layout, no completion marker. A user edits the new layout before retry.
        f.success(take(f.store.renameLayout(id, "user edit after interruption")));
        w::WorkspaceStore restarted(f.root, f.coordinator, f.backend);
        auto retry = take(restarted.prepareLegacyMigration());
        assert(retry.layouts[0].id == id);
        unsigned writes{};
        while (auto ticket = take(restarted.continueMigration(retry)))
        {
            f.success(*ticket);
            assert(++writes <= 3);
        }
        assert(writes == 3); // recovery, preferences, verified marker
        assert(take(restarted.readLayout(id)).value.label == "user edit after interruption");
        assert(take(restarted.listLayouts()).layouts.size() == 1);
        assert(take(restarted.readPreferences()).value.selected_layout == id);
        assert(take(restarted.readRecovery()).value.entries[0].locator.starts_with("asset:"));
        assert(!take(restarted.continueMigration(retry)));
        assert(raw(old) == text);
        std::puts("X09-05 REAL files: new record before marker, recreated Store, stable mapping, user edit retained, "
                  "old bytes unchanged");
    }
    void budgets()
    {
        auto value = layout();
        w::WorkspaceLimits limits;
        limits.entries = 0;
        assert(w::ValidatedLayout::validate(value, limits).error().code == w::EWorkspaceError::CAPACITY);
        limits = {};
        limits.depth = 0;
        assert(w::ValidatedLayout::validate(value, limits).error().code == w::EWorkspaceError::CAPACITY);
        auto encoded = take(w::encodeLayout(value));
        auto text = std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
        const auto pos = text.find("schema = 1");
        assert(pos != text.npos);
        text.replace(pos, 10, "schema = 9");
        assert(w::decodeLayout(bytes(text)).error().code == w::EWorkspaceError::UNSUPPORTED_VERSION);
        std::puts("count/depth/file/schema budgets fail closed PASS");
    }
}
int main(int argc, char** argv)
{
    assert(argc == 3);
    const std::string scenario = argv[2];
    const fs::path path = fs::absolute(fs::path(argv[1]) / scenario);
    // Test-owned path only. Each scenario gets an isolated directory below the explicitly supplied test root.
    assert(path.parent_path() == fs::absolute(argv[1]));
    fs::remove_all(path);
    Fixture f(path);
    if (scenario == "validation")
        validation();
    else if (scenario == "opaque")
        opaque();
    else if (scenario == "rename")
        rename(f);
    else if (scenario == "remove")
        remove(f);
    else if (scenario == "ordering")
        ordering(f, false);
    else if (scenario == "unknown")
        ordering(f, true);
    else if (scenario == "aliases")
        aliases(f);
    else if (scenario == "reads")
        reads(f);
    else if (scenario == "catalog")
        catalog(f);
    else if (scenario == "migration")
        legacy(f, false);
    else if (scenario == "collision")
        legacy(f, true);
    else if (scenario == "budgets")
        budgets();
    else
        std::abort();
}
