find_package(imgui REQUIRED COMPONENTS core)
find_package(lux-cxx REQUIRED COMPONENTS core compile_time)

add_library(
    ui_imgui_backend
    OBJECT
    src/DragDrop.cpp
    src/DrawDataSnapshot.cpp
    src/Frame.cpp
    src/MenuToolbar.cpp
    src/UISession.cpp
    src/ViewportElement.cpp
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
)

lux_classify_target(
    TARGET ui_imgui_backend
    LAYER FUNCTION
    PRODUCT RUNTIME
    ROLE INTEGRATION
)

add_component(
    COMPONENT_NAME ui
    NAMESPACE lux::engine::function
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CommandRouter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Pane.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Theme.cpp
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

option(
    LUX_UI_CONTRACT_CHECKS
    "Enable UI owner-thread contract checks outside Debug builds"
    OFF
)
if(LUX_UI_CONTRACT_CHECKS)
    target_compile_definitions(ui PRIVATE LUX_UI_CONTRACT_CHECKS=1)
endif()

target_link_libraries(
    ui
    PRIVATE ui_imgui_backend
    PUBLIC
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
    "find_package(lux-cxx REQUIRED COMPONENTS core compile_time)"
    "find_package(lux-engine-core REQUIRED COMPONENTS object)"
)

lux_classify_target(
    TARGET ui
    LAYER FUNCTION
    PRODUCT RUNTIME
    ROLE DOMAIN
)

if(LUX_PROFILE_HAS_EDITOR)
    find_package(imgui REQUIRED COMPONENTS vulkan)
    find_package(Vulkan REQUIRED)
    add_library(ui_vulkan_backend STATIC src/UiVulkanBackend.cpp)
    add_library(lux::engine::function::ui_vulkan_backend ALIAS ui_vulkan_backend)
    target_include_directories(
        ui_vulkan_backend
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    )
    target_link_libraries(
        ui_vulkan_backend
        PRIVATE
            ui
            imgui::vulkan
        PUBLIC Vulkan::Vulkan
    )
    target_link_libraries(ui_imgui_backend PRIVATE Vulkan::Vulkan)
    lux_classify_target(
        TARGET  ui_vulkan_backend
        LAYER   FUNCTION
        PRODUCT EDITOR
        ROLE    INTEGRATION
    )
endif()

include_component_cmake_scripts(meta)
engine_add_type_runtime_info(
    NAME ui_pane_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ui/Pane.hpp
    REGISTER_FUNC_NAME lux_ui_pane
)
engine_target_add_type_runtime_info(
    TARGET ui
    METAS ui_pane_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen
)

set(ENABLE_UI_TEST ON CACHE BOOL "Enable final UI Foundation tests")
if(ENABLE_UI_TEST)
    target_compile_definitions(ui PRIVATE LUX_UI_TEST_DIAGNOSTICS=1)
    target_compile_definitions(ui_imgui_backend PRIVATE LUX_UI_TEST_DIAGNOSTICS=1)
    add_executable(ui_test test/ui_test.cpp)
    target_link_libraries(ui_test PRIVATE ui imgui::core)
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
    target_include_directories(
        ui_reentrancy_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    )
    target_compile_definitions(
        ui_reentrancy_test PRIVATE LUX_UI_TEST_DIAGNOSTICS=1
    )
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

    foreach(
        probe_and_pattern
        IN ITEMS
            "ui_public_route_mutation_probe|updateRoute"
            "ui_public_command_pointer_probe|command"
            "ui_retired_menu_model_probe|MenuModel"
            "ui_retired_toolbar_model_probe|ToolbarModel"
            "ui_menu_aggregate_probe|private"
            "ui_toolbar_aggregate_probe|private"
            "ui_layout_default_probe|LayoutSnapshot"
    )
        string(REPLACE "|" ";" probe_fields "${probe_and_pattern}")
        list(GET probe_fields 0 probe)
        list(GET probe_fields 1 expected_pattern)
        add_executable(${probe} EXCLUDE_FROM_ALL test/${probe}.cpp)
        target_link_libraries(${probe} PRIVATE ui)
        add_test(
            NAME ${probe}
            COMMAND
                ${CMAKE_COMMAND}
                -DBUILD_DIR=${CMAKE_BINARY_DIR}
                -DTARGET=${probe}
                "-DEXPECTED_PATTERN=${expected_pattern}"
                -P ${CMAKE_SOURCE_DIR}/cmake/ExpectTargetCompileFailure.cmake
        )
    endforeach()

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

    add_executable(
        ui_drawdata_test
        test/ui_drawdata_test.cpp
        src/DrawDataSnapshot.cpp
    )
    target_link_libraries(ui_drawdata_test PRIVATE imgui::core)
    target_compile_definitions(ui_drawdata_test PRIVATE LUX_FUNCTION_LIBRARY)
    target_include_directories(
        ui_drawdata_test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
            ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
    )
    add_test(NAME ui_drawdata_test COMMAND ui_drawdata_test)

    add_executable(ui_public_frame_test test/ui_public_frame_test.cpp)
    target_link_libraries(ui_public_frame_test PRIVATE ui)
    lux_classify_target(
        TARGET ui_public_frame_test
        LAYER FUNCTION
        PRODUCT TEST
        ROLE FOUNDATION
    )
    add_test(NAME ui_public_frame_test COMMAND ui_public_frame_test)

    add_executable(ui_backend_string_view_test test/ui_backend_string_view_test.cpp)
    target_include_directories(ui_backend_string_view_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude)
    target_link_libraries(ui_backend_string_view_test PRIVATE ui)
    lux_classify_target(
        TARGET  ui_backend_string_view_test
        LAYER   FUNCTION
        PRODUCT TEST
        ROLE    FOUNDATION
    )
    add_test(NAME ui_backend_string_view_test COMMAND ui_backend_string_view_test)
    add_test(
        NAME ui_backend_string_boundary_guard
        COMMAND
            ${CMAKE_COMMAND}
            "-DSOURCES=${CMAKE_CURRENT_SOURCE_DIR}/src/Frame.cpp;${CMAKE_CURRENT_SOURCE_DIR}/src/MenuToolbar.cpp"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test/ValidateBackendStringBoundaries.cmake
    )

    function(ui_add_contract_probe target)
        add_executable(${target} test/${target}.cpp)
        target_link_libraries(${target} PRIVATE ui)
        add_test(
            NAME ${target}
            COMMAND
                ${CMAKE_COMMAND}
                -DPROBE=$<TARGET_FILE:${target}>
                -P ${CMAKE_SOURCE_DIR}/modules/core/object/test/expect_process_failure.cmake
        )
    endfunction()

    ui_add_contract_probe(ui_wrong_thread_pane_registration_probe)
    ui_add_contract_probe(ui_wrong_thread_command_registration_probe)
    ui_add_contract_probe(ui_invalid_frame_sequence_probe)

    if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR LUX_UI_CONTRACT_CHECKS)
        ui_add_contract_probe(ui_wrong_thread_session_probe)
        ui_add_contract_probe(ui_wrong_thread_router_probe)
    endif()

endif()
