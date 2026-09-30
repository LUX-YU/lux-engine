#pragma once
#include <lux/engine/editor/project/ProjectCatalogAccess.hpp>
#include <lux/engine/editor/ui/visibility.h>
namespace lux::editor
{
    class ProjectStorage;
}
namespace lux::editor::ui
{
    // P12 bridge for the original Material/Scene UI and generated controls only.
    [[nodiscard]] LUX_EDITOR_UI_PUBLIC project::ProjectCatalogAccess
    projectCatalogAccess(const ProjectStorage*) noexcept;
}
