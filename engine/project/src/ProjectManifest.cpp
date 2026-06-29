#include <lux/engine/project/ProjectManifest.hpp>

// vcpkg's prebuilt `tomlplusplus.lib` is compiled with exceptions
// enabled and the `tomlplusplus::tomlplusplus` link interface forces
// `TOML_HEADER_ONLY=0` on consumers, so toml++ parse failures arrive
// here as thrown `toml::parse_error`. The rest of the lux engine is
// built no-exceptions, so we narrow the throwing surface to this
// single translation unit: it catches at the API boundary and returns
// `lux::cxx::expected<T, std::string>`. No exception escapes.
#include <toml++/toml.hpp>

#include <array>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>

namespace lux::project
{
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
        m.default_scene  = ""; // populated by Phase D once the scene exists
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

        toml::table root;
        try
        {
            root = toml::parse(*text, path.string());
        }
        catch (const toml::parse_error& e)
        {
            std::ostringstream ss;
            ss << "toml parse error in '" << path.string() << "': "
               << e.description() << " at line "
               << e.source().begin.line;
            return lux::cxx::unexpected(ss.str());
        }
        catch (const std::exception& e)
        {
            return lux::cxx::unexpected(
                std::string{"toml parse error in '"} + path.string() +
                "': " + e.what());
        }


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

        // default_scene — optional
        if (auto v = (*project)["default_scene"].value<std::string>())
            m.default_scene = *v;

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
        project_tbl.insert_or_assign("default_scene",  default_scene);

        toml::table root;
        root.insert_or_assign("project", std::move(project_tbl));

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

} // namespace lux::project
