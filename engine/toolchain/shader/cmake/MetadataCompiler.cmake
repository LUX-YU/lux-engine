include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/../../../../modules/resource/description/cmake/ShaderMetadata.cmake")
find_package(spirv_cross_core CONFIG REQUIRED HINTS "${stdexec_DIR}/../spirv_cross_core")
set_property(TARGET spirv-cross-core PROPERTY MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)

# Build-time consumer of the existing reflection implementation, never a Render runtime dependency.
add_executable(lux_shader_metadata
    "${CMAKE_CURRENT_LIST_DIR}/../src/MetadataCompiler.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/SpirvReflection.cpp"
)

target_include_directories(lux_shader_metadata PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../sinclude")
target_link_libraries(lux_shader_metadata PRIVATE lux::engine::resource::shader_metadata spirv-cross-core)
target_compile_features(lux_shader_metadata PRIVATE cxx_std_20)
