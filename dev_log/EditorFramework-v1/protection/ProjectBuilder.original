#include <lux/engine/editor/project/ProjectBuilder.hpp>
#include <lux/engine/scene/ScenePackage.hpp>

namespace lux::editor
{
    ProjectBuilder::BuildResult ProjectBuilder::build() && noexcept
    {
        const auto valid =
            validateProjectManifest(ProjectManifest{value_.project_id, value_.name, {}, {}, value_.plugins});
        if (!valid)
            return lux::cxx::unexpected(ProjectBuildFailure{EProjectBuildError::MANIFEST, valid.error()});
        if (value_.initial_scene)
        {
            const auto& initial = *value_.initial_scene;
            const bool invalid_path =
                !validProjectPath(initial.source_path) || !validProjectPath(initial.mount_path) ||
                initial.source_path.size() + sizeof("Content/") > ProjectManifestLimits{}.max_path_bytes ||
                initial.mount_path.size() > ProjectManifestLimits{}.max_path_bytes;
            if (invalid_path)
                return lux::cxx::unexpected(ProjectBuildFailure{EProjectBuildError::INVALID_PATH});
            const auto* package = initial.package.get();
            const bool missing_roots = !package || !package->scene || !package->world || !package->simulation;
            if (missing_roots)
                return lux::cxx::unexpected(ProjectBuildFailure{EProjectBuildError::INVALID_SCENE});
            const bool mismatched_roots = package->scene->id().isNull() ||
                                          package->scene->data().world() != package->world->id() ||
                                          package->scene->data().simulation() != package->simulation->id();
            const auto& world = package->world->data();
            const bool invalid_storage = world.partitioner().id.name != "lux.spatial.builtin.single" ||
                                         world.partitioner().version != 1 || !world.partitionIndexes().empty() ||
                                         package->partitions.size() != 1 || package->volumes.empty();
            if (mismatched_roots || invalid_storage)
                return lux::cxx::unexpected(ProjectBuildFailure{EProjectBuildError::INVALID_SCENE});
        }
        return std::move(value_);
    }
}
