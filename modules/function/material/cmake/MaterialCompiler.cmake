generate_visibility_header(
    ENABLE_MACRO_NAME LUX_MATERIAL_COMPILER_LIBRARY
    PUBLIC_MACRO_NAME LUX_MATERIAL_COMPILER_PUBLIC
    GENERATE_FILE_PATH lux/engine/material/compiler/visibility.h
)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/MaterialTypes.slang" material_types_source)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/GBufferEncode.slang" gbuffer_source)
set_property(
    DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/MaterialTypes.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/GBufferEncode.slang"
)

configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/ShaderSources.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/lux/engine/material/compiler/ShaderSources.hpp"
    @ONLY
)

add_component(
    COMPONENT_NAME material_compiler
    NAMESPACE lux::engine::function
    OUTPUT_NAME lux_engine_material_compiler
    SOURCE_FILES src/compiler/MaterialCompiler.cpp src/compiler/SlangSource.cpp src/MaterialValidation.cpp
)

component_include_directories(
    material_compiler
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include" "${LUX_GENERATE_HEADER_DIR}"
    INSTALL_TIME include
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/pinclude" "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_compile_definitions(material_compiler PRIVATE LUX_MATERIAL_COMPILER_LIBRARY)
target_compile_features(material_compiler PUBLIC cxx_std_20)
target_link_libraries(
    material_compiler
    PUBLIC lux::engine::function::material_graph lux::engine::resource::identity lux::cxx::compile_time
    PUBLIC lux::engine::resource::shader_compiler
)

component_add_transitive_commands(
    material_compiler
    "find_package(lux-engine-function REQUIRED COMPONENTS material_graph)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS identity shader_compiler)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
)

# Only the command-line adapter links the cooked asset codec.
add_executable(lux_material_tool src/compiler/MaterialTool.cpp)
target_link_libraries(lux_material_tool PRIVATE material_graph material_compiler lux::engine::resource::asset)
install(TARGETS lux_material_tool RUNTIME DESTINATION bin COMPONENT lux_engine_function)
