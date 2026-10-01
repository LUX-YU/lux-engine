# Tests of the remaining P12 product save adapter; not a new activity provider.
if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_asset_save_test test/asset_save.cpp)
    target_link_libraries(editor_asset_save_test PRIVATE editor_assets editor_editing)
    target_include_directories(editor_asset_save_test PRIVATE sinclude ${PROJECT_SOURCE_DIR}/editor/transition
        ${PROJECT_SOURCE_DIR}/editor/activities/project/sinclude ${PROJECT_SOURCE_DIR}/editor/activities/sinclude)
    if(MSVC)
        target_compile_options(editor_asset_save_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(editor_asset_save_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME editor.asset_save_target COMMAND editor_asset_save_test "${CMAKE_CURRENT_BINARY_DIR}/asset-save")
    set_tests_properties(editor.asset_save_target PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
endif()

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    set_tests_properties(editor.asset_save_target
        PROPERTIES LABELS "editor;native")
endif()
