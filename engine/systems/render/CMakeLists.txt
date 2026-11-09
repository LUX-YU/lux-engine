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
    "find_package(lux-engine-core-execution CONFIG REQUIRED COMPONENTS core_execution)"
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

    if(WIN32 AND TARGET window)
        set(surface_shader_headers)
        foreach(source IN ITEMS
            "${CMAKE_CURRENT_SOURCE_DIR}/test/SurfaceSample.frag"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../../modules/function/render/test/shaders/GraphTriangle.vert"
        )
            get_filename_component(shader_name "${source}" NAME_WE)
            set(binary "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader_name}.spv")
            set(header "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader_name}.hpp")
            add_custom_command(
                OUTPUT "${binary}" "${header}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
                COMMAND "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" -V --target-env vulkan1.3 -o "${binary}" "${source}"
                COMMAND lux_shader_metadata "${binary}" "${header}" "k${shader_name}"
                DEPENDS "${source}" lux_shader_metadata
                VERBATIM
            )
            list(APPEND surface_shader_headers "${header}")
        endforeach()

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
