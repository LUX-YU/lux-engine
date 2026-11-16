include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Slang.cmake")

generate_visibility_header(
    ENABLE_MACRO_NAME LUX_SHADER_COMPILER_LIBRARY
    PUBLIC_MACRO_NAME LUX_SHADER_COMPILER_PUBLIC
    GENERATE_FILE_PATH lux/engine/shader/visibility.h
)

add_component(
    COMPONENT_NAME shader_compiler
    NAMESPACE lux::engine::resource
    OUTPUT_NAME lux_engine_shader_compiler
    SOURCE_FILES src/ShaderCompiler.cpp
)

component_include_directories(
    shader_compiler
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include" "${LUX_GENERATE_HEADER_DIR}"
    INSTALL_TIME include
)

target_compile_definitions(
    shader_compiler PRIVATE LUX_SHADER_COMPILER_LIBRARY
    "LUX_SHADER_COMPILER_VERSION=\"${LUX_SHADER_COMPILER_VERSION}\""
)
target_compile_features(shader_compiler PUBLIC cxx_std_20)
target_link_libraries(
    shader_compiler
    PUBLIC lux::engine::resource::shader_metadata lux::cxx::compile_time
    PRIVATE slang::slang
)

component_add_transitive_commands(
    shader_compiler
    "find_package(lux-engine-resource REQUIRED COMPONENTS shader_metadata)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
)

add_executable(lux_shader_compile src/ShaderCompilerTool.cpp)
target_link_libraries(lux_shader_compile PRIVATE shader_compiler)
install(TARGETS lux_shader_compile RUNTIME DESTINATION bin COMPONENT lux_engine_resource)

if(WIN32)
    add_custom_command(
        TARGET shader_compiler POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${LUX_SHADER_COMPILER_RUNTIMES}
            "$<TARGET_FILE_DIR:shader_compiler>"
        VERBATIM
    )
endif()
