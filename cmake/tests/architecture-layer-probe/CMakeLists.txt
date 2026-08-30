cmake_minimum_required(VERSION 3.22)
project(lux_architecture_layer_probe LANGUAGES CXX)

if(NOT LUX_ENGINE_SOURCE_DIR OR NOT CONSUMER_LAYER OR NOT DEPENDENCY_LAYER)
    message(FATAL_ERROR "LUX_ENGINE_SOURCE_DIR, CONSUMER_LAYER and DEPENDENCY_LAYER are required")
endif()

include("${LUX_ENGINE_SOURCE_DIR}/cmake/TargetArchitecture.cmake")

add_library(probe_dependency INTERFACE)
lux_classify_target(
    TARGET  probe_dependency
    LAYER   ${DEPENDENCY_LAYER}
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_library(probe_consumer INTERFACE)
target_link_libraries(probe_consumer INTERFACE probe_dependency)
lux_classify_target(
    TARGET  probe_consumer
    LAYER   ${CONSUMER_LAYER}
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

lux_validate_target_dag()
