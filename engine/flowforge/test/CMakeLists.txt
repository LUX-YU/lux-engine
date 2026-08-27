add_executable(
    flowforge_dynamism_test
    flowforge_dynamism_test.cpp
)
target_link_libraries(
    flowforge_dynamism_test
    PRIVATE authoring_flowforge
)
lux_classify_target(
    TARGET  flowforge_dynamism_test
    LAYER   AUTHORING
    PRODUCT TEST
    ROLE    DOMAIN
)
