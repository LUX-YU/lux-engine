find_package(imgui REQUIRED COMPONENTS core)
find_package(lux-cxx REQUIRED COMPONENTS core compile_time)

add_component(
    COMPONENT_NAME ui
    NAMESPACE lux::engine::function
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CommandRouter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/DragDrop.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/DrawDataSnapshot.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/MenuToolbar.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Pane.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/UISession.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ViewportElement.cpp
)

component_include_directories(
    ui
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE pinclude
)

target_compile_definitions(ui PRIVATE LUX_FUNCTION_LIBRARY)

target_link_libraries(
    ui
    PUBLIC
        imgui::core
        lux::cxx::core
        lux::cxx::compile_time
        lux::engine::core::object
)

add_custom_command(
    TARGET ui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:imgui::core>
        $<TARGET_FILE_DIR:ui>
)

component_add_transitive_commands(
    ui
    "find_package(imgui REQUIRED COMPONENTS core)"
    "find_package(lux-cxx REQUIRED COMPONENTS core compile_time)"
    "find_package(lux-engine-core REQUIRED COMPONENTS object)"
)

lux_classify_target(
    TARGET ui
    LAYER FUNCTION
    PRODUCT RUNTIME
    ROLE DOMAIN
)

include_component_cmake_scripts(meta)
engine_add_meta(
    NAME ui_pane_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ui/Pane.hpp
    REGISTER_FUNC_NAME lux_ui_pane
)
engine_target_add_meta(
    TARGET ui
    METAS ui_pane_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen
)

set(ENABLE_UI_TEST ON CACHE BOOL "Enable final UI Foundation tests")
if(ENABLE_UI_TEST)
    target_compile_definitions(ui PRIVATE LUX_UI_TEST_DIAGNOSTICS=1)
    add_executable(ui_test test/ui_test.cpp)
    target_link_libraries(ui_test PRIVATE ui)
    target_include_directories(
        ui_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    )
    target_compile_definitions(ui_test PRIVATE LUX_UI_TEST_DIAGNOSTICS=1)
    add_test(NAME ui_test COMMAND ui_test)

    add_executable(
        ui_reentrancy_test
        test/ui_reentrancy_test.cpp
    )
    target_link_libraries(ui_reentrancy_test PRIVATE ui)
    add_test(NAME ui_reentrancy_test COMMAND ui_reentrancy_test)

    add_executable(ui_demo test/ui_demo.cpp)
    target_link_libraries(ui_demo PRIVATE ui)
    add_test(NAME ui_demo COMMAND ui_demo)

    add_executable(
        ui_throwing_command_probe EXCLUDE_FROM_ALL
        test/ui_throwing_command_probe.cpp
    )
    target_link_libraries(ui_throwing_command_probe PRIVATE ui)
    add_test(
        NAME ui_throwing_command_probe
        COMMAND
            ${CMAKE_COMMAND}
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -DTARGET=ui_throwing_command_probe
            "-DEXPECTED_PATTERN=Command handlers must be noexcept"
            -P ${CMAKE_SOURCE_DIR}/cmake/ExpectTargetCompileFailure.cmake
    )

    add_executable(
        ui_release_benchmark
        test/ui_release_benchmark.cpp
    )
    target_link_libraries(
        ui_release_benchmark
        PRIVATE ui
    )
    target_compile_definitions(
        ui_release_benchmark
        PRIVATE
            LUX_UI_TEST_DIAGNOSTICS=1
            NDEBUG
    )
    target_include_directories(
        ui_release_benchmark
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    )
    set_property(
        TARGET ui_release_benchmark
        PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE
    )
    lux_classify_target(
        TARGET ui_release_benchmark
        LAYER FUNCTION
        PRODUCT TEST
        ROLE FOUNDATION
    )
    add_test(
        NAME ui_release_benchmark
        COMMAND ui_release_benchmark
    )

    add_executable(ui_drawdata_test test/ui_drawdata_test.cpp)
    target_link_libraries(ui_drawdata_test PRIVATE ui)
    add_test(NAME ui_drawdata_test COMMAND ui_drawdata_test)

endif()
