#include <lux/engine/editor/storage/ProjectCreation.hpp>
#include <lux/engine/editor/project/ProjectBuilder.hpp>
#include <lux/engine/scene/ScenePackage.hpp>

namespace lux::editor::detail
{
    ProjectPublicationPlan::PrepareResult prepareProjectCreation(
        std::filesystem::path directory,
        ProjectBuildConfig config,
        std::stop_token stop
    ) noexcept
    {
        if (stop.stop_requested())
            return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "project.create"});
        ProjectBuilder builder(config.project_id, std::move(config.name));
        builder.setPlugins(std::move(config.plugins));
        if (config.initial_scene)
            builder.setInitialScene(std::move(*config.initial_scene));
        auto built = std::move(builder).build();
        if (!built)
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::INVALID_ARGUMENT, "project.build", 0, {}, built.error()}
            );
        ProjectManifest manifest{built->project_id, std::move(built->name), {}, {}, std::move(built->plugins)};
        std::vector<ProjectFileChange> files;
        if (built->initial_scene)
        {
            const auto& initial = *built->initial_scene;
            auto encoded = lux::scene::encodeScenePackage(*initial.package, 256U * 1024U * 1024U, stop);
            if (!encoded)
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::SOURCE_FAILURE, "project.scene", 0, {}, encoded.error()}
                );
            auto owner = std::make_shared<const std::vector<std::byte>>(std::move(*encoded));
            const auto bytes = lux::cxx::SharedBytes<>::fromOwner(owner, *owner);
            ProjectAssetEntry entry{
                initial.package->scene->id(),
                "lux.scene.package",
                "Content/" + initial.source_path,
                {},
                projectContentDigest(bytes.view()),
                {},
                initial.mount_path
            };
            manifest.default_scene = entry.source_path;
            files.push_back({entry.source_path, "missing", bytes});
            manifest.assets.push_back(std::move(entry));
        }
        return ProjectPublicationPlan::prepare(
            std::move(directory), "Project.luxproject", "missing", std::move(manifest), std::move(files)
        );
    }

    EditorResult<ProjectCreationResult> publishNewProject(
        std::shared_ptr<const ProjectPublicationPlan> plan,
        std::stop_token stop
    ) noexcept
    {
        if (!plan)
            return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "project.create.plan"});
        if (stop.stop_requested())
            return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "project.create"});
        std::error_code error;
        const auto& publication = *plan;
        // This operation claims exactly one new directory. Existing empty directories are conflicts too.
        const bool claimed = std::filesystem::create_directory(publication.root(), error);
        if (!claimed)
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::SOURCE_FAILURE,
                "project.create.conflict",
                static_cast<std::uint64_t>(error.value()),
                "The destination must be a new directory"
            });
        auto lease = ProjectWriteLease::acquire(publication.root());
        if (!lease)
            return lux::cxx::unexpected(std::move(lease.error()));
        if (!lease->writable())
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "project.create.writer"});
        ProjectCreationResult result{
            publication.root() / publication.manifestPath(),
            publication.manifest().id,
            ProjectPublicationFailure{EProjectPublicationError::JOURNAL, publication.root()}
        };
        auto published = publishProjectFiles(plan, stop);
        if (!published)
        {
            // Recovery checks the exact files and digests owned by our journal. Never recursively
            // remove the directory: another application may already have written unrelated files.
            auto recovered = recoverProjectFiles(publication.root());
            if (!recovered)
                return lux::cxx::unexpected(std::move(recovered.error()));
            return lux::cxx::unexpected(std::move(published.error()));
        }
        if (!published->cleanup)
        {
            auto* warning = std::any_cast<ProjectPublicationFailure>(&published->cleanup.error().cause);
            if (warning)
                result.cleanup_warning = std::move(*warning);
        }
        else
            result.cleanup_warning.reset();
        // The writer lease is released before the sender delivers this committed result on Main.
        return result;
    }
}
