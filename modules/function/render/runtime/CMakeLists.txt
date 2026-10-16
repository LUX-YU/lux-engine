find_package(lux-cxx CONFIG REQUIRED COMPONENTS concurrent)

generate_visibility_header(
    ENABLE_MACRO_NAME LUX_RENDER_CLIENT_LIBRARY
    PUBLIC_MACRO_NAME LUX_RENDER_CLIENT_PUBLIC
    GENERATE_FILE_PATH lux/engine/render/runtime/visibility.h
)

add_component(
    COMPONENT_NAME render_client
    NAMESPACE lux::engine::render
    OUTPUT_NAME lux_engine_render_client
    SOURCE_FILES src/RenderClient.cpp
)

component_include_directories(render_client
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include" "${LUX_GENERATE_HEADER_DIR}"
    PRIVATE pinclude
    INSTALL_TIME include
)

component_add_internal_dependencies(render_client
    lux::engine::render::render_core lux::engine::render::render_data lux::engine::render::render_protocol
)

target_link_libraries(render_client PUBLIC lux::engine::platform::platform_ipc)

target_compile_definitions(render_client PRIVATE LUX_RENDER_CLIENT_LIBRARY)

component_add_transitive_commands(render_client
    [=[find_package(lux-engine-platform CONFIG REQUIRED COMPONENTS platform_ipc
        HINTS "${CMAKE_CURRENT_LIST_DIR}/../../lux-engine-platform")]=]
)

add_component(
    STATIC
    COMPONENT_NAME render_runtime
    NAMESPACE lux::engine::render
    OUTPUT_NAME lux_engine_render_runtime
    SOURCE_FILES src/RenderServer.cpp src/Request.cpp
)

component_include_directories(render_runtime
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE pinclude
    INSTALL_TIME include
)

component_add_internal_dependencies(render_runtime
    lux::engine::render::render_client lux::engine::render::render_features
    lux::engine::render::render_native_vulkan_extensions lux::engine::render::render_protocol_async
)

target_link_libraries(render_runtime PRIVATE lux::engine::platform::platform_ipc lux::cxx::concurrent bcrypt)

component_add_transitive_commands(render_runtime
    "find_package(lux-cxx CONFIG REQUIRED COMPONENTS concurrent)"
)

target_include_directories(render_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../native/vulkan/sinclude")

foreach(component render_client render_runtime)
    target_compile_features(${component} PUBLIC cxx_std_20)
endforeach()

if(BUILD_TESTING)
    target_sources(render_client PRIVATE test/ClientAllocation.cpp)
    set_target_properties(render_client PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")
    add_subdirectory(test)
endif()
