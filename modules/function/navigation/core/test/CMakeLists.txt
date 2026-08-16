add_executable(
    navigation_contract_test
    navigation_contract_test.cpp
)
target_link_libraries(
    navigation_contract_test
    PRIVATE lux::engine::function::navigation
)
if(MSVC)
    target_compile_options(navigation_contract_test PRIVATE /UNDEBUG)
else()
    target_compile_options(navigation_contract_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  navigation_contract_test
    LAYER   FUNCTION
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(NAME navigation_contract_test COMMAND navigation_contract_test)
set_tests_properties(
    navigation_contract_test
    PROPERTIES LABELS "cpu;navigation" TIMEOUT 30
)
