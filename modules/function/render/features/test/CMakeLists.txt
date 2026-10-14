add_executable(render_features_test Feature.cpp)

target_link_libraries(render_features_test PRIVATE render_features)

foreach(group composition identity config operations claims ownership fallback lifetime concurrency counters dependencies)
    add_test(NAME render.features.${group} COMMAND render_features_test ${group})
    set_tests_properties(render.features.${group} PROPERTIES TIMEOUT 60)
endforeach()

add_executable(render_features_cost Cost.cpp)

target_link_libraries(render_features_cost PRIVATE render_features)

foreach(header Feature Resources)
    string(CONCAT probe
        "#include <lux/engine/render/features/${header}.hpp>\nstatic_assert(__cplusplus >= 202002L);\n"
        "#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)\n#error Native header leaked into Feature\n#endif\n"
    )

    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/headers/${header}.cpp" CONTENT "${probe}")

    add_library(render_features_header_${header} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/headers/${header}.cpp")

    target_link_libraries(render_features_header_${header} PRIVATE render_features)
endforeach()

get_target_property(feature_links render_features INTERFACE_LINK_LIBRARIES)
set(expected_links lux::engine::render::render_graph lux::engine::render::render_data
    lux::engine::render::render_protocol
)
if(NOT feature_links STREQUAL expected_links)
    message(FATAL_ERROR "Unexpected Feature dependencies: ${feature_links}")
endif()

file(
    GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/feature-dependencies.txt"
    CONTENT "public_links=$<TARGET_PROPERTY:render_features,INTERFACE_LINK_LIBRARIES>\n"
)

add_test(
    NAME render.features.installed
    COMMAND "${CMAKE_COMMAND}"
        "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
        "-DCONSUMER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/installed"
        "-DDEPENDENCY_DIR=${lux-cxx_DIR}"
        "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DCONFIG=$<CONFIG>"
        "-DGENERATOR=${CMAKE_GENERATOR}"
        "-DCXX_FLAGS=${CMAKE_CXX_FLAGS}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/../../core/test/Installed.cmake"
)
