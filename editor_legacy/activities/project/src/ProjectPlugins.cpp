#include <lux/engine/editor/storage/ProjectPlugins.hpp>
#include <algorithm>

namespace lux::editor
{
    lux::project::PluginResult<lux::project::PluginManager> loadProjectPlugins(
        const std::filesystem::path& project_root,
        std::span<const ProjectPluginEntry> selection,
        const std::filesystem::path& installation
    )
    {
        lux::project::PluginCatalog catalog;
        if (!installation.empty())
        {
            auto read = catalog.read(installation / "share/lux-engine/plugins/catalog.json", installation);
            if (!read)
                return cxx::unexpected(read.error());
        }
        std::vector<lux::project::MetadataIdentity> selected;
        std::vector<std::filesystem::path> descriptions;
        selected.reserve(selection.size());
        for (const auto& plugin : selection)
        {
            selected.push_back({plugin.id, plugin.version});
            if (plugin.description_path.empty())
                continue;
            std::error_code error;
            const auto root = std::filesystem::canonical(project_root, error);
            if (error)
                return cxx::unexpected(lux::project::PluginFailure{
                    lux::project::EPluginError::IO_FAILURE,
                    plugin.id,
                    plugin.description_path,
                    error.message()
                });
            const auto path =
                std::filesystem::canonical(root / std::filesystem::u8path(plugin.description_path), error);
            if (error)
                return cxx::unexpected(lux::project::PluginFailure{
                    lux::project::EPluginError::IO_FAILURE,
                    plugin.id,
                    plugin.description_path,
                    error.message()
                });
            const auto relative = path.lexically_relative(root);
            const bool is_invalid_relative = relative.empty() || relative.is_absolute();
            const bool escapes_root =
                !is_invalid_relative && std::ranges::any_of(relative, [](const auto& part) { return part == ".."; });
            if (is_invalid_relative || escapes_root)
                return cxx::unexpected(lux::project::PluginFailure{
                    lux::project::EPluginError::INVALID_PATH,
                    plugin.id,
                    plugin.description_path,
                    "Plugin description must be inside its project"
                });
            if (std::ranges::find(descriptions, path) != descriptions.end())
                continue;
            auto read = catalog.read(path, root);
            if (!read)
                return cxx::unexpected(read.error());
            descriptions.push_back(path);
        }
        return lux::project::PluginManager::create(std::move(catalog), selected);
    }
}
