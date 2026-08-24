#include <lux/engine/authoring/project/ProjectManifest.hpp>

#undef TOML_HEADER_ONLY
#define TOML_HEADER_ONLY 1
#undef TOML_EXCEPTIONS
#define TOML_EXCEPTIONS 0
#undef TOML_SHARED_LIB
#define TOML_SHARED_LIB 0
#include <toml++/toml.hpp>

#include <array>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>

namespace lux::authoring
{
    namespace
    {
        [[nodiscard]] bool isCanonicalExtensionName(
            std::string_view name) noexcept
        {
            if (name.empty() || name.front() == '.' || name.back() == '.')
                return false;
            bool has_dot = false;
            bool previous_dot = false;
            for (const char value : name)
            {
                const bool dot = value == '.';
                if (dot)
                {
                    if (previous_dot)
                        return false;
                    has_dot = true;
                }
                else if (!((value >= 'a' && value <= 'z') ||
                           (value >= '0' && value <= '9') ||
                           value == '_' || value == '-'))
                {
                    return false;
                }
                previous_dot = dot;
            }
            return has_dot;
        }
    }

    namespace
    {
        /// Engine version stamp baked at compile time. Manifests created
        /// fresh by the editor adopt this value; older manifests keep
        /// whatever string they were saved with (the editor reads but
        /// does not auto-rewrite the engine_version field).
        constexpr std::string_view kCurrentEngineVersion = "0.1.0";

        /// Generate a random v4 UUID. uses `<random>` to avoid pulling in
        /// platform-specific crypto APIs for what is just a project ID.
        uuids::uuid makeRandomUuid()
        {
            std::random_device rd;
            std::array<int, std::mt19937::state_size> seed_data{};
            std::generate(seed_data.begin(), seed_data.end(), std::ref(rd));
            std::seed_seq seq(seed_data.begin(), seed_data.end());
            std::mt19937 gen(seq);
            uuids::uuid_random_generator uuid_gen{gen};
            return uuid_gen();
        }

        /// Read a `.luxproject` into memory as a string. Filesystem errors
        /// produce an Expected error rather than throwing — matches the
        /// rest of the project's no-exceptions convention.
        lux::cxx::expected<std::string, std::string>
            readFileToString(const std::filesystem::path& path)
        {
            std::ifstream f(path, std::ios::binary);
            if (!f.is_open())
                return lux::cxx::unexpected(
                    std::string{"failed to open '"} + path.string() + "'");

            std::ostringstream ss;
            ss << f.rdbuf();
            return ss.str();
        }
    } // namespace

    // ─────────────────────────────────────────────────────────────────
    //  ProjectManifest::makeDefault
    // ─────────────────────────────────────────────────────────────────

    ProjectManifest ProjectManifest::makeDefault(std::string_view project_name)
    {
        ProjectManifest m{};
        m.name           = std::string{project_name};
        m.display_name   = m.name;
        m.project_guid   = makeRandomUuid();
        m.engine_version = std::string{kCurrentEngineVersion};
        m.default_world  = "";
        return m;
    }

    // ─────────────────────────────────────────────────────────────────
    //  ProjectManifest::loadFromFile
    // ─────────────────────────────────────────────────────────────────

    lux::cxx::expected<ProjectManifest, std::string>
    ProjectManifest::loadFromFile(const std::filesystem::path& path)
    {
        auto text = readFileToString(path);
        if (!text)
            return lux::cxx::unexpected(std::move(text.error()));

        auto parsed = toml::parse(*text, path.string());
        if (!parsed)
        {
            const auto& error = parsed.error();
            std::ostringstream ss;
            ss << "toml parse error in '" << path.string() << "': "
               << error.description() << " at line "
               << error.source().begin.line;
            return lux::cxx::unexpected(ss.str());
        }
        toml::table root = std::move(parsed).table();

        const auto* project = root["project"].as_table();
        if (!project)
            return lux::cxx::unexpected(
                std::string{"manifest '"} + path.string() +
                "' missing required [project] table");

        ProjectManifest m{};

        // name — required
        if (auto v = (*project)["name"].value<std::string>())
            m.name = *v;
        else
            return lux::cxx::unexpected(
                std::string{"manifest '"} + path.string() +
                "' missing required field 'project.name'");

        // display_name — optional, defaults to name
        if (auto v = (*project)["display_name"].value<std::string>())
            m.display_name = *v;
        else
            m.display_name = m.name;

        // project_guid — required, must parse as UUID
        if (auto v = (*project)["project_guid"].value<std::string>())
        {
            if (auto parsed_uuid = uuids::uuid::from_string(*v))
                m.project_guid = *parsed_uuid;
            else
                return lux::cxx::unexpected(
                    std::string{"manifest '"} + path.string() +
                    "' has invalid 'project.project_guid' (not a UUID)");
        }
        else
        {
            return lux::cxx::unexpected(
                std::string{"manifest '"} + path.string() +
                "' missing required field 'project.project_guid'");
        }

        // engine_version — required
        if (auto v = (*project)["engine_version"].value<std::string>())
            m.engine_version = *v;
        else
            return lux::cxx::unexpected(
                std::string{"manifest '"} + path.string() +
                "' missing required field 'project.engine_version'");

        // default_world — optional. The removed default_scene key is
        // intentionally not accepted: legacy scene data has no compatibility
        // path.
        if (auto v = (*project)["default_world"].value<std::string>())
            m.default_world = *v;

        // binary_name — optional; empty falls back to `name` via binaryName().
        // Deliberately NOT defaulted to `name` here: round-tripping a manifest
        // must not silently turn an unset field into a set one, or the file
        // would grow a value the user never chose and could no longer change
        // by renaming the project.
        if (auto v = (*project)["binary_name"].value<std::string>())
            m.binary_name = *v;

        if (const auto* extensions = root["extensions"].as_array())
        {
            for (const auto& node : *extensions)
            {
                const auto* table = node.as_table();
                if (!table)
                {
                    return lux::cxx::unexpected(
                        std::string{"manifest '"} + path.string() +
                        "' contains a non-table [[extensions]] entry");
                }
                const auto id = (*table)["id"].value<std::string>();
                const auto module_path = (*table)["path"].value<std::string>();
                const auto target = (*table)["target"].value<std::string>();
                const auto major = (*table)["major"].value<std::int64_t>();
                const auto minimum_minor =
                    (*table)["minimum_minor"].value<std::int64_t>();
                if (!id || !module_path || !target || !major ||
                    !minimum_minor ||
                    !isCanonicalExtensionName(*id) ||
                    module_path->empty() || *major < 0 || *major > 65535 ||
                    *minimum_minor < 0 || *minimum_minor > 65535)
                {
                    return lux::cxx::unexpected(
                        std::string{"manifest '"} + path.string() +
                        "' has an invalid [[extensions]] entry");
                }

                EProjectExtensionTarget parsed_target;
                if (*target == "runtime")
                {
                    parsed_target =
                        EProjectExtensionTarget::RUNTIME;
                }
                else if (*target == "editor")
                {
                    parsed_target =
                        EProjectExtensionTarget::EDITOR;
                }
                else
                {
                    return lux::cxx::unexpected(
                        std::string{"manifest '"} + path.string() +
                        "' has unknown extension target '" + *target + "'");
                }

                lux::extensions::ExtensionId parsed_id{*id};
                for (const auto& existing : m.extensions)
                {
                    if (existing.id.hash() != parsed_id.hash())
                        continue;
                    return lux::cxx::unexpected(
                        std::string{"manifest '"} + path.string() +
                        (existing.id.name() == parsed_id.name()
                             ? "' repeats extension '"
                             : "' contains an extension hash collision at '") +
                        *id + "'");
                }
                m.extensions.push_back(ProjectExtensionEntry{
                    std::move(parsed_id),
                    std::filesystem::path{*module_path},
                    parsed_target,
                    static_cast<std::uint16_t>(*major),
                    static_cast<std::uint16_t>(*minimum_minor)});
            }
        }

        return m;
    }

    // ─────────────────────────────────────────────────────────────────
    //  ProjectManifest::saveToFile
    // ─────────────────────────────────────────────────────────────────

    lux::cxx::expected<void, std::string>
    ProjectManifest::saveToFile(const std::filesystem::path& path) const
    {
        // Build the document.
        toml::table project_tbl;
        project_tbl.insert_or_assign("name",           name);
        project_tbl.insert_or_assign("display_name",   display_name);
        project_tbl.insert_or_assign("project_guid",   uuids::to_string(project_guid));
        project_tbl.insert_or_assign("engine_version", engine_version);
        project_tbl.insert_or_assign("default_world",  default_world);
        // Written only when set, so an unset field stays absent rather than
        // becoming an empty string that later reads as "explicitly blank".
        if (!binary_name.empty())
            project_tbl.insert_or_assign("binary_name", binary_name);

        toml::table root;
        root.insert_or_assign("project", std::move(project_tbl));

        if (!extensions.empty())
        {
            toml::array extension_array;
            for (const auto& extension : extensions)
            {
                toml::table table;
                table.insert_or_assign("id", extension.id.name());
                table.insert_or_assign(
                    "path",
                    extension.path.generic_string());
                table.insert_or_assign(
                    "target",
                    extension.target == EProjectExtensionTarget::EDITOR
                        ? "editor"
                        : "runtime");
                table.insert_or_assign(
                    "major",
                    static_cast<std::int64_t>(extension.required_major));
                table.insert_or_assign(
                    "minimum_minor",
                    static_cast<std::int64_t>(extension.minimum_minor));
                extension_array.push_back(std::move(table));
            }
            root.insert_or_assign(
                "extensions",
                std::move(extension_array));
        }

        // Atomic write: temp file + rename. Avoids leaving a corrupted
        // manifest if the process is killed mid-write.
        std::filesystem::path tmp = path;
        tmp += ".tmp";

        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f.is_open())
                return lux::cxx::unexpected(
                    std::string{"failed to open '"} + tmp.string() +
                    "' for writing");
            f << root;
            if (!f)
                return lux::cxx::unexpected(
                    std::string{"failed to write '"} + tmp.string() + "'");
        }

        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec)
        {
            std::filesystem::remove(tmp, ec); // best-effort cleanup
            return lux::cxx::unexpected(
                std::string{"failed to replace '"} + path.string() +
                "': " + ec.message());
        }

        return {};
    }

} // namespace lux::authoring
