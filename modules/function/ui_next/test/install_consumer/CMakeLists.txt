cmake_minimum_required(VERSION 3.25)
project(lux_ui_next_installed_consumer LANGUAGES CXX)

enable_testing()

find_package(lux-engine-core REQUIRED COMPONENTS object)
find_package(
    lux-engine-function
    REQUIRED
    COMPONENTS ui_next ui_next_drawdata
)

add_executable(lux_ui_next_installed_consumer main.cpp)
target_compile_features(lux_ui_next_installed_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_ui_next_installed_consumer
    PRIVATE
        lux::engine::core::object
        lux::engine::function::ui_next
        lux::engine::function::ui_next_drawdata
)

add_test(
    NAME lux_ui_next_installed_consumer
    COMMAND lux_ui_next_installed_consumer
)
set_tests_properties(
    lux_ui_next_installed_consumer
    PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:lux::engine::function::ui_next>"
)
