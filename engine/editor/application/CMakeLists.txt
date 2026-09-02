add_library(
    editor_application
    STATIC
    src/EditorApplication.cpp
)
target_include_directories(
    editor_application
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
)
target_link_libraries(
    editor_application
    PUBLIC
        lux::engine::editor::editor_context
        lux::engine::process::asset_loading
)
lux_classify_target(
    TARGET  editor_application
    LAYER   EDITOR
    PRODUCT EDITOR
    ROLE    COMPOSITION
)

add_executable(lux_editor app/main.cpp)
target_link_libraries(
    lux_editor
    PRIVATE
        editor_application
        lux::engine::scene::scene_meta
)
lux_classify_target(
    TARGET  lux_editor
    LAYER   EDITOR
    PRODUCT EDITOR
    ROLE    COMPOSITION
)
set_target_properties(lux_editor PROPERTIES OUTPUT_NAME "lux_editor")

add_executable(editor_application_test test/editor_application_test.cpp)
target_link_libraries(
    editor_application_test
    PRIVATE
        editor_application
        lux::engine::scene::scene_meta
)
if(MSVC)
    target_compile_options(editor_application_test PRIVATE /UNDEBUG)
else()
    target_compile_options(editor_application_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  editor_application_test
    LAYER   EDITOR
    PRODUCT TEST
    ROLE    COMPOSITION
)
add_test(NAME editor_application COMMAND editor_application_test)

install(
    TARGETS lux_editor
    RUNTIME DESTINATION bin
    COMPONENT lux_editor
)
