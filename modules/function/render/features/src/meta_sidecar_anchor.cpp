// Placeholder source for the `render_meta` sidecar SHARED library.
//
// The meta generator (target_add_meta) needs at least one .cpp file on the
// owning target to derive include paths + compile flags. The sidecar has no
// hand-authored sources of its own — every .cpp it carries is emitted by the
// code generator — so we anchor it here with this otherwise-empty TU.
//
// Reflection is editor-only: this sidecar (and the meta generation that feeds
// it) is gated behind the Editor product profile so a shipped render DLL embeds no
// reflection symbols. See .internal/feature-quality-tiers-design-2026-06-19.md.
namespace lux::render { namespace { inline void meta_sidecar_anchor() {} } }
