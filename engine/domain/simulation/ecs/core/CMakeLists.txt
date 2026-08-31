find_package(EnTT CONFIG REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SIMULATION_ECS_CORE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SIMULATION_ECS_CORE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/simulation/ecs/core/visibility.h
)

add_component(
    COMPONENT_NAME core
    NAMESPACE      lux::engine::simulation::ecs
    OUTPUT_NAME    lux_engine_simulation_ecs_core
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EcsCommandBuffer.cpp
)

component_include_directories(
    core
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    BUILD_TIME_SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
    INSTALL_TIME include
)

target_compile_definitions(
    core
    PUBLIC ENTT_NO_ETO
    PRIVATE LUX_ENGINE_SIMULATION_ECS_CORE_LIBRARY
)
target_link_libraries(core PUBLIC EnTT::EnTT lux::cxx::compile_time)
component_add_transitive_commands(
    core
    "find_package(EnTT CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
)

lux_classify_target(
    TARGET  core
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_executable(ecs_core_test test/core_test.cpp)
target_link_libraries(ecs_core_test PRIVATE lux::engine::simulation::ecs::core)
if(MSVC)
    target_compile_options(ecs_core_test PRIVATE /UNDEBUG)
else()
    target_compile_options(ecs_core_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  ecs_core_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME vnext_ecs_core COMMAND ecs_core_test)
set_tests_properties(vnext_ecs_core PROPERTIES LABELS "vnext;l1;core;cpu")

foreach(reactive_test IN ITEMS reactive_storage_probe extraction_change_set_test)
    add_executable(${reactive_test} test/${reactive_test}.cpp)
    target_link_libraries(${reactive_test} PRIVATE lux::engine::simulation::ecs::core)
    if(MSVC)
        target_compile_options(${reactive_test} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${reactive_test} PRIVATE -UNDEBUG)
    endif()
    lux_classify_target(
        TARGET  ${reactive_test}
        LAYER   SIMULATION
        PRODUCT TEST
        ROLE    FOUNDATION
    )
    add_test(NAME vnext_ecs_${reactive_test} COMMAND ${reactive_test})
    set_tests_properties(
        vnext_ecs_${reactive_test}
        PROPERTIES LABELS "vnext;l1;core;reactive"
    )
endforeach()

foreach(probe IN ITEMS
    retired_ecs_state_header_negative
    retired_ecs_commands_header_negative
    retired_ecs_query_header_negative
    retired_ecs_change_journal_header_negative
    retired_scene_services_negative
    retired_world_edit_header_negative
    retired_world_edit_type_negative
    retired_world_edit_method_negative
    retired_change_journal_negative
    retired_ecs_world_header_negative
    retired_ecs_world_type_negative
    retired_world_change_type_negative
    retired_world_command_type_negative
    retired_world_snapshot_header_negative
    retired_world_section_header_negative
    retired_persistent_entity_header_negative
    retired_top_level_ecs_header_negative
    retired_top_level_ecs_namespace_negative
)
    add_executable(
        ecs_${probe} EXCLUDE_FROM_ALL
        test/${probe}.cpp
    )
    target_link_libraries(ecs_${probe} PRIVATE lux::engine::simulation::ecs::core)
    lux_classify_target(
        TARGET  ecs_${probe}
        LAYER   SIMULATION
        PRODUCT TEST
        ROLE    FOUNDATION
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
        PROPERTIES LABELS "vnext;l1;core;negative"
    )
endforeach()
