add_interface_component(
    COMPONENT_NAME simulation_scripting_core
    NAMESPACE      lux::engine::simulation
)

component_include_directories(
    simulation_scripting_core
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include
)

target_include_directories(simulation_scripting_core INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sinclude>
)

target_link_libraries(
    simulation_scripting_core
    INTERFACE
        lux::engine::simulation::system
        lux::engine::simulation::ecs::core
        lux::engine::function::script_core
        lux::engine::resource::identity
        lux::engine::function::script_artifact
)

component_add_transitive_commands(
    simulation_scripting_core
    "find_package(lux-engine-simulation-system REQUIRED COMPONENTS system)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS core)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_core)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS identity)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_artifact)"
)

lux_classify_target(
    TARGET  simulation_scripting_core
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

if(BUILD_TESTING)
    if(MSVC)
        add_executable(simulation_script_coroutine_alignment_contract_test test/coroutine_alignment_contract_probe.cpp)
        target_link_libraries(simulation_script_coroutine_alignment_contract_test
            PRIVATE lux::engine::simulation::simulation_scripting_core)
        lux_classify_target(TARGET simulation_script_coroutine_alignment_contract_test
            LAYER SIMULATION PRODUCT TEST ROLE FOUNDATION)
        add_test(NAME simulation_script_coroutine_alignment_contract_test
            COMMAND simulation_script_coroutine_alignment_contract_test)
    endif()
    add_executable(simulation_script_class_storage_test test/class_storage_test.cpp)
    target_link_libraries(simulation_script_class_storage_test
        PRIVATE lux::engine::simulation::simulation_scripting_core)
    if(MSVC)
        target_compile_options(simulation_script_class_storage_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(simulation_script_class_storage_test PRIVATE -UNDEBUG)
    endif()
    lux_classify_target(TARGET simulation_script_class_storage_test
        LAYER SIMULATION PRODUCT TEST ROLE FOUNDATION)
    add_test(NAME simulation_script_class_storage_test COMMAND simulation_script_class_storage_test)

    add_executable(simulation_script_class_storage_failure_test test/frame_storage_failure_test.cpp)
    target_link_libraries(simulation_script_class_storage_failure_test
        PRIVATE lux::engine::simulation::simulation_scripting_core)
    lux_classify_target(TARGET simulation_script_class_storage_failure_test
        LAYER SIMULATION PRODUCT TEST ROLE FOUNDATION)
    add_test(NAME simulation_script_class_storage_move_test COMMAND simulation_script_class_storage_failure_test)
    foreach(failure_point RANGE 1 4)
        add_test(NAME simulation_script_class_storage_failure_${failure_point}_test
            COMMAND simulation_script_class_storage_failure_test ${failure_point})
    endforeach()

endif()
