cmake_minimum_required(VERSION 3.28)

project(UiActionsConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(lux-engine-editor-framework REQUIRED COMPONENTS lux_editor_ui)
find_package(lux-engine-platform REQUIRED COMPONENTS dynamic_library)

enable_testing()

add_executable(ui_actions "${CMAKE_CURRENT_LIST_DIR}/../../../modules/function/ui/test/transaction.cpp")
target_link_libraries(ui_actions PRIVATE lux::engine::function::ui)

add_library(menu_provider MODULE provider.cpp)
target_link_libraries(menu_provider PRIVATE lux::engine::editor::lux_editor_ui)

add_executable(menu_contributions main.cpp)
target_link_libraries(
    menu_contributions
    PRIVATE lux::engine::editor::lux_editor_ui lux::engine::platform::dynamic_library
)

foreach(header Menu Root Command Ids)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/header_${header}.cpp" "#include <lux/engine/ui/${header}.hpp>\n")

    add_library(header_${header} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/header_${header}.cpp")
    target_link_libraries(header_${header} PRIVATE lux::engine::function::ui)
endforeach()

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/header_composition.cpp" "#include <lux/engine/editor/EditorMenuComposition.hpp>\n")

add_library(header_composition OBJECT "${CMAKE_CURRENT_BINARY_DIR}/header_composition.cpp")
target_link_libraries(header_composition PRIVATE lux::engine::editor::lux_editor_ui)

foreach(target
    ui_actions menu_provider menu_contributions
    header_Menu header_Root header_Command header_Ids header_composition
)
    if(MSVC)
        target_compile_options(${target} PRIVATE /UNDEBUG /permissive- /GR- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -UNDEBUG -fno-rtti)
    endif()
endforeach()

add_test(NAME installed.ui.actions COMMAND ui_actions)
add_test(NAME installed.editor.menu_contributions COMMAND menu_contributions "$<TARGET_FILE:menu_provider>")
