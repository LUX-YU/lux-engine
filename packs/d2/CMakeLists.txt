# pack_d2 — the 2D reference pack (traditional 2D + pixel field glue over the
# engine mechanisms). namespace lux::pack.

add_library(pack_d2 SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PixelFieldRuntime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PixelFieldSaveIo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Scene2D.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Simulation2DSystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SpriteAnimationSystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SpriteAnim2DResolver.cpp
)
set_target_properties(pack_d2 PROPERTIES OUTPUT_NAME lux_pack_d2)

target_include_directories(pack_d2 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_definitions(pack_d2 PRIVATE LUX_FUNCTION_LIBRARY)

target_link_libraries(pack_d2
    PUBLIC
    lux::engine::function::render_bridge
    lux::engine::function::ecs
    lux::engine::function::render      # canvas2d feature ops / RegionUploadPlanner (public SDK)
    lux::engine::function::animation   # sprite clip sampling
    lux::engine::resource::asset
    lux::engine::core::math
    lux::engine::core::meta
)
# NOTE (ADR §3, done in phase 2): pack_d2 links NO physics pack — the demo
# solver self-registers via addPhysics2DDemoPack and drains the
# CollisionProbes2D seam; the dependency arrow points physics → d2 only.

add_subdirectory(test)
