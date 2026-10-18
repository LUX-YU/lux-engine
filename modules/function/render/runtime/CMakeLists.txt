if(LUX_RENDER_CLIENT_ONLY)
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


target_compile_features(render_runtime PUBLIC cxx_std_20)
