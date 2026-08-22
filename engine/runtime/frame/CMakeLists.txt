## engine/runtime/frame — the host-neutral frame lifecycle coordinator.
##
## Hosts may have different work inside a frame, but they must not each own a
## private copy of the render-channel protocol.  This component centralises the
## previous-frame flush, frame-open, main-thread completion, event safe-point,
## recording, and submit order.

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_FRAME_RUNTIME_LIBRARY
    PUBLIC_MACRO_NAME       LUX_FRAME_RUNTIME_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_FRAME_RUNTIME_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/frame/visibility.h
)

add_component(
    COMPONENT_NAME  runtime_frame
    NAMESPACE       lux::engine::runtime
    OUTPUT_NAME     lux_engine_runtime_frame
    SOURCE_FILES    ${CMAKE_CURRENT_SOURCE_DIR}/src/FrameCoordinator.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/MainCloseDriver.cpp
)

component_include_directories(
    runtime_frame
    BUILD_TIME_EXPORT   ${CMAKE_CURRENT_SOURCE_DIR}/include
                        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME        include
)

target_compile_definitions(
    runtime_frame
    PRIVATE
    LUX_FRAME_RUNTIME_LIBRARY
)

target_link_libraries(
    runtime_frame
    PUBLIC
    lux::engine::function::render_client
    lux::engine::core::events
    lux::engine::runtime::runtime_execution
    lux::engine::runtime::runtime_scene_core
    lux::engine::runtime::runtime_render_scene
    lux::engine::runtime::runtime_extension_orchestration
    PRIVATE
    lux::cxx::concurrent
    lux::engine::runtime::runtime_logging
)

component_add_transitive_commands(
    runtime_frame
    "find_package(lux-cxx REQUIRED COMPONENTS concurrent)"
    "find_package(lux-engine-function REQUIRED COMPONENTS render_client)"
    "find_package(lux-engine-core REQUIRED COMPONENTS events)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_execution runtime_logging runtime_scene_core runtime_render_scene runtime_extension_orchestration)"
)

if(MSVC)
    target_compile_options(
        runtime_frame
        PRIVATE /permissive- /Zc:__cplusplus /Zc:preprocessor)
endif()

lux_classify_target(
    TARGET  runtime_frame
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)

# Frame scope is an architectural contract: render-facing Systems run beside
# RenderSystem, so a regression that narrows the lexical frame to one node is
# a permanent Developer-profile test.  It must not enter Player/Android product
# closures merely because BUILD_TESTING happens to be enabled globally.
if(BUILD_TESTING AND LUX_BUILD_PROFILE STREQUAL "DEVELOPER")
    add_subdirectory(test)
endif()
