#include <lux/engine/editor/storage/ProjectCreation.hpp>
#include <lux/engine/editor/project/ProjectBuilder.hpp>
#include <lux/engine/scene/ScenePackage.hpp>

namespace lux::editor::detail
{
    EditorResult<ProjectPublication> prepareProjectCreation(
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
        ProjectPublication result;
        result.root = std::move(directory);
        result.manifest_path = "Project.luxproject";
        result.before_manifest_digest = "missing";
        result.manifest = {built->project_id, std::move(built->name), {}, {}, std::move(built->plugins)};
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
                EProjectAssetKind::SCENE,
                "Content/" + initial.source_path,
                {},
                projectContentDigest(bytes.view()),
                {},
                initial.mount_path
            };
            result.manifest.default_scene = entry.source_path;
            result.files.push_back({entry.source_path, "missing", bytes});
            result.manifest.assets.push_back(std::move(entry));
        }
        auto manifest = encodeProjectManifest(result.manifest);
        if (!manifest)
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::SOURCE_FAILURE, "project.manifest", 0, {}, manifest.error()}
            );
        auto owner = std::make_shared<const std::string>(std::move(*manifest));
        result.manifest_bytes = lux::cxx::SharedBytes<>::fromOwner(owner, std::as_bytes(std::span(*owner)));
        return result;
    }

    EditorResult<ProjectCreationResult> publishNewProject(
        ProjectPublication& publication,
        std::stop_token stop
    ) noexcept
    {
        if (stop.stop_requested())
            return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "project.create"});
        std::error_code error;
        publication.root = std::filesystem::absolute(publication.root, error).lexically_normal();
        if (error || publication.root.filename().empty())
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "project.create.directory"});
        // This operation claims exactly one new directory. Existing empty directories are conflicts too.
        const bool claimed = std::filesystem::create_directory(publication.root, error);
        if (!claimed)
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::SOURCE_FAILURE,
                "project.create.conflict",
                static_cast<std::uint64_t>(error.value()),
                "The destination must be a new directory"
            });
        auto lease = ProjectWriteLease::acquire(publication.root);
        if (!lease)
            return lux::cxx::unexpected(std::move(lease.error()));
        if (!lease->writable())
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "project.create.writer"});
        ProjectCreationResult result{
            publication.root / publication.manifest_path,
            publication.manifest.id,
            ProjectPublicationFailure{EProjectPublicationError::JOURNAL, publication.root}
        };
        auto published = publishProjectFiles(publication, stop);
        if (!published)
        {
            // Recovery checks the exact files and digests owned by our journal. Never recursively
            // remove the directory: another application may already have written unrelated files.
            auto recovered = recoverProjectFiles(publication.root);
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
