add_executable(
    flowforge_dynamism_test
    flowforge_dynamism_test.cpp
)
target_link_libraries(
    flowforge_dynamism_test
    PRIVATE flowforge
)
lux_classify_target(
    TARGET  flowforge_dynamism_test
    LAYER   FUNCTION
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME flowforge_dynamism COMMAND flowforge_dynamism_test)

add_executable(
    flowforge_script_ability_node_test
    script_ability_node_test.cpp
)
target_link_libraries(
    flowforge_script_ability_node_test
    PRIVATE flowforge
)
lux_classify_target(
    TARGET  flowforge_script_ability_node_test
    LAYER   FUNCTION
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME flowforge_script_ability_node COMMAND flowforge_script_ability_node_test)
