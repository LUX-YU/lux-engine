find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SIMULATION_ASSET_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SIMULATION_ASSET_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SIMULATION_ASSET_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/simulation/asset/visibility.h
)
add_component(
    COMPONENT_NAME simulation_asset
    NAMESPACE      lux::engine::simulation
    OUTPUT_NAME    lux_engine_simulation_asset
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SimulationAssetCodec.cpp
)
add_library(lux::engine::simulation::asset ALIAS simulation_asset)
component_include_directories(
    simulation_asset
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(
    simulation_asset
    PRIVATE LUX_ENGINE_SIMULATION_ASSET_LIBRARY
)
target_link_libraries(
    simulation_asset
    PUBLIC
        lux::engine::simulation::description
        lux::engine::resource::asset
    PRIVATE lux::engine::core::serialization
)
component_add_transitive_commands(
    simulation_asset
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_description)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization)"
    "if(NOT TARGET lux::engine::simulation::asset)"
    "add_library(lux::engine::simulation::asset INTERFACE IMPORTED GLOBAL)"
    "set_property(TARGET lux::engine::simulation::asset PROPERTY INTERFACE_LINK_LIBRARIES lux::engine::simulation::simulation_asset)"
    "endif()"
)
lux_classify_target(
    TARGET  simulation_asset
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)
add_executable(
    simulation_asset_codec_test
    test/simulation_asset_codec_test.cpp
)
target_include_directories(
    simulation_asset_codec_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)
target_link_libraries(
    simulation_asset_codec_test
    PRIVATE lux::engine::simulation::asset
)
if(MSVC)
    target_compile_options(simulation_asset_codec_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_asset_codec_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  simulation_asset_codec_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    INTEGRATION
)
add_test(NAME simulation_asset_codec_test COMMAND simulation_asset_codec_test)
install_components(
    PROJECT_NAME lux-engine-simulation-asset
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::simulation
    COMPONENTS   simulation_asset
)
