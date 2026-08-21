add_executable(input_system_test input_system_test.cpp)
target_link_libraries(input_system_test PRIVATE input)

add_executable(input_public_link_test input_public_link_test.cpp)
target_link_libraries(input_public_link_test PRIVATE input)

add_executable(input_interactive_test input_interactive_test.cpp)
target_link_libraries(
    input_interactive_test
    PRIVATE
        input
        lux::cxx::core
        lux::engine::platform::window
)

add_executable(input_platform_translation_test input_platform_translation_test.cpp)
target_include_directories(
    input_platform_translation_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../pinclude
)
target_link_libraries(
    input_platform_translation_test
    PRIVATE
        input
        lux::engine::platform::window
)
