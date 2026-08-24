find_package(lux-cxx REQUIRED COMPONENTS algorithm container reflection_runtime)

# FlowForge graphs are editable source documents. Runtime receives only the
# cooked NativeModuleScript contract, so this model belongs to Authoring and
# must never be discovered by a Player-only configuration.
set(AUTHORING_FLOWFORGE_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/NodeRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/NodeBase.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ControlNode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FunctionalNode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ObjectNode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ArithmeticNode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FlowGraph.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/StateLayout.cpp
)

add_component(
    STATIC
    COMPONENT_NAME authoring_flowforge
    NAMESPACE      lux::engine::authoring
    SOURCE_FILES   ${AUTHORING_FLOWFORGE_SRCS}
)

component_include_directories(
    authoring_flowforge
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_link_libraries(
    authoring_flowforge
    PUBLIC
        lux::cxx::container
        lux::cxx::algorithm
        lux::cxx::reflection_runtime
        lux::engine::core::meta
)
set_target_properties(
    authoring_flowforge PROPERTIES POSITION_INDEPENDENT_CODE ON)

component_add_transitive_commands(
    authoring_flowforge
    "find_package(lux-cxx REQUIRED COMPONENTS container algorithm reflection_runtime)"
    "find_package(lux-engine-core REQUIRED COMPONENTS meta)"
)

lux_classify_target(
    TARGET  authoring_flowforge
    LAYER   AUTHORING
    PRODUCT TOOLCHAIN
    ROLE    DOMAIN
)

set(ENABLE_FLOWFORGE_TEST OFF CACHE BOOL "Enable FlowForge graph-model tests")
if(ENABLE_FLOWFORGE_TEST)
    add_subdirectory(test)
endif()
