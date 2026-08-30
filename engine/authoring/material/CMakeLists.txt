generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_AUTHORING_MATERIAL_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_AUTHORING_MATERIAL_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_AUTHORING_MATERIAL_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/authoring/material/visibility.h
)

add_component(
    COMPONENT_NAME authoring_material
    NAMESPACE      lux::engine::authoring
    OUTPUT_NAME    lux_engine_authoring_material
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Node.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialGraph.cpp
)

component_include_directories(
    authoring_material
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    authoring_material
    PRIVATE LUX_ENGINE_AUTHORING_MATERIAL_LIBRARY
)

target_link_libraries(
    authoring_material
    PUBLIC
        lux::engine::resource::description
        lux::engine::resource::identity
)

component_add_transitive_commands(
    authoring_material
    "find_package(lux-engine-resource REQUIRED COMPONENTS identity description)"
)

lux_classify_target(
    TARGET  authoring_material
    LAYER   AUTHORING
    PRODUCT TOOLCHAIN
    ROLE    DOMAIN
)

add_executable(
    authoring_material_graph_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/MaterialGraphTest.cpp
)
target_link_libraries(
    authoring_material_graph_test
    PRIVATE lux::engine::authoring::authoring_material
)
if(MSVC)
    target_compile_options(authoring_material_graph_test PRIVATE /UNDEBUG)
else()
    target_compile_options(authoring_material_graph_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  authoring_material_graph_test
    LAYER   AUTHORING
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(NAME authoring_material_graph_test COMMAND authoring_material_graph_test)
