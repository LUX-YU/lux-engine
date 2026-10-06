#include <lux/engine/editor/workspace/WorkspaceStore.hpp>
#include <lux/engine/editor/storage/FilePublication.hpp>
#include <algorithm>

namespace lux::editor::workspace
{
    namespace
    {
        auto failed(EWorkspaceError code, std::string detail, std::uint64_t native = 0)
        {
            return lux::cxx::unexpected(WorkspaceFailure{code, std::move(detail), native});
        }
        WorkspaceFailure translate(const persistence::PersistenceFailure& error)
        {
            using E = persistence::EPersistenceError;
            EWorkspaceError code = EWorkspaceError::IO;
            if (error.code == E::BUSY || error.code == E::WRITER_ACTIVE)
                code = EWorkspaceError::BUSY;
            if (error.code == E::CONFLICT)
                code = EWorkspaceError::CONFLICT;
            if (error.code == E::CAPACITY)
                code = EWorkspaceError::CAPACITY;
            return {code, error.detail, error.native_code};
        }
        WorkspaceFailure translate(const settings::SettingsFailure& error)
        {
            using E = settings::ESettingsError;
            auto code = EWorkspaceError::INVALID_DATA;
            if (error.code == E::UNSUPPORTED_VERSION)
                code = EWorkspaceError::UNSUPPORTED_VERSION;
            if (error.code == E::CAPACITY)
                code = EWorkspaceError::CAPACITY;
            if (error.code == E::CONFLICT)
                code = EWorkspaceError::CONFLICT;
            return {code, error.detail};
        }
        std::string layoutPath(const LayoutId& id)
        {
            return ".lux/workspace/layouts/" + id.value + ".layout";
        }
        constexpr std::string_view preferencesPath = ".lux/workspace/preferences.toml";
        constexpr std::string_view recoveryPath = ".lux/workspace/recovery.toml";
    } // namespace
    WorkspaceStore::WorkspaceStore(
        std::filesystem::path root,
        persistence::WriteCoordinator& coordinator,
        persistence::IArtifactStore& artifacts,
        WorkspaceLimits limits
    )
        : root_(std::move(root)), coordinator_(coordinator), artifacts_(artifacts), limits_(limits)
    {
    }
    WorkspaceResult<persistence::WriteTarget> WorkspaceStore::target(std::string_view relative) const
    {
        auto canonical = storage::publicationTargetKey(root_, std::filesystem::u8path(relative));
        if (!canonical)
            return failed(EWorkspaceError::IO, std::string(relative), canonical.error().native_code);
        auto resolved = artifacts_.resolve(*canonical);
        if (!resolved)
            return lux::cxx::unexpected(translate(resolved.error()));
        if (resolved->key.value != *canonical)
            return failed(EWorkspaceError::CONFLICT, "backend changed the physical publication identity");
        return std::move(*resolved);
    }
    WorkspaceResult<WorkspaceStore::ReadFile> WorkspaceStore::read(std::string_view relative) const
    {
        auto resolved = target(relative);
        if (!resolved)
            return lux::cxx::unexpected(resolved.error());
        if (resolved->expected_version == "missing")
            return failed(EWorkspaceError::NOT_FOUND, std::string(relative));
        const auto path = std::filesystem::u8path(resolved->key.value);
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error)
            return failed(EWorkspaceError::IO, std::string(relative), error.value());
        if (size > limits_.file_bytes)
            return failed(EWorkspaceError::CAPACITY, std::string(relative));
        auto bytes = storage::readPublicationFile(path, limits_.file_bytes);
        if (!bytes)
            return failed(EWorkspaceError::IO, std::string(relative), bytes.error().native_code);
        if (storage::publicationDigest(*bytes) != resolved->expected_version)
            return failed(EWorkspaceError::CONFLICT, "file changed while reading");
        return ReadFile{std::move(*bytes), std::move(*resolved)};
    }
    WorkspaceResult<StoredLayout> WorkspaceStore::readLayout(const LayoutId& id) const
    {
        if (!id.valid())
            return failed(EWorkspaceError::INVALID_DATA, "layout ID");
        auto file = read(layoutPath(id));
        if (!file)
            return lux::cxx::unexpected(file.error());
        auto value = decodeLayout(file->bytes, limits_);
        if (!value)
            return lux::cxx::unexpected(value.error());
        if (value->id != id)
            return failed(EWorkspaceError::CONFLICT, "layout filename/body identity mismatch");
        return StoredLayout{std::move(*value), std::move(file->target)};
    }
    WorkspaceResult<StoredPreferences> WorkspaceStore::readPreferences() const
    {
        auto file = read(preferencesPath);
        if (!file)
            return lux::cxx::unexpected(file.error());
        auto value = decodePreferences(file->bytes, limits_);
        if (!value)
            return lux::cxx::unexpected(value.error());
        return StoredPreferences{std::move(*value), std::move(file->target)};
    }
    WorkspaceResult<StoredRecovery> WorkspaceStore::readRecovery() const
    {
        auto file = read(recoveryPath);
        if (!file)
            return lux::cxx::unexpected(file.error());
        auto value = decodeRecovery(file->bytes, limits_);
        if (!value)
            return lux::cxx::unexpected(value.error());
        return StoredRecovery{std::move(*value), std::move(file->target)};
    }
    WorkspaceResult<settings::SettingsDocument> WorkspaceStore::readSettings(
        std::string_view relative,
        settings::ESettingsScope scope
    ) const
    {
        if (scope >= settings::ESettingsScope::LAUNCH)
            return failed(EWorkspaceError::INVALID_DATA, "launch settings are not a stored scope");
        auto file = read(relative);
        if (!file)
            return cxx::unexpected(file.error());
        auto value = settings::decodeSettings(file->bytes);
        if (!value)
            return cxx::unexpected(translate(value.error()));
        if (value->scope != scope)
            return failed(EWorkspaceError::CONFLICT, "settings file scope mismatch");
        value->file_version = std::move(file->target.expected_version);
        return std::move(*value);
    }
    WorkspaceResult<persistence::WriteTicket> WorkspaceStore::writeSettings(
        std::string_view relative,
        const settings::SettingsDocument& value
    )
    {
        const bool is_read_only_scope =
            value.scope == settings::ESettingsScope::INSTALLATION || value.scope >= settings::ESettingsScope::LAUNCH;
        if (is_read_only_scope)
            return failed(EWorkspaceError::INVALID_DATA, "settings scope is not writable");
        auto encoded = settings::encodeSettings(value);
        if (!encoded)
            return cxx::unexpected(translate(encoded.error()));
        return write(relative, value.file_version, std::move(*encoded));
    }
    WorkspaceResult<LayoutCatalog> WorkspaceStore::listLayouts() const
    {
        std::error_code error;
        auto directory = std::filesystem::weakly_canonical(root_ / ".lux/workspace/layouts", error);
        if (error)
            return failed(EWorkspaceError::IO, "catalog", error.value());
        const auto base = std::filesystem::canonical(root_, error);
        if (error)
            return failed(EWorkspaceError::IO, "catalog root", error.value());
        const auto relative = directory.lexically_relative(base);
        if (relative.empty() || *relative.begin() == "..")
            return failed(EWorkspaceError::CONFLICT, "catalog outside root");
        const bool exists = std::filesystem::exists(directory, error);
        if (error)
            return failed(EWorkspaceError::IO, "catalog status", error.value());
        LayoutCatalog result;
        if (!exists)
            return result;
        std::vector<std::filesystem::path> files;
        std::filesystem::directory_iterator it(directory, error), end;
        for (; !error && it != end; it.increment(error))
        {
            if (it->path().extension() != ".layout")
                continue;
            if (files.size() == limits_.entries)
                return failed(EWorkspaceError::CAPACITY, "catalog entries");
            files.push_back(it->path());
        }
        if (error)
            return failed(EWorkspaceError::IO, "catalog enumeration incomplete", error.value());
        std::ranges::sort(files);
        std::string version;
        for (const auto& path : files)
        {
            LayoutId id{path.stem().string()};
            auto value = readLayout(id);
            version += path.filename().generic_string() + "\n";
            if (!value)
            {
                version += "error:" + std::to_string(static_cast<unsigned>(value.error().code)) + "\n";
                result.diagnostics.push_back({path.filename().generic_string(), value.error()});
            }
            else
            {
                version += value->target.expected_version + "\n";
                result.layouts.push_back({std::move(id), std::move(value->value.label), value->target.expected_version}
                );
            }
        }
        result.version.digest = storage::publicationDigest(std::as_bytes(std::span(version)));
        return result;
    }
    WorkspaceResult<LayoutChoice> WorkspaceStore::chooseLayout(const UserPreferences& value) const
    {
        auto valid = validatePreferences(value, limits_);
        if (!valid)
            return lux::cxx::unexpected(valid.error());
        if (!value.selected_layout)
            return LayoutChoice{};
        auto layout = readLayout(*value.selected_layout);
        if (layout)
            return LayoutChoice{std::move(layout->value), {}};
        const auto code = layout.error().code;
        const bool confirmed_fallback = code == EWorkspaceError::NOT_FOUND || code == EWorkspaceError::INVALID_DATA ||
                                        code == EWorkspaceError::UNSUPPORTED_VERSION;
        if (!confirmed_fallback)
            return lux::cxx::unexpected(layout.error());
        return LayoutChoice{{}, layout.error()};
    }
    WorkspaceResult<persistence::WriteTicket> WorkspaceStore::write(
        std::string_view relative,
        std::string expected_version,
        std::vector<std::byte> bytes
    )
    {
        if (bytes.size() > limits_.file_bytes)
            return failed(EWorkspaceError::CAPACITY, "write bytes");
        auto resolved = target(relative);
        if (!resolved)
            return lux::cxx::unexpected(resolved.error());
        if (expected_version.empty())
            return failed(EWorkspaceError::INVALID_DATA, "missing write precondition");
        resolved->expected_version = std::move(expected_version);
        auto ticket = coordinator_.reserve(std::move(*resolved), {});
        if (!ticket)
            return lux::cxx::unexpected(translate(ticket.error()));
        auto encoded = coordinator_.provideEncoded(*ticket, {std::move(bytes)});
        if (!encoded)
        {
            // Admission failed. No external owner has received this ticket.
            auto cancelled = coordinator_.cancelBeforePublish(*ticket, encoded.error());
            if (cancelled)
                static_cast<void>(coordinator_.acknowledge(*ticket));
            return lux::cxx::unexpected(translate(encoded.error()));
        }
        return *ticket;
    }
    WorkspaceResult<persistence::WriteTicket> WorkspaceStore::saveLayout(const DockLayout& value, std::string version)
    {
        auto encoded = encodeLayout(value, limits_);
        if (!encoded)
            return lux::cxx::unexpected(encoded.error());
        return write(layoutPath(value.id), std::move(version), std::move(*encoded));
    }
    WorkspaceResult<persistence::WriteTicket> WorkspaceStore::renameLayout(const LayoutId& id, std::string label)
    {
        auto current = readLayout(id);
        if (!current)
            return lux::cxx::unexpected(current.error());
        current->value.label = std::move(label);
        return saveLayout(current->value, std::move(current->target.expected_version));
    }
    WorkspaceResult<persistence::WriteTicket> WorkspaceStore::removeLayout(const LayoutId& id)
    {
        if (!id.valid())
            return failed(EWorkspaceError::INVALID_DATA, "layout ID");
        auto resolved = target(layoutPath(id));
        if (!resolved)
            return lux::cxx::unexpected(resolved.error());
        auto ticket = coordinator_.reserve(std::move(*resolved), {});
        if (!ticket)
            return lux::cxx::unexpected(translate(ticket.error()));
        auto ready = coordinator_.provideRemoval(*ticket);
        if (!ready)
            return lux::cxx::unexpected(translate(ready.error()));
        return *ticket;
    }
    WorkspaceResult<persistence::WriteTicket> WorkspaceStore::writePreferences(
        const UserPreferences& value,
        std::string version
    )
    {
        auto encoded = encodePreferences(value, limits_);
        if (!encoded)
            return lux::cxx::unexpected(encoded.error());
        return write(preferencesPath, std::move(version), std::move(*encoded));
    }
    WorkspaceResult<persistence::WriteTicket> WorkspaceStore::writeRecovery(
        const RecoveryManifest& value,
        std::string version
    )
    {
        auto encoded = encodeRecovery(value, limits_);
        if (!encoded)
            return lux::cxx::unexpected(encoded.error());
        return write(recoveryPath, std::move(version), std::move(*encoded));
    }
} // namespace lux::editor::workspace
