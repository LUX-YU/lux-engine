include("${CMAKE_CURRENT_SOURCE_DIR}/../../../modules/resource/description/cmake/ShaderMetadata.cmake")

find_package(spirv_cross_core CONFIG REQUIRED)

# vcpkg exports Debug/Release only; optimized callers must use the matching CRT.
set_property(TARGET spirv-cross-core PROPERTY MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)
find_package(lux-cxx REQUIRED COMPONENTS compile_time core)

find_library(
    LUX_SPIRV_TOOLS_SHARED NAMES SPIRV-Tools-shared
    HINTS "${Vulkan_INCLUDE_DIR}/../Lib" REQUIRED
)

find_path(LUX_SPIRV_TOOLS_INCLUDE spirv-tools/libspirv.h HINTS "${Vulkan_INCLUDE_DIR}" REQUIRED)

# Shared implementation object: both the ShaderAsset cooker and Material compiler
# own the resulting object code, while no installed interface exposes spirv-cross.
add_library(
    toolchain_shader_reflection OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SpirvReflection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PassValidation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SpirvRelocation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderAssets.cpp
)
set_property(TARGET toolchain_shader_reflection PROPERTY POSITION_INDEPENDENT_CODE ON)
target_include_directories(
    toolchain_shader_reflection
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sinclude ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_SPIRV_TOOLS_INCLUDE}
)
target_link_libraries(
    toolchain_shader_reflection
    PRIVATE
        lux::engine::resource::shader_metadata
        spirv-cross-core
        lux::cxx::compile_time
        "${LUX_SPIRV_TOOLS_SHARED}"
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
        lux::engine::resource::shader_metadata
    PRIVATE lux::cxx::core
)
component_add_transitive_commands(
    toolchain_shader_emitter
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time core)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS shader_metadata)"
)

add_executable(
    lux_shader_emitter
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderEmitterTool.cpp
)
target_link_libraries(
    lux_shader_emitter
    PRIVATE lux::engine::toolchain::toolchain_shader_emitter
)

if(LUX_PROFILE_HAS_TOOLCHAIN)
    install(
        TARGETS lux_shader_emitter
        RUNTIME DESTINATION bin
        COMPONENT lux_toolchain
    )
    lux_engine_install_components(
        PROJECT_NAME lux-engine-toolchain-shader
        VERSION      ${PROJECT_VERSION}
        NAMESPACE    lux::engine::toolchain
        COMPONENTS   toolchain_shader_emitter
    )
endif()

if(BUILD_TESTING)
    foreach(header IN ITEMS PassValidation SpirvRelocation ShaderAssets lglsl/LglslEmitter)
        string(REPLACE "/" "_" name "${header}")
        set(header_source "${CMAKE_CURRENT_BINARY_DIR}/header_${name}.cpp")
        file(WRITE "${header_source}" "#include <lux/engine/toolchain/shader/${header}.hpp>\n")

        add_library(toolchain_shader_header_${name} OBJECT "${header_source}")

        target_include_directories(toolchain_shader_header_${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

        target_link_libraries(toolchain_shader_header_${name} PRIVATE shader_metadata lux::cxx::compile_time)
    endforeach()
endif()
