cmake_minimum_required(VERSION 3.30)
project(lux_editor_ui_render LANGUAGES CXX)

find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render_extensions)

find_package(imgui CONFIG REQUIRED COMPONENTS core)

find_program(LUX_UI_SHADER_COMPILER lux_shader_compile REQUIRED)

set(shader_headers)
foreach(shader UiRasterVertex UiRasterFragment UiOutputLinear UiOutputSrgb FullscreenVertex)
    set(source "${shader}")
    set(stage fragment)
    set(options)
    if(shader MATCHES "Vertex$")
        set(stage vertex)
    endif()
    if(shader MATCHES "^UiOutput")
        set(source UiOutputFragment)
    endif()
    if(shader STREQUAL "UiOutputSrgb")
        list(APPEND options -DOUTPUT_SRGB=1)
    endif()
    set(header "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.hpp")
    add_custom_command(
        OUTPUT "${header}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND "${LUX_UI_SHADER_COMPILER}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ui/${source}.slang"
            main ${stage} bundle "${header}" "--symbol=k${shader}" ${options}
        DEPENDS
            "${LUX_UI_SHADER_COMPILER}"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ui/${source}.slang"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Common.slang"
        VERBATIM
    )
    list(APPEND shader_headers "${header}")
endforeach()

add_library(lux_editor_ui_render STATIC src/Ui.cpp src/UiState.cpp src/UiFeature.cpp ${shader_headers})

add_library(lux::editor::ui_render ALIAS lux_editor_ui_render)

set_target_properties(lux_editor_ui_render PROPERTIES
    EXPORT_NAME ui_render
    CXX_EXTENSIONS OFF
)

target_compile_features(lux_editor_ui_render PUBLIC cxx_std_20)

target_include_directories(lux_editor_ui_render
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
    PRIVATE pinclude "${CMAKE_CURRENT_BINARY_DIR}/shaders"
)

target_link_libraries(lux_editor_ui_render PUBLIC lux::engine::function::render_extensions imgui::core)

if(MSVC)
    target_compile_options(lux_editor_ui_render PRIVATE /utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor /GR-)
endif()

install(TARGETS lux_editor_ui_render EXPORT LuxEditorUiRender ARCHIVE DESTINATION lib)

install(DIRECTORY include/ DESTINATION include)

install(EXPORT LuxEditorUiRender NAMESPACE lux::editor:: DESTINATION lib/cmake/lux-editor-ui-render)

install(FILES cmake/lux-editor-ui-render-config.cmake DESTINATION lib/cmake/lux-editor-ui-render)
