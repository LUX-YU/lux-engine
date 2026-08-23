find_package(imgui REQUIRED COMPONENTS core)
find_package(lux-cxx REQUIRED COMPONENTS core compile_time)

add_component(
    COMPONENT_NAME ui_next
    NAMESPACE lux::engine::function
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CommandRouter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/MenuToolbar.cpp
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

target_compile_definitions(ui_next PRIVATE LUX_FUNCTION_LIBRARY)

target_link_libraries(
    ui_next
    PUBLIC
        imgui::core
        lux::cxx::core
        lux::cxx::compile_time
        lux::engine::core::object
)

add_custom_command(
    TARGET ui_next POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:imgui::core>
        $<TARGET_FILE_DIR:ui_next>
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

include_component_cmake_scripts(meta)
engine_add_meta(
    NAME ui_next_pane_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ui_next/Pane.hpp
    REGISTER_FUNC_NAME lux_ui_next_pane
)
engine_target_add_meta(
    TARGET ui_next
    METAS ui_next_pane_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen
)

set(ENABLE_UI_NEXT_TEST ON CACHE BOOL "Enable UI vNext tests")
if(ENABLE_UI_NEXT_TEST)
    target_compile_definitions(ui_next PRIVATE LUX_UI_NEXT_TEST_DIAGNOSTICS=1)
    add_executable(ui_next_test test/ui_next_test.cpp)
    target_link_libraries(ui_next_test PRIVATE ui_next)
    target_include_directories(
        ui_next_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    )
    add_test(NAME ui_next_test COMMAND ui_next_test)

    add_executable(
        ui_next_reentrancy_test
        test/ui_next_reentrancy_test.cpp
    )
    target_link_libraries(ui_next_reentrancy_test PRIVATE ui_next)
    add_test(NAME ui_next_reentrancy_test COMMAND ui_next_reentrancy_test)

    add_executable(ui_next_demo test/ui_next_demo.cpp)
    target_link_libraries(ui_next_demo PRIVATE ui_next)
    add_test(NAME ui_next_demo COMMAND ui_next_demo)

    add_executable(
        ui_next_throwing_command_probe EXCLUDE_FROM_ALL
        test/ui_next_throwing_command_probe.cpp
    )
    target_link_libraries(ui_next_throwing_command_probe PRIVATE ui_next)
    add_test(
        NAME ui_next_throwing_command_probe
        COMMAND
            ${CMAKE_COMMAND}
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -DTARGET=ui_next_throwing_command_probe
            "-DEXPECTED_PATTERN=Command handlers must be noexcept"
            -P ${CMAKE_SOURCE_DIR}/cmake/ExpectTargetCompileFailure.cmake
    )

    add_executable(
        ui_next_performance_contract_test
        test/ui_next_performance_contract_test.cpp
    )
    target_link_libraries(
        ui_next_performance_contract_test
        PRIVATE ui_next
    )
    target_compile_definitions(
        ui_next_performance_contract_test
        PRIVATE LUX_UI_NEXT_TEST_DIAGNOSTICS=1
    )
    target_include_directories(
        ui_next_performance_contract_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    )
    add_test(
        NAME ui_next_performance_contract_test
        COMMAND ui_next_performance_contract_test
    )

endif()
