// Placeholder source for the `pack_d3_meta` sidecar SHARED library.
//
// The meta generator (target_add_meta) needs at least one .cpp file on the
// owning target to derive include paths + compile flags. The sidecar has no
// hand-authored sources of its own — every .cpp it carries is emitted by
// the code generator — so we anchor it here with this otherwise-empty TU.
namespace lux::pack { namespace { inline void meta_sidecar_anchor() {} } }
