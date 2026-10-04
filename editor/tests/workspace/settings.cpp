#include <lux/engine/editor/configuration/Settings.hpp>
#include <lux/engine/editor/configuration/EditorReflection.hpp>
#include <lux/engine/editor/workspace/WorkspaceStore.hpp>
#include <lux/engine/editor/workspace/WorkspaceChanges.hpp>
#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <lux/engine/editor/storage/FilePublication.hpp>
#include <lux/engine/meta/TypeStaticInfo.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>

struct SettingsFixtureValue final
{
    float scale{1};
    std::string font;
};
namespace lux::meta
{
    template <> struct TTypeStaticInfo<SettingsFixtureValue>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&SettingsFixtureValue::scale>("scale"),
            typeStaticField<&SettingsFixtureValue::font>("font")
        );
    };
} // namespace lux::meta
namespace
{
    using namespace lux;
    using namespace lux::editor;
    using namespace lux::editor::settings;
    template <class T> auto take(T result)
    {
        if (!result)
        {
            if constexpr (requires { result.error().detail; })
                std::fprintf(
                    stderr,
                    "unexpected %u %s\n",
                    unsigned(result.error().code),
                    result.error().detail.c_str()
                );
            else
                std::fprintf(stderr, "unexpected file publication error\n");
            std::abort();
        }
        return std::move(*result);
    }
    void registerValue(meta::ReflectionRegistry& registry, meta::qual_type_index_fix_list&)
    {
        auto type = std::make_unique<meta::RefClass>();
        type->name = "SettingsFixtureValue";
        type->full_name = cxx::type_name<SettingsFixtureValue>();
        type->hash = cxx::type_hash<SettingsFixtureValue>();
        type->type = meta::ref_type_of_v<SettingsFixtureValue>;
        type->type.ptr = type.get();
        type->construct = [](void* p) { std::construct_at(static_cast<SettingsFixtureValue*>(p)); };
        type->destruct = [](void* p) { std::destroy_at(static_cast<SettingsFixtureValue*>(p)); };
        registry.registerClass(std::move(type));
    }
    const ConfigurationDescriptor configuration{
        "test.settings",
        1,
        serialization::makePortableValueCodec<SettingsFixtureValue>(),
        +[](meta::ReflectionRegistry& registry) noexcept
        { return registry.findClass(cxx::type_name<SettingsFixtureValue>()); }
    };
    constexpr SettingsDescriptor descriptor{
        SettingsIdView{"test.settings"},
        "Settings",
        &configuration,
        kPersonalScopes,
        ESettingsApply::RESTART,
        +[](const ConfigurationValue& value) noexcept -> SettingsResult<void>
        {
            const auto& typed = *static_cast<const SettingsFixtureValue*>(value.data());
            if (!(typed.scale >= 0.5F && typed.scale <= 3.0F))
                return cxx::unexpected(SettingsFailure{ESettingsError::INVALID_VALUE, "scale"});
            return {};
        }
    };
    std::vector<std::byte> bytes(std::string_view text)
    {
        auto data = std::as_bytes(std::span(text));
        return {data.begin(), data.end()};
    }
    SettingsValue payload(float scale)
    {
        SettingsFixtureValue value{scale, "owned.ttf"};
        std::vector<std::byte> data;
        assert(configuration.codec.encode(&value, data));
        return {"test.settings", 1, std::move(data)};
    }
    void values()
    {
        auto entry = SettingsEntry::bind<descriptor>(lux::object::CodeLease::builtin());
        assert(&entry->descriptor() == &descriptor);
        assert(validateSettingsEntries(std::array{entry}));
        assert(validateSettingsEntries(std::array{entry, entry}).error().code == ESettingsError::DUPLICATE);
        SettingsDocument user, install, launch;
        user.values = {payload(1.5F)};
        user.file_version = "S0";
        install.scope = ESettingsScope::INSTALLATION;
        install.values = {payload(0.75F)};
        launch.scope = ESettingsScope::LAUNCH;
        launch.values = {payload(2.0F)};
        auto resolved = take(resolveSettings(entry, std::array{user, launch, install}));
        assert(resolved.source == ESettingsScope::LAUNCH);
        assert(static_cast<const SettingsFixtureValue*>(resolved.desired.data())->scale == 2.0F);
        auto duplicate = resolveSettings(entry, std::array{user, user});
        assert(!duplicate && duplicate.error().code == ESettingsError::DUPLICATE);
        auto project = user;
        project.scope = ESettingsScope::PROJECT;
        assert(resolveSettings(entry, std::array{project}).error().code == ESettingsError::INVALID_SCOPE);
        auto bad = user;
        bad.values = {payload(9)};
        assert(resolveSettings(entry, std::array{bad}).error().code == ESettingsError::INVALID_VALUE);
        bad.values[0].schema = 4;
        assert(resolveSettings(entry, std::array{bad}).error().code == ESettingsError::UNSUPPORTED_VERSION);
        auto draft = take(makeSettingsDraft(resolved, user));
        assert(!draft.applied && draft.persisted == user.values[0].bytes);
        auto changed = user;
        changed.file_version = "S1";
        assert(prepareSettings(draft, changed, *entry).error().code == ESettingsError::CONFLICT);
        auto replacement = SettingsEntry::bind<descriptor>(lux::object::CodeLease::builtin());
        assert(prepareSettings(draft, user, *replacement).error().code == ESettingsError::CONFLICT);
        user.values.push_back({"missing.plugin", 97, {std::byte{13}, std::byte{255}}});
        auto prepared = take(prepareSettings(draft, user, *entry));
        assert(prepared.values.back() == user.values.back());
        assert(prepared.file_version == "S0" && !draft.applied);
        assert(draft.persisted == user.values[0].bytes && prepared.values[0].bytes != *draft.persisted);
        std::string name = "dynamic.settings", label = "Dynamic settings";
        auto dynamic_descriptor = descriptor;
        dynamic_descriptor.id = SettingsIdView{name};
        dynamic_descriptor.label = label;
        auto dynamic = SettingsEntry::create(lux::object::CodeLease::builtin(), dynamic_descriptor);
        name.assign(4096, 'x');
        label.clear();
        assert(
            dynamic->descriptor().id.name() == "dynamic.settings" && dynamic->descriptor().label == "Dynamic settings"
        );
        assert(dynamic->defaults());
        auto applied_scale = 1.0F;
        auto immediate = descriptor;
        immediate.apply = ESettingsApply::IMMEDIATE;
        assert(!SettingsEntry::create(lux::object::CodeLease::builtin(), immediate)->validateDescriptor());
        auto active = SettingsEntry::create(
            lux::object::CodeLease::builtin(),
            immediate,
            [&](const ConfigurationValue& value) -> SettingsResult<void>
            {
                applied_scale = static_cast<const SettingsFixtureValue*>(value.data())->scale;
                return {};
            }
        );
        assert(active->apply(draft.desired) && applied_scale == 2.0F);
        assert(!draft.applied && draft.persisted == user.values[0].bytes);
        // A page records the actual application receipt; file preparation does not invent one.
        std::vector<std::byte> applied;
        assert(draft.desired.encode(applied));
        draft.applied = std::move(applied);
        assert(draft.applied != draft.persisted);
        std::puts(
            "V26/V30: real ConfigurationValue, ordered scopes, validation, file/descriptor conflict, independent facts"
        );
    }
    void codec()
    {
        const auto source = bytes(
            "schema=1\nscope=2\nunknown={ nested=[1,2,3] }\n[[values]]\nid='missing.plugin'\nschema=47\nsize=2\nbytes='00ff'\nfuture='keep me'\n"
        );
        auto document = take(decodeSettings(source));
        document.values.push_back(payload(1.25F));
        auto encoded = take(encodeSettings(document));
        auto decoded = take(decodeSettings(encoded));
        assert(decoded.values == document.values);
        assert(decoded.preserved.find("keep me") != std::string::npos);
        assert(decoded.preserved.find("nested") != std::string::npos);
        auto again = take(encodeSettings(decoded));
        assert(again == encoded);
        assert(
            decodeSettings(bytes("schema=9\nscope=2\nvalues=[]")).error().code == ESettingsError::UNSUPPORTED_VERSION
        );
        assert(!decodeSettings(bytes("invalid TOML [")));
        auto future = document;
        future.preserved = "schema=9\nscope=2\nvalues=[]";
        assert(encodeSettings(future).error().code == ESettingsError::UNSUPPORTED_VERSION);
        auto duplicate = document;
        duplicate.values.push_back(duplicate.values.front());
        assert(encodeSettings(duplicate).error().code == ESettingsError::DUPLICATE);
        assert(decodeSettings(source, {32, 256, 32}).error().code == ESettingsError::CAPACITY);
        assert(decodeSettings(source, {4096, 0, 32}).error().code == ESettingsError::CAPACITY);
        assert(decodeSettings(source, {4096, 256, 1}).error().code == ESettingsError::CAPACITY);
        std::puts(
            "V28: unknown rows/fields value-preserved; future/syntax/size/depth reject; no format/comment guarantee"
        );
    }
    void io(const std::filesystem::path& base)
    {
        auto root = base / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(root);
        persistence::WriteCoordinator coordinator;
        storage::FileArtifactStore backend(root);
        workspace::WorkspaceStore store(root, coordinator, backend);
        const auto settle = [&](persistence::WriteTicket ticket)
        {
            auto work = take(coordinator.takeReady());
            assert(work && work->ticket == ticket);
            auto outcome = backend.publish(*work);
            assert(coordinator.complete(ticket, outcome));
            return outcome;
        };
        auto missing = store.readSettings("settings.toml", ESettingsScope::USER);
        assert(!missing && missing.error().code == workspace::EWorkspaceError::NOT_FOUND);
        SettingsDocument value;
        value.values = {payload(1.5F), {"missing.plugin", 7, bytes("preserved")}};
        auto first = take(store.writeSettings("settings.toml", value));
        assert(std::holds_alternative<persistence::CommitReceipt>(settle(first)));
        assert(coordinator.acknowledge(first));
        auto read = take(store.readSettings("settings.toml", ESettingsScope::USER));
        assert(read.values == value.values && read.file_version != "missing");
        assert(!store.readSettings("settings.toml", ESettingsScope::USER_PROJECT));
        // A valid but old file source must still conflict at the real file publication boundary.
        value.values[0] = payload(2);
        auto stale = take(store.writeSettings("settings.toml", value));
        auto outcome = settle(stale);
        assert(std::holds_alternative<persistence::NotPublished>(outcome));
        assert(coordinator.acknowledge(stale));
        assert(take(store.readSettings("settings.toml", ESettingsScope::USER)).values == read.values);
        auto second_value = read;
        second_value.values[0] = payload(2);
        auto second = take(store.writeSettings("settings.toml", second_value));
        auto ready = take(coordinator.takeReady());
        assert(ready && ready->ticket == second);
        auto published = backend.publish(*ready);
        assert(std::holds_alternative<persistence::CommitReceipt>(published));
        // Hide the actual receipt: the original lane must remain occupied until reconciliation.
        assert(coordinator.complete(
            second,
            persistence::PublicationUnknown{{persistence::EPersistenceError::IO, "lost reply"}}
        ));
        assert(!coordinator.acknowledge(second));
        auto third_value = second_value;
        third_value.file_version = std::get<persistence::CommitReceipt>(published).version;
        third_value.values[0] = payload(2.25F);
        auto third = take(store.writeSettings("settings.toml", third_value));
        assert(!take(coordinator.takeReady()));
        assert(coordinator.reconcile(second, backend));
        assert(coordinator.acknowledge(second));
        assert(take(store.readSettings("settings.toml", ESettingsScope::USER)).values == second_value.values);
        assert(std::holds_alternative<persistence::CommitReceipt>(settle(third)));
        assert(coordinator.acknowledge(third));
        auto corrupt = root / "corrupt.toml";
        {
            std::ofstream out(corrupt);
            out << "invalid [";
        }
        const auto before = take(storage::readPublicationFile(corrupt, 4096));
        auto invalid = store.readSettings("corrupt.toml", ESettingsScope::USER);
        assert(!invalid && invalid.error().code == workspace::EWorkspaceError::INVALID_DATA);
        assert(take(storage::readPublicationFile(corrupt, 4096)) == before);
        workspace::WorkspaceChanges changes(store, coordinator, backend);
        persistence::WriteTicket accepted;
        {
            // Simulates closing the editing page immediately after admission. The queued file
            // and terminal result still belong to the same application-lifetime activity owner.
            auto page_document = take(store.readSettings("settings.toml", ESettingsScope::USER));
            page_document.values[0] = payload(1.75F);
            accepted = take(changes.saveSettings("settings.toml", page_document));
        }
        assert(!changes.settled());
        assert(!changes.acknowledge(accepted));
        assert(std::holds_alternative<persistence::CommitReceipt>(settle(accepted)));
        assert(changes.update());
        assert(changes.settled() && changes.publications().size() == 1);
        const auto& report = changes.publications().front();
        assert(report.ticket == accepted && report.result && !report.catalog_failure && !report.refresh_catalog);
        assert(!coordinator.status(accepted)); // Exactly one coordinator acknowledgement owner.
        assert(take(store.readSettings("settings.toml", ESettingsScope::USER)).values[0] == payload(1.75F));
        assert(changes.acknowledge(accepted) && changes.publications().empty());
        std::puts("V31: real file read/write/conflict and Unknown original lane; corrupt file untouched");
    }
    void profile(const std::filesystem::path& base)
    {
        const auto root =
            base / ("profile-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root / "project");
        std::filesystem::create_directories(root / "user");
        persistence::WriteCoordinator coordinator;
        storage::FileArtifactStore backend(root);
        workspace::WorkspaceStore old(root / "project", coordinator, backend),
            user(root / "user", coordinator, backend);
        const asset::AssetId project{*uuids::uuid::from_string("491f06e3-8618-4dfe-ae93-4c06b2b0e172")};
        const auto settle = [&](persistence::WriteTicket ticket)
        {
            auto ready = take(coordinator.takeReady());
            assert(ready && ready->ticket == ticket);
            auto result = backend.publish(*ready);
            assert(std::holds_alternative<persistence::CommitReceipt>(result));
            assert(coordinator.complete(ticket, std::move(result)) && coordinator.acknowledge(ticket));
        };
        assert(!take(user.continueProfileMigration(old, project))); // Real missing; never writes defaults.
        workspace::UserPreferences preferences{1, workspace::LayoutId{"0123456789abcdef0123456789abcdef"}};
        preferences.opaque.push_back({"absent.plugin", 9, {std::byte{17}}});
        preferences.legacy_origin = workspace::LegacyOrigin{"original", "preserved"};
        settle(take(old.writePreferences(preferences, "missing")));
        const auto source_file = root / "project/.lux/workspace/preferences.toml";
        const auto original = take(storage::readPublicationFile(source_file, 4096));
        auto prepared = take(user.continueProfileMigration(old, project));
        assert(prepared);
        settle(*prepared);
        assert(!user.readPreferences()); // Durable preparation is not a copied or completed profile.
        auto copy = take(user.continueProfileMigration(old, project));
        assert(copy);
        settle(*copy);
        auto copied = take(user.readPreferences());
        assert(copied.value.selected_layout == preferences.selected_layout);
        assert(copied.value.opaque == preferences.opaque && copied.value.legacy_origin == preferences.legacy_origin);
        assert(std::filesystem::exists(root / "user/.lux/workspace/profile-migration-v1.toml"));
        auto mark = take(user.continueProfileMigration(old, project));
        assert(mark);
        settle(*mark);
        assert(!take(user.continueProfileMigration(old, project)));
        copied.value.selected_layout.reset();
        settle(take(user.writePreferences(copied.value, copied.target.expected_version)));
        assert(!take(user.continueProfileMigration(old, project)));
        assert(!take(user.readPreferences()).value.selected_layout); // Same-origin personal edit remains.
        assert(take(storage::readPublicationFile(source_file, 4096)) == original);
        const asset::AssetId wrong_project{*uuids::uuid::from_string("591f06e3-8618-4dfe-ae93-4c06b2b0e172")};
        assert(user.continueProfileMigration(old, wrong_project).error().code == workspace::EWorkspaceError::CONFLICT);
        std::filesystem::create_directories(root / "unmarked");
        workspace::WorkspaceStore unmarked(root / "unmarked", coordinator, backend);
        settle(take(unmarked.writePreferences(copied.value, "missing")));
        assert(unmarked.continueProfileMigration(old, project).error().code == workspace::EWorkspaceError::CONFLICT);
        workspace::WorkspaceStore corrupt(root / "corrupt-profile", coordinator, backend);
        std::filesystem::create_directories(root / "corrupt-profile/.lux/workspace");
        {
            std::ofstream file(root / "corrupt-profile/.lux/workspace/preferences.toml");
            file << "invalid [";
        }
        std::filesystem::create_directories(root / "other-user");
        workspace::WorkspaceStore destination(root / "other-user", coordinator, backend);
        assert(
            destination.continueProfileMigration(corrupt, project).error().code ==
            workspace::EWorkspaceError::INVALID_DATA
        );
        assert(std::filesystem::is_empty(root / "other-user"));
        std::puts(
            "V29: preferences copy/marker/retry/selected-layout/unknown/origin, personal edits, conflicts and corrupt source"
        );
    }
    void completeProfile(const std::filesystem::path& base)
    {
        const auto root =
            base / ("complete-profile-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root / "project");
        std::filesystem::create_directories(root / "profile");
        persistence::WriteCoordinator coordinator;
        storage::FileArtifactStore backend(root);
        workspace::WorkspaceStore source(root / "project", coordinator, backend),
            target(root / "profile", coordinator, backend);
        const asset::AssetId project{*uuids::uuid::from_string("591f06e3-8618-4dfe-ae93-4c06b2b0e172")};
        const auto publish = [&](persistence::WriteTicket ticket, bool acknowledge)
        {
            auto ready = take(coordinator.takeReady());
            assert(ready && ready->ticket == ticket);
            auto outcome = backend.publish(*ready);
            assert(std::holds_alternative<persistence::CommitReceipt>(outcome));
            assert(coordinator.complete(ticket, std::move(outcome)));
            if (acknowledge)
                assert(coordinator.acknowledge(ticket));
        };
        std::vector<std::pair<std::string, std::vector<std::byte>>> originals;
        for (int i{}; i < 24; ++i)
        {
            workspace::DockLayout layout;
            layout.id.value = "1234567890abcdef1234567890ab" + std::to_string(1000 + i);
            layout.label = "Layout " + std::to_string(i);
            layout.slots = {
                {{1},
                 views::ViewRestoreKey{"material"},
                 views::ViewTypeId{"test.material"},
                 true,
                 {91, bytes("unknown view payload")}}
            };
            layout.dock.nodes = {{1, workspace::EDockSplit::LEAF, 0, 0, 0.5, {{1}}}};
            layout.dock.roots = {{1}};
            publish(take(source.saveLayout(layout, "missing")), true);
            const auto relative = ".lux/workspace/layouts/" + layout.id.value + ".layout";
            originals.emplace_back(relative, take(storage::readPublicationFile(root / "project" / relative, 4096)));
        }
        workspace::UserPreferences prefs;
        prefs.selected_layout = workspace::LayoutId{"1234567890abcdef1234567890ab1007"};
        prefs.opaque.push_back({"unknown", 22, bytes("keep")});
        publish(take(source.writePreferences(prefs, "missing")), true);
        workspace::RecoveryManifest recovery;
        recovery.entries.push_back(
            {views::ViewRestoreKey{"material"},
             views::ViewTypeId{"test.material"},
             {{"asset:591f06e3-8618-4dfe-ae93-4c06b2b0e172", true}},
             0}
        );
        recovery.opaque = prefs.opaque;
        publish(take(source.writeRecovery(recovery, "missing")), true);
        for (const auto file : {"preferences.toml", "recovery.toml"})
        {
            const auto relative = std::string{".lux/workspace/"} + file;
            originals.emplace_back(relative, take(storage::readPublicationFile(root / "project" / relative, 4096)));
        }
        workspace::WorkspaceChanges changes(target, coordinator, backend, &source);
        assert(changes.migrateProfile(source, project));
        assert(changes.migrationPending() && changes.refresh().error().code == EEditorError::BUSY);
        auto first = changes.publications().front().ticket;
        auto ready = take(coordinator.takeReady());
        assert(ready && ready->ticket == first);
        auto confirmed = backend.publish(*ready);
        assert(std::holds_alternative<persistence::CommitReceipt>(confirmed));
        assert(coordinator.complete(
            first,
            persistence::PublicationUnknown{{persistence::EPersistenceError::IO, "lost reply"}}
        ));
        for (int i{}; i < 4; ++i)
            assert(changes.update());
        assert(changes.publications().size() == 1 && !changes.publications().front().result);
        assert(!take(coordinator.takeReady())); // Unknown keeps the original lane; no repeated copy.
        assert(changes.reconcile(first));
        for (int i{}; i < 40 && !changes.migrationComplete(); ++i)
        {
            assert(changes.update());
            assert(!changes.migrationFailure() && changes.publications().size() <= 1);
            if (changes.migrationPending())
                publish(changes.publications().front().ticket, false);
        }
        assert(changes.migrationComplete() && !changes.migrationPending());
        assert(changes.catalog().layouts.size() == 24 && changes.settled());
        assert(take(target.readPreferences()).value.selected_layout == prefs.selected_layout);
        assert(
            take(target.readRecovery()).value.entries.front().contents.front().locator ==
            recovery.entries.front().contents.front().locator
        );
        for (const auto& [file, original] : originals)
        {
            assert(take(storage::readPublicationFile(root / "project" / file, 4096)) == original);
            assert(take(storage::readPublicationFile(root / "profile" / file, 4096)) == original);
        }
        // A prepared migration cannot combine records from two source revisions.
        std::filesystem::create_directories(root / "interrupted");
        workspace::WorkspaceStore interrupted(root / "interrupted", coordinator, backend);
        publish(*take(interrupted.continueProfileMigration(source, project)), true);
        auto original_preferences = take(source.readPreferences());
        original_preferences.value.selected_layout.reset();
        publish(
            take(source.writePreferences(original_preferences.value, original_preferences.target.expected_version)),
            true
        );
        assert(
            interrupted.continueProfileMigration(source, project).error().code == workspace::EWorkspaceError::CONFLICT
        );
        assert(!interrupted.readPreferences());
        assert(!take(target.continueProfileMigration(source, project))); // Completed profile is never overwritten.
        std::puts(
            "V29: complete 24-layout profile, selected/recovery/raw bytes, bounded receipts, Unknown, source pin and idempotency"
        );
    }

} // namespace
int main(int argc, char** argv)
{
    assert(argc == 2);
    auto reflection = lux::editor::acquireEditorReflection();
    auto draft = lux::meta::ReflectionRegistry::beginDraft();
    assert(draft.append(&registerValue, std::make_shared<int>(1)) && draft.commit());
    values();
    codec();
    io(argv[1]);
    profile(argv[1]);
    completeProfile(argv[1]);
}
