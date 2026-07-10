# ─────────────────────────────────────────────────────────────────────────────
#  packs/ — first-party reference packs (ADR: lux-engine-pack-architecture v5).
#
#  A pack assembles engine mechanisms (ecs / render_bridge / render / animation)
#  into ONE concrete domain. Packs are libraries, but deliberately NOT engine
#  modules: they compile against the PUBLIC SDK only (include/ headers — never
#  sinclude/pinclude), so every pack is a permanent dogfood of the extension
#  surface a third-party pack would use. Apps (engine/editor, engine/launcher,
#  tests) link packs; modules never link a pack.
#
#  namespace: ALL packs share `lux::pack` (v5 ruling) — class names carry the
#  domain qualifier (…2D… vs unsuffixed 3D); a cross-pack name clash is a
#  compile error and is fixed by renaming the class, never by sub-namespacing.
# ─────────────────────────────────────────────────────────────────────────────
message("-------- Packs Tier --------")

add_subdirectory(d3)
add_subdirectory(physics2d_demo)   # before d2: pack_d2 links it (phase-1 scaffold)
add_subdirectory(d2)
