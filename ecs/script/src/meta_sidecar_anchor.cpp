// Placeholder source for the `script_meta` sidecar SHARED library.
//
// The meta generator (target_add_meta) needs at least one .cpp file on the
// owning target to derive include paths + compile flags. The sidecar has no
// hand-authored sources of its own — every .cpp it carries is emitted by the
// code generator (reflecting ScriptComponent) — so we anchor it here with this
// otherwise-empty TU. Mirrors the other ecs domains' meta_sidecar_anchor.cpp.
namespace lux::ecs { namespace { inline void meta_sidecar_anchor() {} } }
