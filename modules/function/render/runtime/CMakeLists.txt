# Real builtin command value declarations; no Client object, Client DLL, Pending table or forwarding API.
add_interface_component(
    COMPONENT_NAME render_commands
    NAMESPACE lux::engine::render
)

component_include_directories(render_commands
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include"
    INSTALL_TIME include
)

component_add_internal_dependencies(render_commands
    lux::engine::render::render_core lux::engine::render::render_data lux::engine::render::render_protocol
)

target_compile_features(render_commands INTERFACE cxx_std_20)

if(LUX_RENDER_CLIENT_ONLY)
    if(BUILD_TESTING)
        add_executable(render_sync_source_consumer test/client_installed/Main.cpp)
        target_link_libraries(render_sync_source_consumer PRIVATE render_commands lux::engine::platform::platform_ipc)
        add_custom_command(TARGET render_sync_source_consumer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:lux::engine::platform::platform_ipc>" "$<TARGET_FILE_DIR:render_sync_source_consumer>"
        )
        add_test(NAME render.client.sync_source COMMAND render_sync_source_consumer)
    endif()
    return()
endif()

find_package(stdexec CONFIG REQUIRED)

set(LUX_RUNTIME_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderServer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/CommandRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ServerRegistration.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ServerScene.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ServerEffect.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ServerCompletion.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ServerLoop.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/CommandDispatch.cpp
)

add_component(
    STATIC
    COMPONENT_NAME render_runtime
    NAMESPACE lux::engine::render
    OUTPUT_NAME lux_engine_render_runtime
    SOURCE_FILES ${LUX_RUNTIME_SOURCES}
)

component_include_directories(render_runtime
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE pinclude
    INSTALL_TIME include
)

component_add_internal_dependencies(render_runtime
    lux::engine::render::render_commands lux::engine::render::render_features
    lux::engine::render::render_native_vulkan_extensions
)

target_link_libraries(render_runtime PUBLIC
    STDEXEC::stdexec lux::engine::platform::platform_ipc lux::engine::core::core_execution
)

target_link_libraries(render_runtime PRIVATE
    lux::engine::platform::platform_process
)

component_add_transitive_commands(render_runtime
    [=[find_package(stdexec CONFIG REQUIRED)]=]
    [=[find_package(lux-engine-core-execution CONFIG REQUIRED COMPONENTS core_execution
        HINTS "${CMAKE_CURRENT_LIST_DIR}/../../lux-engine-core-execution")]=]
    [=[find_package(lux-engine-platform CONFIG REQUIRED COMPONENTS platform_process platform_ipc
        HINTS "${CMAKE_CURRENT_LIST_DIR}/../../lux-engine-platform")]=]
)

target_include_directories(render_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../native/vulkan/sinclude")

target_compile_features(render_runtime PUBLIC cxx_std_20)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()
