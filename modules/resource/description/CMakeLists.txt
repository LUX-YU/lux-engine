set(DESCRIPTION_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Texture.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Shader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderInfo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/material_graph/Node.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/material_graph/MaterialGraph.cpp
)

find_package(stduuid CONFIG REQUIRED)

add_component(
    COMPONENT_NAME  description
    NAMESPACE       lux::engine::resource
    SOURCE_FILES    ${DESCRIPTION_SRCS}
)

component_include_directories(
    description
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
)

target_compile_definitions(description
    PRIVATE
    LUX_RESOURCE_LIBRARY
)

target_link_libraries(description
    PUBLIC
    lux::engine::core::math
    lux::engine::core::script
    stduuid
)

component_add_transitive_commands(
    description
    "find_package(stduuid CONFIG REQUIRED)"
    "find_package(lux-engine-core REQUIRED COMPONENTS math script)"
)
