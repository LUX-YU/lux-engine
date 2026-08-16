## engine::editor_graphkit — the ONE shared node-graph editor framework (GraphKit).
##
## Domain-agnostic imgui-node-editor integration: canvas + link rendering, the
## imgui-id <-> GraphPinRef bimap, palette popup, drag-to-connect validation,
## cap-aware replace-on-reconnect and the inverse-intent undo stack. It drives
## ANY domain graph through the stateless IGraphView port + a per-domain
## IGraphSchema (the UEdGraph/UEdGraphSchema analogue — minus UE's UObject
## persistence: lux domains keep their own SSOTs and asset-tier codecs).
##
## It deliberately links ONLY function::ui + imgui::node_editor — never
## flowforge / description / asset — so domain-freeness is enforced at BUILD
## time (it cannot include headers it does not link).
##
## Editor tier (selected by LUX_BUILD_PROFILE at the engine root): runtime
## builds keep linking zero node_editor. If lux-robotics ever schedules its
## graph editor, this self-contained component (own visibility header,
## tier-independent namespace lux::graphkit) moves down to modules/function
## mechanically.
##
## Build target: lux::engine::editor::editor_graphkit

find_package(node_editor REQUIRED COMPONENTS node_editor)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_GRAPHKIT_LIBRARY
    PUBLIC_MACRO_NAME       LUX_GRAPHKIT_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_GRAPHKIT_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/editor/framework/graphkit/visibility_graphkit.h
)

set(LUX_GRAPHKIT_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/GraphCommandStack.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/GraphLayout.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/GraphEditor.cpp
)

add_component(
    COMPONENT_NAME editor_graphkit
    STATIC
    NAMESPACE      lux::engine::editor
    SOURCE_FILES   ${LUX_GRAPHKIT_SRCS}
)

lux_classify_target(
    TARGET editor_graphkit
    LAYER EDITOR
    PRODUCT EDITOR
    ROLE DOMAIN
)

component_include_directories(
    editor_graphkit
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME
        include
)

target_compile_definitions(
    editor_graphkit
    PRIVATE
    LUX_GRAPHKIT_LIBRARY
    PUBLIC
    LUX_GRAPHKIT_DLL_DISABLE
)

target_link_libraries(
    editor_graphkit
    PUBLIC
    lux::engine::function::ui
    imgui::node_editor
)

component_add_transitive_commands(
    editor_graphkit
    "find_package (lux-engine-function REQUIRED COMPONENTS ui)"
    "find_package (node_editor REQUIRED COMPONENTS node_editor)"
)

set(ENABLE_GRAPHKIT_TEST OFF CACHE BOOL "Enable GraphKit tests")
if(ENABLE_GRAPHKIT_TEST)
    add_subdirectory(test)
endif()
