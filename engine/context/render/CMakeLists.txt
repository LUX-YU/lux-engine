include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/TargetClassification.cmake)

add_component(
    COMPONENT_NAME engine_render_context
    NAMESPACE lux::engine
    STATIC
    SOURCE_FILES
        src/RenderRequester.cpp
        src/RenderExecution.cpp
        src/RenderDiagnostics.cpp
        src/RenderAssets.cpp
)

component_include_directories(
    engine_render_context
    BUILD_TIME_SHARED "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/pinclude"
    INSTALL_TIME include
)

# WindowSurface is a real optional window dependency; offscreen SDKs do not advertise its API.
install(
    DIRECTORY include/
    DESTINATION include
    COMPONENT lux_engine_render_context
    PATTERN WindowSurface.hpp EXCLUDE
)

if(LUX_BUILD_WINDOW)
    target_sources(engine_render_context PRIVATE src/WindowSurface.cpp)

    target_link_libraries(engine_render_context PUBLIC lux::engine::platform::window)

    component_add_transitive_commands(engine_render_context "find_package(lux-engine-platform REQUIRED COMPONENTS window)")

    install(
        FILES include/lux/engine/context/render/WindowSurface.hpp
        DESTINATION include/lux/engine/context/render
        COMPONENT lux_engine_render_context
    )
endif()

target_compile_features(engine_render_context PUBLIC cxx_std_20)

target_link_libraries(
    engine_render_context
    PUBLIC
        lux::engine::function::render
        lux::engine::core::core_execution
        lux::engine::process::asset_loading
    PRIVATE
        lux::engine::core::log
        lux::engine::core::error
)

component_add_transitive_commands(
    engine_render_context
    "find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render)"
    "find_package(lux-engine-core-execution CONFIG REQUIRED COMPONENTS core_execution)"
    "find_package(lux-engine-core CONFIG REQUIRED COMPONENTS log error)"
    "find_package(lux-engine-process-asset-loading CONFIG REQUIRED COMPONENTS process_asset_loading)"
)

lux_classify_target(TARGET engine_render_context LAYER ENGINE PRODUCT RUNTIME ROLE LIBRARY)

lux_engine_install_components(
    PROJECT_NAME lux-engine-context-render
    VERSION 0.0.1
    NAMESPACE lux::engine
    COMPONENTS engine_render_context
)

if(BUILD_TESTING)
    add_executable(engine_render_transport_test test/RenderRequester.cpp)
    target_link_libraries(engine_render_transport_test PRIVATE engine_render_context)
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
        target_include_directories(engine_render_surface_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/shaders")
        target_link_libraries(engine_render_surface_test PRIVATE
            engine_render_context lux::engine::platform::window lux::engine::function::render_extensions
        )
        add_test(NAME engine.render_surface COMMAND engine_render_surface_test)
        add_test(NAME engine.render_surface_uploads COMMAND engine_render_surface_test uploads)
        set_tests_properties(engine.render_surface engine.render_surface_uploads PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
    endif()
endif()
