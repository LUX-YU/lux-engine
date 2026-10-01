#include <lux/engine/editor/project/ProjectManifest.hpp>
#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <unordered_set>

namespace lux::editor
{
    namespace
    {
        constexpr std::array kinds{"scene", "material_graph", "flow_graph", "model", "texture"};

        auto fail(EProjectManifestError code, std::string field = {}, std::size_t asset = 0) noexcept
        {
            return lux::cxx::unexpected(ProjectManifestFailure{code, std::move(field), asset});
        }

        bool digest(std::string_view value) noexcept
        {
            return value.empty() || (value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
                                         return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                                     }));
        }

        std::string pathKey(std::string_view path)
        {
            std::string result(path);
            for (char& c : result)
            {
                if (c >= 'A' && c <= 'Z')
                {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            return result;
        }

        void quote(std::string& output, std::string_view value)
        {
            output.push_back('"');
            for (const char c : value)
            {
                switch (c)
                {
                case '\\':
                    output += "\\\\";
                    break;
                case '"':
                    output += "\\\"";
                    break;
                case '\n':
                    output += "\\n";
                    break;
                case '\r':
                    output += "\\r";
                    break;
                case '\t':
                    output += "\\t";
                    break;
                default:
                    output.push_back(c);
                    break;
                }
            }
            output += "\"\n";
        }

        template <std::size_t N>
        ProjectManifestResult<void> fields(
            const toml::table& table,
            const std::array<const char*, N>& allowed,
            std::size_t asset = 0
        ) noexcept
        {
            for (const auto& [key, value] : table)
            {
                (void)value;
                if (std::find(allowed.begin(), allowed.end(), key.str()) == allowed.end())
                {
                    return fail(EProjectManifestError::UNKNOWN_FIELD, std::string(key.str()), asset);
                }
            }
            return {};
        }

        ProjectManifestResult<std::string> text(
            const toml::table& table,
            std::string_view key,
            std::size_t asset = 0,
            bool optional = false
        ) noexcept
        {
            if (!table.contains(key) && optional)
            {
                return std::string{};
            }
            auto value = table[key].value<std::string>();
            if (!value)
            {
                return fail(EProjectManifestError::MISSING_FIELD, std::string(key), asset);
            }
            return std::move(*value);
        }
    } // namespace

    bool validProjectPath(std::string_view path) noexcept
    {
        const bool is_empty_path = path.empty();
        if (is_empty_path)
        {
            return false;
        }
        const bool has_absolute_prefix = path.front() == '/';
        const bool has_trailing_separator = path.back() == '/';
        const bool is_invalid_boundary = has_absolute_prefix || has_trailing_separator;
        if (is_invalid_boundary)
        {
            return false;
        }
        if (path.find_first_of("\\:*?\"<>|") != std::string_view::npos)
        {
            return false;
        }
        for (const unsigned char c : path)
        {
            if (c < 32 || c == 127)
            {
                return false;
            }
        }
        for (std::size_t begin = 0; begin < path.size();)
        {
            auto end = path.find('/', begin);
            if (end == std::string_view::npos)
            {
                end = path.size();
            }
            const auto segment = path.substr(begin, end - begin);
            const bool is_empty_segment = segment.empty();
            const bool is_dot_segment = segment == "." || segment == "..";
            const bool has_trailing_dot = !is_empty_segment && segment.back() == '.';
            const bool has_trailing_space = !is_empty_segment && segment.back() == ' ';
            const bool is_invalid_segment =
                is_empty_segment || is_dot_segment || has_trailing_dot || has_trailing_space;
            if (is_invalid_segment)
            {
                return false;
            }
            const auto stem = pathKey(segment.substr(0, segment.find('.')));
            const bool numbered_device = stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) &&
                                         stem[3] >= '1' && stem[3] <= '9';
            const bool is_reserved_name = stem == "con" || stem == "prn" || stem == "aux" || stem == "nul";
            const bool is_reserved_device = is_reserved_name || numbered_device;
            if (is_reserved_device)
            {
                return false;
            }
            begin = end + 1;
        }
        return true;
    }

    ProjectManifestResult<void> validateProjectManifest(
        const ProjectManifest& manifest,
        ProjectManifestLimits limits
    ) noexcept
    {
        const bool is_invalid_byte_limit = limits.max_bytes == 0;
        const bool is_invalid_asset_limit = limits.max_assets == 0;
        const bool is_invalid_path_limit = limits.max_path_bytes == 0;
        const bool is_invalid_plugin_limit = limits.max_plugins == 0;
        const bool is_invalid_limits =
            is_invalid_byte_limit || is_invalid_asset_limit || is_invalid_path_limit || is_invalid_plugin_limit;
        if (is_invalid_limits)
        {
            return fail(EProjectManifestError::INVALID_ARGUMENT);
        }
        if (manifest.id.isNull())
        {
            return fail(EProjectManifestError::INVALID_IDENTITY, "project_id");
        }
        if (manifest.name.empty())
        {
            return fail(EProjectManifestError::MISSING_FIELD, "name");
        }
        const bool is_name_too_long = manifest.name.size() > limits.max_path_bytes;
        const bool has_too_many_assets = manifest.assets.size() > limits.max_assets;
        const bool is_limit_exceeded = is_name_too_long || has_too_many_assets;
        if (is_limit_exceeded)
        {
            return fail(EProjectManifestError::LIMIT_EXCEEDED);
        }
        for (const unsigned char c : manifest.name)
        {
            if (c < 32 || c == 127)
            {
                return fail(EProjectManifestError::INVALID_ARGUMENT, "name");
            }
        }
        if (!manifest.default_scene.empty() && !validProjectPath(manifest.default_scene))
        {
            return fail(EProjectManifestError::INVALID_PATH, "default_scene");
        }
        if (manifest.plugins.size() > limits.max_plugins)
            return fail(EProjectManifestError::LIMIT_EXCEEDED, "plugins");
        std::unordered_set<std::string> plugin_ids;
        for (std::size_t index{}; index < manifest.plugins.size(); ++index)
        {
            const auto& plugin = manifest.plugins[index];
            const bool is_missing_plugin_id = plugin.id.empty();
            const bool is_plugin_id_too_long = plugin.id.size() > limits.max_path_bytes;
            const bool has_invalid_plugin_character = !std::ranges::all_of(plugin.id, [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                       c == '_' || c == '-';
            });
            const bool is_invalid_plugin_id =
                is_missing_plugin_id || is_plugin_id_too_long || has_invalid_plugin_character;
            const bool is_missing_version = !plugin.version;
            const bool is_invalid_plugin = is_invalid_plugin_id || is_missing_version;
            if (is_invalid_plugin)
                return fail(EProjectManifestError::INVALID_IDENTITY, "plugins.id/version", index);
            if (!plugin_ids.insert(plugin.id).second)
                return fail(EProjectManifestError::DUPLICATE_IDENTITY, "plugins.id", index);
            const bool invalid_path =
                !plugin.description_path.empty() &&
                (!validProjectPath(plugin.description_path) || plugin.description_path.size() > limits.max_path_bytes);
            if (invalid_path)
                return fail(EProjectManifestError::INVALID_PATH, "plugins.description_path", index);
        }
        std::unordered_set<asset::AssetId> identities;
        std::unordered_set<std::string> paths;
        std::unordered_set<std::string> mounts;
        identities.reserve(manifest.assets.size());
        paths.reserve(manifest.assets.size());
        bool default_found = manifest.default_scene.empty();
        for (std::size_t i = 0; i < manifest.assets.size(); ++i)
        {
            const auto& asset = manifest.assets[i];
            if (asset.id.isNull())
            {
                return fail(EProjectManifestError::INVALID_IDENTITY, "id", i);
            }
            if (!identities.insert(asset.id).second)
            {
                return fail(EProjectManifestError::DUPLICATE_IDENTITY, "id", i);
            }
            if (static_cast<std::size_t>(asset.kind) >= kinds.size())
            {
                return fail(EProjectManifestError::UNKNOWN_ASSET_KIND, "kind", i);
            }
            const bool is_source_path_too_long = asset.source_path.size() > limits.max_path_bytes;
            const bool is_cooked_path_too_long = asset.cooked_path.size() > limits.max_path_bytes;
            const bool is_path_limit_exceeded = is_source_path_too_long || is_cooked_path_too_long;
            if (is_path_limit_exceeded)
            {
                return fail(EProjectManifestError::LIMIT_EXCEEDED, "path", i);
            }
            const bool is_invalid_source_path = !validProjectPath(asset.source_path);
            const bool has_cooked_path = !asset.cooked_path.empty();
            const bool is_invalid_cooked_path = has_cooked_path && !validProjectPath(asset.cooked_path);
            const bool is_invalid_asset_path = is_invalid_source_path || is_invalid_cooked_path;
            if (is_invalid_asset_path)
            {
                return fail(EProjectManifestError::INVALID_PATH, "path", i);
            }
            if (!paths.insert(pathKey(asset.source_path)).second)
            {
                return fail(EProjectManifestError::DUPLICATE_PATH, "source_path", i);
            }
            const bool is_duplicate_cooked_path = has_cooked_path && !paths.insert(pathKey(asset.cooked_path)).second;
            if (is_duplicate_cooked_path)
            {
                return fail(EProjectManifestError::DUPLICATE_PATH, "cooked_path", i);
            }
            const bool is_invalid_source_digest = !digest(asset.source_digest);
            const bool is_invalid_compiled_digest = !digest(asset.compiled_source_digest);
            const bool is_invalid_digest = is_invalid_source_digest || is_invalid_compiled_digest;
            if (is_invalid_digest)
            {
                return fail(EProjectManifestError::INVALID_DIGEST, "digest", i);
            }
            if (!asset.mount_path.empty())
            {
                const bool is_mount_path_too_long = asset.mount_path.size() > limits.max_path_bytes;
                const bool is_invalid_mount_path = !validProjectPath(asset.mount_path);
                const bool is_invalid_mount = is_mount_path_too_long || is_invalid_mount_path;
                if (is_invalid_mount)
                {
                    return fail(EProjectManifestError::INVALID_PATH, "mount_path", i);
                }
                if (!mounts.insert(pathKey(asset.mount_path)).second)
                {
                    return fail(EProjectManifestError::DUPLICATE_PATH, "mount_path", i);
                }
            }
            if (asset.source_path == manifest.default_scene && asset.kind == EProjectAssetKind::SCENE)
            {
                default_found = true;
            }
        }
        if (!default_found)
        {
            return fail(EProjectManifestError::INVALID_DEFAULT_SCENE, "default_scene");
        }
        return {};
    }

    ProjectManifestResult<ProjectManifest> decodeProjectManifest(
        std::string_view bytes,
        ProjectManifestLimits limits
    ) noexcept
    {
        if (bytes.size() > limits.max_bytes)
        {
            return fail(EProjectManifestError::LIMIT_EXCEEDED);
        }
        auto parsed = toml::parse(bytes);
        if (!parsed)
        {
            const auto position = parsed.error().source().begin;
            return lux::cxx::unexpected(
                ProjectManifestFailure{EProjectManifestError::PARSE_FAILURE, {}, 0, position.line, position.column}
            );
        }
        const auto& table = parsed.table();
        const auto known =
            fields(table, std::array{"format", "version", "project_id", "name", "default_scene", "assets", "plugins"});
        if (!known)
        {
            return lux::cxx::unexpected(known.error());
        }
        const auto format = table["format"].value<std::string>();
        const auto version = table["version"].value<std::int64_t>();
        const bool is_invalid_format = !format || *format != "lux.editor.project";
        const bool is_invalid_version = !version || *version != 2;
        const bool is_unsupported_format = is_invalid_format || is_invalid_version;
        if (is_unsupported_format)
        {
            return fail(EProjectManifestError::UNSUPPORTED_FORMAT, "format/version");
        }
        const auto id = text(table, "project_id");
        const auto name = text(table, "name");
        const auto default_scene = text(table, "default_scene", 0, true);
        if (!id)
        {
            return lux::cxx::unexpected(id.error());
        }
        if (!name)
        {
            return lux::cxx::unexpected(name.error());
        }
        if (!default_scene)
        {
            return lux::cxx::unexpected(default_scene.error());
        }
        const auto uuid = uuids::uuid::from_string(*id);
        if (!uuid)
        {
            return fail(EProjectManifestError::INVALID_IDENTITY, "project_id");
        }
        ProjectManifest result{asset::AssetId{*uuid}, *name, *default_scene, {}};
        const auto* plugins = table["plugins"].as_array();
        if (!plugins)
            return fail(EProjectManifestError::MISSING_FIELD, "plugins");
        if (plugins->size() > limits.max_plugins)
            return fail(EProjectManifestError::LIMIT_EXCEEDED, "plugins");
        for (std::size_t index{}; index < plugins->size(); ++index)
        {
            const auto* entry = (*plugins)[index].as_table();
            if (!entry)
                return fail(EProjectManifestError::PARSE_FAILURE, "plugins", index);
            const auto checked = fields(*entry, std::array{"id", "version", "description_path"}, index);
            if (!checked)
                return lux::cxx::unexpected(checked.error());
            auto plugin_id = text(*entry, "id", index);
            auto description = text(*entry, "description_path", index, true);
            const auto plugin_version = (*entry)["version"].value<std::int64_t>();
            if (!plugin_id)
                return lux::cxx::unexpected(plugin_id.error());
            if (!description)
                return lux::cxx::unexpected(description.error());
            const bool is_missing_version = !plugin_version;
            const bool is_nonpositive_version = !is_missing_version && *plugin_version <= 0;
            const bool is_excessive_version = !is_missing_version && *plugin_version > UINT32_MAX;
            const bool is_invalid_version = is_missing_version || is_nonpositive_version || is_excessive_version;
            if (is_invalid_version)
                return fail(EProjectManifestError::INVALID_IDENTITY, "plugins.version", index);
            result.plugins.push_back(
                {std::move(*plugin_id), static_cast<std::uint32_t>(*plugin_version), std::move(*description)}
            );
        }
        const auto* assets = table["assets"].as_array();
        if (table.contains("assets") && !assets)
        {
            return fail(EProjectManifestError::PARSE_FAILURE, "assets");
        }
        if (assets && assets->size() > limits.max_assets)
        {
            return fail(EProjectManifestError::LIMIT_EXCEEDED, "assets");
        }
        if (assets)
        {
            for (std::size_t i = 0; i < assets->size(); ++i)
            {
                const auto* entry = (*assets)[i].as_table();
                if (!entry)
                {
                    return fail(EProjectManifestError::PARSE_FAILURE, "assets", i);
                }
                const auto allowed = std::array{
                    "id",
                    "kind",
                    "source_path",
                    "cooked_path",
                    "source_digest",
                    "compiled_source_digest",
                    "mount_path"
                };
                const auto checked = fields(*entry, allowed, i);
                if (!checked)
                {
                    return lux::cxx::unexpected(checked.error());
                }
                const auto asset_id = text(*entry, "id", i);
                const auto kind = text(*entry, "kind", i);
                const auto path = text(*entry, "source_path", i);
                const auto cooked = text(*entry, "cooked_path", i, true);
                const auto source_digest = text(*entry, "source_digest", i, true);
                const auto compiled_digest = text(*entry, "compiled_source_digest", i, true);
                const auto mount = text(*entry, "mount_path", i, true);
                if (!asset_id)
                {
                    return lux::cxx::unexpected(asset_id.error());
                }
                if (!kind)
                {
                    return lux::cxx::unexpected(kind.error());
                }
                if (!path)
                {
                    return lux::cxx::unexpected(path.error());
                }
                if (!cooked)
                {
                    return lux::cxx::unexpected(cooked.error());
                }
                if (!source_digest)
                {
                    return lux::cxx::unexpected(source_digest.error());
                }
                if (!compiled_digest)
                {
                    return lux::cxx::unexpected(compiled_digest.error());
                }
                if (!mount)
                {
                    return lux::cxx::unexpected(mount.error());
                }
                const auto asset_uuid = uuids::uuid::from_string(*asset_id);
                if (!asset_uuid)
                {
                    return fail(EProjectManifestError::INVALID_IDENTITY, "id", i);
                }
                const auto found = std::find(kinds.begin(), kinds.end(), *kind);
                if (found == kinds.end())
                {
                    return fail(EProjectManifestError::UNKNOWN_ASSET_KIND, "kind", i);
                }
                const auto asset_kind = static_cast<EProjectAssetKind>(found - kinds.begin());
                result.assets.push_back(
                    {asset::AssetId{*asset_uuid}, asset_kind, *path, *cooked, *source_digest, *compiled_digest, *mount}
                );
            }
        }
        const auto checked = validateProjectManifest(result, limits);
        if (!checked)
        {
            return lux::cxx::unexpected(checked.error());
        }
        return result;
    }

    ProjectManifestResult<std::string> encodeProjectManifest(
        const ProjectManifest& manifest,
        ProjectManifestLimits limits
    ) noexcept
    {
        const auto checked = validateProjectManifest(manifest, limits);
        if (!checked)
        {
            return lux::cxx::unexpected(checked.error());
        }
        std::string result = "format = \"lux.editor.project\"\nversion = 2\nproject_id = ";
        quote(result, uuids::to_string(manifest.id.uuid()));
        result += "name = ";
        quote(result, manifest.name);
        result += "default_scene = ";
        quote(result, manifest.default_scene);
        if (manifest.plugins.empty())
            result += "plugins = []\n";
        for (const auto& plugin : manifest.plugins)
        {
            result += "\n[[plugins]]\nid = ";
            quote(result, plugin.id);
            result += "version = " + std::to_string(plugin.version) + "\n";
            if (!plugin.description_path.empty())
            {
                result += "description_path = ";
                quote(result, plugin.description_path);
            }
        }
        for (const auto& asset : manifest.assets)
        {
            result += "\n[[assets]]\nid = ";
            quote(result, uuids::to_string(asset.id.uuid()));
            result += "kind = ";
            quote(result, kinds[static_cast<std::size_t>(asset.kind)]);
            result += "source_path = ";
            quote(result, asset.source_path);
            result += "cooked_path = ";
            quote(result, asset.cooked_path);
            result += "source_digest = ";
            quote(result, asset.source_digest);
            result += "compiled_source_digest = ";
            quote(result, asset.compiled_source_digest);
            result += "mount_path = ";
            quote(result, asset.mount_path);
            if (result.size() > limits.max_bytes)
            {
                return fail(EProjectManifestError::LIMIT_EXCEEDED);
            }
        }
        if (result.size() > limits.max_bytes)
        {
            return fail(EProjectManifestError::LIMIT_EXCEEDED);
        }
        // The installed TOML parser also verifies generated UTF-8; never save an unreadable manifest.
        if (!toml::parse(result))
        {
            return fail(EProjectManifestError::INVALID_ARGUMENT);
        }
        return result;
    }
} // namespace lux::editor
