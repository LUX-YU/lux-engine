cmake_minimum_required(VERSION 3.22)

project(render-features-installed-consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor /GR- /we4541)
else()
    add_compile_options(-fno-rtti)
endif()

enable_testing()

find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render_features)

foreach(forbidden render_client render_server render_vulkan render_runtime platform_ipc)
    if(TARGET lux::engine::render::${forbidden} OR TARGET lux::engine::platform::${forbidden})
        message(FATAL_ERROR "Feature consumer imported unrelated target ${forbidden}")
    endif()
endforeach()

add_executable(feature_consumer Main.cpp)

target_link_libraries(feature_consumer PRIVATE lux::engine::render::render_features)

foreach(scenario 0 1 2)
    add_test(NAME installed.feature.${scenario} COMMAND feature_consumer ${scenario})
endforeach()

foreach(header Feature Resources)
    string(CONCAT probe
        "#include <lux/engine/render/features/${header}.hpp>\nstatic_assert(__cplusplus >= 202002L);\n"
        "#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)\n#error Native header leaked into Feature\n#endif\n"
    )

    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp" CONTENT "${probe}")

    add_library(installed_feature_header_${header} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp")

    target_link_libraries(installed_feature_header_${header} PRIVATE lux::engine::render::render_features)
endforeach()

get_target_property(links lux::engine::render::render_features INTERFACE_LINK_LIBRARIES)
set(expected lux::engine::render::render_graph lux::engine::render::render_data
    lux::engine::render::render_protocol
)
if(NOT links STREQUAL expected)
    message(FATAL_ERROR "Unexpected installed Feature dependencies: ${links}")
endif()
