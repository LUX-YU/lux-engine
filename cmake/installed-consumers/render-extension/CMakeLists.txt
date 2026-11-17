cmake_minimum_required(VERSION 3.28)

project(render_extension_consumer LANGUAGES CXX)

find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render_extensions)

find_package(lux-engine-core-execution CONFIG REQUIRED COMPONENTS core_execution)

add_executable(render_extension Probe.cpp)
target_link_libraries(render_extension PRIVATE lux::engine::function::render_extensions lux::engine::core::core_execution)
target_compile_features(render_extension PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(render_extension PRIVATE /utf-8 /permissive- /Zc:__cplusplus /GR- /bigobj)
endif()

enable_testing()

foreach(header IN ITEMS PassEncoding PassRecordInput)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/${header}Header.cpp")
    file(GENERATE OUTPUT "${source}" CONTENT "#include <lux/engine/render/extensions/graph/${header}.hpp>\n")
    add_library(header_${header} OBJECT "${source}")
    target_link_libraries(header_${header} PRIVATE lux::engine::function::render_extensions)
    target_compile_features(header_${header} PRIVATE cxx_std_20)
endforeach()

add_executable(effect_build EffectBuild.cpp)

target_link_libraries(effect_build PRIVATE
    lux::engine::function::render_extensions
    lux::engine::core::core_execution
)

target_compile_features(effect_build PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(effect_build PRIVATE /utf-8 /permissive- /Zc:__cplusplus /GR- /bigobj)
endif()

add_test(NAME installed.render.effect_build_vulkan COMMAND effect_build)
add_test(NAME installed.render.effect_build_d3d12 COMMAND effect_build d3d12)

set_tests_properties(installed.render.effect_build_vulkan installed.render.effect_build_d3d12 PROPERTIES TIMEOUT 45)

add_test(NAME installed.render.extension COMMAND render_extension)
set_tests_properties(installed.render.extension PROPERTIES TIMEOUT 45)

add_executable(frame_columns FrameColumns.cpp)

target_link_libraries(frame_columns PRIVATE lux::engine::function::render)

target_compile_features(frame_columns PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(frame_columns PRIVATE /utf-8 /permissive- /Zc:__cplusplus /GR-)
endif()

add_test(NAME installed.render.frame_columns COMMAND frame_columns)

foreach(case IN ITEMS reset move assign destroy missing-read)
    add_test(NAME installed.render.frame_columns_${case}
        COMMAND ${CMAKE_COMMAND}
            -DPROBE=$<TARGET_FILE:frame_columns>
            -DCASE=${case}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckFrameRejection.cmake"
    )
endforeach()

add_executable(preparation_callbacks Preparation.cpp)

target_link_libraries(preparation_callbacks PRIVATE lux::engine::function::render_extensions)

target_compile_features(preparation_callbacks PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(preparation_callbacks PRIVATE /utf-8 /permissive- /Zc:__cplusplus /GR-)
endif()

add_test(NAME installed.render.preparation_callbacks COMMAND preparation_callbacks)

foreach(case IN ITEMS pending reenter)
    add_test(NAME installed.render.preparation_${case}
        COMMAND ${CMAKE_COMMAND}
            -DPROBE=$<TARGET_FILE:preparation_callbacks>
            -DCASE=${case}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckFrameRejection.cmake"
    )
endforeach()
