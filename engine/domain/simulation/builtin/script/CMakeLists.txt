generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SIMULATION_SCRIPT_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SIMULATION_SCRIPT_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SIMULATION_SCRIPT_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/simulation/script_system/visibility.h
)

add_component(
    COMPONENT_NAME simulation_script
    NAMESPACE      lux::engine::simulation
    OUTPUT_NAME    lux_engine_simulation_script
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptSystemDescription.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptBindingAuthoring.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptSystemDescriptionCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptSystem.cpp
)
component_include_directories(
    simulation_script
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_include_directories(
    simulation_script
    PUBLIC $<INSTALL_INTERFACE:include/lux/engine/simulation/abilities>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)
target_compile_definitions(
    simulation_script
    PRIVATE
        LUX_ENGINE_SIMULATION_SCRIPT_LIBRARY
        $<$<CONFIG:Debug>:LUX_SCRIPT_OWNER_AFFINITY_PROBE=1>
)
target_link_libraries(
    simulation_script
    PUBLIC
        lux::engine::simulation::description
        lux::engine::simulation::system
        lux::engine::simulation::simulation_scripting_core
        lux::engine::simulation::ecs::core
        lux::engine::function::script_core
        lux::engine::world::identity
        lux::engine::resource::identity
        lux::engine::function::script_artifact
    PRIVATE
        lux::cxx::container
)

find_package(lux-cxx REQUIRED COMPONENTS container)

add_executable(simulation_script_ingress_frontier_test test/script_ingress_frontier_test.cpp)
target_include_directories(simulation_script_ingress_frontier_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude)
target_link_libraries(simulation_script_ingress_frontier_test PRIVATE lux::engine::simulation::simulation_scripting_core)
if(MSVC)
    target_compile_options(simulation_script_ingress_frontier_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_script_ingress_frontier_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(TARGET simulation_script_ingress_frontier_test LAYER SIMULATION PRODUCT TEST ROLE DOMAIN)
add_test(NAME simulation_script_ingress_frontier_test COMMAND simulation_script_ingress_frontier_test)
set_tests_properties(simulation_script_ingress_frontier_test PROPERTIES TIMEOUT 30)
component_add_transitive_commands(
    simulation_script
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_description)"
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_scripting_core)"
    "find_package(lux-engine-simulation-system REQUIRED COMPONENTS system)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS core)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_core)"
    "find_package(lux-engine-world-identity REQUIRED COMPONENTS world_identity)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS identity)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_artifact)"
)
lux_classify_target(
    TARGET  simulation_script
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

include_component_cmake_scripts(script_core)
lux_script_abilities(
    TARGET simulation_script
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/simulation/abilities/DelayAbility.hpp
    LOGICAL_PATHS lux/engine/simulation/abilities/DelayAbility.hpp
)
get_target_property(_delay_ability_generated_dir simulation_script LUX_SCRIPT_ABILITY_GENERATED_DIR)
install(
    FILES ${_delay_ability_generated_dir}/DelayAbility.ability.generated.hpp
    DESTINATION include/lux/engine/simulation/abilities
    COMPONENT lux_sdk
)

add_executable(
    simulation_script_description_test
    test/script_system_description_test.cpp
)
target_link_libraries(
    simulation_script_description_test
    PRIVATE lux::engine::simulation::simulation_script
)
if(MSVC)
    target_compile_options(simulation_script_description_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_script_description_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  simulation_script_description_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(
    NAME simulation_script_description_test
    COMMAND simulation_script_description_test
)

add_executable(
    simulation_script_runtime_test
    test/script_system_runtime_test.cpp
)
target_link_libraries(
    simulation_script_runtime_test
    PRIVATE lux::engine::simulation::simulation_script
)
target_compile_definitions(
    simulation_script_runtime_test
    PRIVATE $<$<CONFIG:Debug>:LUX_SCRIPT_OWNER_AFFINITY_PROBE=1>
)
if(MSVC)
    target_compile_options(simulation_script_runtime_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_script_runtime_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  simulation_script_runtime_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(
    NAME simulation_script_runtime_test
    COMMAND simulation_script_runtime_test
)

add_executable(
    simulation_script_continuation_test
    test/script_system_continuation_test.cpp
)
target_include_directories(
    simulation_script_continuation_test
    PRIVATE ${PROJECT_SOURCE_DIR}/engine/domain/simulation/scripting/core/test
)
target_link_libraries(
    simulation_script_continuation_test
    PRIVATE lux::engine::simulation::simulation_script
)
lux_script_abilities(
    TARGET simulation_script_continuation_test
    SOURCES ${PROJECT_SOURCE_DIR}/engine/domain/simulation/scripting/core/test/TestAbility.hpp
    LOGICAL_PATHS TestAbility.hpp
)
if(MSVC)
    target_compile_options(simulation_script_continuation_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_script_continuation_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  simulation_script_continuation_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(
    NAME simulation_script_continuation_test
    COMMAND simulation_script_continuation_test
)

add_executable(
    simulation_script_event_wait_test
    test/script_system_event_wait_test.cpp
)
target_link_libraries(
    simulation_script_event_wait_test
    PRIVATE lux::engine::simulation::simulation_script
)
if(MSVC)
    target_compile_options(simulation_script_event_wait_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_script_event_wait_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  simulation_script_event_wait_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(
    NAME simulation_script_event_wait_test
    COMMAND simulation_script_event_wait_test
)
add_test(NAME simulation_script_event_wait_race_test COMMAND simulation_script_event_wait_test --race-only)

add_executable(
    simulation_script_lifecycle_test
    test/script_system_lifecycle_test.cpp
)
target_link_libraries(
    simulation_script_lifecycle_test
    PRIVATE lux::engine::simulation::simulation_script
)
if(MSVC)
    target_compile_options(simulation_script_lifecycle_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_script_lifecycle_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  simulation_script_lifecycle_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(
    NAME simulation_script_lifecycle_test
    COMMAND simulation_script_lifecycle_test
)

add_library(
    script_benchmark_ability_contract STATIC
    benchmark/ScriptBenchmarkAbility.cpp
)
target_include_directories(
    script_benchmark_ability_contract
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/benchmark
)
target_link_libraries(
    script_benchmark_ability_contract
    PUBLIC lux::engine::function::script_core
)
lux_script_abilities(
    TARGET script_benchmark_ability_contract
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/ScriptBenchmarkAbility.hpp
    LOGICAL_PATHS ScriptBenchmarkAbility.hpp
)
lux_classify_target(
    TARGET  script_benchmark_ability_contract
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_library(
    script_lua_portability_ability_contract
    STATIC benchmark/LuaPortabilityTestAbility.cpp
)
target_include_directories(
    script_lua_portability_ability_contract
    PUBLIC ${PROJECT_SOURCE_DIR}/engine/domain/simulation/scripting/lua/test
)
target_link_libraries(
    script_lua_portability_ability_contract
    PUBLIC lux::engine::function::script_core
)
lux_script_abilities(
    TARGET script_lua_portability_ability_contract
    SOURCES ${PROJECT_SOURCE_DIR}/engine/domain/simulation/scripting/lua/test/LuaRuntimeTestAbility.hpp
    LOGICAL_PATHS LuaRuntimeTestAbility.hpp
)
lux_classify_target(
    TARGET  script_lua_portability_ability_contract
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(
    script_runtime_benchmark
    benchmark/script_runtime_benchmark.cpp
)
execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    OUTPUT_VARIABLE LUX_SCRIPT_RUNTIME_BENCHMARK_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(LUX_SCRIPT_RUNTIME_BENCHMARK_GIT_COMMIT STREQUAL "")
    set(LUX_SCRIPT_RUNTIME_BENCHMARK_GIT_COMMIT "unknown")
endif()
target_include_directories(
    script_runtime_benchmark
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/benchmark
)
target_compile_definitions(
    script_runtime_benchmark
    PRIVATE
        LUX_BENCHMARK_GIT_COMMIT="${LUX_SCRIPT_RUNTIME_BENCHMARK_GIT_COMMIT}"
        LUX_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
        LUX_BENCHMARK_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}"
)
target_link_libraries(
    script_runtime_benchmark
    PRIVATE
        lux::engine::simulation::simulation_script
        lux::engine::simulation::composition
        lux::engine::simulation::simulation_script_cpp_static
        script_benchmark_ability_contract
)
if(TARGET simulation_script_lua)
    target_compile_definitions(script_runtime_benchmark PRIVATE LUX_BENCHMARK_HAS_LUA=1)
    target_link_libraries(
        script_runtime_benchmark
        PRIVATE lux::engine::simulation::simulation_script_lua
    )
else()
    target_compile_definitions(script_runtime_benchmark PRIVATE LUX_BENCHMARK_HAS_LUA=0)
endif()
if(TARGET simulation_script_native AND TARGET script_native_fixture)
    target_compile_definitions(
        script_runtime_benchmark
        PRIVATE
            LUX_BENCHMARK_HAS_NATIVE=1
            LUX_BENCHMARK_NATIVE_FIXTURE="$<TARGET_FILE:script_native_fixture>"
    )
    target_link_libraries(
        script_runtime_benchmark
        PRIVATE lux::engine::simulation::simulation_script_native
    )
    add_dependencies(script_runtime_benchmark script_native_fixture)
else()
    target_compile_definitions(script_runtime_benchmark PRIVATE LUX_BENCHMARK_HAS_NATIVE=0)
endif()
lux_classify_target(
    TARGET  script_runtime_benchmark
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(
    NAME script_runtime_benchmark_smoke
    COMMAND
        ${CMAKE_COMMAND}
        -DEXECUTABLE=$<TARGET_FILE:script_runtime_benchmark>
        -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/benchmark/smoke
        -P ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/run_smoke.cmake
)

target_link_libraries(script_runtime_benchmark PRIVATE lux::engine::core::type_info)
lux_cpp_static_scripts(TARGET script_runtime_benchmark NAME CppLifecycle
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/CppBenchmarkScripts.hpp LOGICAL_PATH CppBenchmarkScripts.hpp
    MODULE lux.benchmark.cpp-lifecycle DESCRIPTOR cpp-lifecycle-v1
    OWNER lux::simulation::benchmark::CppLifecycleObject SCOPE ENTITY
    SYMBOL_LEDGER ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/cpp.symbols.json)
lux_cpp_static_scripts(TARGET script_runtime_benchmark NAME CppCoroutineBenchmark
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/CppBenchmarkScripts.hpp LOGICAL_PATH CppBenchmarkScripts.hpp
    MODULE lux.benchmark.cpp-coroutine DESCRIPTOR cpp-coroutine-benchmark-v1
    OWNER lux::simulation::benchmark::CppCoroutineBenchmarkObject SCOPE ENTITY
    SYMBOL_LEDGER ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/cpp.symbols.json)

add_executable(script_binding_authoring_test test/script_binding_authoring_test.cpp)
lux_cpp_static_scripts(TARGET script_runtime_benchmark NAME RegionScript
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/CppBenchmarkScripts.hpp LOGICAL_PATH CppBenchmarkScripts.hpp
    MODULE lux.benchmark.regions DESCRIPTOR regions OWNER lux::simulation::benchmark::RegionScript SCOPE SIMULATION
    SYMBOL_LEDGER ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/cpp.symbols.json)
lux_cpp_static_scripts(TARGET script_runtime_benchmark NAME CppUpdate
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/CppBenchmarkScripts.hpp LOGICAL_PATH CppBenchmarkScripts.hpp
    MODULE lux.benchmark.cpp-update DESCRIPTOR cpp-update-v1 OWNER lux::simulation::benchmark::CppUpdateObject SCOPE ENTITY
    SYMBOL_LEDGER ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/cpp.symbols.json)

add_executable(script_sequence_schema_writer benchmark/SequenceEventSchemaWriter.cpp)
target_link_libraries(script_sequence_schema_writer PRIVATE simulation_script)
lux_classify_target(TARGET script_sequence_schema_writer LAYER SIMULATION PRODUCT BUILD_TOOL ROLE DOMAIN)
set(sequence_schema "${CMAKE_CURRENT_BINARY_DIR}/benchmark/sequence.events.json")
add_custom_command(OUTPUT "${sequence_schema}" COMMAND script_sequence_schema_writer "${sequence_schema}"
    DEPENDS script_sequence_schema_writer VERBATIM)
add_custom_target(script_sequence_schema DEPENDS "${sequence_schema}")
lux_classify_target(TARGET script_sequence_schema LAYER SIMULATION PRODUCT BUILD_TOOL ROLE DOMAIN)
lux_cpp_static_scripts(TARGET script_runtime_benchmark NAME CppSequence
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/CppSequenceScript.hpp LOGICAL_PATH CppSequenceScript.hpp
    MODULE lux.benchmark.cpp-sequence DESCRIPTOR cpp-sequence-v1
    OWNER lux::simulation::benchmark::CppSequenceScript SCOPE ENTITY
    SYMBOL_LEDGER ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/cpp.symbols.json
    ABILITY_TYPES lux::simulation::script::DelayAbility lux::simulation::script::benchmark::ValueAbility
    ABILITY_HEADERS lux/engine/simulation/scripting/cpp_static/ScriptDelayCoroutine.hpp
        ScriptBenchmarkAbility.ability.generated.hpp
    EVENT_SCHEMA "${sequence_schema}" EVENT_NAMES Benchmark.event DEPENDS script_sequence_schema)
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(script_binding_authoring_test PRIVATE simulation_script
    lux::engine::simulation::simulation_script_cpp_static lux::engine::core::type_info nlohmann_json::nlohmann_json)
set(authoring_hints "${CMAKE_CURRENT_BINARY_DIR}/generated/script_binding_authoring_test/cpp_scripts/AuthoringBehavior/Behavior.AuthoringBehavior.bindings.json")
target_compile_definitions(script_binding_authoring_test PRIVATE LUX_AUTHORING_HINT_FILE="${authoring_hints}")
if(MSVC)
    target_compile_options(script_binding_authoring_test PRIVATE /UNDEBUG)
else()
    target_compile_options(script_binding_authoring_test PRIVATE -UNDEBUG)
endif()
lux_cpp_static_scripts(TARGET script_binding_authoring_test NAME AuthoringBehavior
    SOURCE ${PROJECT_SOURCE_DIR}/cmake/installed-consumers/cpp-generated-script/Behavior.hpp LOGICAL_PATH Behavior.hpp
    MODULE authoring DESCRIPTOR authoring OWNER installed_generated::Behavior SCOPE ENTITY
    SYMBOL_LEDGER ${PROJECT_SOURCE_DIR}/cmake/installed-consumers/cpp-generated-script/behavior.symbols.json
    ABILITY_TYPES lux::simulation::script::DelayAbility
    ABILITY_HEADERS lux/engine/simulation/scripting/cpp_static/ScriptDelayCoroutine.hpp
    DEPENDS simulation_script_script_abilities_generate)
lux_classify_target(TARGET script_binding_authoring_test LAYER SIMULATION PRODUCT TEST ROLE DOMAIN)
add_test(NAME script_binding_authoring_test COMMAND script_binding_authoring_test)
