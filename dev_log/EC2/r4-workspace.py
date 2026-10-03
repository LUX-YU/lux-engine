from pathlib import Path
import re,json
s=Path('E:/SyncForder/CodeRepos/lux-engine-ec2');b=s/'editor/activities/workspace';h=b/'include/lux/engine/editor/workspace'
p=h/'WorkspaceStore.hpp';t=p.read_text();a=t.index('    struct LayoutCommitReceipt');z=t.index('    struct LayoutChoice',a);t=t[:a]+t[z:]
a=t.index('    struct LegacyMigration');z=t.index('    class WorkspaceStore',a);t=t[:a]+t[z:]
t=t.replace('#include <filesystem>', '#include <lux/engine/editor/workspace/LegacyWorkspaceMigration.hpp>\n#include <filesystem>')
t=re.sub(r'        \[\[nodiscard\]\] WorkspaceResult<(?:LayoutCommitReceipt|PreferenceWriteResult)>.*?;\n','',t)
t=t.replace('        struct ReadFile final', '        [[nodiscard]] WorkspaceResult<LegacyWorkspaceInput> captureLegacyInput() const;\n        struct ReadFile final')
p.write_text(t)
(h/'LegacyWorkspaceMigration.hpp').write_text('''#pragma once
#include <lux/engine/editor/workspace/LayoutCatalog.hpp>
#include <lux/engine/editor/workspace/LayoutPlan.hpp>
#include <lux/engine/editor/workspace/RecoveryManifest.hpp>

namespace lux::editor::workspace
{
    struct LegacyWorkspaceFile final
    {
        std::string relative_path;
        std::vector<std::byte> bytes;
        std::string version;
    };
    struct LegacyWorkspaceInput final
    {
        std::vector<LegacyWorkspaceFile> layouts;
        std::optional<LegacyWorkspaceFile> settings;
    };
    class LegacyMigration final
    {
    public:
        [[nodiscard]] const std::vector<DockLayout>& layouts() const noexcept { return layouts_; }
        [[nodiscard]] const RecoveryManifest& recovery() const noexcept { return recovery_; }
        [[nodiscard]] const UserPreferences& preferences() const noexcept { return preferences_; }
        [[nodiscard]] const std::string& sourceDigest() const noexcept { return source_digest_; }
        [[nodiscard]] const std::vector<std::string>& diagnostics() const noexcept { return diagnostics_; }

    private:
        friend WorkspaceResult<LegacyMigration> prepareLegacyMigration(LegacyWorkspaceInput, WorkspaceLimits);
        LegacyMigration() = default;
        std::vector<DockLayout> layouts_;
        RecoveryManifest recovery_;
        UserPreferences preferences_;
        std::string source_digest_;
        std::vector<std::string> diagnostics_;
    };
    // Owned bytes only: no files, Root or content service are consulted by conversion.
    [[nodiscard]] WorkspaceResult<LegacyMigration>
    prepareLegacyMigration(LegacyWorkspaceInput, WorkspaceLimits = {});
}
''')
p=b/'src/WorkspaceStore.cpp';t=p.read_text();a=t.index('    WorkspaceResult<LayoutCommitReceipt>');t=t[:a]+'}\n';p.write_text(t)
p=b/'src/LegacyWorkspaceImporter.cpp';t=p.read_text()
head=t[:t.index('    WorkspaceResult<LegacyMigration> WorkspaceStore::prepareLegacyMigration()')]
body=t[t.index('        LegacyMigration migration;'):t.index('    WorkspaceResult<std::optional<persistence::WriteTicket>> WorkspaceStore::continueMigration')]
body=body[:body.rfind('    }')]
body=body.replace('auto settings = read(".lux/editor/settings.toml");','const auto& settings = input.settings;')
body=body.replace('''        else if (settings.error().code != EWorkspaceError::NOT_FOUND)
            return lux::cxx::unexpected(settings.error());
''','')
body=body.replace('const auto& file : files','const auto& file : input.layouts').replace('file.generic_string()', 'std::string_view(file.relative_path).substr(20)')
# The layout-relative directory prefix is 20 characters, preserved in canonical fingerprints.
assert len('.lux/editor/layouts/')==20
body=body.replace('const auto relative = ".lux/editor/layouts/" + std::string_view(file.relative_path).substr(20);','const auto& relative = file.relative_path;')
body=body.replace('''            auto input = read(relative);
            if (!input)
                return lux::cxx::unexpected(input.error());''','            const auto* input = &file;')
body=body.replace('input->target.expected_version','input->version').replace('settings->target.expected_version','settings->version')
body=body.replace('file.stem().string()', 'std::string(std::string_view(file.relative_path).substr(20, file.relative_path.size() - 25))')
body=body.replace('legacyId(std::string_view(file.relative_path).substr(20))','legacyId(std::string_view(file.relative_path).substr(20))')
body=body.replace('limits_','limits')
for member in ['layouts','recovery','preferences','source_digest','diagnostics']:
 body=body.replace('migration.'+member,'migration.'+member+'_')
pure='''    WorkspaceResult<LegacyMigration> prepareLegacyMigration(LegacyWorkspaceInput input, WorkspaceLimits limits)
    {
        if (input.layouts.size() > limits.entries)
            return failed(EWorkspaceError::CAPACITY, "legacy count");
        std::ranges::sort(input.layouts, {}, &LegacyWorkspaceFile::relative_path);
        std::string_view previous;
        for (const auto& file : input.layouts)
        {
            const bool is_invalid_path = !file.relative_path.starts_with(".lux/editor/layouts/") ||
                !file.relative_path.ends_with(".toml") || file.relative_path.size() <= 25;
            if (is_invalid_path)
                return failed(EWorkspaceError::INVALID_DATA, "legacy relative path");
            const auto name = std::string_view(file.relative_path).substr(20);
            const bool has_nested_path = name.find_first_of("/\\\\") != name.npos;
            const bool is_duplicate = file.relative_path == previous;
            if (has_nested_path || is_duplicate)
                return failed(EWorkspaceError::INVALID_DATA, "legacy relative path");
            if (file.version != storage::publicationDigest(file.bytes))
                return failed(EWorkspaceError::CONFLICT, "legacy input version");
            previous = file.relative_path;
        }
        if (input.settings)
        {
            if (input.settings->relative_path != ".lux/editor/settings.toml")
                return failed(EWorkspaceError::INVALID_DATA, "legacy settings path");
            if (input.settings->bytes.size() > limits.file_bytes)
                return failed(EWorkspaceError::CAPACITY, "legacy settings bytes");
            if (input.settings->version != storage::publicationDigest(input.settings->bytes))
                return failed(EWorkspaceError::CONFLICT, "legacy settings version");
        }
'''+body+'    }\n}\n'
head=head.replace('#include <lux/engine/editor/workspace/WorkspaceStore.hpp>','#include <lux/engine/editor/workspace/LegacyWorkspaceMigration.hpp>')
p.write_text(head+pure)
# Store performs real capture and version proof. Prepared migration is fixed and does not need reparsing.
old=t[t.index('        const auto directory = root_'):t.index('        LegacyMigration migration;')]
capture='''#include <lux/engine/editor/workspace/WorkspaceStore.hpp>
#include <lux/engine/editor/storage/FilePublication.hpp>
#include <algorithm>

namespace lux::editor::workspace
{
    namespace
    {
        auto failed(EWorkspaceError code, std::string detail)
        {
            return cxx::unexpected(WorkspaceFailure{code, std::move(detail)});
        }
        std::string fingerprint(const LegacyWorkspaceInput& input)
        {
            std::string manifest;
            for (const auto& file : input.layouts)
                manifest += file.relative_path + "\\n" + file.version + "\\n";
            if (input.settings)
                manifest += "settings\\n" + input.settings->version;
            return storage::publicationDigest(std::as_bytes(std::span(manifest)));
        }
    }
    WorkspaceResult<LegacyWorkspaceInput> WorkspaceStore::captureLegacyInput() const
    {
'''+old+'''        LegacyWorkspaceInput result;
        std::size_t total{};
        for (const auto& file : files)
        {
            auto relative = ".lux/editor/layouts/" + file.generic_string();
            auto captured = read(relative);
            if (!captured)
                return cxx::unexpected(captured.error());
            if (captured->bytes.size() > limits_.file_bytes - total)
                return failed(EWorkspaceError::CAPACITY, "migration aggregate input bytes");
            total += captured->bytes.size();
            result.layouts.push_back({std::move(relative), std::move(captured->bytes), captured->target.expected_version});
        }
        auto settings = read(".lux/editor/settings.toml");
        if (settings)
            result.settings = LegacyWorkspaceFile{
                ".lux/editor/settings.toml", std::move(settings->bytes), settings->target.expected_version
            };
        else if (settings.error().code != EWorkspaceError::NOT_FOUND)
            return cxx::unexpected(settings.error());
        return result;
    }
    WorkspaceResult<LegacyMigration> WorkspaceStore::prepareLegacyMigration() const
    {
        auto input = captureLegacyInput();
        if (!input)
            return cxx::unexpected(input.error());
        return workspace::prepareLegacyMigration(std::move(*input), limits_);
    }
'''
cont=t[t.index('    WorkspaceResult<std::optional<persistence::WriteTicket>> WorkspaceStore::continueMigration'):]
cont=cont.replace('auto current = prepareLegacyMigration();','auto current = captureLegacyInput();').replace('current->source_digest != migration.source_digest','fingerprint(*current) != migration.sourceDigest()')
for mem,method in [('source_digest','sourceDigest'),('layouts','layouts'),('recovery','recovery'),('preferences','preferences')]:
 cont=cont.replace('current->'+mem,'migration.'+method+'()')
cont=cont.replace('// Compare against freshly read legacy input so callers cannot forge provenance or accidentally mix projects.', '// Revalidate bytes/versions without parsing the unchanged immutable migration plan again.')
(b/'src/WorkspaceMigration.cpp').write_text(capture+cont)
p=b/'CMakeLists.txt';t=p.read_text().replace('src/LegacyWorkspaceImporter.cpp)','src/LegacyWorkspaceImporter.cpp src/WorkspaceMigration.cpp)');p.write_text(t)
# Same tests assert publication facts independently of catalog IO, without the removed mixed receipt.
p=s/'editor/tests/workspace/workspace.cpp';t=p.read_text()
t=t.replace('take(f.store.preferenceResult(preferences))','take(f.coordinator.status(preferences))').replace('preference_result.publication.outcome','preference_result.outcome')
t=t.replace('auto receipt = take(f.store.layoutResult(ticket));\n        assert(receipt.catalog && receipt.catalog->complete());','auto catalog = take(f.store.listLayouts());\n        assert(catalog.complete());')
t=t.replace('auto receipt = take(f.store.layoutResult(rename));','auto receipt = take(f.coordinator.status(rename));\n        auto catalog = f.store.listLayouts();').replace('receipt.publication.outcome','receipt.outcome').replace('!receipt.catalog && receipt.catalog.error().code','!catalog && catalog.error().code')
idx=t.index('    void legacy(Fixture&')
prefix=t[:idx];rest=t[idx:]
for mem,method in [('source_digest','sourceDigest'),('layouts','layouts'),('recovery','recovery'),('preferences','preferences'),('diagnostics','diagnostics')]:
 rest=re.sub(rf'(\b(?:plan|retry|result|input)(?:\.|->)){mem}\b',rf'\1{method}()',rest)
rest=rest.replace('take(store.prepareLegacyMigration()).layouts[','take(store.prepareLegacyMigration()).layouts()[')
p.write_text(prefix+rest)
p=s/'editor/application/src/EditorWorkspace.cpp';t=p.read_text().replace('migration_->diagnostics','migration_->diagnostics()');p.write_text(t)
p=s/'editor/tests/architecture/rules.json';j=json.loads(p.read_text())
for f in ['src/WorkspaceMigration.cpp','include/lux/engine/editor/workspace/LegacyWorkspaceMigration.hpp']:
 j['editor_layering']['files']['editor/activities/workspace/'+f]=['workspace_store']
for v in j.values():
 if isinstance(v,dict) and isinstance(v.get('files'),list) and 'editor/activities/workspace/src/LegacyWorkspaceImporter.cpp' in v['files']:
  v['files']+=['editor/activities/workspace/src/WorkspaceMigration.cpp','editor/activities/workspace/include/lux/engine/editor/workspace/LegacyWorkspaceMigration.hpp']
p.write_text(json.dumps(j,indent=2)+'\n')
print('R4 workspace fixed-byte conversion and explicit IO separated')
