set(DESCRIPTION_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Texture.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Shader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderInfo.cpp
)

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
        lux::engine::core::math
        lux::engine::function::script_core
        lux::engine::resource::identity
)

component_add_transitive_commands(
    description
    "find_package(lux-cxx REQUIRED COMPONENTS memory)"
    "find_package(lux-engine-core REQUIRED COMPONENTS math)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_core)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS identity)"
)

lux_classify_target(
    TARGET description
    LAYER RESOURCE
    PRODUCT RUNTIME
    ROLE FOUNDATION
)

add_executable(
    script_description_contract_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/script_description_contract_test.cpp
)
target_link_libraries(
    script_description_contract_test
    PRIVATE lux::engine::resource::description
)
lux_classify_target(
    TARGET  script_description_contract_test
    LAYER   RESOURCE
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(
    NAME script_description_contract_test
    COMMAND script_description_contract_test
)
