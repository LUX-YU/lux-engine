find_package(lux-cxx REQUIRED COMPONENTS core compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_ECS_SCHEDULE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_ECS_SCHEDULE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_ECS_SCHEDULE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/ecs/schedule/visibility.h
)

add_component(
    COMPONENT_NAME schedule
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_schedule
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Schedule.cpp
)

component_include_directories(
    schedule
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
        ${CMAKE_CURRENT_SOURCE_DIR}/../core/sinclude
)

target_compile_definitions(schedule PRIVATE LUX_ENGINE_ECS_SCHEDULE_LIBRARY)
target_link_libraries(
    schedule
    PUBLIC
        lux::engine::ecs::core
        lux::engine::core::object
        lux::cxx::core
        lux::cxx::compile_time
)
component_add_internal_dependencies(schedule core)
component_add_transitive_commands(
    schedule
    "find_package(lux-cxx REQUIRED COMPONENTS core compile_time)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core)"
    "find_package(lux-engine-core REQUIRED COMPONENTS object)"
)

lux_classify_target(
    TARGET  schedule
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_executable(ecs_schedule_test test/schedule_test.cpp)
target_link_libraries(ecs_schedule_test PRIVATE lux::engine::ecs::schedule)
target_include_directories(
    ecs_schedule_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)
if(MSVC)
    target_compile_options(ecs_schedule_test PRIVATE /UNDEBUG)
else()
    target_compile_options(ecs_schedule_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  ecs_schedule_test
    LAYER   ECS
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME vnext_ecs_schedule COMMAND ecs_schedule_test)
set_tests_properties(vnext_ecs_schedule PROPERTIES LABELS "vnext;l1;schedule;cpu")

add_executable(
    ecs_schedule_contract_test
    test/schedule_contract_test.cpp
)
target_include_directories(
    ecs_schedule_contract_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)
target_link_libraries(
    ecs_schedule_contract_test
    PRIVATE lux::engine::ecs::schedule
)
if(MSVC)
    target_compile_options(ecs_schedule_contract_test PRIVATE /UNDEBUG)
else()
    target_compile_options(ecs_schedule_contract_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  ecs_schedule_contract_test
    LAYER   ECS
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME vnext_ecs_schedule_contract COMMAND ecs_schedule_contract_test)
set_tests_properties(
    vnext_ecs_schedule_contract
    PROPERTIES LABELS "vnext;l1;schedule;commands;cpu"
)

add_executable(
    ecs_second_schedule_lease_probe
    test/second_schedule_lease_probe.cpp
)
target_link_libraries(
    ecs_second_schedule_lease_probe
    PRIVATE lux::engine::ecs::schedule
)
lux_classify_target(
    TARGET  ecs_second_schedule_lease_probe
    LAYER   ECS
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(
    NAME vnext_ecs_second_schedule_lease_rejected
    COMMAND
        ${CMAKE_COMMAND}
        -DPROGRAM=$<TARGET_FILE:ecs_second_schedule_lease_probe>
        -P ${PROJECT_SOURCE_DIR}/cmake/ExpectProcessFailure.cmake
)
set_tests_properties(
    vnext_ecs_second_schedule_lease_rejected
    PROPERTIES
        LABELS "vnext;l1;schedule;contract;negative"
)

foreach(probe IN ITEMS retired_isystem_negative retired_schedule_builder_negative)
    add_executable(
        ecs_${probe} EXCLUDE_FROM_ALL
        test/${probe}.cpp
    )
    target_link_libraries(ecs_${probe} PRIVATE lux::engine::ecs::schedule)
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
        PROPERTIES LABELS "vnext;l1;schedule;negative"
    )
endforeach()

include_component_cmake_scripts(meta)
engine_add_meta(
    NAME ecs_object_boundary_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/test/ObjectBoundaryProbe.hpp
    REGISTER_FUNC_NAME lux_ecs_object_boundary_probe
)
add_executable(
    ecs_object_boundary_test
    test/object_boundary_test.cpp
)
target_include_directories(
    ecs_object_boundary_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/test
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)
target_link_libraries(
    ecs_object_boundary_test
    PRIVATE lux::engine::ecs::schedule
)
if(MSVC)
    target_compile_options(ecs_object_boundary_test PRIVATE /UNDEBUG)
else()
    target_compile_options(ecs_object_boundary_test PRIVATE -UNDEBUG)
endif()
engine_target_add_meta(
    TARGET ecs_object_boundary_test
    METAS ecs_object_boundary_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/object_boundary_meta
)
lux_classify_target(
    TARGET  ecs_object_boundary_test
    LAYER   ECS
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME vnext_ecs_object_boundary COMMAND ecs_object_boundary_test)
set_tests_properties(
    vnext_ecs_object_boundary
    PROPERTIES LABELS "vnext;l1;schedule;object;cpu"
)
