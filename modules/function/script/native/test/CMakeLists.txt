add_library(
    script_native_fixture
    SHARED
    native_fixture.cpp
)

add_library(
    script_native_second_fixture
    SHARED
    native_fixture.cpp
)

add_library(
    script_native_collision_fixture
    SHARED
    native_collision_fixture.cpp
)

add_library(
    script_native_step_fixture
    SHARED
    native_step_fixture.cpp
)
target_include_directories(
    script_native_step_fixture
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../core/include
)
lux_classify_target(
    TARGET  script_native_step_fixture
    LAYER   FUNCTION
    PRODUCT TEST
    ROLE    DOMAIN
)
target_include_directories(
    script_native_collision_fixture
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../core/include
)
target_link_libraries(
    script_native_collision_fixture
    PRIVATE lux::cxx::compile_time
)
lux_classify_target(
    TARGET  script_native_collision_fixture
    LAYER   FUNCTION
    PRODUCT TEST
    ROLE    DOMAIN
)
target_compile_definitions(
    script_native_second_fixture
    PRIVATE LUX_SCRIPT_FIXTURE_SECOND_MODULE=1
)
target_include_directories(
    script_native_second_fixture
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../core/include
)
target_link_libraries(
    script_native_second_fixture
    PRIVATE lux::cxx::compile_time
)
lux_classify_target(
    TARGET  script_native_second_fixture
    LAYER   FUNCTION
    PRODUCT TEST
    ROLE    DOMAIN
)
target_include_directories(
    script_native_fixture
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../core/include
)
lux_classify_target(
    TARGET  script_native_fixture
    LAYER   FUNCTION
    PRODUCT TEST
    ROLE    DOMAIN
)

target_link_libraries(
    script_native_fixture
    PRIVATE lux::cxx::compile_time
)

add_library(
    script_native_bad_entry_fixture
    SHARED
    native_fixture.cpp
)
target_compile_definitions(
    script_native_bad_entry_fixture
    PRIVATE
        LUX_SCRIPT_FIXTURE_BAD_ENTRY=1
)
target_include_directories(
    script_native_bad_entry_fixture
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../core/include
)
lux_classify_target(
    TARGET  script_native_bad_entry_fixture
    LAYER   FUNCTION
    PRODUCT TEST
    ROLE    DOMAIN
)

target_link_libraries(
    script_native_bad_entry_fixture
    PRIVATE lux::cxx::compile_time
)

add_library(
    script_native_missing_entry_fixture
    SHARED
    native_missing_entry_fixture.cpp
)
target_include_directories(
    script_native_missing_entry_fixture
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../core/include
)
lux_classify_target(
    TARGET  script_native_missing_entry_fixture
    LAYER   FUNCTION
    PRODUCT TEST
    ROLE    DOMAIN
)

add_library(
    script_native_bind_failure_fixture
    SHARED
    native_fixture.cpp
)
target_compile_definitions(
    script_native_bind_failure_fixture
    PRIVATE
        LUX_SCRIPT_FIXTURE_BIND_FAILURE=1
)
target_include_directories(
    script_native_bind_failure_fixture
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../core/include
)
lux_classify_target(
    TARGET  script_native_bind_failure_fixture
    LAYER   FUNCTION
    PRODUCT TEST
    ROLE    DOMAIN
)

target_link_libraries(
    script_native_bind_failure_fixture
    PRIVATE lux::cxx::compile_time
)

add_library(
    script_native_bad_abi_fixture
    SHARED
    native_fixture.cpp
)
target_compile_definitions(
    script_native_bad_abi_fixture
    PRIVATE
        LUX_SCRIPT_FIXTURE_BAD_ABI=1
)
target_include_directories(
    script_native_bad_abi_fixture
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../core/include
)
lux_classify_target(
    TARGET  script_native_bad_abi_fixture
    LAYER   FUNCTION
    PRODUCT TEST
    ROLE    DOMAIN
)

target_link_libraries(
    script_native_bad_abi_fixture
    PRIVATE lux::cxx::compile_time
)

add_executable(
    script_native_runtime_contract_test
    script_native_runtime_contract_test.cpp
)
target_link_libraries(
    script_native_runtime_contract_test
    PRIVATE
        lux::engine::function::script_native
)
target_compile_definitions(
    script_native_runtime_contract_test
    PRIVATE
        LUX_SCRIPT_NATIVE_FIXTURE="$<TARGET_FILE:script_native_fixture>"
        LUX_SCRIPT_NATIVE_BAD_ABI_FIXTURE="$<TARGET_FILE:script_native_bad_abi_fixture>"
        LUX_SCRIPT_NATIVE_BAD_ENTRY_FIXTURE="$<TARGET_FILE:script_native_bad_entry_fixture>"
        LUX_SCRIPT_NATIVE_MISSING_ENTRY_FIXTURE="$<TARGET_FILE:script_native_missing_entry_fixture>"
        LUX_SCRIPT_NATIVE_BIND_FAILURE_FIXTURE="$<TARGET_FILE:script_native_bind_failure_fixture>"
)
add_dependencies(
    script_native_runtime_contract_test
    script_native_fixture
    script_native_bad_abi_fixture
    script_native_bad_entry_fixture
    script_native_missing_entry_fixture
    script_native_bind_failure_fixture
)
if(MSVC)
    target_compile_options(script_native_runtime_contract_test PRIVATE /UNDEBUG)
else()
    target_compile_options(script_native_runtime_contract_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  script_native_runtime_contract_test
    LAYER   FUNCTION
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(
    NAME script_native_runtime_contract_test
    COMMAND script_native_runtime_contract_test
)
