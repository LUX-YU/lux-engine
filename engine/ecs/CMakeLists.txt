foreach(component IN ITEMS
    core
    schedule
    schema
    schema_reflection
    snapshot
    persistence
    hierarchy
    transform
)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${component}/CMakeLists.txt")
        add_subdirectory(${component})
    endif()
endforeach()

set(_lux_ecs_components)
foreach(component IN ITEMS
    core
    schedule
    schema
    schema_reflection
    snapshot
    persistence
    hierarchy
    transform
)
    if(TARGET ${component})
        list(APPEND _lux_ecs_components ${component})
    endif()
endforeach()

if(_lux_ecs_components)
    install_components(
        PROJECT_NAME lux-engine-ecs
        VERSION      ${PROJECT_VERSION}
        NAMESPACE    lux::engine::ecs
        COMPONENTS   ${_lux_ecs_components}
    )
endif()

add_executable(ecs_l1_benchmark test/l1_benchmark.cpp)
target_link_libraries(
    ecs_l1_benchmark
    PRIVATE
        lux::engine::ecs::schedule
        lux::engine::ecs::snapshot
        lux::engine::ecs::persistence
        lux::engine::core::object
)
lux_classify_target(
    TARGET  ecs_l1_benchmark
    LAYER   ECS
    PRODUCT TEST
    ROLE    FOUNDATION
)
