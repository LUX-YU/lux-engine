find_package(Threads REQUIRED)

add_executable(
    script_runtime_contract_test
    script_runtime_contract_test.cpp
)

target_link_libraries(
    script_runtime_contract_test
    PRIVATE
        lux::engine::function::script_core
        Threads::Threads
)

if(MSVC)
    target_compile_options(script_runtime_contract_test PRIVATE /UNDEBUG)
else()
    target_compile_options(script_runtime_contract_test PRIVATE -UNDEBUG)
endif()

lux_classify_target(
    TARGET  script_runtime_contract_test
    LAYER   FUNCTION
    PRODUCT TEST
    ROLE    DOMAIN
)

add_test(NAME script_runtime_contract_test COMMAND script_runtime_contract_test)
