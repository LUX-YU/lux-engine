# lux::engine::function::input
#
# UE-style input action mapping: physical inputs -> actions -> contexts ->
# triggers/modifiers. Domain-neutral (no ECS / no render) — 2D / 3D / robotics
# teleop / tools all map input the same way. Extracted from gameplay (it was a
# self-contained subsystem with zero ECS coupling).

find_package(lux-cxx REQUIRED COMPONENTS container)

set(LUX_INPUT_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ActionMapper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/BindingIdAllocator.cpp
)

add_component(
    COMPONENT_NAME  input
    NAMESPACE       lux::engine::function
    OUTPUT_NAME     lux_engine_function_input
    SOURCE_FILES    ${LUX_INPUT_SRCS}
)

lux_classify_target(
    TARGET input
    LAYER FUNCTION
    PRODUCT RUNTIME
    ROLE DOMAIN
)

component_include_directories(
    input
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
)

target_compile_definitions(input PRIVATE LUX_FUNCTION_LIBRARY)

target_link_libraries(
    input
    PUBLIC
    lux::engine::platform::window   # InputSnapshot / physical input
    lux::cxx::container             # SparseSet for the action registry
)

component_add_transitive_commands(
    input
    "find_package (lux-engine-platform REQUIRED COMPONENTS window)"
    "find_package (lux-cxx              REQUIRED COMPONENTS container)"
)

# Interactive tests drive a GLFW window (GlfwRuntime) — desktop-only; the
# Android backend has no GLFW and tests don't cross-compile anyway.
if(NOT ANDROID)
    add_subdirectory(test)
endif()
