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
add_subdirectory(physics2d_demo)
add_subdirectory(d2)

# ─────────────────────────────────────────────────────────────────────────────
#  PUBLIC-SDK-ONLY enforcement (ADR §2/§9).
#
#  Module-private headers (sinclude/ pinclude/) ARE on the compile-time include
#  path (they are BUILD_TIME_SHARED between engine targets), so nothing but this
#  check keeps a pack from reaching into one. The check is a build-time script
#  (not a configure-time scan) so adding a bad #include fails the NEXT build,
#  not only a fresh configure. Every pack target depends on it.
# ─────────────────────────────────────────────────────────────────────────────
set(_packs_sdk_stamp ${CMAKE_CURRENT_BINARY_DIR}/packs_public_sdk_check.stamp)
file(GLOB_RECURSE _packs_all_sources CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
     ${CMAKE_CURRENT_SOURCE_DIR}/*.h   ${CMAKE_CURRENT_SOURCE_DIR}/*.inl)

add_custom_command(
    OUTPUT  ${_packs_sdk_stamp}
    COMMAND ${CMAKE_COMMAND}
            -DPACKS_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DREPO_ROOT=${CMAKE_SOURCE_DIR}
            -DSTAMP=${_packs_sdk_stamp}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_public_sdk_includes.cmake
    DEPENDS ${_packs_all_sources}
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_public_sdk_includes.cmake
    COMMENT "[packs] verifying packs include the PUBLIC SDK only"
    VERBATIM)

add_custom_target(packs_public_sdk_check ALL DEPENDS ${_packs_sdk_stamp})

foreach(_pack pack_d2 pack_d3 pack_physics2d_demo)
    add_dependencies(${_pack} packs_public_sdk_check)
endforeach()
