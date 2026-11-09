find_package(lux-cxx REQUIRED COMPONENTS core compile_time container)

find_package(nlohmann_json CONFIG REQUIRED)

find_package(imgui REQUIRED COMPONENTS core)
find_package(Eigen3 CONFIG REQUIRED)

add_component(
    COMPONENT_NAME lux_editor_project
    NAMESPACE lux::engine::editor
    STATIC
    SOURCE_FILES
        project/src/EditorLayout.cpp
        project/src/ProjectManifest.cpp
        project/src/ProjectErrors.cpp
)

component_include_directories(
    lux_editor_project
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/project/include
    INSTALL_TIME include
)

target_link_libraries(
    lux_editor_project
    PUBLIC
        lux::cxx::compile_time
        lux::engine::resource::identity
        lux::engine::scene::scene_description
        lux::engine::core::error
    PRIVATE
        nlohmann_json::nlohmann_json
        lux::engine::platform::platform_file
        lux::engine::plugins::plugin_identity
)

component_add_transitive_commands(
    lux_editor_project
    "find_package(lux-engine-core REQUIRED COMPONENTS error)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS identity)"
    "find_package(lux-engine-scene-description REQUIRED COMPONENTS scene_description)"
    "find_package(nlohmann_json CONFIG REQUIRED)"
    "find_package(lux-engine-plugins REQUIRED COMPONENTS plugin_identity)"
    "find_package(lux-engine-platform REQUIRED COMPONENTS platform_file)"
)

lux_classify_target(TARGET lux_editor_project LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)

add_component(
    COMPONENT_NAME lux_editor_context
    NAMESPACE lux::engine::editor
    STATIC
    SOURCE_FILES
        context/src/ContextErrors.cpp
        context/src/EditorContext.cpp
        context/src/EditorServices.cpp
        context/src/EditorUiRegistry.cpp
        context/src/SceneProfileRegistry.cpp
        context/src/EditorComposition.cpp
)

component_include_directories(
    lux_editor_context
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/context/include
    INSTALL_TIME include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/context/sinclude
)

target_link_libraries(
    lux_editor_context
    PUBLIC
        lux::cxx::core
        lux::cxx::compile_time
        lux::engine::core::object
        lux::engine::core::error
        lux::engine::resource::asset
        lux::engine::scene::scene_asset
        lux::engine::core::core_execution
        lux::engine::engine_runtime
        lux::engine::process::process_assets
    PRIVATE
        lux::engine::plugins::plugin_runtime
)

component_add_internal_dependencies(lux_editor_context lux_editor_project)

component_add_transitive_commands(
    lux_editor_context
    "find_package(lux-cxx REQUIRED COMPONENTS core compile_time)"
    "find_package(lux-engine-core REQUIRED COMPONENTS object error)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-engine-core-execution REQUIRED COMPONENTS core_execution)"
    "find_package(lux-engine-scene-asset REQUIRED COMPONENTS scene_asset)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS engine_runtime)"
    "find_package(lux-engine-process-assets REQUIRED COMPONENTS process_assets)"
    "find_package(lux-engine-plugins REQUIRED COMPONENTS plugin_runtime)"
)

lux_classify_target(TARGET lux_editor_context LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)

add_component(
    COMPONENT_NAME lux_editor_ui
    NAMESPACE lux::engine::editor
    STATIC
    SOURCE_FILES
        ui/src/EditorUiErrors.cpp
        ui/src/EditorUiFactories.cpp
        ui/src/EditorMenuComposition.cpp
        ui/src/ValueEditing.cpp
        window/src/EditorWindow.cpp
)

component_include_directories(
    lux_editor_ui
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/include
        ${CMAKE_CURRENT_SOURCE_DIR}/window/include
    INSTALL_TIME include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ui/pinclude
)

target_link_libraries(
    lux_editor_ui
    PUBLIC
        lux::engine::function::ui
        lux::engine::function::input
        lux::engine::platform::window
        imgui::core
        Eigen3::Eigen
    PRIVATE lux::cxx::container
)

component_add_internal_dependencies(lux_editor_ui lux_editor_context)

component_add_transitive_commands(
    lux_editor_ui
    "find_package(lux-engine-function REQUIRED COMPONENTS ui input)"
    "find_package(lux-engine-platform REQUIRED COMPONENTS window)"
    "find_package(lux-cxx REQUIRED COMPONENTS container)"
    "find_package(imgui REQUIRED COMPONENTS core)"
    "find_package(Eigen3 CONFIG REQUIRED)"
)

lux_classify_target(TARGET lux_editor_ui LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)

add_component(
    COMPONENT_NAME lux_editor_app
    NAMESPACE lux::engine::editor
    STATIC
    SOURCE_FILES
        app/src/AppErrors.cpp
        app/src/EditorApplication.cpp
        app/src/ProjectUiMount.cpp
        app/src/EditorProject.cpp
        app/src/ProjectCreationPane.cpp
        app/src/ProjectPreparation.cpp
        app/src/ProjectScenes.cpp
)

component_include_directories(
    lux_editor_app
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/app/include
    INSTALL_TIME include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/app/pinclude
        ${CMAKE_CURRENT_SOURCE_DIR}/context/sinclude
)

target_link_libraries(
    lux_editor_app
    PUBLIC
        lux::engine::function::render
        lux::engine::systems::system_render
        lux::engine::plugins::plugin_runtime
    PRIVATE
        lux::engine::core::object
)

component_add_internal_dependencies(lux_editor_app lux_editor_context lux_editor_ui)

# Finite Scene/viewport Sender instantiations need the extended COFF format under ASan.
target_compile_options(lux_editor_app PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)

component_add_transitive_commands(
    lux_editor_app
    "find_package(lux-engine-plugins REQUIRED COMPONENTS plugin_runtime)"
    "find_package(lux-engine-core REQUIRED COMPONENTS object)"
    "find_package(lux-engine-systems-render REQUIRED COMPONENTS system_render)"
    "find_package(lux-engine-render REQUIRED COMPONENTS render)"
)

lux_classify_target(TARGET lux_editor_app LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)

add_component(
    COMPONENT_NAME lux_editor_scene_document
    NAMESPACE lux::engine::editor
    STATIC
    SOURCE_FILES
        scene/document/src/SceneDocument.cpp
        scene/document/src/SceneEditing.cpp
)

component_include_directories(
    lux_editor_scene_document
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/scene/document/include
    INSTALL_TIME include
)

target_include_directories(lux_editor_scene_document PRIVATE scene/document/pinclude)

target_link_libraries(lux_editor_scene_document
    PUBLIC
        lux::engine::scene::scene_asset
        lux::engine::scene::scene_composition
        lux::engine::scene::scene_world_loading
        lux::engine::ecs::ecs_hierarchy
        lux::engine::core::object
)

component_add_transitive_commands(
    lux_editor_scene_document
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(lux-engine-scene-world-loading REQUIRED COMPONENTS scene_world_loading)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS ecs_hierarchy)"
    "find_package(lux-engine-core REQUIRED COMPONENTS object)"
    "find_package(lux-engine-scene-asset REQUIRED COMPONENTS scene_asset)"
)

lux_classify_target(TARGET lux_editor_scene_document LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)

component_add_internal_dependencies(lux_editor_app lux_editor_scene_document)

target_link_libraries(lux_editor_app PUBLIC lux::engine::scene::scene_asset)

component_add_transitive_commands(lux_editor_app "find_package(lux-engine-scene-asset REQUIRED COMPONENTS scene_asset)")


# Concrete authoring preset; the host/context and generic scene provider do not reference it.
add_component(
    COMPONENT_NAME lux_editor_scene_profiles
    NAMESPACE lux::engine::editor
    STATIC
    SOURCE_FILES
        scene/profiles/src/SceneProfile3D.cpp
        scene/profiles/src/ComponentEditors3D.cpp
        scene/profiles/src/RenderConfigurationEditor.cpp
)

component_include_directories(
    lux_editor_scene_profiles
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/scene/profiles/include
    INSTALL_TIME include
)

target_link_libraries(
    lux_editor_scene_profiles
    PRIVATE
        lux::engine::systems::system_render
        lux::engine::plugins::plugin_runtime
        lux::engine::scene::scene_transform
        lux::engine::scene::scene_world_loading
)

component_add_internal_dependencies(lux_editor_scene_profiles lux_editor_context lux_editor_ui)

target_include_directories(lux_editor_scene_profiles PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/scene/profiles/pinclude)

include(${CMAKE_CURRENT_SOURCE_DIR}/ui/cmake/editor_value_codegen.cmake)
editor_target_add_value_codegen(
    TARGET lux_editor_scene_profiles
    NAME editor_builtin_values
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/scene/profiles/src/ComponentEditors3D.cpp
    HEADER ${CMAKE_CURRENT_SOURCE_DIR}/scene/profiles/pinclude/lux/engine/editor/BuiltinValueTypes.hpp
    LOGICAL_PATH lux/engine/editor/BuiltinValueTypes.hpp
    POLICY ${CMAKE_CURRENT_SOURCE_DIR}/scene/profiles/ValueEditing.json
)

target_link_libraries(lux_editor_scene_profiles
    PRIVATE
        imgui::core
        lux::engine::function::ui
)

component_add_transitive_commands(lux_editor_scene_profiles
    "find_package(imgui REQUIRED COMPONENTS core)"
    "find_package(lux-engine-function REQUIRED COMPONENTS ui)"
)

component_add_transitive_commands(
    lux_editor_scene_profiles
    "find_package(lux-engine-systems-render REQUIRED COMPONENTS system_render)"
    "find_package(lux-engine-scene-transform REQUIRED COMPONENTS scene_transform)"
    "find_package(lux-engine-scene-world-loading REQUIRED COMPONENTS scene_world_loading)"
)

lux_classify_target(TARGET lux_editor_scene_profiles LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)

add_component(
    COMPONENT_NAME lux_editor_assets
    NAMESPACE lux::engine::editor
    STATIC
    SOURCE_FILES
        assets/src/ProjectAssets.cpp
        assets/src/AssetImport.cpp
)

component_include_directories(
    lux_editor_assets
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/assets/include
    INSTALL_TIME include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/assets/pinclude
)

component_add_internal_dependencies(lux_editor_assets lux_editor_context)

target_link_libraries(lux_editor_assets
    PRIVATE
        nlohmann_json::nlohmann_json
        lux::cxx::algorithm
        lux::engine::platform::platform_file
)

component_add_transitive_commands(lux_editor_assets
    "find_package(lux-engine-platform REQUIRED COMPONENTS platform_file)"
    "find_package(nlohmann_json CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS algorithm)"
)

lux_classify_target(TARGET lux_editor_assets LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)

component_add_internal_dependencies(lux_editor_app lux_editor_assets)

component_add_internal_dependencies(lux_editor_scene_profiles lux_editor_assets)

target_link_libraries(lux_editor_scene_profiles PRIVATE lux::engine::scene::scene_mesh_query)
component_add_transitive_commands(lux_editor_scene_profiles
    "find_package(lux-engine-scene-mesh-query REQUIRED COMPONENTS scene_mesh_query)"
)

add_component(
    COMPONENT_NAME lux_editor_asset_tools
    NAMESPACE lux::engine::editor
    STATIC
    SOURCE_FILES assets/tools/src/AssetBrowserPane.cpp
)

component_include_directories(
    lux_editor_asset_tools
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/assets/tools/include
    INSTALL_TIME include
)

component_add_internal_dependencies(lux_editor_asset_tools lux_editor_assets lux_editor_ui)

target_link_libraries(lux_editor_asset_tools PRIVATE imgui::core)

component_add_transitive_commands(lux_editor_asset_tools "find_package(imgui REQUIRED COMPONENTS core)")

lux_classify_target(TARGET lux_editor_asset_tools LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)

add_component(
    COMPONENT_NAME lux_editor_model_import
    NAMESPACE lux::engine::editor
    STATIC
    SOURCE_FILES assets/model/src/ModelImporter.cpp assets/model/src/ModelImportEditor.cpp
)

component_include_directories(
    lux_editor_model_import
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/assets/model/include
    INSTALL_TIME include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/assets/model/pinclude
)

component_add_internal_dependencies(lux_editor_model_import lux_editor_assets lux_editor_asset_tools)

target_link_libraries(lux_editor_model_import
    PUBLIC lux::engine::toolchain::toolchain_model_cooker
    PRIVATE nlohmann_json::nlohmann_json lux::cxx::algorithm
)

component_add_transitive_commands(lux_editor_model_import
    "find_package(lux-engine-toolchain-asset REQUIRED COMPONENTS toolchain_model_cooker)"
    "find_package(nlohmann_json CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS algorithm)"
)

editor_target_add_value_codegen(
    TARGET lux_editor_model_import
    NAME editor_model_import_values
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/assets/model/src/ModelImportEditor.cpp
    HEADER ${CMAKE_CURRENT_SOURCE_DIR}/assets/model/pinclude/lux/engine/editor/ModelImportValues.hpp
    LOGICAL_PATH lux/engine/editor/ModelImportValues.hpp
    POLICY ${CMAKE_CURRENT_SOURCE_DIR}/assets/model/ValueEditing.json
)

lux_classify_target(TARGET lux_editor_model_import LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)


add_component(
    COMPONENT_NAME lux_editor_scene_tools
    NAMESPACE lux::engine::editor
    STATIC
    SOURCE_FILES
        scene/tools/src/SceneCreationPane.cpp
        scene/tools/src/ModelPlacement.cpp
        scene/tools/src/MeshQueryAssets.cpp
        scene/tools/src/SceneHierarchyPane.cpp
        scene/tools/src/SceneInspectorPane.cpp
        scene/tools/src/SceneTransformGizmo.cpp
)

component_include_directories(
    lux_editor_scene_tools
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/scene/tools/include
    INSTALL_TIME include
)

component_add_internal_dependencies(lux_editor_scene_tools lux_editor_app lux_editor_scene_document)

target_include_directories(lux_editor_scene_tools PRIVATE scene/tools/pinclude)

target_link_libraries(lux_editor_scene_tools PRIVATE
    imgui::core
    lux::engine::scene::scene_mesh_query
    lux::engine::plugins::plugin_runtime
    lux::engine::ecs::ecs_transform
    lux::engine::ecs::ecs_visual
)

component_add_transitive_commands(lux_editor_scene_tools
    "find_package(imgui REQUIRED COMPONENTS core)"
    "find_package(lux-engine-scene-mesh-query REQUIRED COMPONENTS scene_mesh_query)"
    "find_package(lux-engine-plugins REQUIRED COMPONENTS plugin_runtime)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS ecs_transform ecs_visual)"
)

lux_classify_target(TARGET lux_editor_scene_tools LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)

add_executable(lux_editor app/main.cpp)

target_include_directories(lux_editor PRIVATE app/pinclude)

target_link_libraries(
    lux_editor
    PRIVATE
        lux_editor_app
        lux_editor_scene_profiles
        lux_editor_scene_tools
        lux_editor_assets
        lux_editor_asset_tools
        lux_editor_model_import
        lux::engine::plugins::plugin_runtime
        lux::engine::platform::platform_process
)

lux_classify_target(TARGET lux_editor LAYER EDITOR PRODUCT EDITOR ROLE EXECUTABLE)

install(TARGETS lux_editor RUNTIME DESTINATION bin)

if(WIN32)
    install(FILES $<TARGET_RUNTIME_DLLS:lux_editor> DESTINATION bin)
endif()

lux_engine_install_components(
    PROJECT_NAME lux-engine-editor-framework
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor
    COMPONENTS
        lux_editor_scene_document
        lux_editor_project
        lux_editor_context
        lux_editor_ui
        lux_editor_app
        lux_editor_scene_profiles
        lux_editor_scene_tools
        lux_editor_assets
        lux_editor_asset_tools
        lux_editor_model_import
)

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    foreach(component project context ui app)
        string(TOUPPER "${component}" macro)

        set(target editor_${component}_errors_test)

        add_executable(${target} tests/error_catalog.cpp)

        target_compile_definitions(${target} PRIVATE LUX_TEST_${macro}_ERRORS=1)

        target_link_libraries(${target} PRIVATE lux_editor_${component})

        lux_editor_test_options(${target})

        add_test(NAME framework.${component}_errors COMMAND ${target})

        add_test(NAME framework.${component}_errors_collision COMMAND ${target} --collision)
    endforeach()

    add_executable(editor_project_ui_mount_test tests/project_ui_mount.cpp)

    target_include_directories(editor_project_ui_mount_test PRIVATE app/pinclude)

    target_link_libraries(editor_project_ui_mount_test PRIVATE lux_editor_app)

    lux_editor_test_options(editor_project_ui_mount_test)

    add_test(NAME framework.project_ui_mount COMMAND editor_project_ui_mount_test)

    add_executable(editor_scene_document_test tests/scene_document.cpp)

    target_link_libraries(editor_scene_document_test PRIVATE
        lux_editor_scene_document
        lux_editor_context
        lux::engine::plugins::plugin_runtime
        lux::engine::ecs::ecs_transform
    )

    target_include_directories(editor_scene_document_test PRIVATE context/sinclude)

    lux_editor_test_options(editor_scene_document_test)

    add_test(NAME framework.scene_document COMMAND editor_scene_document_test)

    add_executable(editor_world_domains_test tests/world_domains.cpp)

    target_link_libraries(editor_world_domains_test PRIVATE
        lux_editor_scene_document
        lux_editor_context
        lux::engine::plugins::plugin_runtime
        lux::engine::ecs::ecs_transform
        lux::engine::spatial::spatial3d_index
    )

    target_include_directories(editor_world_domains_test PRIVATE context/sinclude)

    lux_editor_test_options(editor_world_domains_test)

    add_test(NAME framework.world_domains COMMAND editor_world_domains_test "${CMAKE_CURRENT_BINARY_DIR}/world-domains")

    set_tests_properties(framework.world_domains PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

    add_executable(editor_scene_profiles_test tests/scene_profiles.cpp)

    target_include_directories(editor_scene_profiles_test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/scene/profiles/pinclude
            ${CMAKE_CURRENT_BINARY_DIR}/value-editors/editor_builtin_values
    )


    target_link_libraries(
        editor_scene_profiles_test
        PRIVATE
            lux::engine::scene::scene_asset
            lux_editor_scene_profiles
            lux::engine::engine_runtime
            lux::engine::systems::system_render
            lux::engine::plugins::plugin_runtime
            lux::engine::scene::scene_transform
            lux::engine::scene::scene_world_loading
    )

    lux_editor_test_options(editor_scene_profiles_test)

    add_test(
        NAME framework.scene_profiles
        COMMAND
            editor_scene_profiles_test
            "${CMAKE_BINARY_DIR}/share/lux-engine/plugins/catalog.json"
            "${CMAKE_BINARY_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}/profile-data"
    )

    add_executable(editor_plugin_selection_test tests/plugin_selection.cpp)

    target_link_libraries(
        editor_plugin_selection_test
        PRIVATE
            lux_editor_project
            lux::engine::plugins::plugin_runtime
            nlohmann_json::nlohmann_json
    )

    lux_editor_test_options(editor_plugin_selection_test)

    add_test(
        NAME framework.plugin_selection
        COMMAND
            editor_plugin_selection_test
            "${CMAKE_CURRENT_BINARY_DIR}/identity-data"
    )

    add_executable(editor_project_manifest_test tests/project_manifest.cpp)

    target_link_libraries(editor_project_manifest_test PRIVATE lux_editor_project)

    lux_editor_test_options(editor_project_manifest_test)

    add_test(
        NAME framework.project_manifest
        COMMAND
            editor_project_manifest_test
            ${CMAKE_CURRENT_BINARY_DIR}/manifest-data
    )

    if(CMAKE_VERSION VERSION_LESS 3.27)
        message(
            FATAL_ERROR "Framework isolation tests require CMake 3.27 or newer for first-configure File API queries"
        )

    endif()

    cmake_file_api(QUERY API_VERSION 1 CODEMODEL 2)

    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    add_test(
        NAME framework.isolation
        COMMAND
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_isolation.py"
            "${CMAKE_BINARY_DIR}"
    )

    add_executable(editor_framework_component_test tests/context_component.cpp)

    target_link_libraries(editor_framework_component_test PRIVATE lux_editor_context)

    lux_editor_test_options(editor_framework_component_test)

    add_test(NAME framework.context_component COMMAND editor_framework_component_test)

    add_executable(editor_asset_catalog_test tests/asset_catalog.cpp)

    target_link_libraries(editor_asset_catalog_test
        PRIVATE lux_editor_model_import lux::engine::engine_runtime lux::engine::plugins::plugin_runtime nlohmann_json::nlohmann_json
    )

    target_include_directories(editor_asset_catalog_test PRIVATE tests context/sinclude)

    editor_target_add_value_codegen(
        TARGET editor_asset_catalog_test
        NAME editor_test_import_values
        SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/tests/asset_catalog.cpp
        HEADER ${CMAKE_CURRENT_SOURCE_DIR}/tests/AssetImportValues.hpp
        LOGICAL_PATH lux/engine/editor/AssetImportValues.hpp
        POLICY ${CMAKE_CURRENT_SOURCE_DIR}/tests/AssetImportEditing.json
    )

    lux_editor_test_options(editor_asset_catalog_test)

    add_test(NAME framework.asset_catalog COMMAND editor_asset_catalog_test ${CMAKE_CURRENT_BINARY_DIR}/asset-import-fixture)

    set_tests_properties(framework.asset_catalog PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)

    add_executable(editor_framework_context_test tests/context.cpp)

    target_link_libraries(
        editor_framework_context_test
        PRIVATE
            lux_editor_context
            lux::engine::engine_runtime
            lux::engine::plugins::plugin_runtime
    )

    target_include_directories(editor_framework_context_test PRIVATE context/sinclude)

    target_include_directories(editor_scene_profiles_test PRIVATE context/sinclude)

    lux_editor_test_options(editor_framework_context_test)

    add_test(NAME framework.context COMMAND editor_framework_context_test ${CMAKE_CURRENT_BINARY_DIR}/test-data)

    add_test(NAME framework.error_registration_conflict COMMAND editor_framework_context_test --registration-conflict)

    add_executable(editor_context_lifetime_test tests/context_lifetime.cpp)

    target_link_libraries(
        editor_context_lifetime_test
        PRIVATE
            lux_editor_context
            lux::engine::engine_runtime
            lux::engine::plugins::plugin_runtime
    )

    target_include_directories(editor_context_lifetime_test PRIVATE context/sinclude)

    lux_editor_test_options(editor_context_lifetime_test)

    add_test(NAME framework.context_lifetime COMMAND editor_context_lifetime_test)

    set_tests_properties(framework.context_lifetime PROPERTIES TIMEOUT 15)

    add_executable(editor_framework_input_test tests/input.cpp)

    target_include_directories(editor_framework_input_test PRIVATE ui/pinclude)

    target_link_libraries(editor_framework_input_test PRIVATE lux_editor_ui)

    lux_editor_test_options(editor_framework_input_test)

    add_test(NAME framework.input_conversion COMMAND editor_framework_input_test)

    add_executable(editor_framework_ui_test tests/ui.cpp tests/callback_contract.cpp)

    target_link_libraries(editor_framework_ui_test PRIVATE lux_editor_ui)

    lux_editor_test_options(editor_framework_ui_test)

    add_test(NAME framework.ui COMMAND editor_framework_ui_test)

    add_test(
        NAME framework.callback_contract
        COMMAND
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/callback_contract.py"
            "${CMAKE_BINARY_DIR}"
    )

    set_tests_properties(framework.context framework.ui PROPERTIES TIMEOUT 60)
endif()

if(LUX_EDITOR_BUILD_GPU_TESTS)
    add_executable(editor_framework_transport_test tests/transport.cpp)
    target_link_libraries(editor_framework_transport_test PRIVATE lux::engine::function::render_extensions)

    target_include_directories(editor_framework_transport_test PRIVATE app/pinclude)

    target_link_libraries(
        editor_framework_transport_test
        PRIVATE
            lux_editor_app
            lux::engine::systems::system_render
    )

    lux_editor_test_options(editor_framework_transport_test)

    add_executable(editor_framework_gpu_test tests/gpu.cpp)

    target_link_libraries(
        editor_framework_gpu_test
        PRIVATE
            lux_editor_app
            lux::engine::systems::system_render
            lux::engine::plugins::plugin_runtime
    )

    target_include_directories(editor_framework_gpu_test PRIVATE app/pinclude)

    lux_editor_test_options(editor_framework_gpu_test)

    add_test(NAME framework.gpu COMMAND editor_framework_transport_test)

    set_tests_properties(framework.gpu PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE LABELS gpu)

    if(LUX_EDITOR_BUILD_DESKTOP_TESTS)
        add_executable(editor_scene_workflow_test tests/scene_workflow.cpp)

        target_link_libraries(editor_scene_workflow_test PRIVATE
            lux_editor_app lux_editor_scene_profiles lux_editor_scene_tools lux_editor_model_import lux::engine::systems::system_render
            lux::engine::scene::scene_mesh_query
        )

        target_include_directories(editor_scene_workflow_test PRIVATE
            app/pinclude
            ${CMAKE_CURRENT_SOURCE_DIR}/../engine/systems/render/pinclude
        )

        lux_editor_test_options(editor_scene_workflow_test)

        add_test(NAME framework.scene_workflow COMMAND editor_scene_workflow_test
            "${CMAKE_BINARY_DIR}/share/lux-engine/plugins/catalog.json"
            "${CMAKE_BINARY_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}/scene-workflow"
            --render-only
        )

        set_tests_properties(framework.scene_workflow PROPERTIES TIMEOUT 150 RUN_SERIAL TRUE LABELS "gpu;desktop")

        add_test(
            NAME framework.scene_failed_close
            COMMAND editor_scene_workflow_test
                "${CMAKE_BINARY_DIR}/share/lux-engine/plugins/catalog.json"
                "${CMAKE_BINARY_DIR}"
                "${CMAKE_CURRENT_BINARY_DIR}/scene-workflow"
                --fault-close --render-only
        )

        set_tests_properties(framework.scene_failed_close PROPERTIES TIMEOUT 150 RUN_SERIAL TRUE LABELS "gpu;desktop")

        add_executable(editor_host_shutdown_test tests/shutdown.cpp)

        target_link_libraries(editor_host_shutdown_test PRIVATE lux_editor_app)

        target_include_directories(editor_host_shutdown_test PRIVATE app/pinclude)

        lux_editor_test_options(editor_host_shutdown_test)

        add_test(
            NAME framework.shutdown_posted
            COMMAND
                editor_host_shutdown_test
                "${CMAKE_CURRENT_BINARY_DIR}/shutdown.luxproj"
        )

        set_tests_properties(framework.shutdown_posted PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE LABELS "gpu;desktop")

        add_executable(editor_public_host_test tests/installed/host.cpp)

        target_link_libraries(editor_public_host_test PRIVATE lux_editor_app lux::engine::systems::system_render)

        lux_editor_test_options(editor_public_host_test)

        add_test(NAME framework.public_host COMMAND editor_public_host_test "${CMAKE_CURRENT_BINARY_DIR}/public-host")

        set_tests_properties(framework.public_host PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE LABELS "gpu;desktop")

        add_executable(editor_project_requests_test tests/project_requests.cpp)

        target_link_libraries(editor_project_requests_test PRIVATE lux_editor_app lux::engine::systems::system_render)

        target_include_directories(editor_project_requests_test PRIVATE app/pinclude)

        lux_editor_test_options(editor_project_requests_test)

        add_test(
            NAME framework.project_requests
            COMMAND
                editor_project_requests_test
                "${CMAKE_CURRENT_BINARY_DIR}/request-data"
        )

        set_tests_properties(framework.project_requests PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE LABELS "gpu;desktop")

        add_executable(editor_project_lifecycle_test tests/project_lifecycle.cpp)

        target_link_libraries(editor_project_lifecycle_test PRIVATE lux_editor_app lux::engine::systems::system_render)

        target_include_directories(editor_project_lifecycle_test PRIVATE app/pinclude)

        lux_editor_test_options(editor_project_lifecycle_test)

        foreach(mode global maintenance)
            add_test(
                NAME framework.project_${mode}
                COMMAND
                    editor_project_lifecycle_test
                    ${mode}
                    "${CMAKE_CURRENT_BINARY_DIR}/lifecycle-${mode}"
            )

            set_tests_properties(framework.project_${mode} PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE LABELS "gpu;desktop")
        endforeach()

        add_executable(editor_framework_pacing_test tests/pacing.cpp)

        target_link_libraries(editor_framework_pacing_test PRIVATE lux_editor_app lux::engine::systems::system_render)

        lux_editor_test_options(editor_framework_pacing_test)

        add_test(NAME framework.pacing COMMAND editor_framework_pacing_test)

        set_tests_properties(framework.pacing PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS "gpu;desktop")

        add_test(
            NAME framework.desktop
            COMMAND
                editor_framework_gpu_test
                --native
                "${CMAKE_BINARY_DIR}/share/lux-engine/plugins/catalog.json"
                "${CMAKE_BINARY_DIR}"
        )

        set_tests_properties(framework.desktop PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE LABELS "gpu;desktop")
    endif()
endif()

if(BUILD_TESTING)
    add_executable(engine_render_ui_test tests/render_ui.cpp)
    target_include_directories(engine_render_ui_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/app/pinclude"
        "${CMAKE_CURRENT_SOURCE_DIR}/../engine/systems/render/test"
        "${CMAKE_CURRENT_SOURCE_DIR}/../modules/function/render/test"
        "${CMAKE_CURRENT_SOURCE_DIR}/../modules/function/render/sinclude"
    )
    target_link_libraries(engine_render_ui_test PRIVATE
        lux_editor_app lux::engine::systems::system_render lux::engine::function::render_extensions
        lux::engine::systems::system_render
    )
    add_test(NAME engine.render_ui COMMAND engine_render_ui_test)
    add_test(NAME engine.render_ui32 COMMAND engine_render_ui_test uint32)
    set_tests_properties(engine.render_ui engine.render_ui32 PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
endif()
