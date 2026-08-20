find_package(spirv_cross_core CONFIG REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS compile_time core)

# Shader compilation is a toolchain concern. Keep shaderc discovery here so
# configuring Runtime/Player does not discover or export compiler libraries.
find_package(Vulkan QUIET COMPONENTS shaderc_combined)
find_package(unofficial-shaderc CONFIG QUIET)
find_package(shaderc CONFIG QUIET)

add_library(toolchain_shaderc_bridge INTERFACE)
lux_classify_target(
    TARGET toolchain_shaderc_bridge
    LAYER TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE FOUNDATION
)
if(TARGET unofficial::shaderc::shaderc)
    target_link_libraries(
        toolchain_shaderc_bridge
        INTERFACE unofficial::shaderc::shaderc
    )
elseif(TARGET Vulkan::shaderc_combined)
    target_link_libraries(
        toolchain_shaderc_bridge
        INTERFACE Vulkan::shaderc_combined
    )
elseif(TARGET shaderc::shaderc)
    target_link_libraries(
        toolchain_shaderc_bridge
        INTERFACE shaderc::shaderc
    )
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(TOOLCHAIN_SHADERC REQUIRED shaderc)
    target_include_directories(
        toolchain_shaderc_bridge
        INTERFACE ${TOOLCHAIN_SHADERC_INCLUDE_DIRS}
    )
    target_link_libraries(
        toolchain_shaderc_bridge
        INTERFACE ${TOOLCHAIN_SHADERC_LIBRARIES}
    )
endif()
add_library(shaderc::shaderc ALIAS toolchain_shaderc_bridge)

# Backend-neutral shader IR is a Toolchain product. Canonical .lglsl emission
# remains a host build tool because Runtime shader assets are generated with it.
if(LUX_PROFILE_HAS_TOOLCHAIN)
add_component(
    STATIC
    COMPONENT_NAME toolchain_shader_ir
    NAMESPACE      lux::engine::toolchain
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderIR.cpp
)
component_include_directories(
    toolchain_shader_ir
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_link_libraries(
    toolchain_shader_ir
    PUBLIC lux::engine::resource::description
)
set_target_properties(
    toolchain_shader_ir PROPERTIES POSITION_INDEPENDENT_CODE ON)
lux_classify_target(
    TARGET  toolchain_shader_ir
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    FOUNDATION
)
component_add_transitive_commands(
    toolchain_shader_ir
    "find_package(lux-engine-resource REQUIRED COMPONENTS description)"
)
endif()

add_component(
    STATIC
    COMPONENT_NAME toolchain_shader_emitter
    NAMESPACE      lux::engine::toolchain
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/lglsl/LglslEmitter.cpp
)
component_include_directories(
    toolchain_shader_emitter
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_link_libraries(
    toolchain_shader_emitter
    PUBLIC
        lux::cxx::compile_time
        lux::engine::resource::description
    PRIVATE
        lux::cxx::core
)
set_target_properties(
    toolchain_shader_emitter PROPERTIES POSITION_INDEPENDENT_CODE ON)
lux_classify_target(
    TARGET  toolchain_shader_emitter
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    DOMAIN
)
component_add_transitive_commands(
    toolchain_shader_emitter
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time core)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS description)"
)

add_component(
    STATIC
    COMPONENT_NAME toolchain_shader
    NAMESPACE      lux::engine::toolchain
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SpirvReflection.cpp
)

component_include_directories(
    toolchain_shader
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
)
target_link_libraries(
    toolchain_shader
    PUBLIC
        lux::engine::resource::description
    PRIVATE
        spirv-cross-core
)
set_target_properties(
    toolchain_shader
    PROPERTIES
        POSITION_INDEPENDENT_CODE ON
)

lux_classify_target(
    TARGET  toolchain_shader
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    DOMAIN
)

# Asset-file emission is a build-tool adapter, not part of the reflection
# core. Keeping it separate prevents shader compilers from inheriting the
# runtime AssetManager/SerDeser library merely because both consume ShaderInfo.
add_component(
    STATIC
    COMPONENT_NAME toolchain_shader_asset
    NAMESPACE      lux::engine::toolchain
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SpirvAssetPacker.cpp
)

component_include_directories(
    toolchain_shader_asset
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
)

target_link_libraries(
    toolchain_shader_asset
    PUBLIC
        lux::engine::toolchain::toolchain_shader
        lux::engine::resource::asset
)

set_target_properties(
    toolchain_shader_asset
    PROPERTIES
        POSITION_INDEPENDENT_CODE ON
)

lux_classify_target(
    TARGET  toolchain_shader_asset
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    INTEGRATION
)

# The GLSL/SPIR-V compiler is a Toolchain implementation, never a Player
# configuration concern.
if(LUX_PROFILE_HAS_TOOLCHAIN)
    add_subdirectory(glsl)
endif()
