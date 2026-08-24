#include <lux/engine/authoring/project/Project.hpp>

#include <fstream>
#include <system_error>

namespace lux::authoring
{
    namespace
    {
        constexpr std::string_view kManifestExtension = ".luxproject";
        constexpr std::string_view kAssetExtension    = ".luxasset";
        constexpr std::string_view kWorldExtension    = ".luxworld";

        /// Walk @p root recursively, collecting absolute paths of files
        /// whose extension matches @p ext. Returns an empty vector if the
        /// root does not exist (an empty Content/ or Worlds/ is fine —
        /// not an error).
        std::vector<std::filesystem::path>
            scanByExtension(const std::filesystem::path& root,
                            std::string_view             ext)
        {
            std::vector<std::filesystem::path> result;
            if (!std::filesystem::exists(root))
                return result;

            std::error_code ec;
            for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
                 it != std::filesystem::recursive_directory_iterator{};
                 it.increment(ec))
            {
                if (ec)
                    break;
                if (!it->is_regular_file(ec))
                    continue;
                const auto& p = it->path();
                if (p.extension() == ext)
                    result.push_back(p);
            }
            return result;
        }
    } // namespace

    // ─────────────────────────────────────────────────────────────────
    //  Project::openFromDisk
    // ─────────────────────────────────────────────────────────────────

    lux::cxx::expected<Project, std::string>
    Project::openFromDisk(const std::filesystem::path& luxproject_file)
    {
        std::error_code ec;
        if (!std::filesystem::exists(luxproject_file, ec) || ec)
            return lux::cxx::unexpected(
                std::string{"manifest not found: '"} +
                luxproject_file.string() + "'");

        if (luxproject_file.extension() != kManifestExtension)
            return lux::cxx::unexpected(
                std::string{"not a .luxproject file: '"} +
                luxproject_file.string() + "'");

        auto manifest = ProjectManifest::loadFromFile(luxproject_file);
        if (!manifest)
            return lux::cxx::unexpected(std::move(manifest.error()));

        auto root = std::filesystem::absolute(luxproject_file.parent_path(), ec);
        if (ec)
            return lux::cxx::unexpected(
                std::string{"failed to resolve project root from '"} +
                luxproject_file.string() + "': " + ec.message());

        return Project(std::move(root), std::move(*manifest));
    }

    // ─────────────────────────────────────────────────────────────────
    //  Project::newOnDisk
    // ─────────────────────────────────────────────────────────────────

    lux::cxx::expected<Project, std::string>
    Project::newOnDisk(const std::filesystem::path& root,
                        std::string_view             project_name,
                        std::string_view             template_name)
    {
        if (project_name.empty())
            return lux::cxx::unexpected(std::string{"project name cannot be empty"});

        // M3a only supports the Empty template.  Future templates (Empty3D /
        // Empty2D / sample / ...) opt-in here.
        if (template_name != "Empty")
            return lux::cxx::unexpected(
                std::string{"unsupported template '"} + std::string{template_name} +
                "' (M3a supports only 'Empty')");

        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        if (ec)
            return lux::cxx::unexpected(
                std::string{"failed to create project root '"} +
                root.string() + "': " + ec.message());

        // Refuse to overwrite an existing project: the manifest must not
        // already exist. Anything else in the directory is fine (the user
        // may be creating a project in a folder with notes / docs).
        const auto manifest_path = root / (std::string{project_name} + std::string{".luxproject"});
        if (std::filesystem::exists(manifest_path, ec))
            return lux::cxx::unexpected(
                std::string{"project already exists: '"} +
                manifest_path.string() + "'");

        // Skeleton directories.
        const auto subdirs = {
            root / "Content",
            root / "Worlds",
            root / "Source",
            root / "Plugins",
            root / "Config",
            root / ".lux",
        };
        for (const auto& d : subdirs)
        {
            std::filesystem::create_directories(d, ec);
            if (ec)
                return lux::cxx::unexpected(
                    std::string{"failed to create '"} + d.string() +
                    "': " + ec.message());
        }

        // Manifest.
        auto manifest = ProjectManifest::makeDefault(project_name);
        if (auto saved = manifest.saveToFile(manifest_path); !saved)
            return lux::cxx::unexpected(std::move(saved.error()));

        auto abs_root = std::filesystem::absolute(root, ec);
        if (ec)
            return lux::cxx::unexpected(
                std::string{"failed to resolve project root absolute path: "} +
                ec.message());

        return Project(std::move(abs_root), std::move(manifest));
    }

    // ─────────────────────────────────────────────────────────────────
    //  Project::saveManifest
    // ─────────────────────────────────────────────────────────────────

    lux::cxx::expected<void, std::string>
    Project::saveManifest() const
    {
        return manifest_.saveToFile(manifestPath());
    }

    // ─────────────────────────────────────────────────────────────────
    //  Accessors
    // ─────────────────────────────────────────────────────────────────

    std::filesystem::path Project::manifestPath() const
    {
        return root_ / (manifest_.name + std::string{kManifestExtension});
    }

    std::filesystem::path Project::defaultWorldPath() const
    {
        if (manifest_.default_world.empty())
            return {};

        // The manifest's path is a relative POSIX string; convert to a
        // filesystem path the host can resolve. `std::filesystem::path`
        // already accepts POSIX slashes on Windows transparently.
        auto candidate = root_ / std::filesystem::path(manifest_.default_world);
        std::error_code ec;
        return std::filesystem::exists(candidate, ec) ? candidate : std::filesystem::path{};
    }

    std::vector<std::filesystem::path> Project::scanAssetFiles() const
    {
        return scanByExtension(contentRoot(), kAssetExtension);
    }

    std::vector<std::filesystem::path> Project::scanWorldFiles() const
    {
        return scanByExtension(worldsRoot(), kWorldExtension);
    }

} // namespace lux::authoring
