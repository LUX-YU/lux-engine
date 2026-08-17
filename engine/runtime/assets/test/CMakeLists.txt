add_executable(asset_load_service_test asset_load_service_test.cpp)

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
