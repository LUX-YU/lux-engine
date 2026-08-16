add_executable(asset_load_service_test asset_load_service_test.cpp)
add_test(NAME asset_load_service_test COMMAND asset_load_service_test)
set_tests_properties(asset_load_service_test PROPERTIES
    LABELS "cpu;execution;asset"
    TIMEOUT 30)

target_link_libraries(asset_load_service_test
    PRIVATE
        runtime_assets
        lux::engine::resource::asset_codecs
        STDEXEC::stdexec
)

if(MSVC)
    target_compile_options(asset_load_service_test
        PRIVATE /permissive- /Zc:__cplusplus /Zc:preprocessor)
endif()
