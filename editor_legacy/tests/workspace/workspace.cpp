#include <lux/engine/editor/workspace/WorkspaceStore.hpp>
#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <lux/engine/editor/storage/FilePublication.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <random>
#include <type_traits>

namespace w = lux::editor::workspace;
namespace p = lux::editor::persistence;
namespace v = lux::editor::views;
namespace fs = std::filesystem;
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
        lux::editor::storage::FileArtifactStore backend;
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
            2,
            {{v::ViewRestoreKey{"x"}, v::ViewTypeId{"future.plugin"}, {{"asset:stable", true}}, 0}},
            value.opaque
        };
        auto restored = take(w::decodeRecovery(take(w::encodeRecovery(recovery))));
        assert(restored.entries[0].contents[0].unpersisted_changes && restored.opaque == value.opaque);
        recovery.entries[0].contents.push_back({"asset:second", false});
        recovery.entries[0].primary = 1;
        restored = take(w::decodeRecovery(take(w::encodeRecovery(recovery))));
        assert(restored.entries[0].contents.size() == 2 && restored.entries[0].primary == 1);
        assert(restored.entries[0].contents[1].locator == "asset:second");
        recovery.entries[0].primary = 2;
        assert(!w::encodeRecovery(recovery));
        recovery.entries[0].primary.reset();
        recovery.entries[0].contents[1].locator = "asset:stable";
        assert(!w::encodeRecovery(recovery));
        constexpr std::string_view legacy =
            "schema=1\nopaque=[]\n[[entries]]\nkey='old'\ntype='missing.plugin'\n"
            "locator='asset:old'\nunpersisted=true\nfuture='kept'\n";
        auto legacy_read = take(w::decodeRecovery(bytes(std::string(legacy))));
        assert(legacy_read.schema == 2 && legacy_read.entries[0].primary == 0);
        assert(legacy_read.entries[0].contents[0].locator == "asset:old");
        assert(legacy_read.opaque.back().bytes == bytes(std::string(legacy)));
        const auto reread = take(w::decodeRecovery(take(w::encodeRecovery(legacy_read))));
        assert(reread.opaque == legacy_read.opaque && reread.entries[0].contents[0].unpersisted_changes);
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
        auto preference_result = take(f.coordinator.status(preferences));
        assert(std::holds_alternative<p::NotPublished>(*preference_result.outcome));
        assert(f.coordinator.acknowledge(preferences));
        auto catalog = take(f.store.listLayouts());
        assert(catalog.complete());
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
                lux::editor::storage::FileArtifactStore& real;
                bool retired{};
                explicit Backend(lux::editor::storage::FileArtifactStore& store) : real(store) {}
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
        lux::editor::storage::FileArtifactStore other(f.root / ".lux/..");
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
        auto receipt = take(f.coordinator.status(rename));
        auto catalog = f.store.listLayouts();
        assert(std::holds_alternative<p::CommitReceipt>(*receipt.outcome));
        assert(!catalog && catalog.error().code == w::EWorkspaceError::IO);
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
        assert(plan.layouts().size() == 1 && plan.recovery().entries.size() == 1);
        assert(plan.layouts()[0].slots[0].state.bytes.empty());
        assert(plan.layouts()[0].slots[1].state.bytes == bytes("raw unknown"));
        assert(plan.layouts()[0].opaque[0].bytes == bytes(text));
        assert(plan.layouts()[0].dock.nodes[0].ratio == 2.0 / 3.0);
        const auto id = plan.layouts()[0].id;
        if (collision)
        {
            auto impostor = plan.layouts()[0];
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
        assert(retry.layouts()[0].id == id);
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
        assert(take(restarted.readRecovery()).value.entries[0].contents[0].locator.starts_with("asset:"));
        assert(!take(restarted.continueMigration(retry)));
        assert(raw(old) == text);
        std::puts("X09-05 REAL files: new record before marker, recreated Store, stable mapping, user edit retained, "
                  "old bytes unchanged");
    }
    constexpr std::string_view assetA = "12345678-1234-1234-1234-123456789abc";
    constexpr std::string_view assetB = "12345678-1234-1234-1234-123456789abd";
    std::string legacyMaterial(std::string_view pane, std::string_view asset)
    {
        return "version=1\nfuture='keep original'\ndock='''[Window][Material###" + std::string(pane) +
               "]\nPos=0,0\nSize=800,600\n'''\n[[panes]]\nid='" + std::string(pane) +
               "'\ntype='lux.editor.material.v1'\nvisible=true\npayload='v1:" + std::string(asset) + "'\n";
    }
    void conversion(Fixture& fixture)
    {
        auto capture = [](std::string path, std::string text) {
            auto value = bytes(text);
            auto version = file::publicationDigest(value);
            return w::LegacyWorkspaceFile{std::move(path), std::move(value), std::move(version)};
        };
        w::LegacyWorkspaceInput input;
        input.layouts.push_back(capture(".lux/editor/layouts/Beta.toml", legacyMaterial("material-1", assetB)));
        input.layouts.push_back(capture(".lux/editor/layouts/Alpha.toml", legacyMaterial("material-1", assetA)));
        input.settings = capture(".lux/editor/settings.toml", "version=1\nselected='Beta'\n");
        const auto converted = take(w::prepareLegacyMigration(input));
        assert(converted.layouts().size() == 2 && converted.recovery().entries.size() == 1);
        assert(converted.recovery().entries[0].contents[0].locator == "asset:" + std::string(assetB));
        assert(!fs::exists(fixture.root / ".lux"));
        for (const auto& source : input.layouts)
            writeRaw(
                fixture.root / source.relative_path,
                {reinterpret_cast<const char*>(source.bytes.data()), source.bytes.size()}
            );
        writeRaw(fixture.root / input.settings->relative_path, "version=1\nselected='Beta'\n");
        const auto from_files = take(fixture.store.prepareLegacyMigration());
        assert(converted.sourceDigest() == from_files.sourceDigest());
        assert(take(w::encodeRecovery(converted.recovery())) == take(w::encodeRecovery(from_files.recovery())));
        for (std::size_t i{}; i < converted.layouts().size(); ++i)
        {
            assert(take(w::encodeLayout(converted.layouts()[i])) == take(w::encodeLayout(from_files.layouts()[i])));
            assert(converted.layouts()[i].opaque[0].bytes == input.layouts[1 - i].bytes);
        }
        auto changed = input;
        changed.layouts[0].bytes.push_back(std::byte{});
        assert(w::prepareLegacyMigration(changed).error().code == w::EWorkspaceError::CONFLICT);
        changed = input;
        changed.layouts[0].relative_path = ".lux/editor/layouts/../Beta.toml";
        assert(w::prepareLegacyMigration(changed).error().code == w::EWorkspaceError::INVALID_DATA);
        changed = input;
        changed.layouts.push_back(changed.layouts[0]);
        assert(w::prepareLegacyMigration(changed).error().code == w::EWorkspaceError::INVALID_DATA);
        w::WorkspaceLimits limits;
        limits.file_bytes = std::max(input.layouts[0].bytes.size(), input.layouts[1].bytes.size());
        assert(w::prepareLegacyMigration(input, limits).error().code == w::EWorkspaceError::CAPACITY);
        input.settings.reset();
        const auto unselected = take(w::prepareLegacyMigration(input));
        assert(unselected.layouts().size() == 2 && unselected.recovery().entries.empty());
        assert(!unselected.diagnostics().empty());
        assert(fixture.coordinator.size() == 0);
        std::puts("owned legacy bytes: pure conversion equals IO capture, selected provenance and rejection PASS");
    }
    void legacyPair(
        Fixture& f,
        bool same_key,
        bool same_asset,
        std::optional<std::string_view> selected,
        bool reverse = false
    )
    {
        const auto alpha = legacyMaterial("material-1", assetA);
        const auto beta = legacyMaterial(same_key ? "material-1" : "material-2", same_asset ? assetA : assetB);
        const auto directory = f.root / ".lux/editor/layouts";
        if (reverse)
            writeRaw(directory / "Beta.toml", beta);
        writeRaw(directory / "Alpha.toml", alpha);
        if (!reverse)
            writeRaw(directory / "Beta.toml", beta);
        if (selected)
            writeRaw(f.root / ".lux/editor/settings.toml", "version=1\nselected='" + std::string(*selected) + "'\n");
        for (const auto& filename : {"Alpha.toml", "Beta.toml"})
            std::printf(
                "input %s sha256=%s\n",
                filename,
                file::publicationDigest(bytes(raw(directory / filename))).c_str()
            );
        if (selected)
            std::printf(
                "input settings.toml sha256=%s\n",
                file::publicationDigest(bytes(raw(f.root / ".lux/editor/settings.toml"))).c_str()
            );
        // Both records must independently pass the real SDK's legacy decoder and complete validation.
        for (const auto& name : {"Alpha", "Beta"})
        {
            Fixture single(f.root / "individual" / name);
            const auto filename = std::string(name) + ".toml";
            writeRaw(single.root / ".lux/editor/layouts" / filename, raw(directory / filename));
            writeRaw(single.root / ".lux/editor/settings.toml", "version=1\nselected='" + std::string(name) + "'\n");
            const auto plan = take(single.store.prepareLegacyMigration());
            assert(plan.layouts().size() == 1 && plan.recovery().entries.size() == 1);
            assert(single.coordinator.size() == 0 && !fs::exists(single.root / ".lux/workspace"));
            std::printf("individual %s accepted=1 id=%s\n", name, plan.layouts()[0].id.value.c_str());
        }
    }
    bool scopedRecovery(
        Fixture& f,
        const w::WorkspaceResult<w::LegacyMigration>& result,
        std::string_view selected,
        std::string_view expected_key,
        std::string_view expected_asset
    )
    {
        assert(f.coordinator.size() == 0 && !fs::exists(f.root / ".lux/workspace"));
        if (!result)
        {
            std::printf(
                "multi-layout accepted=0 error=%u detail=%s\n",
                unsigned(result.error().code),
                result.error().detail.c_str()
            );
            return false;
        }
        const auto& plan = *result;
        assert(plan.layouts().size() == 2 && plan.layouts()[0].id != plan.layouts()[1].id);
        std::string manifest;
        for (const auto& layout : plan.layouts())
        {
            const auto relative = ".lux/editor/layouts/" + layout.label + ".toml";
            const auto text = raw(f.root / relative);
            assert(layout.slots.size() == 1 && layout.slots[0].state.bytes.empty());
            assert(layout.opaque[0].bytes == bytes(text));
            assert(layout.legacy_origin == (w::LegacyOrigin{relative, file::publicationDigest(bytes(text))}));
            const auto single = f.root / "individual" / layout.label;
            lux::editor::storage::FileArtifactStore backend(single);
            w::WorkspaceStore store(single, f.coordinator, backend);
            assert(take(store.prepareLegacyMigration()).layouts()[0].id == layout.id);
            if (layout.label == selected)
                assert(plan.preferences().selected_layout == layout.id);
            manifest += relative + "\n" + layout.legacy_origin->digest + "\n";
        }
        const auto settings = f.root / ".lux/editor/settings.toml";
        if (fs::exists(settings))
            manifest += "settings\n" + file::publicationDigest(bytes(raw(settings)));
        assert(plan.sourceDigest() == file::publicationDigest(bytes(manifest))); // Original canonical input digest.
        assert(plan.recovery().legacy_origin == (w::LegacyOrigin{"legacy.workspace.v1", plan.sourceDigest()}));
        assert(plan.preferences().legacy_origin == plan.recovery().legacy_origin);
        const bool expects_empty = expected_asset.empty();
        const bool is_expected_count = plan.recovery().entries.size() == (expects_empty ? 0u : 1u);
        const bool is_scoped =
            is_expected_count &&
            (expects_empty || (plan.recovery().entries[0].contents[0].locator == "asset:" + std::string(expected_asset) &&
                               plan.recovery().entries[0].restore_key.name() == expected_key &&
                               plan.recovery().entries[0].type == v::ViewTypeId{"lux.editor.material.v1"}));
        std::printf(
            "multi-layout accepted=1 layouts=%zu recovery_entries=%zu selected_scope=%d selected=%.*s\n",
            plan.layouts().size(),
            plan.recovery().entries.size(),
            is_scoped,
            int(selected.size()),
            selected.data()
        );
        for (const auto& entry : plan.recovery().entries)
            std::printf(
                "recovery key=%s locator=%s\n",
                std::string(entry.restore_key.name()).c_str(),
                entry.contents[0].locator.c_str()
            );
        return is_scoped;
    }
    bool selectedLegacy(Fixture& f, bool same_key, bool select_beta, bool same_asset = false, bool reverse = false)
    {
        const auto selected = select_beta ? "Beta" : "Alpha";
        legacyPair(f, same_key, same_asset, selected, reverse);
        const auto alpha = raw(f.root / ".lux/editor/layouts/Alpha.toml");
        const auto beta = raw(f.root / ".lux/editor/layouts/Beta.toml");
        const auto settings = raw(f.root / ".lux/editor/settings.toml");
        auto result = f.store.prepareLegacyMigration();
        const auto key = select_beta && !same_key ? "legacy:material-2" : "legacy:material-1";
        const auto asset = select_beta && !same_asset ? assetB : assetA;
        const bool scoped = scopedRecovery(f, result, selected, key, asset);
        assert(raw(f.root / ".lux/editor/layouts/Alpha.toml") == alpha);
        assert(raw(f.root / ".lux/editor/layouts/Beta.toml") == beta);
        assert(raw(f.root / ".lux/editor/settings.toml") == settings);
        if (!scoped)
            return false;
        if (same_key)
            assert(result->layouts()[0].slots[0].restore_key == result->layouts()[1].slots[0].restore_key);
        unsigned writes{};
        while (auto ticket = take(f.store.continueMigration(*result)))
        {
            f.success(*ticket);
            assert(++writes <= 5);
        }
        assert(writes == 5);
        w::WorkspaceStore restarted(f.root, f.coordinator, f.backend);
        const auto saved = take(restarted.readRecovery());
        assert(take(w::encodeRecovery(saved.value)) == take(w::encodeRecovery(result->recovery())));
        assert(take(restarted.listLayouts()).layouts.size() == 2);
        assert(take(restarted.readPreferences()).value.selected_layout == result->preferences().selected_layout);
        for (const auto& layout : result->layouts())
            assert(take(w::encodeLayout(take(restarted.readLayout(layout.id)).value)) == take(w::encodeLayout(layout)));
        assert(!take(restarted.continueMigration(*result)));
        assert(raw(f.root / ".lux/editor/layouts/Alpha.toml") == alpha);
        assert(raw(f.root / ".lux/editor/layouts/Beta.toml") == beta);
        assert(raw(f.root / ".lux/editor/settings.toml") == settings);
        std::puts("REAL files: both layouts preserved, only selected recovery published, original bytes unchanged PASS"
        );
        return true;
    }
    bool selectionFailures(Fixture& f)
    {
        for (const auto& name : {"empty", "absent", "missing"})
        {
            Fixture test(f.root / name);
            const auto selection = std::string_view(name) == "empty"    ? std::optional<std::string_view>{""}
                                   : std::string_view(name) == "absent" ? std::nullopt
                                                                        : std::optional<std::string_view>{"Gone"};
            legacyPair(test, true, false, selection);
            auto result = test.store.prepareLegacyMigration();
            if (!scopedRecovery(test, result, selection.value_or(""), "", ""))
                return false;
            const bool has_selection_diagnostic = std::ranges::any_of(result->diagnostics(), [](const auto& text) {
                return text.find("selected") != text.npos || text.find("settings") != text.npos;
            });
            assert(has_selection_diagnostic);
            assert(bool(result->preferences().selected_layout) == (std::string_view(name) == "missing"));
            unsigned writes{};
            while (auto ticket = take(test.store.continueMigration(*result)))
            {
                test.success(*ticket);
                assert(++writes <= 5);
            }
            assert(take(test.store.readRecovery()).value.entries.empty());
            const auto choice = take(test.store.chooseLayout(take(test.store.readPreferences()).value));
            assert(!choice.layout);
            assert(bool(choice.fallback_reason) == (std::string_view(name) == "missing"));
        }
        Fixture errors(f.root / "errors");
        legacyPair(errors, true, false, "Alpha");
        struct UnreadableSettings final : p::IArtifactStore
        {
            p::IArtifactStore& backend;
            p::EPersistenceError error;
            UnreadableSettings(p::IArtifactStore& value, p::EPersistenceError failure) : backend(value), error(failure)
            {}
            p::PersistenceResult<p::WriteTarget> resolve(std::string_view address) override
            {
                if (address.ends_with("settings.toml"))
                    return lux::cxx::unexpected(p::PersistenceFailure{error, "settings access", 5});
                return backend.resolve(address);
            }
            p::VPublicationOutcome publish(const p::PublicationQuery&, std::stop_token) override
            {
                std::abort();
            }
            p::Reconciliation reconcile(const p::PublicationQuery&) override
            {
                std::abort();
            }
        };
        for (const auto code : {p::EPersistenceError::BUSY, p::EPersistenceError::IO, p::EPersistenceError::CONFLICT})
        {
            UnreadableSettings backend(errors.backend, code);
            w::WorkspaceStore store(errors.root, errors.coordinator, backend);
            const auto result = store.prepareLegacyMigration();
            const auto expected = code == p::EPersistenceError::BUSY       ? w::EWorkspaceError::BUSY
                                  : code == p::EPersistenceError::CONFLICT ? w::EWorkspaceError::CONFLICT
                                                                           : w::EWorkspaceError::IO;
            assert(!result && result.error().code == expected && result.error().native_code == 5);
        }
        const auto settings = errors.root / ".lux/editor/settings.toml";
        for (const auto text : {"version=99\nselected='Alpha'", "version=1\nselected=3", "invalid toml"})
        {
            writeRaw(settings, text);
            assert(!errors.store.prepareLegacyMigration());
        }
        fs::remove(settings);
        fs::create_directory(settings); // Real IO failure, not missing.
        assert(!errors.store.prepareLegacyMigration());
        fs::remove(settings);
        writeRaw(settings, "version=1\nselected='Alpha'");
        const auto old = errors.root / ".lux/editor/layouts/Alpha.toml";
        const auto source = raw(old);
        writeRaw(
            old,
            source + "[[panes]]\nid='material-1'\ntype='lux.editor.material.v1'\npayload='v1:" + std::string(assetB) +
                "'\n"
        );
        assert(!errors.store.prepareLegacyMigration());
        writeRaw(old, "version=1\ndock='invalid'\npanes=[]\n");
        assert(!errors.store.prepareLegacyMigration());
        writeRaw(old, source);
        assert(errors.coordinator.size() == 0 && !fs::exists(errors.root / ".lux/workspace"));
        std::puts("empty/absent/missing selection diagnosed; BUSY/IO/conflict, bad schema/identity/dock rejected PASS");
        return true;
    }
    bool resumeSelected(Fixture& f)
    {
        legacyPair(f, true, false, "Beta");
        auto store = std::make_unique<w::WorkspaceStore>(f.root, f.coordinator, f.backend);
        auto plan = store->prepareLegacyMigration();
        if (!scopedRecovery(f, plan, "Beta", "legacy:material-1", assetB))
            return false;
        f.success(*take(store->continueMigration(*plan)));
        store.reset(); // First record published, no marker. Destroy the actual migration reader.
        const auto first_id = plan->layouts()[0].id;
        f.success(take(f.store.renameLayout(first_id, "user edit after interruption")));
        store = std::make_unique<w::WorkspaceStore>(f.root, f.coordinator, f.backend);
        auto retry = take(store->prepareLegacyMigration());
        assert(retry.sourceDigest() == plan->sourceDigest());
        unsigned writes{};
        while (auto ticket = take(store->continueMigration(retry)))
        {
            f.success(*ticket);
            assert(++writes <= 4);
        }
        assert(writes == 4 && take(store->listLayouts()).layouts.size() == 2);
        assert(take(store->readLayout(first_id)).value.label == "user edit after interruption");
        auto recovery = take(store->readRecovery());
        assert(
            recovery.value.entries.size() == 1 && recovery.value.entries[0].contents[0].locator == "asset:" + std::string(assetB)
        );
        // Existing marker and same-origin user-modified recovery are not permission to silently overwrite it.
        recovery.value.entries.clear();
        f.success(take(store->writeRecovery(recovery.value, recovery.target.expected_version)));
        assert(!take(store->continueMigration(retry)) && take(store->readRecovery()).value.entries.empty());
        for (const auto& layout : retry.layouts())
            assert(bytes(raw(f.root / layout.legacy_origin->key)) == layout.opaque[0].bytes);
        Fixture collision(f.root / "collision");
        legacyPair(collision, true, false, "Beta");
        auto input = take(collision.store.prepareLegacyMigration());
        auto impostor = input.layouts()[0];
        impostor.legacy_origin.reset();
        collision.seed(impostor);
        assert(collision.store.continueMigration(input).error().code == w::EWorkspaceError::CONFLICT);
        assert(!fs::exists(collision.root / ".lux/workspace/recovery.toml"));
        std::puts("two-layout restart: IDs/digest/user edits preserved, selected recovery, marker idempotent, "
                  "collision rejected PASS");
        return true;
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
    // Two invocations of the same scenario must never remove or rename each other's live files.
    // Keep failed directories for diagnosis; directory rename errors are not retried.
    std::mt19937_64 random{std::random_device{}()};
    const fs::path path = fs::absolute(fs::path(argv[1]) / (scenario + "-" + std::to_string(random())));
    assert(path.parent_path() == fs::absolute(argv[1]));
    fs::create_directories(path.parent_path());
    assert(fs::create_directory(path));
    std::printf("isolated workspace fixture: %s\n", path.generic_string().c_str());
    Fixture f(path);
    if (scenario == "r1-alpha")
        return selectedLegacy(f, true, false) ? 0 : 1;
    if (scenario == "r1-beta")
    {
        Fixture forward(path / "forward"), reverse(path / "reverse");
        return selectedLegacy(forward, true, true) && selectedLegacy(reverse, true, true, false, true) ? 0 : 1;
    }
    if (scenario == "r1-extras")
        return selectedLegacy(f, false, false) ? 0 : 1;
    if (scenario == "r1-selection")
        return selectionFailures(f) ? 0 : 1;
    if (scenario == "r1-resume")
        return resumeSelected(f) ? 0 : 1;
    if (scenario == "r1-control")
        return selectedLegacy(f, true, false, true) ? 0 : 1;
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
    else if (scenario == "conversion")
        conversion(f);
    else
        std::abort();
}
