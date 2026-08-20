add_executable(input_system_test input_system_test.cpp)
target_link_libraries(input_system_test PRIVATE input lux::engine::platform::window)

add_executable(input_interactive_test input_interactive_test.cpp)
target_link_libraries(
    input_interactive_test
    PRIVATE
        input
        lux::cxx::core
        lux::engine::platform::window
)
