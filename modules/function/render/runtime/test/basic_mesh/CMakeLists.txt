cmake_minimum_required(VERSION 3.22)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(render-basic-mesh-integration-test LANGUAGES CXX)
    include(CTest)
    find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render_runtime render_native_vulkan_features)
    find_package(lux-engine-core-execution CONFIG REQUIRED COMPONENTS core_execution)
endif()

add_library(render_basic_mesh_client OBJECT Client.cpp)

target_link_libraries(render_basic_mesh_client PRIVATE
    lux::engine::render::render_commands lux::engine::render::render_features
    lux::engine::platform::platform_ipc
)

target_compile_features(render_basic_mesh_client PRIVATE cxx_std_20)

add_executable(render_basic_mesh Host.cpp $<TARGET_OBJECTS:render_basic_mesh_client>)

target_compile_features(render_basic_mesh PRIVATE cxx_std_20)

target_link_libraries(render_basic_mesh PRIVATE
    lux::engine::render::render_runtime lux::engine::render::render_native_vulkan_features
    lux::engine::core::core_execution
)

if(MSVC)
    foreach(target render_basic_mesh_client render_basic_mesh)
        target_compile_options(${target} PRIVATE /utf-8 /permissive- /Zc:__cplusplus /GR-)
    endforeach()
    add_custom_command(TARGET render_basic_mesh POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            $<TARGET_RUNTIME_DLLS:render_basic_mesh> $<TARGET_FILE_DIR:render_basic_mesh>
        COMMAND_EXPAND_LISTS
    )
endif()

if(TARGET render_textured_material_asset)
    add_dependencies(render_basic_mesh render_textured_material_asset)
    set(sample_material "${CMAKE_BINARY_DIR}/render/native/vulkan/test/assets/UnlitTexture.frag.spv")
else()
    set(sample_material "${RENDER_TEST_ASSET_DIR}/UnlitTexture.frag.spv")
endif()

if(BUILD_TESTING)
    add_test(NAME render.sample.basic_mesh COMMAND render_basic_mesh "${sample_material}")
    set_tests_properties(render.sample.basic_mesh PROPERTIES TIMEOUT 90)
endif()

if(BUILD_TESTING AND TARGET render_textured_material_asset)
    add_test(NAME render.sample.installed
        COMMAND "${CMAKE_COMMAND}"
            "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DCONSUMER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DCONSUMER_ASSET_DIR=${CMAKE_BINARY_DIR}/render/native/vulkan/test/assets"
            "-DCONSUMER_ASSETS=UnlitTexture.frag.spv"
            "-DDEPENDENCY_DIR=${lux-cxx_DIR}" "-DCONFIG=$<CONFIG>"
            "-DGENERATOR=${CMAKE_GENERATOR}" "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}" "-DCXX_FLAGS=${CMAKE_CXX_FLAGS}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/../../../core/test/Installed.cmake"
    )
    set_tests_properties(render.sample.installed PROPERTIES TIMEOUT 120)
endif()
