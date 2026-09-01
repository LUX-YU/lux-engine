find_package(spirv_cross_core CONFIG REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS compile_time core)

# Shared implementation object: both the ShaderAsset cooker and Material compiler
# own the resulting object code, while no installed interface exposes spirv-cross.
add_library(
    toolchain_shader_reflection OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SpirvReflection.cpp
)
set_property(TARGET toolchain_shader_reflection PROPERTY POSITION_INDEPENDENT_CODE ON)
target_include_directories(
    toolchain_shader_reflection
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
)
target_link_libraries(
    toolchain_shader_reflection
    PRIVATE
        lux::engine::resource::description
        spirv-cross-core
)
lux_classify_target(
    TARGET  toolchain_shader_reflection
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    FOUNDATION
)

add_component(
    STATIC
    COMPONENT_NAME toolchain_shader_emitter
    NAMESPACE      lux::engine::toolchain
    OUTPUT_NAME    lux_engine_toolchain_shader_emitter
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/lglsl/LglslEmitter.cpp
)
component_include_directories(
    toolchain_shader_emitter
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME      include
)
target_link_libraries(
    toolchain_shader_emitter
    PUBLIC
        lux::cxx::compile_time
        lux::engine::resource::description
    PRIVATE lux::cxx::core
)
component_add_transitive_commands(
    toolchain_shader_emitter
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time core)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS description)"
)
lux_classify_target(
    TARGET  toolchain_shader_emitter
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    FOUNDATION
)

add_executable(
    lux_shader_emitter
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderEmitterTool.cpp
)
target_link_libraries(
    lux_shader_emitter
    PRIVATE lux::engine::toolchain::toolchain_shader_emitter
)
lux_classify_target(
    TARGET  lux_shader_emitter
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    COMPOSITION
)

if(LUX_PROFILE_HAS_TOOLCHAIN)
    install(
        TARGETS lux_shader_emitter
        RUNTIME DESTINATION bin
        COMPONENT lux_toolchain
    )
    install_components(
        PROJECT_NAME lux-engine-toolchain-shader
        VERSION      ${PROJECT_VERSION}
        NAMESPACE    lux::engine::toolchain
        COMPONENTS   toolchain_shader_emitter
    )
endif()
