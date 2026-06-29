#pragma once
/**
 * @file Project.hpp  (editor-side alias shim)
 *
 * The real `Project` class lives in `lux::project` (see
 * `<lux/engine/project/Project.hpp>`). This header re-exports it under
 * the editor's namespace so existing editor code can keep writing
 * `lux::editor::Project` without churn — namespace migration is
 * deliberately gradual.
 *
 * New code should prefer `lux::project::Project` directly; this alias
 * may be removed in a future cleanup milestone.
 */

#include <lux/engine/project/Project.hpp>

namespace lux::editor
{
    using ::lux::project::Project;
}
