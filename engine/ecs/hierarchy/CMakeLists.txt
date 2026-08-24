generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_ECS_HIERARCHY_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_ECS_HIERARCHY_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_ECS_HIERARCHY_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/ecs/hierarchy/visibility.h
)

add_component(
    COMPONENT_NAME hierarchy
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_hierarchy
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/HierarchyIndex.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/HierarchySchema.cpp
)

component_include_directories(
    hierarchy
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../core/sinclude
)

target_compile_definitions(hierarchy PRIVATE LUX_ENGINE_ECS_HIERARCHY_LIBRARY)
target_link_libraries(
    hierarchy
    PUBLIC
        lux::engine::ecs::core
        lux::engine::ecs::schema
)
component_add_internal_dependencies(hierarchy core schema)
component_add_transitive_commands(
    hierarchy
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core schema)"
)

lux_classify_target(
    TARGET  hierarchy
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(ecs_hierarchy_test test/hierarchy_test.cpp)
target_link_libraries(ecs_hierarchy_test PRIVATE lux::engine::ecs::hierarchy)
if(MSVC)
    target_compile_options(ecs_hierarchy_test PRIVATE /UNDEBUG)
else()
    target_compile_options(ecs_hierarchy_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  ecs_hierarchy_test
    LAYER   ECS
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(NAME vnext_ecs_hierarchy COMMAND ecs_hierarchy_test)
set_tests_properties(vnext_ecs_hierarchy PROPERTIES LABELS "vnext;l1;hierarchy;cpu")
