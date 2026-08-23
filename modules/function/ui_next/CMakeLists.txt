find_package(imgui REQUIRED COMPONENTS core)
find_package(lux-cxx REQUIRED COMPONENTS core compile_time)

add_component(
    COMPONENT_NAME ui_next
    STATIC
    NAMESPACE lux::engine::function
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CommandRouter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Pane.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/UISession.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ViewportElement.cpp
)

component_include_directories(
    ui_next
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE pinclude
)

target_compile_definitions(ui_next PUBLIC LUX_FUNCTION_DLL_DISABLE)

target_link_libraries(
    ui_next
    PUBLIC
        imgui::core
        lux::cxx::core
        lux::cxx::compile_time
        lux::engine::core::object
)

component_add_transitive_commands(
    ui_next
    "find_package(imgui REQUIRED COMPONENTS core)"
    "find_package(lux-cxx REQUIRED COMPONENTS core compile_time)"
    "find_package(lux-engine-core REQUIRED COMPONENTS object)"
)

lux_classify_target(
    TARGET ui_next
    LAYER FUNCTION
    PRODUCT RUNTIME
    ROLE DOMAIN
)

set(ENABLE_UI_NEXT_TEST ON CACHE BOOL "Enable UI vNext tests")
if(ENABLE_UI_NEXT_TEST)
    add_executable(ui_next_test test/ui_next_test.cpp)
    target_link_libraries(ui_next_test PRIVATE ui_next)
    add_test(NAME ui_next_test COMMAND ui_next_test)

    add_executable(ui_next_demo test/ui_next_demo.cpp)
    target_link_libraries(ui_next_demo PRIVATE ui_next)
    add_test(NAME ui_next_demo COMMAND ui_next_demo)
endif()
