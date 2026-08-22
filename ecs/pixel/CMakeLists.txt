# lux::engine::ecs::pixel — the pixel-field CA simulation System's folder
# (ecs-layer ADR §3/§6): the comm-free vocabulary (PixelFieldTypes), the
# deterministic chunked CA runtime (PixelFieldRuntime — owns cell storage,
# NEVER in ECS components), and the owning component (PixelField2DComponent).
# The RENDER half (PixelField2DSubsystem) is ecs/render's; the physics probe
# adapter (FieldCollisionAdapter) stays domain glue in pack_d2 until
# PhysicsSystem lands. namespace: lux::ecs

find_package(lux-cxx REQUIRED COMPONENTS algorithm container)

set(LUX_ECS_PIXEL_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PixelChunkPreparation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PixelChunkPersistence.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PixelFieldRuntime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PixelFieldSystem.cpp
)

if(NOT TARGET TBB::tbb)
    find_package(TBB CONFIG REQUIRED)
endif()

add_component(
    COMPONENT_NAME  pixel
    NAMESPACE       lux::engine::ecs
    OUTPUT_NAME     lux_engine_ecs_pixel
    SOURCE_FILES    ${LUX_ECS_PIXEL_SRCS}
)

lux_classify_target(
    TARGET pixel
    LAYER ECS
    PRODUCT RUNTIME
    ROLE DOMAIN
)

component_include_directories(
    pixel
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    BUILD_TIME_SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
    INSTALL_TIME
        include
)

target_compile_definitions(
    pixel
    PRIVATE
    LUX_FUNCTION_LIBRARY
)

target_link_libraries(
    pixel
    PUBLIC
    lux::engine::ecs::core             # World / EntityRegistry (component + owner maintenance)
    # ★ 这里此前链着 lux::engine::function::render —— 整条渲染客户端 SDK，只为一个
    #   `RegionUploadPlanner`。逐个核实之后发现那个类的**唯一用户就是本目标**，
    #   渲染模块自己一次都没用过：它是纯 CPU 的矩形记账，放错了地方。现在它是
    #   `PixelDirtyLedger`，就在本目标里。像素模拟从此不认识渲染。
    lux::engine::core::math
    lux::engine::core::meta
    lux::cxx::algorithm
    lux::cxx::container
    lux::engine::resource::asset
    lux::engine::ecs::scene_format
    PRIVATE
    TBB::tbb
)

component_add_transitive_commands(
    pixel
    "find_package (lux-engine-ecs      REQUIRED COMPONENTS core scene_format)"
    "find_package (lux-cxx             REQUIRED COMPONENTS algorithm container)"
    "find_package (lux-engine-core     REQUIRED COMPONENTS math meta)"
    "find_package (lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package (TBB CONFIG REQUIRED)"
)
add_subdirectory(test)
add_subdirectory(streaming)

# ---------------------------------------------------------------------------
# PixelFieldRuntime 的那个组件 的反射 sidecar（批 6-G：反射跟着拥有它的目标走，不再挂在 pack 上 ——
# 链哪个目标就带哪些反射符号，这是 ecs 将来能作为插件模块存在的前提）。
# ---------------------------------------------------------------------------
if(LUX_BUILD_COMPONENT_META)
    include_component_cmake_scripts(meta)

    add_library(pixel_meta SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/src/meta_sidecar_anchor.cpp)
    lux_classify_target(
        TARGET pixel_meta
        LAYER ECS
        PRODUCT RUNTIME
        ROLE INTEGRATION
    )
    set_target_properties(pixel_meta PROPERTIES
        OUTPUT_NAME lux_engine_ecs_pixel_meta)
    target_link_libraries(pixel_meta
        PRIVATE
            pixel
            lux::engine::core::meta
    )
    target_compile_definitions(pixel_meta PRIVATE LUX_FUNCTION_LIBRARY)

    add_library(pixel_lua_meta SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/src/meta_sidecar_anchor.cpp)
    lux_classify_target(
        TARGET pixel_lua_meta
        LAYER ECS
        PRODUCT RUNTIME
        ROLE INTEGRATION
    )
    set_target_properties(pixel_lua_meta PROPERTIES
        OUTPUT_NAME lux_engine_ecs_pixel_lua_meta)
    target_link_libraries(pixel_lua_meta
        PRIVATE
            pixel
            lux::engine::core::meta
            lux::engine::function::script_lua
    )
    target_compile_definitions(pixel_lua_meta PRIVATE LUX_FUNCTION_LIBRARY)

    engine_enable_module_meta(
        TARGET         pixel
        SIDECAR_TARGET pixel_meta
        LUA_SIDECAR_TARGET pixel_lua_meta
        TARGET_FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ecs/pixel/components/PixelField2DComponent.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ecs/pixel/components/PixelChunk2DComponent.hpp
        LUA_BINDING
    )
endif()
