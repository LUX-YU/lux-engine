#include <algorithm>
#include <lux/engine/editor/ProjectManifest.hpp>
#include <lux/engine/editor/detail/ProjectFiles.hpp>
#include <nlohmann/json.hpp>
#include <unordered_set>

namespace lux::editor
{
    namespace
    {
        constexpr std::size_t MaxManifestBytes = 1024 * 1024;
        constexpr std::size_t MaxRecords = 4096;

        bool validText(std::string_view text) noexcept
        {
            return !text.empty() && text.size() <= 1024 &&
                   std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 32; });
        }
        bool validPath(std::string_view path) noexcept
        {
            const bool has_invalid_character = path.find_first_of("\\:") != path.npos;
            if (!validText(path) || has_invalid_character || path.front() == '/')
            {
                return false;
            }
            while (!path.empty())
            {
                const auto end = path.find('/');
                const auto part = path.substr(0, end);
                const bool is_invalid_part =
                    part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ';
                if (is_invalid_part)
                {
                    return false;
                }
                if (end == path.npos)
                {
                    return true;
                }
                path.remove_prefix(end + 1);
            }
            return false;
        }
        bool unsigned32(const nlohmann::json& value) noexcept
        {
            return value.is_number_unsigned() && value.get<std::uint64_t>() <= UINT32_MAX;
        }
        std::optional<uuids::uuid> readId(const nlohmann::json& value) noexcept
        {
            if (!value.is_string())
            {
                return {};
            }
            return uuids::uuid::from_string(value.get_ref<const std::string&>());
        }
    } // namespace
    bool isCanonicalProjectName(std::string_view value) noexcept
    {
        if (value.empty() || value.size() > 256)
        {
            return false;
        }
        bool first = true;
        for (const char c : value)
        {
            if (c == '.')
            {
                if (first)
                {
                    return false;
                }
                first = true;
                continue;
            }
            const bool is_letter = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
            const bool is_suffix = !first && ((c >= '0' && c <= '9') || c == '_');
            if (!is_letter && !is_suffix)
            {
                return false;
            }
            first = false;
        }
        return !first;
    }
    ProjectResult<void> validateProjectManifest(const ProjectManifest& value) noexcept
    {
        if (value.format_version != 1)
        {
            return cxx::unexpected(ProjectFailure{EProjectError::UNSUPPORTED_VERSION});
        }
        if (value.id.is_nil())
        {
            return cxx::unexpected(ProjectFailure{EProjectError::INVALID_IDENTITY});
        }
        if (!validText(value.name))
        {
            return cxx::unexpected(ProjectFailure{EProjectError::INVALID_NAME});
        }
        if (value.plugins.size() > MaxRecords || value.scenes.size() > MaxRecords)
        {
            return cxx::unexpected(ProjectFailure{EProjectError::LIMIT});
        }
        std::unordered_set<std::string_view> plugins;
        for (std::size_t i = 0; i < value.plugins.size(); ++i)
        {
            const auto& plugin = value.plugins[i];
            if (!isCanonicalProjectName(plugin.id) || plugin.version == 0)
            {
                return cxx::unexpected(ProjectFailure{EProjectError::INVALID_PLUGIN, i});
            }
            if (!plugins.insert(plugin.id).second)
            {
                return cxx::unexpected(ProjectFailure{EProjectError::DUPLICATE_IDENTITY, i});
            }
        }
        std::unordered_set<asset::AssetId> scenes;
        std::unordered_set<std::string> paths;
        for (std::size_t i = 0; i < value.scenes.size(); ++i)
        {
            const auto& scene = value.scenes[i];
            if (scene.id.isNull())
            {
                return cxx::unexpected(ProjectFailure{EProjectError::INVALID_IDENTITY, i});
            }
            if (!validText(scene.name))
            {
                return cxx::unexpected(ProjectFailure{EProjectError::INVALID_NAME, i});
            }
            if (!isCanonicalProjectName(scene.profile))
            {
                return cxx::unexpected(ProjectFailure{EProjectError::INVALID_PROFILE, i});
            }
            if (!validPath(scene.path))
            {
                return cxx::unexpected(ProjectFailure{EProjectError::INVALID_PATH, i});
            }
            if (!scenes.insert(scene.id).second)
            {
                return cxx::unexpected(ProjectFailure{EProjectError::DUPLICATE_IDENTITY, i});
            }
            // Portable manifest paths reject ASCII case aliases, including on case-sensitive hosts.
            auto path = scene.path;
            for (auto& c : path)
            {
                if (c >= 'A' && c <= 'Z')
                {
                    c += 'a' - 'A';
                }
            }
            if (!paths.insert(std::move(path)).second)
            {
                return cxx::unexpected(ProjectFailure{EProjectError::DUPLICATE_PATH, i});
            }
        }
        if (value.startup_scene && !scenes.contains(*value.startup_scene))
        {
            return cxx::unexpected(ProjectFailure{EProjectError::INVALID_STARTUP_SCENE});
        }
        return {};
    }
    ProjectResult<ProjectManifest> decodeProjectManifest(std::string_view text) noexcept
    {
        if (text.size() > MaxManifestBytes)
        {
            return cxx::unexpected(ProjectFailure{EProjectError::LIMIT});
        }
        auto input = nlohmann::json::parse(text, nullptr, false);
        const auto invalid = [] { return cxx::unexpected(ProjectFailure{EProjectError::INVALID_FORMAT}); };
        if (!input.is_object() || !input.contains("format_version") || !unsigned32(input["format_version"]))
        {
            return invalid();
        }
        ProjectManifest result;
        result.format_version = input["format_version"].get<std::uint32_t>();
        if (result.format_version != 1)
        {
            return cxx::unexpected(ProjectFailure{EProjectError::UNSUPPORTED_VERSION});
        }
        const bool has_fields =
            input.contains("id") && input.contains("name") && input.contains("plugins") && input.contains("scenes");
        if (!has_fields)
        {
            return invalid();
        }
        auto id = readId(input["id"]);
        const bool has_valid_types =
            id && input["name"].is_string() && input["plugins"].is_array() && input["scenes"].is_array();
        if (!has_valid_types)
        {
            return invalid();
        }
        result.id = *id;
        result.name = input["name"].get<std::string>();
        for (const auto& item : input["plugins"])
        {
            const bool has_plugin_fields = item.is_object() && item.contains("id") && item.contains("version");
            if (!has_plugin_fields || !item["id"].is_string() || !unsigned32(item["version"]))
            {
                return invalid();
            }
            result.plugins.push_back({item["id"].get<std::string>(), item["version"].get<std::uint32_t>()});
        }
        for (const auto& item : input["scenes"])
        {
            const bool has_scene_fields = item.is_object() && item.contains("id") && item.contains("name") &&
                                          item.contains("path") && item.contains("profile");
            if (!has_scene_fields)
            {
                return invalid();
            }
            auto scene_id = readId(item["id"]);
            const bool has_scene_types =
                scene_id && item["name"].is_string() && item["path"].is_string() && item["profile"].is_string();
            if (!has_scene_types)
            {
                return invalid();
            }
            result.scenes.push_back({asset::AssetId{*scene_id}, item["name"], item["path"], item["profile"]});
        }
        if (input.contains("startup_scene") && !input["startup_scene"].is_null())
        {
            auto startup = readId(input["startup_scene"]);
            if (!startup)
            {
                return invalid();
            }
            result.startup_scene = asset::AssetId{*startup};
        }
        if (auto valid = validateProjectManifest(result); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        return result;
    }
    ProjectResult<std::string> encodeProjectManifest(const ProjectManifest& value) noexcept
    try
    {
        if (auto valid = validateProjectManifest(value); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        nlohmann::json result{
            {"format_version", value.format_version},
            {"id", uuids::to_string(value.id)},
            {"name", value.name},
            {"plugins", nlohmann::json::array()},
            {"scenes", nlohmann::json::array()},
            {"startup_scene", nullptr}
        };
        for (const auto& plugin : value.plugins)
        {
            result["plugins"].push_back({{"id", plugin.id}, {"version", plugin.version}});
        }
        for (const auto& scene : value.scenes)
        {
            result["scenes"].push_back(
                {{"id", uuids::to_string(scene.id.uuid())},
                 {"name", scene.name},
                 {"path", scene.path},
                 {"profile", scene.profile}}
            );
        }
        if (value.startup_scene)
        {
            result["startup_scene"] = uuids::to_string(value.startup_scene->uuid());
        }
        // Invalid UTF-8 is an input error, never an exception across the codec boundary.
        auto encoded = result.dump(2) + '\n';
        if (encoded.size() > MaxManifestBytes)
        {
            return cxx::unexpected(ProjectFailure{EProjectError::LIMIT});
        }
        return encoded;
    }
    catch (const std::bad_alloc&)
    {
        std::terminate();
    }
    catch (const nlohmann::json::type_error&)
    {
        return cxx::unexpected(ProjectFailure{EProjectError::INVALID_FORMAT});
    }
    ProjectResult<ProjectManifest> readProjectManifest(const std::filesystem::path& path, std::stop_token stop) noexcept
    {
        auto bytes = detail::readProjectBytes(path, MaxManifestBytes, stop);
        if (!bytes)
        {
            return cxx::unexpected(bytes.error());
        }
        return decodeProjectManifest({reinterpret_cast<const char*>(bytes->data()), bytes->size()});
    }
    ProjectResult<void> writeProjectManifestAtomic(
        const std::filesystem::path& path,
        const ProjectManifest& value,
        EProjectWrite mode,
        std::stop_token stop
    ) noexcept
    {
        auto encoded = encodeProjectManifest(value);
        if (!encoded)
        {
            return cxx::unexpected(encoded.error());
        }
        return detail::writeProjectBytesAtomic(path, std::as_bytes(std::span{*encoded}), mode, stop);
    }
} // namespace lux::editor
