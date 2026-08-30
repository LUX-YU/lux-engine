add_interface_component(
    COMPONENT_NAME process_asset
    NAMESPACE      lux::engine::process
)
component_include_directories(
    process_asset
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME      include
)
target_link_libraries(
    process_asset
    INTERFACE
        lux::engine::process::process_execution
        lux::engine::resource::asset
)
component_add_transitive_commands(
    process_asset
    "find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
)
lux_classify_target(
    TARGET  process_asset
    LAYER   PROCESS
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(
    process_asset_test
    test/process_asset_test.cpp
)
target_link_libraries(
    process_asset_test
    PRIVATE
        lux::engine::process::process_asset
        lux::engine::resource::asset
)
if(MSVC)
    target_compile_options(process_asset_test PRIVATE /UNDEBUG)
else()
    target_compile_options(process_asset_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  process_asset_test
    LAYER   PROCESS
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(NAME process_asset_test COMMAND process_asset_test)

install_components(
    PROJECT_NAME lux-engine-process-asset
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::process
    COMPONENTS   process_asset
)
