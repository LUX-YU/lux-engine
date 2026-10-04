add_component(COMPONENT_NAME engine_context NAMESPACE lux::engine STATIC SOURCE_FILES src/EngineContext.cpp)
component_include_directories(engine_context BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(engine_context PUBLIC lux::engine::process::process_execution lux::engine::resource::asset lux::engine::scene::scene_composition)
component_add_transitive_commands(engine_context
    "find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)")
lux_classify_target(TARGET engine_context LAYER ENGINE PRODUCT RUNTIME ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-context VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine COMPONENTS engine_context)
if(TARGET render_runtime)
    add_subdirectory(rendering)
endif()
if(BUILD_TESTING)
    add_executable(service_tasks_test test/service_tasks.cpp)
    target_link_libraries(service_tasks_test PRIVATE lux::engine::core::services lux::engine::process::process_execution)
    if(MSVC)
        target_compile_options(service_tasks_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(service_tasks_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME services.tasks COMMAND service_tasks_test)
    set_tests_properties(services.tasks PROPERTIES TIMEOUT 45)
endif()
