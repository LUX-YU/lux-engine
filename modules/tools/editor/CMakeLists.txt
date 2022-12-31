set(LUX_EDITOR_SRCS
    src/builtin-obj/BasicCamera.cpp
)

add_module(
    MODULE_NAME         builtin_obj
    NAMESPACE           tools
    SOURCE_FILES        ${LUX_EDITOR_SRCS}
    EXPORT_INCLUDE_DIRS include
    PUBLIC_LIBRARIES    lux::engine::platform::system
                        lux::engine::platform::cxx
                        lux::engine::core::meta
                        lux::engine::core::math
                        lux::engine::core::reflection
)

add_executable(
    LuxEditor
    src/editor/LuxEditor.cpp
)

target_link_libraries(
    LuxEditor
    PRIVATE
    lux::engine::resource::asset
    lux::engine::function::render
    lux::engine::tools::builtin_obj
)