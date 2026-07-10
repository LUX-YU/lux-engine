# pack_d3 — the 3D reference pack (components/systems/controllers/render_bridge
# glue over the engine mechanisms). namespace lux::pack.
#
# Visibility: packs reuse the function-layer macro pair (LUX_FUNCTION_PUBLIC /
# LUX_FUNCTION_LIBRARY). Cross-DLL sharing of one macro is the established
# in-repo pattern (render ↔ old gameplay) — direct calls resolve through the
# import-library thunks.

add_library(pack_d3 SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AnimationSystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SkeletalAnimationResolver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldStreamingSystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Scene3D.cpp
)
set_target_properties(pack_d3 PROPERTIES OUTPUT_NAME lux_pack_d3)

target_include_directories(pack_d3 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_definitions(pack_d3 PRIVATE LUX_FUNCTION_LIBRARY)

target_link_libraries(pack_d3
    PUBLIC
    lux::engine::function::render_bridge
    lux::engine::function::ecs
    lux::engine::function::render
    lux::engine::function::animation   # PoseSampling (AnimationSystem)
    lux::engine::function::input       # ActionMapper (camera controllers)
    lux::engine::resource::asset
    lux::engine::core::math
    lux::engine::core::meta
)

# Editor-only reflection sidecar for the 3D reflected components (split heir
# of the old gameplay_meta; the ECS-kernel types live in ecs_meta).
if(LUX_BUILD_EDITOR)
    include_component_cmake_scripts(meta)

    add_library(pack_d3_meta SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/src/meta_sidecar_anchor.cpp)
    set_target_properties(pack_d3_meta PROPERTIES
        OUTPUT_NAME lux_pack_d3_meta)
    target_link_libraries(pack_d3_meta
        PRIVATE
            pack_d3
            lux::engine::core::meta
    )
    target_compile_definitions(pack_d3_meta PRIVATE LUX_FUNCTION_LIBRARY)

    engine_enable_module_meta(
        TARGET         pack_d3
        SIDECAR_TARGET pack_d3_meta
        TARGET_FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/pack/d3/world/components/TransformComponent.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/pack/d3/world/components/MeshComponent.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/pack/d3/world/components/SkeletalMeshComponent.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/pack/d3/world/components/AnimatorComponent.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/pack/d3/world/components/CameraComponent.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/pack/d3/world/components/SkyboxComponent.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/pack/d3/world/components/GridComponent.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/pack/d3/world/components/DirectionalLightComponent.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/pack/d3/world/components/PointLightComponent.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/pack/d3/world/components/SpotLightComponent.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/pack/d3/world/components/SceneSettingsComponent.hpp
        LUA_BINDING
    )
endif()
