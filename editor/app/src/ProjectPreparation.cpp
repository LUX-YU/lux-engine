#include <lux/engine/editor/ProjectErrors.hpp>
#include <lux/engine/editor/detail/ProjectPreparation.hpp>
#include <lux/engine/plugins/SceneRegistrations.hpp>

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

        error::Error failure(const plugins::PluginFailure& value) noexcept
        {
            return {Errors::ProjectPlugins, {static_cast<std::uint64_t>(value.code)}};
        }
    } // namespace

    ProjectPreparation prepareProject(
        const std::filesystem::path& file,
        const std::optional<ProjectManifest>& create,
        std::span<const ProjectPluginLocation> locations,
        execution::TaskReporter reporter
    ) noexcept
    {
        const auto stop = reporter.stopToken();
        auto publication = EManifestPublication::UNCHANGED;
        const auto failed = [&](error::Error error) { return ProjectPreparation{cxx::unexpected(error), publication}; };
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
                if (written.error().code == EProjectError::PUBLICATION_UNKNOWN)
                {
                    publication = EManifestPublication::UNKNOWN;
                }
                return failed(failure(written.error()));
            }
            publication = EManifestPublication::PUBLISHED;
        }
        std::error_code ec;
        auto manifest_file = std::filesystem::canonical(file, ec);
        if (ec)
        {
            return failed(failure({EProjectError::IO, 0, ec}));
        }
        auto manifest = create ? ProjectResult<ProjectManifest>{*create} : readProjectManifest(manifest_file, stop);
        if (!manifest)
        {
            return failed(failure(manifest.error()));
        }
        reporter.setPhase("Verify project plugins");
        plugins::PluginCatalog catalog;
        for (const auto& location : locations)
        {
            if (stop.stop_requested())
            {
                return failed({Errors::ProjectCancelled});
            }
            plugins::PluginCatalog addition;
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
        std::vector<plugins::MetadataIdentity> selected;
        selected.reserve(manifest->plugins.size());
        for (const auto& plugin : manifest->plugins)
        {
            selected.push_back({plugin.id, plugin.version});
        }
        if (stop.stop_requested())
        {
            return failed({Errors::ProjectCancelled});
        }
        auto plugins = plugins::PluginManager::create(std::move(catalog), selected);
        if (!plugins)
        {
            return failed(failure(plugins.error()));
        }
        reporter.setPhase("Prepare immutable scene registrations");
        auto registrations = plugins::readSceneRegistrations(
            {},
            plugins->libraries(),
            manifest->content ? &*manifest->content : nullptr
        );
        if (!registrations)
        {
            return failed(failure(registrations.error()));
        }
        if (stop.stop_requested())
        {
            return failed({Errors::ProjectCancelled});
        }
        PreparedProject result{
            {manifest->name, manifest_file.parent_path(), std::move(manifest_file)},
            std::move(*manifest),
            std::make_unique<plugins::PluginManager>(std::move(*plugins)),
            std::make_shared<const plugins::SceneRegistrations>(std::move(*registrations))
        };
        if (result.manifest.editor_layout)
        {
            const auto path = std::filesystem::canonical(result.description.root / *result.manifest.editor_layout, ec);
            const auto relative = path.lexically_relative(result.description.root);
            const bool outside_project = relative.empty() || *relative.begin() == ".." || relative.is_absolute();
            if (ec || outside_project)
            {
                return failed(failure({EProjectError::INVALID_PATH, 0, ec}));
            }
            auto layout = readProjectLayout(path, stop);
            if (!layout)
            {
                return failed(failure(layout.error()));
            }
            result.layout = std::move(*layout);
        }
        reporter.setProgress(1, 1);
        return {std::move(result), publication};
    }
} // namespace lux::editor::detail
