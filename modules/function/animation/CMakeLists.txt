# lux::engine::function::animation
#
# Runtime animation playback: stateless pose sampling + skinning-matrix
# composition (PoseSampling) + pooled scratch. Function tier — no Vulkan, no
# MLIR; deps are resource::description + core::math.
# The ECS side (AnimatorComponent + AnimationSystem) consumes this from the
# gameplay 3D kit (lux::gameplay::d3).

set(LUX_ANIMATION_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PoseSampling.cpp
)

add_component(
    COMPONENT_NAME animation
    NAMESPACE      lux::engine::function
    SOURCE_FILES   ${LUX_ANIMATION_SRCS}
)

lux_classify_target(
    TARGET animation
    LAYER FUNCTION
    PRODUCT RUNTIME
    ROLE DOMAIN
)

target_compile_definitions(
    animation
    PRIVATE
    LUX_FUNCTION_LIBRARY
)

component_include_directories(
    animation
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
)

target_link_libraries(
    animation
    PUBLIC
    lux::engine::core::math
    lux::engine::resource::description
)

component_add_transitive_commands(
    animation
    "find_package(lux-engine-core REQUIRED COMPONENTS math)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS description)"
)

set(ENABLE_ANIMATION_TEST OFF CACHE BOOL "Enable animation module tests")
if(ENABLE_ANIMATION_TEST)
    add_subdirectory(test)
endif()
