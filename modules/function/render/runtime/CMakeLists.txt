generate_visibility_header(ENABLE_MACRO_NAME LUX_RENDER_RUNTIME_LIBRARY
    PUBLIC_MACRO_NAME LUX_RENDER_RUNTIME_PUBLIC DISABLE_DLL_MACRO_NAME LUX_RENDER_RUNTIME_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/render/runtime/visibility.h)
add_component(COMPONENT_NAME render_runtime NAMESPACE lux::engine::function
    SOURCE_FILES src/RenderRuntime.cpp src/RendererThread.cpp src/RenderView.cpp src/ViewResources.cpp src/ViewImage.cpp)
component_include_directories(render_runtime
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/sinclude INSTALL_TIME include)
target_compile_definitions(render_runtime PRIVATE LUX_RENDER_RUNTIME_LIBRARY)
target_link_libraries(render_runtime PUBLIC render_client PRIVATE render_vulkan)
component_add_transitive_commands(render_runtime
    "find_package(lux-engine-function REQUIRED COMPONENTS render_client)")

if(BUILD_TESTING)
    add_executable(render_runtime_resource_test test/resources.cpp)
    target_link_libraries(render_runtime_resource_test PRIVATE render_runtime)
    if(MSVC)
        target_compile_options(render_runtime_resource_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(render_runtime_resource_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME render.runtime_resources COMMAND render_runtime_resource_test)
endif()

if(BUILD_TESTING AND LUX_RENDER_LIFECYCLE_DIAGNOSTICS)
    add_executable(render_failed_view_test test/failed_view.cpp)
    target_link_libraries(render_failed_view_test PRIVATE render_runtime)
    if(MSVC)
        target_compile_options(render_failed_view_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(render_failed_view_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME render.failed_view_reopen COMMAND render_failed_view_test)
    set_tests_properties(render.failed_view_reopen PROPERTIES ENVIRONMENT "LUX_RENDER_REJECT_RESIZE_REQUEST=1" TIMEOUT 30)
endif()
