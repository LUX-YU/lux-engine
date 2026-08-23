find_package(lux-cxx CONFIG REQUIRED COMPONENTS core compile_time)

add_component(
    COMPONENT_NAME object
    NAMESPACE lux::engine::core
    OUTPUT_NAME lux_engine_core_object
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Connection.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Object.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ObjectDispatcher.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ObjectReflection.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ObjectWeakRef.cpp
)

component_include_directories(
    object
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE pinclude
)

target_compile_definitions(object PRIVATE LUX_CORE_LIBRARY)

target_link_libraries(
    object
    PUBLIC
        meta
        lux::cxx::core
        lux::cxx::compile_time
)

component_add_transitive_commands(
    object
    "find_package(lux-cxx REQUIRED COMPONENTS core compile_time)"
    "find_package(lux-engine-core REQUIRED COMPONENTS meta)"
)

lux_classify_target(
    TARGET object
    LAYER CORE
    PRODUCT RUNTIME
    ROLE FOUNDATION
)

set(ENABLE_OBJECT_TEST ON CACHE BOOL "Enable core object tests")
if(ENABLE_OBJECT_TEST)
    include_component_cmake_scripts(meta)

    add_executable(object_signal_test test/object_signal_test.cpp)
    target_link_libraries(object_signal_test PRIVATE object)
    add_test(NAME object_signal_test COMMAND object_signal_test)

    add_executable(object_dispatcher_test test/object_dispatcher_test.cpp)
    target_link_libraries(object_dispatcher_test PRIVATE object)
    add_test(NAME object_dispatcher_test COMMAND object_dispatcher_test)

    add_executable(
        object_performance_contract_test
        test/object_performance_contract_test.cpp
    )
    target_link_libraries(object_performance_contract_test PRIVATE object)
    add_test(
        NAME object_performance_contract_test
        COMMAND object_performance_contract_test
    )

    engine_add_meta(
        NAME object_reflection_test_meta
        TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/test/object_reflection_fixture.hpp
        REGISTER_FUNC_NAME lux_object_reflection_test
    )
    add_executable(
        object_reflection_test
        test/object_reflection_test.cpp
    )
    target_include_directories(
        object_reflection_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test
    )
    target_link_libraries(object_reflection_test PRIVATE object)
    engine_target_add_meta(
        TARGET object_reflection_test
        METAS object_reflection_test_meta
        OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen
    )
    add_test(NAME object_reflection_test COMMAND object_reflection_test)
endif()
