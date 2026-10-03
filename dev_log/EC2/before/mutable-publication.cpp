#include <lux/engine/editor/storage/ProjectPublication.hpp>
#include <type_traits>
using lux::editor::ProjectPublication;
static_assert(requires(ProjectPublication& p) { p.manifest.name = "changed after encoding"; p.manifest_bytes = {}; });
static_assert(!std::is_copy_constructible_v<ProjectPublication>);
