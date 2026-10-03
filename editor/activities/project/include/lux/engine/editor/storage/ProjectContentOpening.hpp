#pragma once

#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/editor/project/AssetCatalog.hpp>
#include <lux/engine/editor/sessions/SessionOpening.hpp>

namespace lux::editor::persistence
{
    class IArtifactStore;
}
namespace lux::editor
{
    class ProjectStorage;
    // Resolves a versioned project reference and submits its owned source to the original opening
    // service. The returned waiter belongs to the caller; no view or application record is created.
    [[nodiscard]] EditorResult<sessions::OpenAssetId> openProjectContent(
        ProjectStorage&, persistence::IArtifactStore&, sessions::SessionOpening&,
        AssetReference, const sessions::SessionFactorySnapshot&
    );
    [[nodiscard]] EditorResult<AssetReference> initialSceneReference(const ProjectStorage&);

}
