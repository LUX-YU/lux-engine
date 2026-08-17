add_executable(scene_services_test scene_services_test.cpp)
target_link_libraries(scene_services_test PRIVATE core)


add_executable(schedule_topology_test schedule_topology_test.cpp)
target_link_libraries(schedule_topology_test PRIVATE core)


add_executable(component_type_catalog_test component_type_catalog_test.cpp)
target_link_libraries(component_type_catalog_test PRIVATE core)


add_executable(persistent_entity_index_test persistent_entity_index_test.cpp)
target_link_libraries(persistent_entity_index_test PRIVATE core)


# R1a:反应式抽取改造的前置验证。它断言的是 EnTT reactive storage 的性质 ——
# 选型建立在这些性质上,所以它们必须被断言而不是被相信。
add_executable(reactive_storage_probe reactive_storage_probe.cpp)
target_link_libraries(reactive_storage_probe PRIVATE core)


add_executable(
    registry_storage_capacity_test
    registry_storage_capacity_test.cpp
)
target_link_libraries(registry_storage_capacity_test PRIVATE core)
