#include <lux/engine/editor/workspace/WorkspaceStore.hpp>
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
    }
    WorkspaceResult<LegacyWorkspaceInput> WorkspaceStore::captureLegacyInput() const
    {
        const auto directory = root_ / ".lux/editor/layouts";
        std::error_code error;
        const bool exists = std::filesystem::exists(directory, error);
        if (error)
            return failed(EWorkspaceError::IO, "legacy directory status");
        std::vector<std::filesystem::path> files;
        if (exists)
        {
            std::filesystem::directory_iterator it(directory, error), end;
            for (; !error && it != end; it.increment(error))
            {
                if (it->path().extension() != ".toml")
                    continue;
                if (files.size() >= limits_.entries)
                    return failed(EWorkspaceError::CAPACITY, "legacy count");
                files.push_back(it->path().filename());
            }
            if (error)
                return failed(EWorkspaceError::IO, "legacy enumeration incomplete");
        }
        std::ranges::sort(files);
        LegacyWorkspaceInput result;
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
        std::ranges::sort(result.layouts, {}, &LegacyWorkspaceFile::relative_path);
        return result;
    }
    WorkspaceResult<LegacyMigration> WorkspaceStore::prepareLegacyMigration() const
    {
        auto input = captureLegacyInput();
        if (!input)
            return cxx::unexpected(input.error());
        return workspace::prepareLegacyMigration(std::move(*input), limits_);
    }
    WorkspaceResult<std::optional<persistence::WriteTicket>> WorkspaceStore::continueMigration(
        const LegacyMigration& migration
    )
    {
        // Every retry consults the real disk, never a private in-memory 'done' bit.
        // Revalidate bytes/versions without parsing the unchanged immutable migration plan again.
        auto current = captureLegacyInput();
        if (!current)
            return lux::cxx::unexpected(current.error());
        if (current->sourceDigest() != migration.sourceDigest())
            return failed(EWorkspaceError::CONFLICT, "legacy input changed");
        const auto marker_path = ".lux/workspace/migration-v1.toml";
        const auto marker_text = "schema = 1\nsource = \"" + migration.sourceDigest() + "\"\n";
        const auto marker_bytes = std::as_bytes(std::span(marker_text));
        auto marker = read(marker_path);
        if (marker)
        {
            if (!std::ranges::equal(marker->bytes, marker_bytes))
                return failed(EWorkspaceError::CONFLICT, "migration marker");
            return std::optional<persistence::WriteTicket>{};
        }
        if (marker.error().code != EWorkspaceError::NOT_FOUND)
            return lux::cxx::unexpected(marker.error());
        auto accepted = [](WorkspaceResult<persistence::WriteTicket> value
                        ) -> WorkspaceResult<std::optional<persistence::WriteTicket>> {
            if (!value)
                return lux::cxx::unexpected(value.error());
            return std::optional{*value};
        };
        for (const auto& layout : migration.layouts())
        {
            auto existing = readLayout(layout.id);
            if (!existing)
            {
                if (existing.error().code != EWorkspaceError::NOT_FOUND)
                    return lux::cxx::unexpected(existing.error());
                return accepted(saveLayout(layout, "missing"));
            }
            if (existing->value.legacy_origin != layout.legacy_origin)
                return failed(EWorkspaceError::CONFLICT, "migration layout collision");
        }
        auto recovery = readRecovery();
        if (!recovery)
        {
            if (recovery.error().code != EWorkspaceError::NOT_FOUND)
                return lux::cxx::unexpected(recovery.error());
            return accepted(writeRecovery(migration.recovery(), "missing"));
        }
        if (recovery->value.legacy_origin != migration.recovery().legacy_origin)
            return failed(EWorkspaceError::CONFLICT, "migration recovery collision");
        auto preferences = readPreferences();
        if (!preferences)
        {
            if (preferences.error().code != EWorkspaceError::NOT_FOUND)
                return lux::cxx::unexpected(preferences.error());
            return accepted(writePreferences(migration.preferences(), "missing"));
        }
        if (preferences->value.legacy_origin != migration.preferences().legacy_origin)
            return failed(EWorkspaceError::CONFLICT, "migration preferences collision");
        return accepted(write(marker_path, "missing", {marker_bytes.begin(), marker_bytes.end()}));
    }
}
