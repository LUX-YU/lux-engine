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
        ${CMAKE_CURRENT_SOURCE_DIR}/src/HierarchySystem.cpp
)

component_include_directories(
    hierarchy
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../core/sinclude
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)

target_compile_definitions(hierarchy PRIVATE LUX_ENGINE_ECS_HIERARCHY_LIBRARY)
target_link_libraries(
    hierarchy
    PUBLIC
        lux::engine::ecs::core
        lux::engine::ecs::schedule
        lux::engine::ecs::schema
)
component_add_internal_dependencies(hierarchy core schedule schema)
component_add_transitive_commands(
    hierarchy
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core schedule schema)"
)

lux_classify_target(
    TARGET  hierarchy
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(ecs_hierarchy_test test/hierarchy_test.cpp)
target_link_libraries(ecs_hierarchy_test PRIVATE lux::engine::ecs::hierarchy)
target_include_directories(
    ecs_hierarchy_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
        ${CMAKE_CURRENT_SOURCE_DIR}/../schedule/pinclude
)
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

foreach(probe IN ITEMS
    retired_hierarchy_rebuild_negative
    retired_hierarchy_subtree_negative
    retired_set_parent_negative
)
    add_executable(
        ecs_${probe} EXCLUDE_FROM_ALL
        test/${probe}.cpp
    )
    target_link_libraries(ecs_${probe} PRIVATE lux::engine::ecs::hierarchy)
    lux_classify_target(
        TARGET  ecs_${probe}
        LAYER   ECS
        PRODUCT TEST
        ROLE    DOMAIN
    )
    add_test(
        NAME vnext_ecs_${probe}
        COMMAND
            ${CMAKE_COMMAND}
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -DTARGET=ecs_${probe}
            "-DEXPECTED_PATTERN=${probe}"
            -P ${PROJECT_SOURCE_DIR}/cmake/ExpectTargetCompileFailure.cmake
    )
    set_tests_properties(
        vnext_ecs_${probe}
        PROPERTIES LABELS "vnext;l1;hierarchy;negative"
    )
endforeach()
