find_package(imgui REQUIRED COMPONENTS core)

add_component(
    COMPONENT_NAME ui_next_drawdata
    NAMESPACE lux::engine::function
    SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/DrawDataSnapshot.cpp
)

component_include_directories(
    ui_next_drawdata
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(ui_next_drawdata PRIVATE LUX_FUNCTION_LIBRARY)
target_link_libraries(ui_next_drawdata PUBLIC imgui::core)

add_custom_command(
    TARGET ui_next_drawdata POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:imgui::core>
        $<TARGET_FILE_DIR:ui_next_drawdata>
)

component_add_transitive_commands(
    ui_next_drawdata
    "find_package(imgui REQUIRED COMPONENTS core)"
)

lux_classify_target(
    TARGET ui_next_drawdata
    LAYER FUNCTION
    PRODUCT RUNTIME
    ROLE INTEGRATION
)

if(ENABLE_UI_NEXT_TEST)
    add_executable(
        ui_next_drawdata_smoke
        test/ui_next_drawdata_smoke.cpp
    )
    target_link_libraries(
        ui_next_drawdata_smoke
        PRIVATE ui_next_drawdata
    )
    add_test(
        NAME ui_next_drawdata_smoke
        COMMAND ui_next_drawdata_smoke
    )
endif()
