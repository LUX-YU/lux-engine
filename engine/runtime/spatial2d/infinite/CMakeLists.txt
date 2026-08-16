generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_RUNTIME_SPATIAL2D_INFINITE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_RUNTIME_SPATIAL2D_INFINITE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_RUNTIME_SPATIAL2D_INFINITE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/spatial2d/infinite/visibility.h
)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_RUNTIME_INFINITE2D_PIXEL_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_RUNTIME_INFINITE2D_PIXEL_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_RUNTIME_INFINITE2D_PIXEL_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/spatial2d/infinite/pixel_visibility.h
)

add_component(
    COMPONENT_NAME runtime_spatial2d_infinite
    NAMESPACE      lux::engine::runtime
    OUTPUT_NAME    lux_engine_runtime_spatial2d_infinite
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Spatial2DSectionIndex.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SpatialInterest2DSystem.cpp
)

component_include_directories(
    runtime_spatial2d_infinite
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    runtime_spatial2d_infinite
    PRIVATE LUX_ENGINE_RUNTIME_SPATIAL2D_INFINITE_LIBRARY
)

target_link_libraries(
    runtime_spatial2d_infinite
    PUBLIC
        lux::engine::ecs::core
        lux::engine::ecs::spatial2d_components
        lux::engine::ecs::transform
        lux::engine::resource::entity_scene
        lux::engine::resource::spatial
        lux::engine::runtime::runtime_spatial_partition
        lux::cxx::compile_time
)

component_add_transitive_commands(
    runtime_spatial2d_infinite
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core spatial2d_components transform)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS entity_scene spatial)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_spatial_partition)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
)

lux_classify_target(
    TARGET  runtime_spatial2d_infinite
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_component(
    COMPONENT_NAME runtime_pack_infinite2d
    NAMESPACE      lux::engine::runtime
    OUTPUT_NAME    lux_engine_runtime_pack_infinite2d
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Infinite2DPixelContent.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Infinite2DPixelPrepareService.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Infinite2DPixelSystem.cpp
)

component_include_directories(
    runtime_pack_infinite2d
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_include_directories(
    runtime_pack_infinite2d
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
)

target_compile_definitions(
    runtime_pack_infinite2d
    PRIVATE LUX_ENGINE_RUNTIME_INFINITE2D_PIXEL_LIBRARY
)

target_link_libraries(
    runtime_pack_infinite2d
    PUBLIC
        lux::engine::runtime::runtime_spatial2d_infinite
        lux::engine::runtime::runtime_entity_scene
        lux::engine::runtime::runtime_execution
        lux::engine::ecs::pixel
    PRIVATE
        lux::cxx::algorithm
        lux::engine::core::serialization
)

component_add_transitive_commands(
    runtime_pack_infinite2d
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_entity_scene runtime_execution runtime_spatial2d_infinite)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS pixel)"
)

lux_classify_target(
    TARGET  runtime_pack_infinite2d
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)

add_executable(
    spatial2d_interest_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/spatial2d_interest_test.cpp
)
target_link_libraries(
    spatial2d_interest_test
    PRIVATE
        lux::engine::runtime::runtime_spatial2d_infinite
)
if(MSVC)
    target_compile_options(spatial2d_interest_test PRIVATE /UNDEBUG)
else()
    target_compile_options(spatial2d_interest_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  spatial2d_interest_test
    LAYER   RUNTIME
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(NAME spatial2d_interest_test COMMAND spatial2d_interest_test)
set_tests_properties(
    spatial2d_interest_test
    PROPERTIES LABELS "cpu;runtime;spatial2d" TIMEOUT 30
)

add_executable(
    infinite2d_headless_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/infinite2d_headless_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/infinite2d_headless_test.cpp
)
target_link_libraries(
    infinite2d_headless_test
    PRIVATE
        lux::engine::runtime::runtime_pack_infinite2d
        lux::engine::runtime::runtime_execution
        lux::engine::runtime::runtime_spatial2d_tilemap
        lux::engine::core::serialization
        lux::engine::ecs::tilemap
        lux::engine::resource::tilemap_content
)
if(MSVC)
    target_compile_options(infinite2d_headless_test PRIVATE /UNDEBUG)
else()
    target_compile_options(infinite2d_headless_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  infinite2d_headless_test
    LAYER   RUNTIME
    PRODUCT TEST
    ROLE    INTEGRATION
)
add_test(NAME infinite2d_headless_test COMMAND infinite2d_headless_test)
set_tests_properties(
    infinite2d_headless_test
    PROPERTIES LABELS "cpu;runtime;spatial2d;entity_scene;pixel;tilemap;async;open_closed" TIMEOUT 60
)
