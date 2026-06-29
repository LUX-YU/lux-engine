## engine::graphkit — the ONE shared node-graph editor framework (GraphKit).
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
## Editor tier (engine/, LUX_BUILD_EDITOR-gated at the engine root): runtime
## builds keep linking zero node_editor. If lux-robotics ever schedules its
## graph editor, this self-contained component (own visibility header,
## tier-independent namespace lux::graphkit) moves down to modules/function
## mechanically.
##
## Build target: lux::engine::graphkit

find_package(node_editor REQUIRED COMPONENTS node_editor)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_GRAPHKIT_LIBRARY
    PUBLIC_MACRO_NAME       LUX_GRAPHKIT_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_GRAPHKIT_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/graphkit/visibility_graphkit.h
)

set(LUX_GRAPHKIT_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/GraphCommandStack.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/GraphLayout.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/GraphEditor.cpp
)

add_component(
    COMPONENT_NAME graphkit
    STATIC
    NAMESPACE      lux::engine
    SOURCE_FILES   ${LUX_GRAPHKIT_SRCS}
)

component_include_directories(
    graphkit
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME
        include
)

target_compile_definitions(
    graphkit
    PRIVATE
    LUX_GRAPHKIT_LIBRARY
    PUBLIC
    LUX_GRAPHKIT_DLL_DISABLE
)

target_link_libraries(
    graphkit
    PUBLIC
    lux::engine::function::ui
    imgui::node_editor
)

component_add_transitive_commands(
    graphkit
    "find_package (lux-engine-function REQUIRED COMPONENTS ui)"
    "find_package (node_editor REQUIRED COMPONENTS node_editor)"
)

set(ENABLE_GRAPHKIT_TEST OFF CACHE BOOL "Enable GraphKit tests")
if(ENABLE_GRAPHKIT_TEST)
    add_subdirectory(test)
endif()
