add_interface_component(
    COMPONENT_NAME scene_runtime_presentation
    NAMESPACE      lux::engine::scene
)
component_include_directories(
    scene_runtime_presentation
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include
)
lux_classify_target(
    TARGET  scene_runtime_presentation
    LAYER   SCENE
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_executable(latest_spsc_exchange_test test/latest_spsc_exchange_test.cpp)
target_link_libraries(
    latest_spsc_exchange_test
    PRIVATE lux::engine::scene::scene_runtime_presentation
)
if(MSVC)
    target_compile_options(latest_spsc_exchange_test PRIVATE /UNDEBUG)
else()
    target_compile_options(latest_spsc_exchange_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  latest_spsc_exchange_test
    LAYER   SCENE
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME latest_spsc_exchange_test COMMAND latest_spsc_exchange_test)

install_components(
    PROJECT_NAME lux-engine-scene-presentation
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::scene
    COMPONENTS   scene_runtime_presentation
)
