generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SIMULATION_SCRIPT_CPP_STATIC_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SIMULATION_SCRIPT_CPP_STATIC_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SIMULATION_SCRIPT_CPP_STATIC_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/simulation/scripting/cpp_static/visibility.h
)

add_component(
    COMPONENT_NAME simulation_script_cpp_static
    NAMESPACE      lux::engine::simulation
    OUTPUT_NAME    lux_engine_simulation_script_cpp_static
    SOURCE_FILES src/CppStaticScriptBridge.cpp
)
component_include_directories(
    simulation_script_cpp_static
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(
    simulation_script_cpp_static
    PRIVATE LUX_ENGINE_SIMULATION_SCRIPT_CPP_STATIC_LIBRARY
)
target_link_libraries(
    simulation_script_cpp_static
    PUBLIC
        lux::engine::simulation::simulation_script
)
component_add_transitive_commands(
    simulation_script_cpp_static
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script)"
)
lux_classify_target(
    TARGET  simulation_script_cpp_static
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)

component_add_cmake_scripts(simulation_script_cpp_static ${CMAKE_CURRENT_SOURCE_DIR}/cmake/lux_cpp_static_scripts.cmake)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/template/
    DESTINATION share/lux-engine-simulation/simulation_script_cpp_static/cmake_scripts/template)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/lux_cpp_static_scripts.cmake)
add_executable(simulation_script_cpp_prepared_shape_test test/prepared_shape_test.cpp)
target_link_libraries(simulation_script_cpp_prepared_shape_test
    PRIVATE lux::engine::simulation::simulation_script_cpp_static)
if(MSVC)
    target_compile_options(simulation_script_cpp_prepared_shape_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_script_cpp_prepared_shape_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(TARGET simulation_script_cpp_prepared_shape_test LAYER SIMULATION PRODUCT TEST ROLE INTEGRATION)
add_test(NAME simulation_script_cpp_prepared_shape_test COMMAND simulation_script_cpp_prepared_shape_test)

target_link_libraries(simulation_script_cpp_prepared_shape_test PRIVATE lux::engine::core::type_info)
foreach(shape Narrow Wide)
    lux_cpp_static_scripts(TARGET simulation_script_cpp_prepared_shape_test NAME Shape${shape}
        SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/test/${shape}ShapeFixture.hpp LOGICAL_PATH ${shape}ShapeFixture.hpp
        MODULE shape DESCRIPTOR shape SCOPE SIMULATION SYMBOL_LEDGER ${CMAKE_CURRENT_SOURCE_DIR}/test/shape.symbols.json)
endforeach()

add_executable(
    simulation_script_cpp_static_test
    test/cpp_static_script_bridge_test.cpp
)
include_component_cmake_scripts(script_core)
lux_script_abilities(
    TARGET simulation_script_cpp_static_test
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test/CppCoroutineAbility.hpp
    LOGICAL_PATHS CppCoroutineAbility.hpp
)
target_include_directories(
    simulation_script_cpp_static_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test
)
target_link_libraries(
    simulation_script_cpp_static_test
    PRIVATE lux::engine::simulation::simulation_script_cpp_static
)
target_link_libraries(simulation_script_cpp_static_test PRIVATE lux::engine::core::type_info)
lux_cpp_static_scripts(TARGET simulation_script_cpp_static_test NAME Bridge
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/test/CppStaticScriptBridgeFixture.hpp LOGICAL_PATH CppStaticScriptBridgeFixture.hpp
    MODULE lux.test.bridge-behavior DESCRIPTOR bridge-behavior-v1 OWNER lux::simulation::test::BridgeBehavior SCOPE ENTITY
    SYMBOL_LEDGER ${CMAKE_CURRENT_SOURCE_DIR}/test/bridge.symbols.json
    ABILITY_TYPES lux::simulation::script::DelayAbility
    ABILITY_HEADERS lux/engine/simulation/scripting/cpp_static/ScriptDelayCoroutine.hpp
    EVENT_SOURCES lux::simulation::test::BridgeEvent
    DEPENDS simulation_script_script_abilities_generate)
lux_cpp_static_scripts(TARGET simulation_script_cpp_static_test NAME BridgeFree
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/test/CppStaticScriptBridgeFixture.hpp LOGICAL_PATH CppStaticScriptBridgeFixture.hpp
    MODULE lux.test.bridge-free DESCRIPTOR bridge-free-v1 SCOPE SIMULATION
    SYMBOL_LEDGER ${CMAKE_CURRENT_SOURCE_DIR}/test/bridge.symbols.json
    DEPENDS simulation_script_script_abilities_generate)
lux_cpp_static_scripts(TARGET simulation_script_cpp_static_test NAME BridgeAbility
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/test/CppAbilityObjectFixture.hpp LOGICAL_PATH CppAbilityObjectFixture.hpp
    MODULE lux.test.cpp-coroutine-ability DESCRIPTOR cpp-coroutine-ability-v1
    OWNER lux::simulation::test::CoroutineAbilityObject SCOPE ENTITY
    SYMBOL_LEDGER ${CMAKE_CURRENT_SOURCE_DIR}/test/bridge.symbols.json
    ABILITY_TYPES lux::simulation::test::CppCoroutineAbility
    ABILITY_HEADERS CppCoroutineAbility.ability.generated.hpp
    DEPENDS simulation_script_cpp_static_test_script_abilities_generate)
if(MSVC)
    target_compile_options(
        simulation_script_cpp_static_test PRIVATE /UNDEBUG
    )
else()
    target_compile_options(
        simulation_script_cpp_static_test PRIVATE -UNDEBUG
    )
endif()
lux_classify_target(
    TARGET  simulation_script_cpp_static_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    INTEGRATION
)
add_test(
    NAME simulation_script_cpp_static_test
    COMMAND simulation_script_cpp_static_test
)

set(generated_consumer ${PROJECT_SOURCE_DIR}/cmake/installed-consumers/cpp-generated-script)
add_executable(simulation_script_cpp_generated_integration_test ${generated_consumer}/main.cpp)
target_include_directories(simulation_script_cpp_generated_integration_test PRIVATE ${generated_consumer})
target_link_libraries(simulation_script_cpp_generated_integration_test PRIVATE
    simulation_script_cpp_static lux::engine::core::type_info lux::engine::core::task
    lux::engine::simulation::simulation_composition)
lux_cpp_static_scripts(TARGET simulation_script_cpp_generated_integration_test NAME InstalledBehavior
    SOURCE ${generated_consumer}/Behavior.hpp LOGICAL_PATH Behavior.hpp
    MODULE installed.generated DESCRIPTOR installed-generated-v1 OWNER installed_generated::Behavior SCOPE ENTITY
    SYMBOL_LEDGER ${generated_consumer}/behavior.symbols.json
    ABILITY_TYPES lux::simulation::script::DelayAbility
    ABILITY_HEADERS lux/engine/simulation/scripting/cpp_static/ScriptDelayCoroutine.hpp
    DEPENDS simulation_script_script_abilities_generate)
lux_classify_target(TARGET simulation_script_cpp_generated_integration_test LAYER SIMULATION PRODUCT TEST ROLE INTEGRATION)
add_test(NAME simulation_script_cpp_generated_integration_test COMMAND simulation_script_cpp_generated_integration_test)
foreach(scenario IN ITEMS host-failure construct-failure prepare-failure)
    add_test(NAME simulation_script_cpp_generated_${scenario}
        COMMAND simulation_script_cpp_generated_integration_test ${scenario})
endforeach()

set(cpp_negative_patterns
    "const-reference must be trivial"
    "Unsupported CppStatic"
    "Unsupported CppStatic"
    "static_cast"
    "Invalid Script export"
    "Invalid Script export"
    "Invalid Script export"
    "EndPlay requires the semantic reason"
    "owned value"
    "CppStaticCoroutineEntry"
    "Unsupported CppStatic"
    "Invalid Script export"
    "duplicate Script export source identity"
    "Invalid Script attach" "Invalid Script attach" "Invalid Script attach"
    "static_cast" "static_cast" "Invalid Script attach" "Invalid Script attach" "static_cast")
foreach(case RANGE 1 21)
    set(target simulation_script_cpp_codegen_negative_${case})
    set(driver "${CMAKE_CURRENT_BINARY_DIR}/negative-${case}.cpp")
    file(GENERATE OUTPUT "${driver}" CONTENT "#include \"InvalidGeneratedScript.hpp\"\nint main() { return 0; }\n")
    add_executable(${target} EXCLUDE_FROM_ALL "${driver}")
    target_link_libraries(${target} PRIVATE simulation_script_cpp_static lux::engine::core::type_info)
    target_compile_definitions(${target} PRIVATE CPP_INVALID_CASE=${case})
    lux_cpp_static_scripts(TARGET ${target} NAME InvalidBehavior
        SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/test/InvalidGeneratedScript.hpp LOGICAL_PATH InvalidGeneratedScript.hpp
        MODULE negative DESCRIPTOR negative-v1 OWNER invalid_generated::Behavior SCOPE ENTITY
        SYMBOL_LEDGER ${CMAKE_CURRENT_SOURCE_DIR}/test/negative.symbols.json)
    lux_classify_target(TARGET ${target} LAYER SIMULATION PRODUCT TEST ROLE INTEGRATION)
    math(EXPR index "${case} - 1")
    list(GET cpp_negative_patterns ${index} pattern)
    add_test(NAME ${target} COMMAND ${CMAKE_COMMAND}
        -DBUILD_DIR=${CMAKE_BINARY_DIR} -DTARGET=${target} "-DEXPECTED_PATTERN=${pattern}"
        -P ${PROJECT_SOURCE_DIR}/cmake/ExpectTargetCompileFailure.cmake)
    set_tests_properties(${target} PROPERTIES RUN_SERIAL TRUE)
endforeach()
