#pragma once
/**
 * @file ProjectManifest.hpp  (editor-side alias shim)
 *
 * The real `ProjectManifest` struct lives in `lux::project` (see
 * `<lux/engine/project/ProjectManifest.hpp>`). This header re-exports
 * it under the editor's namespace so existing editor code can keep
 * writing `lux::editor::ProjectManifest` without churn.
 *
 * New code should prefer `lux::project::ProjectManifest` directly.
 */

#include <lux/engine/project/ProjectManifest.hpp>

namespace lux::editor
{
    using ::lux::project::ProjectManifest;
}
