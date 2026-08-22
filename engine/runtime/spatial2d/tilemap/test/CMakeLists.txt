add_executable(
    tilemap_prepare_service_test
    tilemap_prepare_service_test.cpp
)
target_link_libraries(
    tilemap_prepare_service_test
    PRIVATE
        lux::engine::runtime::runtime_spatial2d_tilemap
        lux::engine::runtime::runtime_execution
        lux::engine::resource::asset
)
if(MSVC)
    target_compile_options(tilemap_prepare_service_test PRIVATE /UNDEBUG)
else()
    target_compile_options(tilemap_prepare_service_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  tilemap_prepare_service_test
    LAYER   RUNTIME
    PRODUCT TEST
    ROLE    DOMAIN
)
