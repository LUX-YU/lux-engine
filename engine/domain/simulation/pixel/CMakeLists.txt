find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SIMULATION_PIXEL_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SIMULATION_PIXEL_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SIMULATION_PIXEL_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/simulation/pixel/visibility.h
)

add_component(
    COMPONENT_NAME pixel
    NAMESPACE      lux::engine::simulation
    OUTPUT_NAME    lux_engine_simulation_pixel
    SOURCE_FILES src/PixelFieldRuntime.cpp
)

component_include_directories(
    pixel
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(pixel PRIVATE LUX_ENGINE_SIMULATION_PIXEL_LIBRARY)
target_link_libraries(pixel PUBLIC lux::cxx::compile_time)
component_add_transitive_commands(
    pixel
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
)

lux_classify_target(
    TARGET  pixel
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(pixel_field_runtime_test test/pixel_field_runtime_test.cpp)
target_link_libraries(pixel_field_runtime_test PRIVATE lux::engine::simulation::pixel)
if(MSVC)
    target_compile_options(pixel_field_runtime_test PRIVATE /UNDEBUG)
else()
    target_compile_options(pixel_field_runtime_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  pixel_field_runtime_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(NAME vnext_pixel_field_runtime COMMAND pixel_field_runtime_test)
set_tests_properties(
    vnext_pixel_field_runtime
    PROPERTIES LABELS "vnext;l1;pixel;cpu"
)
