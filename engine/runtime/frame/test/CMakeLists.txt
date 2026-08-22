add_executable(frame_coordinator_test frame_coordinator_test.cpp)

target_link_libraries(frame_coordinator_test
    PRIVATE
    runtime_frame
    lux::engine::ecs::render_core
)

if(MSVC)
    target_compile_options(frame_coordinator_test
        PRIVATE /permissive- /Zc:__cplusplus /Zc:preprocessor)
endif()

add_test(NAME phase9_render_frame_scope COMMAND frame_coordinator_test)
set_tests_properties(
    phase9_render_frame_scope
    PROPERTIES LABELS "phase9;cpu"
)
