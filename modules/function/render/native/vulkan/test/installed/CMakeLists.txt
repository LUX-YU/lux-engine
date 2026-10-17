cmake_minimum_required(VERSION 3.22)

project(render-native-installed-consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /GR-)
endif()

enable_testing()

find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render_native_vulkan)
foreach(forbidden render_features render_native_vulkan_extensions render_runtime render_client)
    if(TARGET lux::engine::render::${forbidden})
        message(FATAL_ERROR "Base Native compiler pulled unrelated component ${forbidden}")
    endif()
endforeach()

add_executable(direct_graph_consumer ../DirectGraph.cpp)
target_link_libraries(direct_graph_consumer PRIVATE lux::engine::render::render_native_vulkan)
add_test(NAME installed.native.direct_graph COMMAND direct_graph_consumer)


find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render_native_vulkan render_native_vulkan_extensions)
find_package(lux-engine-core-execution CONFIG REQUIRED COMPONENTS core_execution)

add_executable(native_consumer Main.cpp)

target_link_libraries(native_consumer PRIVATE lux::engine::render::render_native_vulkan lux::engine::render::render_features)

add_test(NAME installed.native.compiler COMMAND native_consumer)

add_executable(texture_consumer Texture.cpp)

target_link_libraries(texture_consumer PRIVATE lux::engine::render::render_native_vulkan_extensions
    lux::engine::core::core_execution
)

foreach(target lux::engine::core::core_execution lux::engine::core::error)
    add_custom_command(TARGET texture_consumer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${target}>" "$<TARGET_FILE_DIR:texture_consumer>"
    )
endforeach()

add_test(NAME installed.native.texture COMMAND texture_consumer)

add_executable(mesh_consumer Mesh.cpp)

target_link_libraries(mesh_consumer PRIVATE lux::engine::render::render_native_vulkan_extensions
    lux::engine::core::core_execution
)

foreach(target lux::engine::core::core_execution lux::engine::core::error)
    add_custom_command(TARGET mesh_consumer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${target}>" "$<TARGET_FILE_DIR:mesh_consumer>"
    )
endforeach()

add_test(NAME installed.native.mesh COMMAND mesh_consumer)

add_executable(material_consumer Material.cpp)

target_link_libraries(material_consumer PRIVATE lux::engine::render::render_native_vulkan_extensions
    lux::engine::core::core_execution
)

foreach(target lux::engine::core::core_execution lux::engine::core::error)
    add_custom_command(TARGET material_consumer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${target}>" "$<TARGET_FILE_DIR:material_consumer>"
    )
endforeach()

add_test(NAME installed.native.material COMMAND material_consumer "${RENDER_TEST_ASSET_DIR}/Unlit.frag.spv")
