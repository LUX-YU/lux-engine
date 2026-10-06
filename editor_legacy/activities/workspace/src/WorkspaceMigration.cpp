#include <lux/engine/editor/workspace/WorkspaceStore.hpp>
#include <lux/engine/editor/storage/FilePublication.hpp>
#include <algorithm>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <toml++/toml.hpp>

namespace lux::editor::workspace
{
    namespace
    {
        auto failed(EWorkspaceError code, std::string detail)
        {
            return cxx::unexpected(WorkspaceFailure{code, std::move(detail)});
        }
    } // namespace
    WorkspaceResult<std::optional<persistence::WriteTicket>> WorkspaceStore::continueProfileMigration(
        const WorkspaceStore& source,
        const asset::AssetId& project
    )
    {
        if (project.isNull())
            return failed(EWorkspaceError::INVALID_DATA, "profile migration requires persistent project identity");
        constexpr std::string_view marker_path = ".lux/workspace/profile-migration-v1.toml";
        const auto project_key = uuids::to_string(project.uuid());
        auto marker = read(marker_path);
        std::string pinned_digest;
        if (marker)
        {
            const std::string_view text{reinterpret_cast<const char*>(marker->bytes.data()), marker->bytes.size()};
            auto parsed = toml::parse(text);
            if (!parsed)
                return failed(EWorkspaceError::INVALID_DATA, "profile migration marker syntax");
            auto complete = parsed["complete"].value<bool>();
            pinned_digest = parsed["source"].value_or(std::string{});
            const bool invalid_marker = parsed["schema"].value<std::int64_t>() != 1 ||
                                        parsed["project"].value<std::string>() != project_key ||
                                        pinned_digest.empty() || !complete;
            if (invalid_marker)
                return failed(EWorkspaceError::CONFLICT, "profile migration marker identity");
            // A completed marker is provenance, not a command to overwrite later personal edits.
            if (*complete)
                return std::optional<persistence::WriteTicket>{};
        }
        else if (marker.error().code != EWorkspaceError::NOT_FOUND)
            return cxx::unexpected(marker.error());

        auto catalog = source.listLayouts();
        if (!catalog)
            return cxx::unexpected(catalog.error());
        if (!catalog->complete())
            return cxx::unexpected(catalog->diagnostics.front().failure);
        std::vector<std::pair<std::string, ReadFile>> input;
        input.reserve(catalog->layouts.size() + 3);
        std::size_t total{};
        const auto append = [&](std::string path, auto validate) -> WorkspaceResult<void>
        {
            auto captured = source.read(path);
            if (!captured)
            {
                if (captured.error().code == EWorkspaceError::NOT_FOUND)
                    return {};
                return cxx::unexpected(captured.error());
            }
            auto valid = validate(captured->bytes);
            if (!valid)
                return cxx::unexpected(valid.error());
            if (captured->bytes.size() > limits_.file_bytes - total)
                return failed(EWorkspaceError::CAPACITY, "profile aggregate input bytes");
            total += captured->bytes.size();
            input.emplace_back(std::move(path), std::move(*captured));
            return {};
        };
        for (const auto& layout : catalog->layouts)
        {
            auto added = append(
                ".lux/workspace/layouts/" + layout.id.value + ".layout",
                [&](auto bytes) { return decodeLayout(bytes, limits_); }
            );
            if (!added)
                return cxx::unexpected(added.error());
            // Enumeration/read races must not turn a missing record into a complete profile.
            const bool missed_layout = input.empty() ||
                                       input.back().first != ".lux/workspace/layouts/" + layout.id.value + ".layout" ||
                                       input.back().second.target.expected_version != layout.version;
            if (missed_layout)
                return failed(EWorkspaceError::CONFLICT, "profile layout changed during capture");
        }
        if (auto added = append(
                ".lux/workspace/preferences.toml",
                [&](auto bytes) { return decodePreferences(bytes, limits_); }
            );
            !added)
            return cxx::unexpected(added.error());
        if (auto added =
                append(".lux/workspace/recovery.toml", [&](auto bytes) { return decodeRecovery(bytes, limits_); });
            !added)
            return cxx::unexpected(added.error());
        if (auto added = append(
                ".lux/workspace/migration-v1.toml",
                [](auto bytes) -> WorkspaceResult<void>
                {
                    const auto parsed =
                        toml::parse(std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
                    if (!parsed)
                        return failed(EWorkspaceError::INVALID_DATA, "legacy conversion marker syntax");
                    const bool invalid =
                        parsed["schema"].value<std::int64_t>() != 1 || parsed["source"].value_or(std::string{}).empty();
                    if (invalid)
                        return failed(EWorkspaceError::INVALID_DATA, "legacy conversion marker identity");
                    return {};
                }
            );
            !added)
            return cxx::unexpected(added.error());
        if (input.empty() && !marker)
            return std::optional<persistence::WriteTicket>{};
        std::string manifest;
        for (const auto& [path, file] : input)
            manifest += path + "\n" + file.target.expected_version + "\n";
        const auto digest = storage::publicationDigest(std::as_bytes(std::span(manifest)));
        if (marker && pinned_digest != digest)
            return failed(EWorkspaceError::CONFLICT, "profile source changed after migration preparation");
        const auto accepted = [](WorkspaceResult<persistence::WriteTicket> result
                              ) -> WorkspaceResult<std::optional<persistence::WriteTicket>>
        {
            if (!result)
                return cxx::unexpected(result.error());
            return std::optional{*result};
        };
        // Validate every existing target before publishing anything. Raw layout/recovery/unknown
        // payload bytes are copied; IDs, selected layout and legacy_origin are not reinterpreted.
        std::optional<std::size_t> next;
        for (std::size_t i{}; i < input.size(); ++i)
        {
            const auto& [path, file] = input[i];
            auto destination = read(path);
            if (!destination)
            {
                if (destination.error().code != EWorkspaceError::NOT_FOUND)
                    return cxx::unexpected(destination.error());
                if (!next)
                    next = i;
                continue;
            }
            if (destination->target.key == file.target.key)
                return failed(EWorkspaceError::CONFLICT, "profile destination aliases the original project file");
            if (destination->bytes != file.bytes)
                return failed(EWorkspaceError::CONFLICT, "unmarked profile differs; explicit choice required");
        }
        const auto mark = [&](bool complete)
        {
            const auto text = "schema = 1\nproject = \"" + project_key + "\"\nsource = \"" + digest +
                              "\"\ncomplete = " + (complete ? "true\n" : "false\n");
            const auto bytes = std::as_bytes(std::span(text));
            return accepted(
                write(marker_path, marker ? marker->target.expected_version : "missing", {bytes.begin(), bytes.end()})
            );
        };
        if (!marker)
            return mark(false); // Pin all source versions before the first destination write.
        if (next)
            return accepted(write(input[*next].first, "missing", std::move(input[*next].second.bytes)));
        return mark(true); // Every destination was observed before confirming the profile.
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
            result.layouts.push_back(
                {std::move(relative), std::move(captured->bytes), captured->target.expected_version}
            );
        }
        auto settings = read(".lux/editor/settings.toml");
        if (settings)
            result.settings = LegacyWorkspaceFile{
                ".lux/editor/settings.toml",
                std::move(settings->bytes),
                settings->target.expected_version
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
        const LegacyMigration& migration,
        const WorkspaceStore* source
    )
    {
        // Every retry consults the real disk, never a private in-memory 'done' bit.
        // Revalidate bytes/versions without parsing the unchanged immutable migration plan again.
        auto current = (source ? *source : *this).captureLegacyInput();
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
                        ) -> WorkspaceResult<std::optional<persistence::WriteTicket>>
        {
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
} // namespace lux::editor::workspace
