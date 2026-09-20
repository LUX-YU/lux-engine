find_package(lux-cxx REQUIRED COMPONENTS core compile_time container concurrent memory)

set(RENDER_CLIENT_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Capacity.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FeatureParamsClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderControlSession.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderErrorRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderFatal.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderProgramSession.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderLease.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderTypes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderUploadClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderUploadSession.cpp
)

add_component(
    COMPONENT_NAME render_client
    NAMESPACE      lux::engine::function
    SOURCE_FILES   ${RENDER_CLIENT_SRCS}
)

component_include_directories(
    render_client
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
)

target_compile_definitions(
    render_client
    PRIVATE
        LUX_FUNCTION_LIBRARY
)

target_link_libraries(
    render_client
    PUBLIC
        lux::cxx::compile_time
        lux::cxx::core
        lux::cxx::container
        lux::cxx::concurrent
        lux::cxx::memory
        lux::engine::core::math
        lux::engine::core::meta
        lux::engine::core::serialization
        lux::engine::resource::description
)

component_add_transitive_commands(
    render_client
    "find_package(lux-cxx REQUIRED COMPONENTS core compile_time container concurrent memory)"
    "find_package(lux-engine-core REQUIRED COMPONENTS math meta serialization)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS description)"
)

# Generic protocol generation is available to external Features. The list of
# builtin Features and their generated sources belongs to features/.
component_add_cmake_scripts(render_client
    ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/engine_render_codegen.cmake)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/template/
    DESTINATION share/lux-engine-function/render_client/cmake_scripts/template)
