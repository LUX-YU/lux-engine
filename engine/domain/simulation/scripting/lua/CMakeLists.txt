generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SIMULATION_SCRIPT_LUA_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SIMULATION_SCRIPT_LUA_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SIMULATION_SCRIPT_LUA_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/simulation/scripting/lua/visibility.h
)
add_component(
    COMPONENT_NAME simulation_script_lua
    NAMESPACE      lux::engine::simulation
    OUTPUT_NAME    lux_engine_simulation_script_lua
    SOURCE_FILES src/LuaScriptBackend.cpp
)
component_include_directories(
    simulation_script_lua
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(
    simulation_script_lua
    PRIVATE
        LUX_ENGINE_SIMULATION_SCRIPT_LUA_LIBRARY
)
target_link_libraries(
    simulation_script_lua
    PUBLIC
        lux::engine::simulation::simulation_script
        lux::engine::function::script_lua
    PRIVATE LuxLua55::Runtime
)
component_add_transitive_commands(
    simulation_script_lua
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_lua)"
)
lux_classify_target(
    TARGET  simulation_script_lua
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(
    simulation_script_lua_test
    test/lua_backend_test.cpp
)
target_link_libraries(
    simulation_script_lua_test
    PRIVATE
        lux::engine::simulation::simulation_script_lua
        LuxLua55::Runtime
)
if(MSVC)
    target_compile_options(simulation_script_lua_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_script_lua_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  simulation_script_lua_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(
    NAME simulation_script_lua_test
    COMMAND simulation_script_lua_test
)

add_executable(simulation_script_lua_closure_provenance_test test/lua_closure_provenance_test.cpp)
target_link_libraries(simulation_script_lua_closure_provenance_test
    PRIVATE lux::engine::simulation::simulation_script_lua LuxLua55::Runtime)
if(MSVC)
    target_compile_options(simulation_script_lua_closure_provenance_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_script_lua_closure_provenance_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(TARGET simulation_script_lua_closure_provenance_test
    LAYER SIMULATION PRODUCT TEST ROLE DOMAIN)
add_test(NAME simulation_script_lua_closure_provenance_test COMMAND simulation_script_lua_closure_provenance_test)


add_executable(
    simulation_script_lua_prepared_storage_test
    test/lua_prepared_storage_test.cpp
)
target_link_libraries(
    simulation_script_lua_prepared_storage_test
    PRIVATE lux::engine::simulation::simulation_script_lua
)
if(MSVC)
    target_compile_options(simulation_script_lua_prepared_storage_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_script_lua_prepared_storage_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  simulation_script_lua_prepared_storage_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(
    NAME simulation_script_lua_prepared_storage_test
    COMMAND simulation_script_lua_prepared_storage_test
)

add_executable(
    simulation_script_lua_vm_coroutine_contract_test
    test/lua_vm_coroutine_contract_test.cpp
)
target_link_libraries(
    simulation_script_lua_vm_coroutine_contract_test
    PRIVATE
        lux::engine::function::script_lua
        LuxLua55::Runtime
)
target_include_directories(
    simulation_script_lua_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test
)
include_component_cmake_scripts(script_core)
lux_script_abilities(
    TARGET simulation_script_lua_test
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test/LuaUnsupportedIntegerAbility.hpp
    LOGICAL_PATHS LuaUnsupportedIntegerAbility.hpp
)
if(MSVC)
    target_compile_options(simulation_script_lua_vm_coroutine_contract_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_script_lua_vm_coroutine_contract_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  simulation_script_lua_vm_coroutine_contract_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(
    NAME simulation_script_lua_vm_coroutine_contract_test
    COMMAND simulation_script_lua_vm_coroutine_contract_test
)


add_executable(
    simulation_script_lua_coroutine_integration_test
    test/lua_coroutine_integration_test.cpp
)
target_link_libraries(
    simulation_script_lua_coroutine_integration_test
    PRIVATE lux::engine::simulation::simulation_script_lua LuxLua55::Runtime
)
target_include_directories(
    simulation_script_lua_coroutine_integration_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test
)
lux_script_abilities(
    TARGET simulation_script_lua_coroutine_integration_test
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test/LuaRuntimeTestAbility.hpp
    LOGICAL_PATHS LuaRuntimeTestAbility.hpp
)
if(MSVC)
    target_compile_options(simulation_script_lua_coroutine_integration_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_script_lua_coroutine_integration_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  simulation_script_lua_coroutine_integration_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(
    NAME simulation_script_lua_coroutine_integration_test
    COMMAND simulation_script_lua_coroutine_integration_test
)

foreach(_creation_failure 0 1)
    add_test(NAME simulation_script_lua_creation_oom_${_creation_failure}
        COMMAND simulation_script_lua_coroutine_integration_test --creation-oom ${_creation_failure})
endforeach()
add_test(NAME simulation_script_lua_creation_retire_test
    COMMAND simulation_script_lua_coroutine_integration_test --creation-retire)
add_test(NAME simulation_script_lua_creation_stop_test
    COMMAND simulation_script_lua_coroutine_integration_test --creation-stop)
add_test(NAME simulation_script_lua_fixed_root_registry_test
    COMMAND simulation_script_lua_coroutine_integration_test --fixed-root-registry)

if(TARGET script_native_collision_fixture)
    add_executable(
        simulation_script_collision_event_test
        test/collision_event_chain_test.cpp
    )
    target_link_libraries(
        simulation_script_collision_event_test
        PRIVATE
            lux::engine::simulation::simulation_script_lua
            lux::engine::simulation::simulation_script_native
            LuxLua55::Runtime
            lux::engine::core::task
    )
    target_compile_definitions(
        simulation_script_collision_event_test
        PRIVATE
            LUX_SCRIPT_NATIVE_COLLISION_FIXTURE="$<TARGET_FILE:script_native_collision_fixture>"
    )
    add_dependencies(
        simulation_script_collision_event_test
        script_native_collision_fixture
    )
    if(MSVC)
        target_compile_options(
            simulation_script_collision_event_test
            PRIVATE /UNDEBUG
        )
    else()
        target_compile_options(
            simulation_script_collision_event_test
            PRIVATE -UNDEBUG
        )
    endif()
    lux_classify_target(
        TARGET  simulation_script_collision_event_test
        LAYER   SIMULATION
        PRODUCT TEST
        ROLE    DOMAIN
    )
    add_test(
        NAME simulation_script_collision_event_test
        COMMAND simulation_script_collision_event_test
    )
endif()

# SR-5 fixtures have one generated value owner shared by their real consumers.
include_component_cmake_scripts(script_lua)
add_library(simulation_script_lua_value_fixtures STATIC test/lua_value_fixtures.cpp)
target_link_libraries(simulation_script_lua_value_fixtures PUBLIC lux::engine::simulation::simulation_script_lua)
target_include_directories(simulation_script_lua_value_fixtures PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/test)
lux_script_lua_values(TARGET simulation_script_lua_value_fixtures
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test/CollisionValue.hpp ${CMAKE_CURRENT_SOURCE_DIR}/test/LuaValueTestTypes.hpp
    RULE_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/test/LuaValueTestRules.hpp)
lux_classify_target(TARGET simulation_script_lua_value_fixtures LAYER SIMULATION PRODUCT TEST ROLE DOMAIN)
target_link_libraries(simulation_script_lua_test PRIVATE simulation_script_lua_value_fixtures)
if(TARGET simulation_script_collision_event_test)
    target_link_libraries(simulation_script_collision_event_test PRIVATE simulation_script_lua_value_fixtures)
endif()
add_executable(simulation_script_lua_value_boundary_test
    ${PROJECT_SOURCE_DIR}/modules/function/script/lua/test/lua_value_test.cpp)
target_link_libraries(simulation_script_lua_value_boundary_test PRIVATE lux::engine::function::script_lua
    LuxLua55::Runtime)
if(MSVC)
    target_compile_options(simulation_script_lua_value_boundary_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_script_lua_value_boundary_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(TARGET simulation_script_lua_value_boundary_test LAYER SIMULATION PRODUCT TEST ROLE DOMAIN)
add_test(NAME simulation_script_lua_value_boundary_test COMMAND simulation_script_lua_value_boundary_test)

add_executable(simulation_script_lua_value_runtime_test test/lua_value_runtime_test.cpp)
target_link_libraries(simulation_script_lua_value_runtime_test PRIVATE simulation_script_lua_value_fixtures)
lux_script_abilities(TARGET simulation_script_lua_value_runtime_test
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test/LuaValueTestAbility.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/test/LuaPushOnlyAbility.hpp)
if(MSVC)
    target_compile_options(simulation_script_lua_value_runtime_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_script_lua_value_runtime_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(TARGET simulation_script_lua_value_runtime_test LAYER SIMULATION PRODUCT TEST ROLE DOMAIN)
add_test(NAME simulation_script_lua_value_runtime_test COMMAND simulation_script_lua_value_runtime_test)
add_test(NAME simulation_script_lua_resume_retire_test COMMAND simulation_script_lua_value_runtime_test --resume-retire)
add_test(NAME simulation_script_lua_resume_stop_test COMMAND simulation_script_lua_value_runtime_test --resume-stop)
