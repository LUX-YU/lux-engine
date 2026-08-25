find_package(lux-cxx CONFIG REQUIRED COMPONENTS core compile_time memory)

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

option(
    LUX_OBJECT_CONTRACT_CHECKS
    "Enable Object affinity/lifetime contract checks outside Debug builds"
    OFF
)
if(LUX_OBJECT_CONTRACT_CHECKS)
    target_compile_definitions(object PUBLIC LUX_OBJECT_CONTRACT_CHECKS=1)
endif()

target_link_libraries(
    object
    PUBLIC
        meta
        lux::cxx::core
        lux::cxx::compile_time
        lux::cxx::memory
)

component_add_transitive_commands(
    object
    "find_package(lux-cxx REQUIRED COMPONENTS core compile_time memory)"
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
    target_compile_definitions(object PRIVATE LUX_OBJECT_TEST_DIAGNOSTICS=1)
    include_component_cmake_scripts(meta)

    engine_add_type_runtime_info(
        NAME object_test_signals_meta
        TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/test/ObjectTestSignals.hpp
        REGISTER_FUNC_NAME lux_object_test_signals
    )
    add_library(
        object_test_signals STATIC
        test/ObjectTestSignals.cpp
    )
    target_include_directories(
        object_test_signals PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/test
    )
    target_compile_definitions(
        object_test_signals PRIVATE LUX_OBJECT_TEST_DIAGNOSTICS=1
    )
    target_link_libraries(object_test_signals PUBLIC object)
    lux_classify_target(
        TARGET object_test_signals
        LAYER CORE
        PRODUCT TEST
        ROLE FOUNDATION
    )
    engine_target_add_type_runtime_info(
        TARGET object_test_signals
        METAS object_test_signals_meta
        OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/object_test_signals_meta
    )

    add_executable(object_signal_test test/object_signal_test.cpp)
    target_link_libraries(object_signal_test PRIVATE object_test_signals)
    add_test(NAME object_signal_test COMMAND object_signal_test)

    add_executable(object_dispatcher_test test/object_dispatcher_test.cpp)
    target_include_directories(
        object_dispatcher_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test/pinclude
    )
    target_compile_definitions(
        object_dispatcher_test PRIVATE LUX_OBJECT_TEST_DIAGNOSTICS=1
    )
    target_link_libraries(object_dispatcher_test PRIVATE object_test_signals)
    add_test(NAME object_dispatcher_test COMMAND object_dispatcher_test)

    add_executable(object_lifetime_test test/object_lifetime_test.cpp)
    target_link_libraries(object_lifetime_test PRIVATE object_test_signals)
    target_compile_definitions(
        object_lifetime_test PRIVATE LUX_OBJECT_TEST_DIAGNOSTICS=1
    )
    add_test(NAME object_lifetime_test COMMAND object_lifetime_test)

    add_executable(
        object_throwing_event_override_probe EXCLUDE_FROM_ALL
        test/object_throwing_event_override_probe.cpp
    )
    target_link_libraries(
        object_throwing_event_override_probe PRIVATE object
    )
    add_test(
        NAME object_throwing_event_override_probe
        COMMAND
            ${CMAKE_COMMAND}
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -DTARGET=object_throwing_event_override_probe
            "-DEXPECTED_PATTERN=less restrictive exception specification"
            -P ${CMAKE_SOURCE_DIR}/cmake/ExpectTargetCompileFailure.cmake
    )

    add_executable(
        object_manual_signal_index_probe EXCLUDE_FROM_ALL
        test/object_manual_signal_index_probe.cpp
    )
    target_link_libraries(object_manual_signal_index_probe PRIVATE object)
    add_test(
        NAME object_manual_signal_index_probe
        COMMAND
            ${CMAKE_COMMAND}
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -DTARGET=object_manual_signal_index_probe
            "-DEXPECTED_PATTERN=SignalIndex"
            -P ${CMAKE_SOURCE_DIR}/cmake/ExpectTargetCompileFailure.cmake
    )

    add_executable(
        object_throwing_callback_probe EXCLUDE_FROM_ALL
        test/object_throwing_callback_probe.cpp
    )
    target_link_libraries(
        object_throwing_callback_probe PRIVATE object_test_signals
    )
    add_test(
        NAME object_throwing_callback_probe
        COMMAND
            ${CMAKE_COMMAND}
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -DTARGET=object_throwing_callback_probe
            "-DEXPECTED_PATTERN=Signal callbacks must be noexcept"
            -P ${CMAKE_SOURCE_DIR}/cmake/ExpectTargetCompileFailure.cmake
    )

    add_executable(
        object_missing_codegen_probe EXCLUDE_FROM_ALL
        test/object_missing_codegen_probe.cpp
    )
    target_link_libraries(object_missing_codegen_probe PRIVATE object)
    add_test(
        NAME object_missing_codegen_probe
        COMMAND
            ${CMAKE_COMMAND}
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -DTARGET=object_missing_codegen_probe
            "-DEXPECTED_PATTERN=unresolved external symbol"
            -P ${CMAKE_SOURCE_DIR}/cmake/ExpectTargetCompileFailure.cmake
    )

    foreach(
        probe_and_pattern
        IN ITEMS
            "object_public_affinity_probe|affinity"
            "object_retired_fwd_header_probe|ObjectFwd.hpp"
            "object_scoped_connection_getter_probe|connection"
    )
        string(REPLACE "|" ";" probe_fields "${probe_and_pattern}")
        list(GET probe_fields 0 probe)
        list(GET probe_fields 1 expected_pattern)
        add_executable(${probe} EXCLUDE_FROM_ALL test/${probe}.cpp)
        target_link_libraries(${probe} PRIVATE object)
        add_test(
            NAME ${probe}
            COMMAND
                ${CMAKE_COMMAND}
                -DBUILD_DIR=${CMAKE_BINARY_DIR}
                -DTARGET=${probe}
                "-DEXPECTED_PATTERN=${expected_pattern}"
                -P ${CMAKE_SOURCE_DIR}/cmake/ExpectTargetCompileFailure.cmake
        )
    endforeach()

    add_executable(
        object_sender_self_destroy_probe
        test/object_sender_self_destroy_probe.cpp
    )
    target_link_libraries(
        object_sender_self_destroy_probe PRIVATE object_test_signals
    )
    add_test(
        NAME object_sender_self_destroy_probe
        COMMAND
            ${CMAKE_COMMAND}
            -DPROBE=$<TARGET_FILE:object_sender_self_destroy_probe>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test/expect_process_failure.cmake
    )

    foreach(
        probe
        IN ITEMS
            object_foreign_disconnect_without_dispatcher_probe
            object_foreign_disconnect_closed_dispatcher_probe
    )
        add_executable(${probe} test/${probe}.cpp)
        target_link_libraries(${probe} PRIVATE object_test_signals)
        add_test(
            NAME ${probe}
            COMMAND
                ${CMAKE_COMMAND}
                -DPROBE=$<TARGET_FILE:${probe}>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/test/expect_process_failure.cmake
        )
    endforeach()

    add_executable(
        object_wrong_affinity_destroy_probe
        test/object_wrong_affinity_destroy_probe.cpp
    )
    target_link_libraries(
        object_wrong_affinity_destroy_probe PRIVATE object_test_signals
    )

    add_executable(
        object_weak_ref_wrong_affinity_probe
        test/object_weak_ref_wrong_affinity_probe.cpp
    )
    target_link_libraries(
        object_weak_ref_wrong_affinity_probe PRIVATE object_test_signals
    )

    add_executable(
        object_event_wrong_affinity_probe
        test/object_event_wrong_affinity_probe.cpp
    )
    target_link_libraries(
        object_event_wrong_affinity_probe PRIVATE object
    )

    if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR LUX_OBJECT_CONTRACT_CHECKS)
        foreach(
            probe
            IN ITEMS
                object_wrong_affinity_destroy_probe
                object_weak_ref_wrong_affinity_probe
                object_event_wrong_affinity_probe
        )
            add_test(
                NAME ${probe}
                COMMAND
                    ${CMAKE_COMMAND}
                    -DPROBE=$<TARGET_FILE:${probe}>
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/test/expect_process_failure.cmake
            )
        endforeach()
    endif()

    add_library(
        object_benchmark_baseline_support SHARED
        test/performance/ObjectBenchmarkBaseline.cpp
    )
    target_include_directories(
        object_benchmark_baseline_support
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/test/performance
    )
    target_compile_definitions(
        object_benchmark_baseline_support
        PRIVATE LUX_OBJECT_BENCHMARK_BASELINE_LIBRARY=1
    )
    set_property(
        TARGET object_benchmark_baseline_support
        PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE
    )
    if(MSVC)
        target_compile_options(
            object_benchmark_baseline_support PRIVATE /GL-
        )
    endif()
    lux_classify_target(
        TARGET object_benchmark_baseline_support
        LAYER CORE
        PRODUCT TEST
        ROLE FOUNDATION
    )

    add_executable(
        object_release_benchmark
        test/object_release_benchmark.cpp
    )
    target_link_libraries(
        object_release_benchmark
        PRIVATE
            object_test_signals
            object_benchmark_baseline_support
    )
    target_include_directories(
        object_release_benchmark
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
            ${CMAKE_CURRENT_SOURCE_DIR}/test
            ${CMAKE_CURRENT_SOURCE_DIR}/test/performance
    )
    target_compile_definitions(
        object_release_benchmark
        PRIVATE
            LUX_OBJECT_TEST_DIAGNOSTICS=1
            NDEBUG
    )
    set_property(
        TARGET object_release_benchmark
        PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE
    )
    lux_classify_target(
        TARGET object_release_benchmark
        LAYER CORE
        PRODUCT TEST
        ROLE FOUNDATION
    )
    add_test(
        NAME object_release_benchmark
        COMMAND object_release_benchmark
    )

    engine_add_type_runtime_info(
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
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/test
            ${CMAKE_CURRENT_SOURCE_DIR}/test/pinclude
            ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    )
    target_compile_definitions(
        object_reflection_test PRIVATE LUX_OBJECT_TEST_DIAGNOSTICS=1
    )
    target_link_libraries(object_reflection_test PRIVATE object)
    engine_target_add_type_runtime_info(
        TARGET object_reflection_test
        METAS object_reflection_test_meta
        OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen
    )
    add_test(NAME object_reflection_test COMMAND object_reflection_test)

    engine_add_type_runtime_info(
        NAME object_cross_dll_base_meta
        TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/test/cross_dll/BaseObject.hpp
        REGISTER_FUNC_NAME lux_object_cross_dll_base
    )
    add_library(
        object_cross_dll_base_test_support SHARED
        test/cross_dll/BaseObject.cpp
    )
    target_compile_definitions(
        object_cross_dll_base_test_support
        PRIVATE LUX_OBJECT_CROSS_DLL_BASE_LIBRARY
    )
    target_include_directories(
        object_cross_dll_base_test_support
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/test/cross_dll
    )
    target_link_libraries(object_cross_dll_base_test_support PUBLIC object)
    engine_target_add_type_runtime_info(
        TARGET object_cross_dll_base_test_support
        METAS object_cross_dll_base_meta
        OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/cross_dll_base_meta
    )

    engine_add_type_runtime_info(
        NAME object_cross_dll_derived_meta
        TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/test/cross_dll/DerivedObject.hpp
        REGISTER_FUNC_NAME lux_object_cross_dll_derived
    )
    add_library(
        object_cross_dll_derived_test_support SHARED
        test/cross_dll/DerivedObject.cpp
    )
    target_compile_definitions(
        object_cross_dll_derived_test_support
        PRIVATE LUX_OBJECT_CROSS_DLL_DERIVED_LIBRARY
    )
    target_include_directories(
        object_cross_dll_derived_test_support
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/test/cross_dll
    )
    target_link_libraries(
        object_cross_dll_derived_test_support
        PUBLIC object_cross_dll_base_test_support
    )
    engine_target_add_type_runtime_info(
        TARGET object_cross_dll_derived_test_support
        METAS object_cross_dll_derived_meta
        OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/cross_dll_derived_meta
    )

    add_executable(
        object_cross_dll_test
        test/cross_dll/object_cross_dll_test.cpp
    )
    target_link_libraries(
        object_cross_dll_test
        PRIVATE object_cross_dll_derived_test_support
    )
    target_include_directories(
        object_cross_dll_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test
    )
    target_compile_definitions(
        object_cross_dll_test PRIVATE LUX_OBJECT_TEST_DIAGNOSTICS=1
    )
    add_test(NAME object_cross_dll_test COMMAND object_cross_dll_test)

endif()
