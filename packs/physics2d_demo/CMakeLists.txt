# pack_physics2d_demo — the DEMO physics pack (ADR v3 ruling: the engine ships
# NO physics module; real physics arrives later as a third-party integration
# pack of the same shape). Minimal swept-AABB solver + the authoring components.
# Linked by tests/demos ONLY — editor and launcher must never link this.
# namespace lux::pack.
#
# Phase-1 scaffold note: pack_d2's Scene2D still wires Physics2DWorld directly,
# so pack_d2 temporarily LINKS this pack; phase 2 (ScenePackRegistry) flips the
# dependency — this pack will then register itself as the backer of the
# Physics/CharacterController capabilities from outside.
#
# Header borrow: our public headers consume pack_d2's component/plan/probe
# headers (a 2D physics integration IS 2D-domain glue). Header-only borrow —
# the d2 components are POD, no link edge back, so no cycle.

add_library(pack_physics2d_demo SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Physics2DWorld.cpp
)
set_target_properties(pack_physics2d_demo PROPERTIES OUTPUT_NAME lux_pack_physics2d_demo)

target_include_directories(pack_physics2d_demo PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../d2/include>
)

target_compile_definitions(pack_physics2d_demo PRIVATE LUX_FUNCTION_LIBRARY)

target_link_libraries(pack_physics2d_demo
    PUBLIC
    lux::engine::function::ecs
    lux::engine::core::math
    lux::engine::core::meta
)
