set(DESCRIPTION_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Texture.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Shader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderInfo.cpp
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
    lux::cxx::memory
    lux::engine::core::extension_abi
    lux::engine::core::math
    stduuid
)

component_add_transitive_commands(
    description
    "find_package(stduuid CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS memory)"
    "find_package(lux-engine-core REQUIRED COMPONENTS extension_abi math)"
)

lux_classify_target(
    TARGET description
    LAYER RESOURCE
    PRODUCT RUNTIME
    ROLE FOUNDATION
)
