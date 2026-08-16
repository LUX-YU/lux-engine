find_package(lux-cxx REQUIRED COMPONENTS core compile_time container concurrent memory)

set(RENDER_CLIENT_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Canvas2DOperationClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FeatureParamsClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LightOperationClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialOperationClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MeshStackOperationClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderControlSession.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderErrorRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderFatal.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderFrameSession.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderLease.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderTypes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderUploadClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderUploadSession.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ViewCameraOperation.cpp
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
        lux::engine::platform::common
        lux::cxx::memory
        lux::engine::core::math
        lux::engine::core::meta
        lux::engine::resource::description
        lux::engine::resource::deployment
)

component_add_transitive_commands(
    render_client
    "find_package(lux-cxx REQUIRED COMPONENTS core compile_time container concurrent memory)"
    "find_package(lux-engine-platform REQUIRED COMPONENTS common)"
    "find_package(lux-engine-core REQUIRED COMPONENTS math meta)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS deployment description)"
)

lux_classify_target(
    TARGET  render_client
    LAYER   FUNCTION
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

# Generated proxy declarations and method bodies are part of render_client's
# backend-neutral public surface.  Generate them here for every profile so a
# pure TOOLCHAIN configure never borrows genops headers from a Developer install
# and never needs to configure render_features/Vulkan.
include_component_cmake_scripts(meta)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/render_comm_operations.cmake)

engine_add_comm_ops(
    NAME           render_comm_ops
    CLIENT_TARGET  render_client
    INCLUDE_PREFIX lux/engine/function/render/client/genops
    TARGET_FILES   ${LUX_RENDER_COMM_OPERATION_HEADERS}
)
