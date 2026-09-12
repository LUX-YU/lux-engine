cmake_minimum_required(VERSION 3.22)
project(native_lua_complete_task_comparison LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT NA1_SOURCE_DIR)
    message(FATAL_ERROR "NA1_SOURCE_DIR must identify the tracked common fixture source")
endif()
set(NA1_BENCHMARK_DIR "${NA1_SOURCE_DIR}/engine/domain/simulation/scripting/native_lua_tasks/benchmark")
option(NA1_CANDIDATE "Compile the native orchestration side against its own installed SDK" OFF)
find_package(lux-cmake-toolset CONFIG REQUIRED)
find_package(lux-engine-function REQUIRED COMPONENTS script_core script_lua script_artifact)
find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script_lua simulation_scripting_core)
find_package(lux-engine-simulation-composition REQUIRED COMPONENTS simulation_composition)
find_package(lux-engine-toolchain REQUIRED COMPONENTS lua_script_packager)
include_component_cmake_scripts(lux::engine::function::script_core)
include_component_cmake_scripts(lux::engine::function::script_lua)
include_component_cmake_scripts(lux::engine::toolchain::lua_script_packager)
add_executable(native_lua_complete_tasks ${NA1_BENCHMARK_DIR}/main.cpp HostResources.cpp)
target_include_directories(native_lua_complete_tasks PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(native_lua_complete_tasks PRIVATE cxx_std_20)
target_compile_definitions(native_lua_complete_tasks PRIVATE NA1_CANDIDATE=$<BOOL:${NA1_CANDIDATE}>)
target_compile_options(native_lua_complete_tasks PRIVATE /UNDEBUG)
target_link_libraries(native_lua_complete_tasks PRIVATE lux::engine::simulation::simulation_script_lua
    lux::engine::simulation::simulation_composition)
file(REAL_PATH "${NA1_BENCHMARK_DIR}/../../../builtin/script/benchmark" old_benchmark)
file(REAL_PATH "${NA1_BENCHMARK_DIR}/../../lua/test" old_values)
target_include_directories(native_lua_complete_tasks PRIVATE ${NA1_BENCHMARK_DIR} ${old_benchmark} ${old_values})
lux_script_lua_values(TARGET native_lua_complete_tasks SOURCES ${old_values}/LuaValueTestTypes.hpp
    RULE_HEADERS ${old_values}/LuaValueTestRules.hpp)
add_library(cost_ability_contract STATIC ${NA1_BENCHMARK_DIR}/contracts.cpp)
target_compile_features(cost_ability_contract PUBLIC cxx_std_20)
target_include_directories(cost_ability_contract PUBLIC ${old_benchmark})
target_link_libraries(cost_ability_contract PUBLIC lux::engine::simulation::simulation_scripting_core)
find_path(delay_include NAMES lux/engine/simulation/abilities/DelayAbility.hpp REQUIRED)
lux_script_abilities(TARGET cost_ability_contract SOURCES ${old_benchmark}/ScriptBenchmarkAbility.hpp
    ${delay_include}/lux/engine/simulation/abilities/DelayAbility.hpp
    LOGICAL_PATHS ScriptBenchmarkAbility.hpp lux/engine/simulation/abilities/DelayAbility.hpp)
target_link_libraries(native_lua_complete_tasks PRIVATE cost_ability_contract)
add_executable(cost_event_schema_writer ${NA1_BENCHMARK_DIR}/EventSchema.cpp)
target_compile_features(cost_event_schema_writer PRIVATE cxx_std_20)
target_compile_options(cost_event_schema_writer PRIVATE /UNDEBUG)
target_link_libraries(cost_event_schema_writer PRIVATE lux::engine::simulation::simulation_script)
set(event_schema "${CMAKE_CURRENT_BINARY_DIR}/events.json")
add_custom_command(OUTPUT "${event_schema}" COMMAND cost_event_schema_writer "${event_schema}"
    DEPENDS cost_event_schema_writer VERBATIM)
add_custom_target(cost_event_schema DEPENDS "${event_schema}")
set(artifact "${CMAKE_CURRENT_BINARY_DIR}/business.lxsa")
lux_package_lua_script(NAME cost_business SOURCE ${NA1_BENCHMARK_DIR}/business.lua OUTPUT ${artifact}
    MODULE lux.benchmark.lua-runtime ENTRY BenchmarkBehavior SCOPE ENTITY
    SYMBOL_LEDGER ${NA1_BENCHMARK_DIR}/business.symbols.json
    SEMANTIC_TYPES lux::simulation::script::EScriptEndPlayReason lux::simulation::script::test::ValuePose
    SEMANTIC_HEADERS lux/engine/simulation/scripting/ScriptLifecycle.hpp LuaValueTestTypes.hpp
    SEMANTIC_INCLUDE_DIRECTORIES ${old_values}
    SEMANTIC_TARGETS lux::engine::simulation::simulation_scripting_core
    ABILITY_TARGETS cost_ability_contract EVENT_SCHEMAS ${event_schema} EVENT_SCHEMA_TARGETS cost_event_schema)
add_dependencies(native_lua_complete_tasks cost_business)
target_compile_definitions(native_lua_complete_tasks PRIVATE NA1_BUSINESS_ARTIFACT="${artifact}")
if(NA1_CANDIDATE)
    find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script_native_lua_tasks)
    target_link_libraries(native_lua_complete_tasks PRIVATE lux::engine::simulation::simulation_script_native_lua_tasks)
    include_component_cmake_scripts(lux::engine::simulation::simulation_script_cpp_static)
    lux_cpp_static_scripts(TARGET native_lua_complete_tasks NAME CompleteTasks
        SOURCE ${NA1_BENCHMARK_DIR}/Tasks.hpp LOGICAL_PATH Tasks.hpp
        MODULE lux.na1.complete-tasks DESCRIPTOR na1-complete-tasks-v1
        OWNER lux::simulation::na1::cost::Tasks SCOPE ENTITY
        SYMBOL_LEDGER ${NA1_BENCHMARK_DIR}/tasks.symbols.json
        ABILITY_TYPES lux::simulation::script::DelayAbility
        ABILITY_HEADERS lux/engine/simulation/scripting/cpp_static/ScriptDelayCoroutine.hpp
        EVENT_SCHEMA "${event_schema}" EVENT_NAMES Benchmark.event DEPENDS cost_event_schema)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NA1_ITT_SDK)
    target_include_directories(native_lua_complete_tasks PRIVATE "${NA1_ITT_SDK}/include")
    target_link_libraries(native_lua_complete_tasks PRIVATE "${NA1_ITT_SDK}/lib64/libittnotify.lib")
    target_compile_definitions(native_lua_complete_tasks PRIVATE NA1_HAS_ITT=1)
else()
    target_compile_definitions(native_lua_complete_tasks PRIVATE NA1_HAS_ITT=0)
endif()
target_link_libraries(native_lua_complete_tasks PRIVATE psapi)
