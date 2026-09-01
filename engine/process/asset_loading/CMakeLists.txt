add_interface_component(
    COMPONENT_NAME process_asset_loading
    NAMESPACE      lux::engine::process
)
add_library(lux::engine::process::asset_loading ALIAS process_asset_loading)
component_include_directories(
    process_asset_loading
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME      include
)
target_link_libraries(
    process_asset_loading
    INTERFACE
        lux::engine::process::process_execution
        lux::engine::resource::asset
)
component_add_transitive_commands(
    process_asset_loading
    "find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "add_library(lux::engine::process::asset_loading INTERFACE IMPORTED GLOBAL)"
    "set_property(TARGET lux::engine::process::asset_loading PROPERTY INTERFACE_LINK_LIBRARIES lux::engine::process::process_asset_loading)"
)
lux_classify_target(
    TARGET  process_asset_loading
    LAYER   PROCESS
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(
    process_asset_loading_test
    test/process_asset_loading_test.cpp
)
target_link_libraries(
    process_asset_loading_test
    PRIVATE
        lux::engine::process::asset_loading
        lux::engine::resource::asset
)
if(MSVC)
    target_compile_options(process_asset_loading_test PRIVATE /UNDEBUG)
else()
    target_compile_options(process_asset_loading_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  process_asset_loading_test
    LAYER   PROCESS
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(NAME process_asset_loading_test COMMAND process_asset_loading_test)

install_components(
    PROJECT_NAME lux-engine-process-asset-loading
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::process
    COMPONENTS   process_asset_loading
)
