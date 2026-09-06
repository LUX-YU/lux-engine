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
