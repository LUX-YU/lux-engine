## engine/runtime/render/scene — render-scene integration.
##
## bringUp / tick / tearDown + the simulation (script) lifecycle + the
## SceneSystem extension point, extracted from EditorScene so the editor and a
## shipped game drive the SAME code path: what editor Play shows IS what runs
## in the packaged binary.
##
## What stays with the HOST (and out of this component): render targets
## (editor offscreen vs game surface), scaffolding cameras
## (EditorTransientComponent is an editor concept this component never sees),
## the editor's Play snapshot/restore, and host-only systems (selection,
## editor camera navmesh) — they register through registerSystem().
##
## Why engine/ and not ecs/: it needs scene_io (engine layer) + the render
## session client, and the root CMakeLists processes modules -> ecs -> engine.
## MUST build in Player-only profiles (the profile/target DAG is the gate).

find_package(EnTT CONFIG REQUIRED)
find_package(Eigen3 REQUIRED)
# RenderResourceOps.cpp 与 MaterialSubservices.cpp(sender 编排 TU)。
# 守卫式查找(editor 同款):
# 别处可能已把 STDEXEC::stdexec 引进来,重复 find 会撞出口集半定义错。
if(NOT TARGET STDEXEC::stdexec)
    find_package(stdexec CONFIG REQUIRED)
endif()

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_RUNTIME_RENDER_SCENE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_RUNTIME_RENDER_SCENE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_RUNTIME_RENDER_SCENE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/render/scene/visibility.h
)

add_component(
    COMPONENT_NAME  runtime_render_scene
    NAMESPACE       lux::engine::runtime
    OUTPUT_NAME     lux_engine_runtime_render_scene
    SOURCE_FILES    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderSceneIntegration.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/AsyncRenderUploadService.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderResourceService.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderResourceOps.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/subservices/TextureSubservice.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/subservices/MeshSubservice.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/subservices/MaterialSubservices.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/ResidencyAssembly.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/StandardFeaturePlan.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderDiagnostics.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneGeometryPrepareService.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/ClassicMeshBatchRenderSubsystem.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/TerrainTileRenderSubsystem.cpp
)

component_include_directories(
    runtime_render_scene
    BUILD_TIME_EXPORT   ${CMAKE_CURRENT_SOURCE_DIR}/include
                        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME        include
    PRIVATE             ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)

lux_classify_target(
    TARGET  runtime_render_scene
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)

target_compile_definitions(
    runtime_render_scene
    PRIVATE
    LUX_RUNTIME_RENDER_SCENE_LIBRARY
)

# 编排管道与材质多段 RPC transaction 的 stdexec 选择性引入
# (ImportPipeline 同款纪律):只有对应 .cpp 吃 stdexec 头与 MSVC
# 一致性开关,门面头保持 stdexec-free。
if(MSVC)
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderResourceOps.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/subservices/MaterialSubservices.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneGeometryPrepareService.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ClassicMeshBatchRenderSubsystem.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/TerrainTileRenderSubsystem.cpp
        PROPERTIES COMPILE_OPTIONS "/permissive-;/Zc:__cplusplus;/Zc:preprocessor")
endif()

target_link_libraries(
    runtime_render_scene
    PUBLIC
    lux::engine::runtime::runtime_scene_core
    lux::engine::runtime::runtime_entity_scene
    lux::engine::ecs::core             # World / ScheduleBuilder / SceneServices
    lux::engine::ecs::scene_format     # ContentBlobRef public contract
    # ★ 这里此前链着 `lux::engine::ecs::scene`（两个 pack 的配方）。删掉了：本运行时
    #   一行都没 include 过 addSceneXxx —— 「装哪些 pack」是**宿主**的决定，经
    #   `Config::packs` 传进来。留着那条链接等于「运行时认识引擎自带的那两个 pack」，
    #   与批 5 把选择权交给宿主的那次改动自相矛盾。
    lux::engine::ecs::render_core # dimension-neutral render lifecycle
    lux::engine::ecs::render_3d   # built-in Grid3D render-effect factory
    lux::engine::ecs::terrain_components
    lux::engine::resource::asset       # AssetManager (bridge + eviction)
    lux::engine::content
    lux::engine::function::render_standard_content
    lux::cxx::memory           # owning contribution/upload payloads
    lux::cxx::algorithm          # ContentBlob integrity admission
    lux::engine::function::render_features # backend + built-in feature assembly
    lux::engine::core::log             # diagnostics route through lux::log (§7.1)
    lux::engine::core::events          # 统一事件总线(段C:渲染错误事件化)
    lux::engine::runtime::runtime_assets # public AssetClient dependency seam
    lux::engine::runtime::runtime_extension_loader
    EnTT::EnTT
    Eigen3::Eigen
    PUBLIC
    lux::engine::runtime::runtime_execution                          # AsyncRuntime facade + upload client assembly
    PRIVATE
    lux::cxx::core
    STDEXEC::stdexec                   # 两个 sender 编排 TU 专用(私有,不出头)
)

# White-box residency probes deliberately exercise the implementation seam,
# but that seam must never become part of the installed runtime_render_scene API.
# Keeping this build-tree-only target explicit prevents tests from pressuring
# low-level residency types back into the public include tree.
add_library(runtime_render_scene_test_support INTERFACE)
target_include_directories(
    runtime_render_scene_test_support
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/pinclude>
)
target_link_libraries(
    runtime_render_scene_test_support
    INTERFACE
        runtime_render_scene
)

component_add_transitive_commands(
    runtime_render_scene
    "find_package(EnTT CONFIG REQUIRED)"
    "find_package(Eigen3 REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS algorithm core memory)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core scene_format render_core render_3d terrain_components)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-engine-content REQUIRED COMPONENTS content)"
    "find_package(lux-engine-function REQUIRED COMPONENTS render_features render_standard_content)"
    "find_package(lux-engine-core REQUIRED COMPONENTS log events)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_execution runtime_assets runtime_entity_scene runtime_extension_loader runtime_scene_core)"
)

# 单元测试(驻留 T4 起;headless,自校验 exe)。
set(ENABLE_SCENE_RUNTIME_TEST OFF CACHE BOOL "Enable runtime_render_scene tests")
if(ENABLE_SCENE_RUNTIME_TEST)
    add_subdirectory(test)
endif()
