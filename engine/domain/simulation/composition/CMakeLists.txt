generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SIMULATION_COMPOSITION_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SIMULATION_COMPOSITION_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SIMULATION_COMPOSITION_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/simulation/composition/visibility.h
)

add_component(
    COMPONENT_NAME simulation_composition
    NAMESPACE      lux::engine::simulation
    OUTPUT_NAME    lux_engine_simulation_composition
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Simulation.cpp
)
add_library(lux::engine::simulation::composition ALIAS simulation_composition)

component_include_directories(
    simulation_composition
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(simulation_composition PRIVATE LUX_ENGINE_SIMULATION_COMPOSITION_LIBRARY)
target_link_libraries(
    simulation_composition
    PUBLIC
        lux::engine::simulation::description
        lux::engine::simulation::system
        lux::engine::simulation::ecs::core
        lux::engine::simulation::ecs::task
        lux::engine::core::task
        lux::engine::simulation::simulation_scripting_core
)
component_add_transitive_commands(
    simulation_composition
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_description)"
    "find_package(lux-engine-simulation-system REQUIRED COMPONENTS system)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS core ecs_task)"
    "find_package(lux-engine-core REQUIRED COMPONENTS task)"
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_scripting_core)"
    "add_library(lux::engine::simulation::composition INTERFACE IMPORTED GLOBAL)"
    "set_property(TARGET lux::engine::simulation::composition PROPERTY INTERFACE_LINK_LIBRARIES lux::engine::simulation::simulation_composition)"
)

lux_classify_target(
    TARGET  simulation_composition
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    COMPOSITION
)

add_executable(simulation_composition_test test/simulation_composition_test.cpp)
target_link_libraries(
    simulation_composition_test
    PRIVATE lux::engine::simulation::composition
)
if(MSVC)
    target_compile_options(simulation_composition_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_composition_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  simulation_composition_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    COMPOSITION
)
add_test(NAME simulation_composition_test COMMAND simulation_composition_test)

include_component_cmake_scripts(script_core)
add_executable(
    simulation_script_ability_codegen_test
    test/script_ability_codegen_test.cpp
)
target_include_directories(
    simulation_script_ability_codegen_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/test
        ${PROJECT_SOURCE_DIR}/engine/domain/simulation/scripting/core/test
)
target_link_libraries(
    simulation_script_ability_codegen_test
    PRIVATE
        lux::engine::simulation::composition
        lux::engine::function::script_core
)
lux_script_abilities(
    TARGET simulation_script_ability_codegen_test
    SOURCES ${PROJECT_SOURCE_DIR}/engine/domain/simulation/scripting/core/test/TestAbility.hpp
    LOGICAL_PATHS TestAbility.hpp
)
if(MSVC)
    target_compile_options(simulation_script_ability_codegen_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_script_ability_codegen_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  simulation_script_ability_codegen_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    COMPOSITION
)
add_test(
    NAME simulation_script_ability_codegen_test
    COMMAND simulation_script_ability_codegen_test
)

add_executable(
    simulation_script_ability_borrowed_async_negative
    EXCLUDE_FROM_ALL
    test/script_ability_invalid_codegen.cpp
)
target_include_directories(
    simulation_script_ability_borrowed_async_negative
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/test
        ${PROJECT_SOURCE_DIR}/engine/domain/simulation/scripting/core/test
)
target_link_libraries(
    simulation_script_ability_borrowed_async_negative
    PRIVATE lux::engine::function::script_core
)
lux_script_abilities(
    TARGET simulation_script_ability_borrowed_async_negative
    SOURCES ${PROJECT_SOURCE_DIR}/engine/domain/simulation/scripting/core/test/InvalidBorrowedAsyncAbility.hpp
    LOGICAL_PATHS InvalidBorrowedAsyncAbility.hpp
)
lux_classify_target(
    TARGET  simulation_script_ability_borrowed_async_negative
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    COMPOSITION
)
add_test(
    NAME simulation_script_ability_borrowed_async_negative
    COMMAND ${CMAKE_COMMAND}
        -DBUILD_DIR=${CMAKE_BINARY_DIR}
        -DTARGET=simulation_script_ability_borrowed_async_negative
        -DEXPECTED_PATTERN=BORROWED_STEP
        -P ${PROJECT_SOURCE_DIR}/cmake/ExpectTargetCompileFailure.cmake
)

add_executable(
    simulation_script_ability_duplicate_method_negative
    EXCLUDE_FROM_ALL
    test/script_ability_duplicate_method_codegen.cpp
)
target_include_directories(
    simulation_script_ability_duplicate_method_negative
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/test
        ${PROJECT_SOURCE_DIR}/engine/domain/simulation/scripting/core/test
)
target_link_libraries(
    simulation_script_ability_duplicate_method_negative
    PRIVATE lux::engine::function::script_core
)
lux_script_abilities(
    TARGET simulation_script_ability_duplicate_method_negative
    SOURCES ${PROJECT_SOURCE_DIR}/engine/domain/simulation/scripting/core/test/DuplicateMethodIdAbility.hpp
    LOGICAL_PATHS DuplicateMethodIdAbility.hpp
)
lux_classify_target(
    TARGET  simulation_script_ability_duplicate_method_negative
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    COMPOSITION
)
add_test(
    NAME simulation_script_ability_duplicate_method_negative
    COMMAND ${CMAKE_COMMAND}
        -DBUILD_DIR=${CMAKE_BINARY_DIR}
        -DTARGET=simulation_script_ability_duplicate_method_negative
        -DEXPECTED_PATTERN=duplicate.Script.Ability.MethodId
        -P ${PROJECT_SOURCE_DIR}/cmake/ExpectTargetCompileFailure.cmake
)

if(TARGET script_lua AND TARGET flowforge)
    include_component_cmake_scripts(script_lua)
    add_executable(
        simulation_script_ability_projection_test
        test/script_ability_projection_test.cpp
    )
    target_include_directories(
        simulation_script_ability_projection_test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/test
            ${PROJECT_SOURCE_DIR}/engine/domain/simulation/scripting/core/test
    )
    target_link_libraries(
        simulation_script_ability_projection_test
        PRIVATE
            lux::engine::simulation::composition
            lux::engine::function::script_core
            lux::engine::function::script_lua
            lux::engine::function::flowforge
    )
    lux_script_abilities(
        TARGET simulation_script_ability_projection_test
        SOURCES ${PROJECT_SOURCE_DIR}/engine/domain/simulation/scripting/core/test/TestAbility.hpp
        LOGICAL_PATHS TestAbility.hpp
    )
    lux_script_ability_lua_projection(
        TARGET simulation_script_ability_projection_test
        SOURCES ${PROJECT_SOURCE_DIR}/engine/domain/simulation/scripting/core/test/TestAbility.hpp
        LOGICAL_PATHS TestAbility.hpp
    )
    if(MSVC)
        target_compile_options(simulation_script_ability_projection_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(simulation_script_ability_projection_test PRIVATE -UNDEBUG)
    endif()
    lux_classify_target(
        TARGET  simulation_script_ability_projection_test
        LAYER   SIMULATION
        PRODUCT TEST
        ROLE    COMPOSITION
    )
    add_test(
        NAME simulation_script_ability_projection_test
        COMMAND simulation_script_ability_projection_test
    )
    add_test(
        NAME simulation_script_ability_codegen_deterministic
        COMMAND ${CMAKE_COMMAND} -E compare_files
            ${CMAKE_CURRENT_BINARY_DIR}/generated/simulation_script_ability_codegen_test/script_abilities/TestAbility.ability.generated.hpp
            ${CMAKE_CURRENT_BINARY_DIR}/generated/simulation_script_ability_projection_test/script_abilities/TestAbility.ability.generated.hpp
    )
endif()

install_components(
    PROJECT_NAME lux-engine-simulation-composition
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::simulation
    COMPONENTS   simulation_composition
)
