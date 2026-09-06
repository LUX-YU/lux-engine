generate_visibility_header(
    ENABLE_MACRO_NAME LUX_ENGINE_SCENE_SCRIPT_DESCRIPTION_LIBRARY
    PUBLIC_MACRO_NAME LUX_ENGINE_SCENE_SCRIPT_DESCRIPTION_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_ENGINE_SCENE_SCRIPT_DESCRIPTION_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/scene/script_description/visibility.h
)
add_component(
    COMPONENT_NAME scene_script_description
    NAMESPACE lux::engine::scene
    OUTPUT_NAME lux_engine_scene_script_description
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptRuntimeAssembly.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptSystemDescription.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptSystemDescriptionCodec.cpp
)
component_include_directories(scene_script_description
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(scene_script_description PRIVATE LUX_ENGINE_SCENE_SCRIPT_DESCRIPTION_LIBRARY)
target_link_libraries(scene_script_description PUBLIC
    lux::engine::simulation::simulation_script
    lux::engine::world::identity
)
component_add_transitive_commands(scene_script_description
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script)"
    "find_package(lux-engine-world-identity REQUIRED COMPONENTS world_identity)"
)
lux_classify_target(TARGET scene_script_description LAYER SCENE PRODUCT RUNTIME ROLE DOMAIN)
add_executable(
    scene_script_description_test
    test/script_system_description_test.cpp
)
target_link_libraries(
    scene_script_description_test
    PRIVATE lux::engine::scene::scene_script_description
)
if(MSVC)
    target_compile_options(scene_script_description_test PRIVATE /UNDEBUG)
else()
    target_compile_options(scene_script_description_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  scene_script_description_test
    LAYER   SCENE
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(
    NAME scene_script_description_test
    COMMAND scene_script_description_test
)


install_components(
    PROJECT_NAME lux-engine-scene-script-description
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::scene
    COMPONENTS scene_script_description
)

add_executable(script_binding_authoring_test test/script_binding_authoring_test.cpp)
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(script_binding_authoring_test PRIVATE lux::engine::scene::scene_script_description
    lux::engine::simulation::simulation_script_cpp_static lux::engine::core::type_info nlohmann_json::nlohmann_json)
set(authoring_hints "${CMAKE_CURRENT_BINARY_DIR}/generated/script_binding_authoring_test/cpp_scripts/AuthoringBehavior/Behavior.AuthoringBehavior.bindings.json")
target_compile_definitions(script_binding_authoring_test PRIVATE LUX_AUTHORING_HINT_FILE="${authoring_hints}")
if(MSVC)
    target_compile_options(script_binding_authoring_test PRIVATE /UNDEBUG)
else()
    target_compile_options(script_binding_authoring_test PRIVATE -UNDEBUG)
endif()
lux_cpp_static_scripts(TARGET script_binding_authoring_test NAME AuthoringBehavior
    SOURCE ${PROJECT_SOURCE_DIR}/cmake/installed-consumers/cpp-generated-script/Behavior.hpp LOGICAL_PATH Behavior.hpp
    MODULE authoring DESCRIPTOR authoring OWNER installed_generated::Behavior SCOPE ENTITY
    SYMBOL_LEDGER ${PROJECT_SOURCE_DIR}/cmake/installed-consumers/cpp-generated-script/behavior.symbols.json
    ABILITY_TYPES lux::simulation::script::DelayAbility
    ABILITY_HEADERS lux/engine/simulation/scripting/cpp_static/ScriptDelayCoroutine.hpp
    DEPENDS simulation_script_script_abilities_generate)
lux_classify_target(TARGET script_binding_authoring_test LAYER SCENE PRODUCT TEST ROLE DOMAIN)
add_test(NAME script_binding_authoring_test COMMAND script_binding_authoring_test)
