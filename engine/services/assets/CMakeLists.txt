find_package(lux-cxx REQUIRED COMPONENTS memory)
find_package(EnTT CONFIG REQUIRED)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_ASSETS_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_ASSETS_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_ASSETS_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/assets/visibility.h
)

add_component(
    COMPONENT_NAME engine_assets
    NAMESPACE      lux::engine::assets
    OUTPUT_NAME    lux_engine_assets
    SOURCE_FILES
        src/AssetRepository.cpp
)
component_include_directories(
    engine_assets
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(engine_assets PRIVATE LUX_ENGINE_ASSETS_LIBRARY)
target_link_libraries(
    engine_assets
    PUBLIC
        lux::engine::core::core_execution
        lux::engine::resource::asset
        lux::cxx::memory
        EnTT::EnTT
    PRIVATE
        lux::cxx::core
        lux::engine::core::log
)

if(TARGET filewatch)
    target_link_libraries(engine_assets PRIVATE lux::engine::platform::filewatch)
    target_compile_definitions(engine_assets PRIVATE LUX_ASSET_FILEWATCH)
endif()
component_add_transitive_commands(
    engine_assets
    "find_package(lux-engine-core-execution REQUIRED COMPONENTS core_execution)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-cxx REQUIRED COMPONENTS memory)"
    "find_package(EnTT CONFIG REQUIRED)"
)

lux_engine_install_components(
    PROJECT_NAME lux-engine-assets
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::assets
    COMPONENTS   engine_assets
)

if(BUILD_TESTING)
    add_executable(asset_read_overlay_test test/overlay.cpp)
    target_link_libraries(asset_read_overlay_test PRIVATE engine_assets)
    if(TARGET filewatch)
        target_link_libraries(asset_read_overlay_test PRIVATE lux::engine::platform::filewatch)
        target_compile_definitions(asset_read_overlay_test PRIVATE LUX_ASSET_FILEWATCH)
    endif()
    if(MSVC)
        target_compile_options(asset_read_overlay_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(asset_read_overlay_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME process.asset_read_overlay COMMAND asset_read_overlay_test)
    add_executable(asset_bounded_read_test test/bounded_read.cpp)
    target_link_libraries(asset_bounded_read_test PRIVATE engine_assets)
    if(MSVC)
        target_compile_options(asset_bounded_read_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(asset_bounded_read_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME process.asset_bounded_read COMMAND asset_bounded_read_test "${CMAKE_CURRENT_BINARY_DIR}/bounded")
    set_tests_properties(process.asset_bounded_read PROPERTIES TIMEOUT 30 LABELS "native;EC2")
    add_executable(asset_endpoint_lifetime_test test/endpoint_lifetime.cpp)
    target_link_libraries(asset_endpoint_lifetime_test PRIVATE engine_assets)
    if(MSVC)
        target_compile_options(asset_endpoint_lifetime_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(asset_endpoint_lifetime_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME process.asset_endpoint_lifetime COMMAND asset_endpoint_lifetime_test)
    set_tests_properties(process.asset_endpoint_lifetime PROPERTIES TIMEOUT 30 LABELS "native;lifecycle")
endif()
