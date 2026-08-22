add_executable(
    script_core_contract_test
    script_core_contract_test.cpp
)

target_link_libraries(
    script_core_contract_test
    PRIVATE
        lux::engine::function::script_core
)

if(MSVC)
    target_compile_options(script_core_contract_test PRIVATE /UNDEBUG)
else()
    target_compile_options(script_core_contract_test PRIVATE -UNDEBUG)
endif()

lux_classify_target(
    TARGET  script_core_contract_test
    LAYER   FUNCTION
    PRODUCT TEST
    ROLE    DOMAIN
)

add_test(NAME script_core_contract_test COMMAND script_core_contract_test)
