generate_visibility_header(
    ENABLE_MACRO_NAME LUX_ENGINE_SYSTEM_RENDER_LIBRARY
    PUBLIC_MACRO_NAME LUX_ENGINE_SYSTEM_RENDER_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_ENGINE_SYSTEM_RENDER_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/systems/render/visibility.h
)

add_component(
    STATIC
    COMPONENT_NAME system_render
    NAMESPACE lux::engine::systems
    OUTPUT_NAME lux_engine_system_render
    SOURCE_FILES
        src/mesh/MeshUploadDefinition.cpp
        src/mesh/MeshInstanceReaction.cpp
        src/mesh/MeshVisibilitySystem.cpp
        src/texture/TextureUploadDefinition.cpp
        src/material/MaterialUploadDefinition.cpp
        src/model/ModelInstantiation.cpp
        src/view/ViewReaction.cpp
        src/view/ViewCollector.cpp
        src/mesh/MeshTransformCollector.cpp
        src/RenderFrameSubmission.cpp
        src/ImguiFrameCapture.cpp
        src/RenderCapabilityMetaRegistry.cpp
        src/RenderDiagnostics.cpp
        src/RenderReaction.cpp
        src/RenderSystem.cpp
        src/RenderSystemConfiguration.cpp
)

component_include_directories(
    system_render
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
    INSTALL_TIME include
)

target_include_directories(system_render PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude)

target_compile_definitions(
    system_render
    PUBLIC LUX_ENGINE_SYSTEM_RENDER_DLL_DISABLE
    PRIVATE LUX_ENGINE_SYSTEM_RENDER_LIBRARY
)

# Sender instantiations plus ASan instrumentation exceed COFF's normal section count.
target_compile_options(system_render PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)

target_link_libraries(
    system_render
    PUBLIC
        lux::engine::function::render
        lux::engine::core::core_execution
        lux::engine::scene::scene_composition
        lux::engine::scene::scene_view
        lux::engine::engine_context
    PRIVATE
        lux::engine::core::log
        lux::engine::core::error
        lux::engine::assets::engine_assets
        lux::engine::ecs::ecs_visual
        lux::engine::ecs::ecs_transform
)

component_add_transitive_commands(
    system_render
    "find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render)"
    "find_package(lux-engine-core REQUIRED COMPONENTS log error)"
    "find_package(lux-engine-core-execution CONFIG REQUIRED COMPONENTS core_execution)"
    "find_package(lux-engine-assets REQUIRED COMPONENTS engine_assets)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS ecs_visual ecs_transform)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(lux-engine-scene-view REQUIRED COMPONENTS scene_view)"
    "find_package(lux-engine-context REQUIRED COMPONENTS engine_context)"
)

target_include_directories(system_render PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/component_info>)

include_component_cmake_scripts(meta)
include(${PROJECT_SOURCE_DIR}/engine/foundation/ecs/schema/cmake/engine_ecs_codegen.cmake)

engine_target_add_ecs_component_codegen(
    NAME render_view_component_codegen
    TARGET system_render
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/component_info
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/view/ViewReaction.cpp
    HEADER ${CMAKE_CURRENT_SOURCE_DIR}/sinclude/lux/engine/systems/render/view/RenderViewComponent.hpp
    LOGICAL_PATH lux/engine/systems/render/view/RenderViewComponent.hpp
    SYMBOL renderView
)

engine_target_add_ecs_component_codegen(
    NAME render_target_component_codegen
    TARGET system_render
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/component_info
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/view/ViewReaction.cpp
    HEADER ${CMAKE_CURRENT_SOURCE_DIR}/sinclude/lux/engine/systems/render/view/RenderTargetComponent.hpp
    LOGICAL_PATH lux/engine/systems/render/view/RenderTargetComponent.hpp
    SYMBOL renderTarget
)

engine_target_add_ecs_component_codegen(
    NAME mesh_render_resource_component_codegen
    TARGET system_render
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/component_info
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/mesh/MeshInstanceReaction.cpp
    HEADER ${CMAKE_CURRENT_SOURCE_DIR}/pinclude/lux/engine/systems/render/mesh/MeshRenderResourceComponent.hpp
    LOGICAL_PATH lux/engine/systems/render/mesh/MeshRenderResourceComponent.hpp
    SYMBOL meshRenderResource
)

lux_classify_target(TARGET system_render LAYER SYSTEMS PRODUCT RUNTIME ROLE LIBRARY)

if(LUX_BUILD_WINDOW)
    target_sources(system_render PRIVATE src/WindowSurface.cpp)

    target_link_libraries(system_render PUBLIC lux::engine::platform::window)

    component_add_transitive_commands(system_render "find_package(lux-engine-platform REQUIRED COMPONENTS window)")

    install(
        FILES sinclude/lux/engine/systems/render/WindowSurface.hpp
        DESTINATION include/lux/engine/systems/render
        COMPONENT lux_system_render
    )
endif()

lux_engine_install_components(
    PROJECT_NAME lux-engine-systems-render
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::systems
    COMPONENTS system_render
)



if(BUILD_TESTING)
    add_executable(engine_scene_render_test test/SceneRendering.cpp test/NoGeometryFeature.cpp)

    target_link_libraries(
        engine_scene_render_test
        PRIVATE
            system_render
            lux::engine::toolchain::toolchain_material_compiler
            lux::engine::function::render_extensions
            lux::engine::engine_initialization
            lux::engine::ecs::ecs_visual
            lux::engine::platform::platform_file
    )

    target_include_directories(engine_scene_render_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude)

    add_test(NAME engine.scene_render COMMAND engine_scene_render_test "${CMAKE_CURRENT_BINARY_DIR}/scene-fixture")

    set_tests_properties(engine.scene_render PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
endif()

if(BUILD_TESTING)
    add_executable(engine_render_transport_test test/RenderClient.cpp test/NoGeometryFeature.cpp)

    target_link_libraries(
        engine_render_transport_test PRIVATE
        system_render
        lux::engine::engine_initialization
        lux::engine::function::render_extensions
    )
    add_test(NAME engine.render_transport COMMAND engine_render_transport_test)
    set_tests_properties(engine.render_transport PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

    add_test(NAME engine.render_lifecycle COMMAND engine_render_transport_test --lifecycle)
    set_tests_properties(engine.render_lifecycle PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

    if(WIN32 AND TARGET window)
        set(surface_sample_header "${CMAKE_CURRENT_BINARY_DIR}/shaders/SurfaceSample.hpp")
        set(surface_triangle_header "${CMAKE_CURRENT_BINARY_DIR}/shaders/GraphTriangle.hpp")
        lux_render_shader("${surface_sample_header}" "test/SurfaceSample.slang" fragment kSurfaceSample)
        lux_render_shader(
            "${surface_triangle_header}"
            "../../../modules/function/render/test/shaders/GraphTriangle.slang"
            vertex
            kGraphTriangle
        )
        set(surface_shader_headers "${surface_sample_header}" "${surface_triangle_header}")

        add_executable(engine_render_surface_test test/Surface.cpp ${surface_shader_headers})
        target_include_directories(engine_render_surface_test PRIVATE
            "${CMAKE_CURRENT_BINARY_DIR}/shaders"
            "${PROJECT_SOURCE_DIR}/modules/function/render/test"
        )
        target_link_libraries(engine_render_surface_test PRIVATE
            system_render lux::engine::platform::window lux::engine::function::render_extensions
            lux::engine::engine_initialization
        )
        add_test(NAME engine.render_surface COMMAND engine_render_surface_test)
        add_test(NAME engine.render_surface_uploads COMMAND engine_render_surface_test uploads)
        set_tests_properties(engine.render_surface engine.render_surface_uploads PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
    endif()
endif()
