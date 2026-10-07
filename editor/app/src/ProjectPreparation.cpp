#include <lux/engine/editor/FrameworkErrors.hpp>
#include <lux/engine/editor/detail/ProjectPreparation.hpp>
#include <lux/engine/project/PluginRendering.hpp>

namespace lux::editor::detail
{
    namespace
    {
        error::Error failure(ProjectFailure value) noexcept
        {
            return {
                Errors::ProjectManifest,
                {static_cast<std::uint64_t>(value.code), value.ordinal, static_cast<std::uint64_t>(value.system.value())
                }
            };
        }
        error::Error failure(const project::PluginFailure& value) noexcept
        {
            return {Errors::ProjectPlugins, {static_cast<std::uint64_t>(value.code)}};
        }
    } // namespace
    ProjectPreparation prepareProject(
        const std::filesystem::path& file,
        const std::optional<ProjectManifest>& create,
        std::span<const ProjectPluginLocation> locations,
        process::TaskReporter reporter
    ) noexcept
    {
        const auto stop = reporter.stopToken();
        bool published = false;
        const auto failed = [&](error::Error error) { return ProjectPreparation{cxx::unexpected(error), published}; };
        if (stop.stop_requested())
        {
            return failed({Errors::ProjectCancelled});
        }
        reporter.setPhase(create ? "Create project manifest" : "Read project manifest");
        if (create)
        {
            std::error_code ec;
            std::filesystem::create_directories(file.parent_path(), ec);
            if (ec)
            {
                return failed(failure({EProjectError::IO, 0, ec}));
            }
            const bool empty = std::filesystem::is_empty(file.parent_path(), ec);
            if (ec || !empty)
            {
                return failed(failure({ec ? EProjectError::IO : EProjectError::DESTINATION_EXISTS, 0, ec}));
            }
            auto written = writeProjectManifestAtomic(file, *create, EProjectWrite::CREATE, stop);
            if (!written)
            {
                return failed(failure(written.error()));
            }
            published = true;
        }
        auto manifest = create ? ProjectResult<ProjectManifest>{*create} : readProjectManifest(file, stop);
        if (!manifest)
        {
            return failed(failure(manifest.error()));
        }
        reporter.setPhase("Verify project plugins");
        project::PluginCatalog catalog;
        for (const auto& location : locations)
        {
            if (stop.stop_requested())
            {
                return failed({Errors::ProjectCancelled});
            }
            project::PluginCatalog addition;
            auto read = addition.read(location.catalog, location.root);
            if (!read)
            {
                return failed(failure(read.error()));
            }
            auto appended = catalog.append(std::move(addition));
            if (!appended)
            {
                return failed(failure(appended.error()));
            }
        }
        std::vector<project::MetadataIdentity> selected;
        selected.reserve(manifest->plugins.size());
        for (const auto& plugin : manifest->plugins)
        {
            selected.push_back({plugin.id, plugin.version});
        }
        if (stop.stop_requested())
        {
            return failed({Errors::ProjectCancelled});
        }
        auto plugins = project::PluginManager::create(std::move(catalog), selected);
        if (!plugins)
        {
            return failed(failure(plugins.error()));
        }
        reporter.setPhase("Prepare immutable scene registrations");
        auto registrations = project::readSceneRegistrations({}, plugins->libraries());
        if (!registrations)
        {
            return failed(failure(registrations.error()));
        }
        if (stop.stop_requested())
        {
            return failed({Errors::ProjectCancelled});
        }
        std::error_code ec;
        auto root = std::filesystem::canonical(file.parent_path(), ec);
        if (ec)
        {
            return failed(failure({EProjectError::IO, 0, ec}));
        }
        PreparedProject result{
            {manifest->name, std::move(root)},
            std::move(*manifest),
            std::make_unique<project::PluginManager>(std::move(*plugins)),
            std::make_shared<const project::SceneRegistrations>(std::move(*registrations))
        };
        reporter.setProgress(1, 1);
        return {std::move(result), published};
    }
} // namespace lux::editor::detail
