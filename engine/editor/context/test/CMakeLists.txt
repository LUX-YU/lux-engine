add_library(
    editor_context_plugin_test_support
    STATIC
    PluginProbe.cpp
)
target_link_libraries(
    editor_context_plugin_test_support
    PUBLIC lux::engine::editor::editor_context
)
lux_classify_target(
    TARGET  editor_context_plugin_test_support
    LAYER   EDITOR
    PRODUCT TEST
    ROLE    FOUNDATION
)

add_executable(editor_context_test editor_context_test.cpp)
target_include_directories(
    editor_context_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../pinclude
)
target_link_libraries(
    editor_context_test
    PRIVATE
        lux::engine::editor::editor_context
        editor_context_plugin_test_support
        lux::engine::scene::scene_composition
        lux::engine::scene::scene_description
        lux::engine::simulation::simulation_description
        lux::engine::world::world_description
)
if(MSVC)
    target_compile_options(editor_context_test PRIVATE /UNDEBUG)
else()
    target_compile_options(editor_context_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  editor_context_test
    LAYER   EDITOR
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME editor_context COMMAND editor_context_test)

foreach(probe IN ITEMS editor_context_singleton_probe toolset_string_lookup_probe)
    add_executable(${probe} EXCLUDE_FROM_ALL ${probe}.cpp)
    target_link_libraries(${probe} PRIVATE lux::engine::editor::editor_context)
    lux_classify_target(
        TARGET  ${probe}
        LAYER   EDITOR
        PRODUCT TEST
        ROLE    FOUNDATION
    )
    string(REPLACE "_probe" "" expected_pattern "${probe}")
    if(probe STREQUAL "editor_context_singleton_probe")
        set(expected_pattern "instance")
    else()
        set(expected_pattern "resolve")
    endif()
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
