add_executable(scene_services_test scene_services_test.cpp)
target_link_libraries(scene_services_test PRIVATE core)

add_test(NAME scene_services_test COMMAND scene_services_test)
set_tests_properties(scene_services_test PROPERTIES LABELS "cpu" TIMEOUT 30)

add_executable(schedule_topology_test schedule_topology_test.cpp)
target_link_libraries(schedule_topology_test PRIVATE core)

add_test(NAME schedule_topology_test COMMAND schedule_topology_test)
set_tests_properties(schedule_topology_test PROPERTIES LABELS "cpu" TIMEOUT 30)

add_executable(component_type_catalog_test component_type_catalog_test.cpp)
target_link_libraries(component_type_catalog_test PRIVATE core)

add_test(NAME component_type_catalog_test COMMAND component_type_catalog_test)
set_tests_properties(
    component_type_catalog_test
    PROPERTIES LABELS "cpu" TIMEOUT 30
)

add_executable(persistent_entity_index_test persistent_entity_index_test.cpp)
target_link_libraries(persistent_entity_index_test PRIVATE core)

add_test(NAME persistent_entity_index_test COMMAND persistent_entity_index_test)
set_tests_properties(
    persistent_entity_index_test
    PROPERTIES LABELS "cpu" TIMEOUT 30
)

# R1a:反应式抽取改造的前置验证。它断言的是 EnTT reactive storage 的性质 ——
# 选型建立在这些性质上,所以它们必须被断言而不是被相信。
add_executable(reactive_storage_probe reactive_storage_probe.cpp)
target_link_libraries(reactive_storage_probe PRIVATE core)

add_test(NAME reactive_storage_probe COMMAND reactive_storage_probe)
set_tests_properties(reactive_storage_probe PROPERTIES LABELS "cpu" TIMEOUT 30)

add_executable(
    registry_storage_capacity_test
    registry_storage_capacity_test.cpp
)
target_link_libraries(registry_storage_capacity_test PRIVATE core)

add_test(
    NAME registry_storage_capacity_test
    COMMAND registry_storage_capacity_test
)
set_tests_properties(
    registry_storage_capacity_test
    PROPERTIES LABELS "cpu" TIMEOUT 30
)
