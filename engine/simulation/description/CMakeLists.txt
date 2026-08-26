find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SIMULATION_DESCRIPTION_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SIMULATION_DESCRIPTION_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SIMULATION_DESCRIPTION_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/simulation/description/visibility.h
)

add_component(
    COMPONENT_NAME simulation_description
    NAMESPACE      lux::engine::simulation
    OUTPUT_NAME    lux_engine_simulation_description
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SimulationDataSchemaId.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SimulationDescription.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SimulationDescriptionBuilder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SimulationDescriptionFailureInjection.cpp
)
add_library(
    lux::engine::simulation::description
    ALIAS simulation_description
)

component_include_directories(
    simulation_description
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_include_directories(
    simulation_description
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)
target_compile_definitions(
    simulation_description
    PRIVATE LUX_ENGINE_SIMULATION_DESCRIPTION_LIBRARY
)
target_link_libraries(
    simulation_description
    PUBLIC lux::cxx::compile_time
)
component_add_transitive_commands(
    simulation_description
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
)
lux_classify_target(
    TARGET  simulation_description
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_executable(
    simulation_description_contract_test
    test/simulation_description_contract_test.cpp
)
target_link_libraries(
    simulation_description_contract_test
    PRIVATE lux::engine::simulation::description
)
target_include_directories(
    simulation_description_contract_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)
if(MSVC)
    target_compile_options(simulation_description_contract_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_description_contract_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  simulation_description_contract_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(
    NAME simulation_description_contract_test
    COMMAND simulation_description_contract_test
)

install_components(
    PROJECT_NAME lux-engine-simulation
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::simulation
    COMPONENTS   simulation_description
)
