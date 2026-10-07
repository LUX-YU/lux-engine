find_package(lux-cxx REQUIRED COMPONENTS core compile_time container)

add_component(COMPONENT_NAME lux_editor_context NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES context/src/FrameworkErrors.cpp context/src/EditorContext.cpp context/src/EditorServiceRegistrar.cpp context/src/EditorUiRegistrar.cpp)
component_include_directories(lux_editor_context BUILD_TIME_EXPORT
    ${CMAKE_CURRENT_SOURCE_DIR}/context/include ${CMAKE_CURRENT_SOURCE_DIR}/project/include INSTALL_TIME include)
target_link_libraries(lux_editor_context PUBLIC lux::cxx::core lux::cxx::compile_time
    lux::engine::core::object lux::engine::core::error lux::engine::resource::asset)
component_add_transitive_commands(lux_editor_context
    "find_package(lux-cxx REQUIRED COMPONENTS core compile_time)"
    "find_package(lux-engine-core REQUIRED COMPONENTS object error)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)")
lux_classify_target(TARGET lux_editor_context LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)

add_component(COMPONENT_NAME lux_editor_ui NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES ui/src/EditorUiFactories.cpp ui/src/WindowInput.cpp window/src/EditorWindow.cpp)
component_include_directories(lux_editor_ui BUILD_TIME_EXPORT
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/include ${CMAKE_CURRENT_SOURCE_DIR}/window/include INSTALL_TIME include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ui/pinclude)
target_link_libraries(lux_editor_ui PUBLIC lux_editor_context lux::engine::function::ui
    lux::engine::function::input lux::engine::platform::window
    PRIVATE lux::cxx::container)
component_add_transitive_commands(lux_editor_ui
    "find_package(lux-engine-function REQUIRED COMPONENTS ui input)"
    "find_package(lux-engine-platform REQUIRED COMPONENTS window)"
    "find_package(lux-cxx REQUIRED COMPONENTS container)"
)
lux_classify_target(TARGET lux_editor_ui LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)

add_component(COMPONENT_NAME lux_editor_app NAMESPACE lux::engine::editor STATIC SOURCE_FILES app/src/LuxEngine.cpp app/src/EditorUiScene.cpp app/src/UiRenderSyncStage.cpp)
component_include_directories(lux_editor_app BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/app/include INSTALL_TIME include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/app/pinclude)
target_link_libraries(lux_editor_app PUBLIC lux_editor_context lux_editor_ui PRIVATE lux::engine::engine_context_render lux::engine::scene::scene_render lux::engine::function::ui_rendering)
component_add_transitive_commands(lux_editor_app
    "find_package(lux-engine-context-render REQUIRED COMPONENTS engine_context_render)"
    "find_package(lux-engine-scene-render REQUIRED COMPONENTS scene_render)"
    "find_package(lux-engine-function REQUIRED COMPONENTS ui_rendering)")
lux_classify_target(TARGET lux_editor_app LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
add_executable(lux_editor app/main.cpp)
target_link_libraries(lux_editor PRIVATE lux_editor_app)
lux_classify_target(TARGET lux_editor LAYER EDITOR PRODUCT EDITOR ROLE EXECUTABLE)
install(TARGETS lux_editor RUNTIME DESTINATION bin)
if(WIN32)
    install(FILES $<TARGET_RUNTIME_DLLS:lux_editor> DESTINATION bin)
endif()
lux_engine_install_components(PROJECT_NAME lux-engine-editor-framework VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS lux_editor_context lux_editor_ui lux_editor_app)

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    if(CMAKE_VERSION VERSION_LESS 3.27)
        message(FATAL_ERROR "Framework isolation tests require CMake 3.27 or newer for first-configure File API queries")
    endif()
    cmake_file_api(QUERY API_VERSION 1 CODEMODEL 2)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME framework.isolation COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_isolation.py" "${CMAKE_BINARY_DIR}")
    add_executable(editor_framework_component_test tests/context_component.cpp)
    target_link_libraries(editor_framework_component_test PRIVATE lux_editor_context)
    lux_editor_test_options(editor_framework_component_test)
    add_test(NAME framework.context_component COMMAND editor_framework_component_test)
    add_executable(editor_framework_context_test tests/context.cpp)
    target_link_libraries(editor_framework_context_test PRIVATE lux_editor_context lux::engine::engine_context)
    lux_editor_test_options(editor_framework_context_test)
    add_test(NAME framework.context COMMAND editor_framework_context_test ${CMAKE_CURRENT_BINARY_DIR}/test-data)
    add_test(NAME framework.error_registration_conflict COMMAND editor_framework_context_test --registration-conflict)
    add_executable(editor_framework_input_test tests/input.cpp)
    target_include_directories(editor_framework_input_test PRIVATE ui/pinclude)
    target_link_libraries(editor_framework_input_test PRIVATE lux_editor_ui)
    lux_editor_test_options(editor_framework_input_test)
    add_test(NAME framework.input_conversion COMMAND editor_framework_input_test)
    add_executable(editor_framework_ui_test tests/ui.cpp tests/callback_contract.cpp)
    target_link_libraries(editor_framework_ui_test PRIVATE lux_editor_ui)
    lux_editor_test_options(editor_framework_ui_test)
    add_test(NAME framework.ui COMMAND editor_framework_ui_test)
    add_test(NAME framework.callback_contract COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/callback_contract.py" "${CMAKE_BINARY_DIR}")
    set_tests_properties(framework.context framework.ui PROPERTIES TIMEOUT 60)
endif()

if(LUX_EDITOR_BUILD_GPU_TESTS)
    add_executable(editor_framework_transport_test tests/transport.cpp)
    target_include_directories(editor_framework_transport_test PRIVATE app/pinclude)
    target_link_libraries(editor_framework_transport_test PRIVATE lux_editor_app lux::engine::engine_context_render lux::engine::function::ui_rendering)
    lux_editor_test_options(editor_framework_transport_test)
    add_executable(editor_framework_gpu_test tests/gpu.cpp)
    target_link_libraries(editor_framework_gpu_test PRIVATE lux_editor_app lux::engine::engine_context_render)
    lux_editor_test_options(editor_framework_gpu_test)
    add_test(NAME framework.gpu COMMAND editor_framework_transport_test)
    set_tests_properties(framework.gpu PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE LABELS gpu)
    if(LUX_EDITOR_BUILD_DESKTOP_TESTS)
        add_executable(editor_framework_pacing_test tests/pacing.cpp)
        target_link_libraries(editor_framework_pacing_test PRIVATE lux_editor_app lux::engine::engine_context_render)
        lux_editor_test_options(editor_framework_pacing_test)
        add_test(NAME framework.pacing COMMAND editor_framework_pacing_test)
        set_tests_properties(framework.pacing PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS "gpu;desktop")
        add_test(NAME framework.desktop COMMAND editor_framework_gpu_test --native)
        set_tests_properties(framework.desktop PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE LABELS "gpu;desktop")
    endif()
endif()
