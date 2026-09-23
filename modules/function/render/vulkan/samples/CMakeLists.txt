cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(lux_external_plugin LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    find_package(lux-engine-editor-metadata REQUIRED COMPONENTS editor_metadata)
    find_package(lux-engine-function REQUIRED COMPONENTS render_vulkan render_runtime)
    find_package(lux-engine-scene-render REQUIRED COMPONENTS scene_render)
    find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS schema)
    find_path(LUX_PLUGIN_CMAKE_DIR LuxPlugins.cmake PATH_SUFFIXES share/lux-engine/plugins REQUIRED)
    include("${LUX_PLUGIN_CMAKE_DIR}/LuxPlugins.cmake")
    get_filename_component(sample_sdk_root "${LUX_PLUGIN_CMAKE_DIR}/../../.." ABSOLUTE)
    enable_testing()
endif()
find_package(Vulkan REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(shaders "${CMAKE_CURRENT_BINARY_DIR}/ExternalShaders.hpp")
add_custom_command(OUTPUT "${shaders}"
    COMMAND "${Vulkan_GLSLC_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/triangle.vert" -o "${CMAKE_CURRENT_BINARY_DIR}/triangle.vert.spv"
    COMMAND "${Vulkan_GLSLC_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/triangle.frag" -o "${CMAKE_CURRENT_BINARY_DIR}/triangle.frag.spv"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/embed_shaders.py" "${shaders}"
        "${CMAKE_CURRENT_BINARY_DIR}/triangle.vert.spv" "${CMAKE_CURRENT_BINARY_DIR}/triangle.frag.spv"
    DEPENDS triangle.vert triangle.frag embed_shaders.py VERBATIM)
add_library(sample_external_plugin SHARED ExternalFeatureSample.cpp "${shaders}")
target_include_directories(sample_external_plugin PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(sample_external_plugin PRIVATE lux::engine::function::render_vulkan
    lux::engine::scene::scene_render lux::engine::simulation::ecs::schema)
lux_add_plugin_exports(TARGET sample_external_plugin DESCRIPTION "${CMAKE_CURRENT_SOURCE_DIR}/triangle.plugin.json"
    INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/ExternalFeatureSample.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/ExternalFeature.hpp"
        "${shaders}")
add_executable(sample_external_test ExternalFeatureTest.cpp)
target_link_libraries(sample_external_test PRIVATE lux::engine::editor::editor_metadata
    lux::engine::function::render_runtime lux::engine::scene::scene_render)
if(MSVC)
    target_compile_options(sample_external_test PRIVATE /UNDEBUG /utf-8)
else()
    target_compile_options(sample_external_test PRIVATE -UNDEBUG)
endif()
add_dependencies(sample_external_test sample_external_plugin)
add_test(NAME plugin.external_gpu COMMAND sample_external_test
    "${CMAKE_BINARY_DIR}/share/lux-engine/editor/capabilities.json" "${CMAKE_BINARY_DIR}")
set_tests_properties(plugin.external_gpu PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
install(TARGETS sample_external_plugin sample_external_test RUNTIME DESTINATION bin LIBRARY DESTINATION lib)
if(sample_sdk_root)
    lux_generate_capabilities(BASE "${sample_sdk_root}/share/lux-engine/editor/capabilities.json")
    add_dependencies(sample_external_test lux_plugin_capabilities)
    if(WIN32)
        file(GLOB sample_runtime_libraries CONFIGURE_DEPENDS "${sample_sdk_root}/bin/*.dll")
        list(FILTER sample_runtime_libraries EXCLUDE REGEX "/sample_external_plugin\\.dll$")
        add_custom_command(TARGET sample_external_test POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${sample_runtime_libraries}
                "$<TARGET_FILE_DIR:sample_external_test>" COMMAND_EXPAND_LISTS VERBATIM)
    endif()
else()
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/" DESTINATION share/lux-engine/examples/external-feature
        FILES_MATCHING PATTERN "*.cpp" PATTERN "*.hpp" PATTERN "*.json" PATTERN "*.py"
        PATTERN "*.vert" PATTERN "*.frag" PATTERN "CMakeLists.txt")
endif()
