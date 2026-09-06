generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SIMULATION_SCRIPT_NATIVE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SIMULATION_SCRIPT_NATIVE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SIMULATION_SCRIPT_NATIVE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/simulation/scripting/native/visibility.h
)
add_component(
    COMPONENT_NAME simulation_script_native
    NAMESPACE      lux::engine::simulation
    OUTPUT_NAME    lux_engine_simulation_script_native
    SOURCE_FILES src/NativeScriptBackend.cpp
)
component_include_directories(
    simulation_script_native
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(
    simulation_script_native
    PRIVATE LUX_ENGINE_SIMULATION_SCRIPT_NATIVE_LIBRARY
)
target_link_libraries(
    simulation_script_native
    PUBLIC
        lux::engine::simulation::simulation_script
        lux::engine::function::script_native
)
component_add_transitive_commands(
    simulation_script_native
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_native)"
)
lux_classify_target(
    TARGET  simulation_script_native
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

if(TARGET script_native_fixture)
    foreach(variant IN ITEMS aligned compact wide narrow)
        add_library(native_population_${variant} SHARED test/population_fixture.cpp)
        target_link_libraries(native_population_${variant} PRIVATE lux::engine::function::script_core)
        set(alignment 16)
        set(frame_bytes 0)
        if(variant STREQUAL "compact")
            set(alignment 8)
        elseif(variant STREQUAL "wide")
            set(frame_bytes 1024)
        elseif(variant STREQUAL "narrow")
            set(frame_bytes 128)
        endif()
        target_compile_definitions(native_population_${variant} PRIVATE
            LUX_POPULATION_ALIGN=${alignment} LUX_POPULATION_FRAME=${frame_bytes})
        lux_classify_target(TARGET native_population_${variant} LAYER SIMULATION PRODUCT TEST ROLE DOMAIN)
    endforeach()
    add_executable(simulation_script_native_population_test test/native_population_test.cpp)
    target_link_libraries(simulation_script_native_population_test PRIVATE simulation_script_native)
    add_dependencies(simulation_script_native_population_test native_population_aligned native_population_compact
        native_population_wide native_population_narrow)
    lux_classify_target(TARGET simulation_script_native_population_test LAYER SIMULATION PRODUCT TEST ROLE DOMAIN)
    if(MSVC)
        target_compile_options(simulation_script_native_population_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(simulation_script_native_population_test PRIVATE -UNDEBUG)
    endif()
    foreach(scenario IN ITEMS state frames sync shared)
        add_test(NAME simulation_script_native_population_${scenario}
            COMMAND simulation_script_native_population_test ${scenario}
                $<TARGET_FILE:native_population_aligned> $<TARGET_FILE:native_population_compact>
                $<TARGET_FILE:native_population_wide> $<TARGET_FILE:native_population_narrow>)
    endforeach()
    add_executable(
        simulation_script_native_test
        test/native_backend_test.cpp
    )
    target_link_libraries(
        simulation_script_native_test
        PRIVATE lux::engine::simulation::simulation_script_native
    )
    target_compile_definitions(
        simulation_script_native_test
        PRIVATE
            LUX_SCRIPT_NATIVE_FIXTURE="$<TARGET_FILE:script_native_fixture>"
            LUX_SCRIPT_NATIVE_SECOND_FIXTURE="$<TARGET_FILE:script_native_second_fixture>"
            LUX_SCRIPT_NATIVE_STEP_FIXTURE="$<TARGET_FILE:script_native_step_fixture>"
    )
    add_dependencies(
        simulation_script_native_test
        script_native_fixture
        script_native_second_fixture
        script_native_step_fixture
    )
    if(MSVC)
        target_compile_options(simulation_script_native_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(simulation_script_native_test PRIVATE -UNDEBUG)
    endif()
    lux_classify_target(
        TARGET  simulation_script_native_test
        LAYER   SIMULATION
        PRODUCT TEST
        ROLE    DOMAIN
    )
    add_test(
        NAME simulation_script_native_test
        COMMAND simulation_script_native_test
    )
endif()
