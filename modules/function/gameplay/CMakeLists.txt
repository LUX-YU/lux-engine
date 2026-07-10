# lux::engine::function::gameplay
#
# The game framework. Organised by concern under one module:
#   gameplay/world/         — neutral ECS core: World, ISystem, Name, Hierarchy
#   gameplay/render_bridge/ — dimension-neutral ECS->renderer bridge mechanism
#                             (RenderableSystem, the 3 generic bridges, EcsRenderTraits,
#                             RenderableBridgeContext), namespace lux::gameplay
#   gameplay/3d/            — the 3D kit (components/systems/controllers/traits +
#                             Scene3D install hooks), namespace lux::gameplay::d3
#   (a future gameplay/2d/ sits beside 3d/)
# Input lives in its own module (lux::engine::function::input).

set(LUX_GAMEPLAY_SRCS
    # neutral core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ComponentTypeRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/DebugDraw.cpp
    # render bridge mechanism (common)
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderableSystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderableBridgeContext.cpp
    # 3D kit
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AnimationSystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SkeletalAnimationResolver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldStreamingSystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Scene3D.cpp
    # 2D kit (lux::gameplay::d2) — sibling of 3d/, plan-driven install (Gate 0)
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PixelFieldRuntime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Scene2D.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Simulation2DSystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SpriteAnimationSystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SpriteAnim2DResolver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Physics2DWorld.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PixelFieldSaveIo.cpp
)

add_component(
    COMPONENT_NAME  gameplay
    NAMESPACE       lux::engine::function
    OUTPUT_NAME     lux_engine_function_gameplay
    SOURCE_FILES    ${LUX_GAMEPLAY_SRCS}
)

component_include_directories(
    gameplay
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
)

target_compile_definitions(
    gameplay
    PRIVATE
    LUX_FUNCTION_LIBRARY
)

target_link_libraries(
    gameplay
    PUBLIC
    lux::engine::core::math
    lux::engine::core::meta
    lux::engine::core::script
    lux::engine::resource::asset       # asset_id_t in mesh/skeletal/animator/skybox components + AssetLoadFn
    lux::engine::function::render       # RenderSession / proxies (bridge + 3D traits + light/grid/skybox components)
    lux::engine::function::animation    # PoseSampling (3D AnimationSystem)
    lux::engine::function::input        # ActionMapper / ActionId (3D camera controllers)
)

component_add_transitive_commands(
    gameplay
    "find_package (lux-engine-function REQUIRED COMPONENTS render animation input)"
    "find_package (lux-engine-core     REQUIRED COMPONENTS math meta script)"
    "find_package (lux-engine-resource REQUIRED COMPONENTS asset)"
)

# ---------------------------------------------------------------------------
# Reflection + Lua binding code generation (editor-only sidecar)
# ---------------------------------------------------------------------------
# The reflected component types' .meta.cpp / .lua.cpp compile into a separate
# SHARED `gameplay_meta` that only the editor links — so a no-editor build ships
# gameplay.dll free of reflection symbols / type-name strings / Lua glue.
if(LUX_BUILD_EDITOR)
    include_component_cmake_scripts(meta)

    add_library(gameplay_meta SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/src/meta_sidecar_anchor.cpp)
    set_target_properties(gameplay_meta PROPERTIES
        OUTPUT_NAME lux_engine_function_gameplay_meta)
    target_link_libraries(gameplay_meta
        PRIVATE
            gameplay                       # registration funcs reference component types
            lux::engine::core::meta        # ReflectionRegistry / MetaModuleRegistrar
    )
    target_compile_definitions(gameplay_meta PRIVATE LUX_FUNCTION_LIBRARY)

    engine_enable_module_meta(
        TARGET         gameplay
        SIDECAR_TARGET gameplay_meta
        TARGET_FILES
            # neutral-core reflected types
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/gameplay/world/components/NameComponent.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/gameplay/DebugDraw.hpp
            # 3D kit reflected components (lux::gameplay::d3)
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/gameplay/3d/world/components/TransformComponent.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/gameplay/3d/world/components/MeshComponent.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/gameplay/3d/world/components/SkeletalMeshComponent.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/gameplay/3d/world/components/AnimatorComponent.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/gameplay/3d/world/components/CameraComponent.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/gameplay/3d/world/components/SkyboxComponent.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/gameplay/3d/world/components/GridComponent.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/gameplay/3d/world/components/DirectionalLightComponent.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/gameplay/3d/world/components/PointLightComponent.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/gameplay/3d/world/components/SpotLightComponent.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/gameplay/3d/world/components/SceneSettingsComponent.hpp
            # 2D kit reflected components (lux::gameplay::d2) — added as they land (Slice A+):
            #   ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/gameplay/2d/world/components/Transform2DComponent.hpp
            #   ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/gameplay/2d/world/components/Camera2DComponent.hpp
            #   ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/gameplay/2d/world/components/SpriteComponent.hpp
        LUA_BINDING
    )
endif()

add_subdirectory(test)
