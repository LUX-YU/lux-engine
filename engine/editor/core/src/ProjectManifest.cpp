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
            return value.empty() ||
                   (value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c)
                                                      { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
        }

        std::string pathKey(std::string_view path)
        {
            std::string result(path);
            for (char &c : result)
            {
                if (c >= 'A' && c <= 'Z')
                {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            return result;
        }

        void quote(std::string &output, std::string_view value)
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
        ProjectManifestResult<void> fields(const toml::table &table, const std::array<const char *, N> &allowed,
                                           std::size_t asset = 0) noexcept
        {
            for (const auto &[key, value] : table)
            {
                (void)value;
                if (std::find(allowed.begin(), allowed.end(), key.str()) == allowed.end())
                {
                    return fail(EProjectManifestError::UNKNOWN_FIELD, std::string(key.str()), asset);
                }
            }
            return {};
        }

        ProjectManifestResult<std::string> text(const toml::table &table, std::string_view key, std::size_t asset = 0,
                                                bool optional = false) noexcept
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
        if (path.empty() || path.front() == '/' || path.back() == '/')
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
            if (segment.empty() || segment == "." || segment == ".." || segment.back() == '.' || segment.back() == ' ')
            {
                return false;
            }
            const auto stem = pathKey(segment.substr(0, segment.find('.')));
            const bool numbered_device = stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) &&
                                         stem[3] >= '1' && stem[3] <= '9';
            if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" || numbered_device)
            {
                return false;
            }
            begin = end + 1;
        }
        return true;
    }

    ProjectManifestResult<void> validateProjectManifest(const ProjectManifest &manifest,
                                                        ProjectManifestLimits limits) noexcept
    {
        if (!limits.max_bytes || !limits.max_assets || !limits.max_path_bytes)
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
        if (manifest.name.size() > limits.max_path_bytes || manifest.assets.size() > limits.max_assets)
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
        std::unordered_set<asset::AssetId> identities;
        std::unordered_set<std::string> paths;
        std::unordered_set<std::string> mounts;
        identities.reserve(manifest.assets.size());
        paths.reserve(manifest.assets.size());
        bool default_found = manifest.default_scene.empty();
        for (std::size_t i = 0; i < manifest.assets.size(); ++i)
        {
            const auto &asset = manifest.assets[i];
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
            if (asset.source_path.size() > limits.max_path_bytes || asset.cooked_path.size() > limits.max_path_bytes)
            {
                return fail(EProjectManifestError::LIMIT_EXCEEDED, "path", i);
            }
            if (!validProjectPath(asset.source_path) ||
                (!asset.cooked_path.empty() && !validProjectPath(asset.cooked_path)))
            {
                return fail(EProjectManifestError::INVALID_PATH, "path", i);
            }
            if (!paths.insert(pathKey(asset.source_path)).second)
            {
                return fail(EProjectManifestError::DUPLICATE_PATH, "source_path", i);
            }
            if (!asset.cooked_path.empty() && !paths.insert(pathKey(asset.cooked_path)).second)
            {
                return fail(EProjectManifestError::DUPLICATE_PATH, "cooked_path", i);
            }
            if (!digest(asset.source_digest) || !digest(asset.compiled_source_digest))
            {
                return fail(EProjectManifestError::INVALID_DIGEST, "digest", i);
            }
            if (!asset.mount_path.empty())
            {
                if (asset.mount_path.size() > limits.max_path_bytes || !validProjectPath(asset.mount_path))
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

    ProjectManifestResult<ProjectManifest> decodeProjectManifest(std::string_view bytes,
                                                                 ProjectManifestLimits limits) noexcept
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
                ProjectManifestFailure{EProjectManifestError::PARSE_FAILURE, {}, 0, position.line, position.column});
        }
        const auto &table = parsed.table();
        const auto known =
            fields(table, std::array{"format", "version", "project_id", "name", "default_scene", "assets"});
        if (!known)
        {
            return lux::cxx::unexpected(known.error());
        }
        const auto format = table["format"].value<std::string>();
        const auto version = table["version"].value<std::int64_t>();
        if (!format || *format != "lux.editor.project" || !version || *version != 1)
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
        const auto *assets = table["assets"].as_array();
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
                const auto *entry = (*assets)[i].as_table();
                if (!entry)
                {
                    return fail(EProjectManifestError::PARSE_FAILURE, "assets", i);
                }
                const auto allowed = std::array{"id",          "kind",          "source_path",
                                                "cooked_path", "source_digest", "compiled_source_digest",
                                                "mount_path"};
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
                result.assets.push_back({asset::AssetId{*asset_uuid}, asset_kind, *path, *cooked, *source_digest,
                                         *compiled_digest, *mount});
            }
        }
        const auto checked = validateProjectManifest(result, limits);
        if (!checked)
        {
            return lux::cxx::unexpected(checked.error());
        }
        return result;
    }

    ProjectManifestResult<std::string> encodeProjectManifest(const ProjectManifest &manifest,
                                                             ProjectManifestLimits limits) noexcept
    {
        const auto checked = validateProjectManifest(manifest, limits);
        if (!checked)
        {
            return lux::cxx::unexpected(checked.error());
        }
        std::string result = "format = \"lux.editor.project\"\nversion = 1\nproject_id = ";
        quote(result, uuids::to_string(manifest.id.uuid()));
        result += "name = ";
        quote(result, manifest.name);
        result += "default_scene = ";
        quote(result, manifest.default_scene);
        for (const auto &asset : manifest.assets)
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
