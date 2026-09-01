find_package(lux-cxx REQUIRED COMPONENTS compile_time)

add_interface_component(
    COMPONENT_NAME scene_system
    NAMESPACE      lux::engine::scene
)
component_include_directories(
    scene_system
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include
)
target_link_libraries(
    scene_system
    INTERFACE
        lux::engine::scene::scene_description
        lux::engine::system::domain_system_description
        lux::engine::core::object
        lux::engine::core::serialization
        lux::cxx::compile_time
)
component_add_transitive_commands(
    scene_system
    "find_package(lux-engine-scene-description REQUIRED COMPONENTS scene_description)"
    "find_package(lux-engine-domain-system-description REQUIRED COMPONENTS domain_system_description)"
    "find_package(lux-engine-core REQUIRED COMPONENTS object serialization)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
)
lux_classify_target(TARGET scene_system LAYER SCENE PRODUCT RUNTIME ROLE FOUNDATION)

add_executable(scene_system_contract_test test/scene_system_contract_test.cpp)
target_link_libraries(scene_system_contract_test PRIVATE lux::engine::scene::scene_system)
if(MSVC)
    target_compile_options(scene_system_contract_test PRIVATE /UNDEBUG)
else()
    target_compile_options(scene_system_contract_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(TARGET scene_system_contract_test LAYER SCENE PRODUCT TEST ROLE FOUNDATION)
add_test(NAME scene_system_contract_test COMMAND scene_system_contract_test)

install_components(
    PROJECT_NAME lux-engine-scene-system
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::scene
    COMPONENTS   scene_system
)
