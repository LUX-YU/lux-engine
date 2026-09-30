find_package(imgui REQUIRED COMPONENTS core)
find_package(lux-cxx REQUIRED COMPONENTS core compile_time)

add_library(
    ui_imgui_backend
    OBJECT
    src/DragDrop.cpp
    src/DrawData.cpp
    src/Table.cpp
    src/Root.cpp
    src/Layout.cpp
    src/Controls.cpp
    src/ImageElement.cpp
)
target_include_directories(
    ui_imgui_backend
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
        ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
        ${LUX_GENERATE_HEADER_DIR}
)
target_compile_definitions(ui_imgui_backend PRIVATE LUX_FUNCTION_LIBRARY)
set_target_properties(ui_imgui_backend PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(
    ui_imgui_backend
    PRIVATE
        imgui::core
        lux::cxx::core
        lux::cxx::compile_time
        lux::engine::core::object
        render_client
        lux::engine::platform::window
)

add_component(
    COMPONENT_NAME ui
    NAMESPACE lux::engine::function
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Pane.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Element.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Theme.cpp
)

component_include_directories(
    ui
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE pinclude ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
)

target_compile_definitions(ui PRIVATE LUX_FUNCTION_LIBRARY)

target_link_libraries(
    ui
    PRIVATE ui_imgui_backend
    PUBLIC
        lux::cxx::core
        lux::cxx::compile_time
        lux::engine::core::object
        render_client
)

add_custom_command(
    TARGET ui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:imgui::core>
        $<TARGET_FILE_DIR:ui>
)

component_add_transitive_commands(
    ui
    "find_package(lux-cxx REQUIRED COMPONENTS core compile_time)"
    "find_package(lux-engine-core REQUIRED COMPONENTS object)"
    "find_package(lux-engine-function REQUIRED COMPONENTS render_client)"
)

if(TARGET render_vulkan)
    find_package(imgui REQUIRED COMPONENTS vulkan)
    find_package(Vulkan REQUIRED)
    add_subdirectory(rendering)
endif()

if(BUILD_TESTING)
    add_executable(ui_root_test test/root.cpp)
    target_link_libraries(ui_root_test PRIVATE ui imgui::core)

    if(MSVC)
        target_compile_options(ui_root_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(ui_root_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME ui.root COMMAND ui_root_test )
    set_tests_properties(ui.root PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
endif()
