find_package(EnTT CONFIG REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_ECS_CORE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_ECS_CORE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_ECS_CORE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/ecs/core/visibility.h
)

add_component(
    COMPONENT_NAME core
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_core
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/World.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldCommands.cpp
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
    PRIVATE LUX_ENGINE_ECS_CORE_LIBRARY
)
target_link_libraries(core PUBLIC EnTT::EnTT lux::cxx::compile_time)
component_add_transitive_commands(
    core
    "find_package(EnTT CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
)

lux_classify_target(
    TARGET  core
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_executable(ecs_core_test test/core_test.cpp)
target_link_libraries(ecs_core_test PRIVATE lux::engine::ecs::core)
if(MSVC)
    target_compile_options(ecs_core_test PRIVATE /UNDEBUG)
else()
    target_compile_options(ecs_core_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  ecs_core_test
    LAYER   ECS
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME vnext_ecs_core COMMAND ecs_core_test)
set_tests_properties(vnext_ecs_core PROPERTIES LABELS "vnext;l1;core;cpu")

foreach(probe IN ITEMS
    world_registry_negative
    retired_registry_header_negative
    retired_scene_services_negative
)
    add_executable(
        ecs_${probe} EXCLUDE_FROM_ALL
        test/${probe}.cpp
    )
    target_link_libraries(ecs_${probe} PRIVATE lux::engine::ecs::core)
    lux_classify_target(
        TARGET  ecs_${probe}
        LAYER   ECS
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
