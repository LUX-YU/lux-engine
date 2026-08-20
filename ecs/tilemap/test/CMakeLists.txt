add_executable(tilemap_sparse_runtime_test tilemap_sparse_runtime_test.cpp)
target_link_libraries(tilemap_sparse_runtime_test PRIVATE tilemap)

add_executable(tilemap_system_test tilemap_system_test.cpp)
target_link_libraries(tilemap_system_test PRIVATE tilemap)

add_executable(tilemap_chunk_codec_test tilemap_chunk_codec_test.cpp)
target_link_libraries(
    tilemap_chunk_codec_test PRIVATE lux::cxx::algorithm tilemap)
if(MSVC)
    target_compile_options(tilemap_chunk_codec_test PRIVATE /UNDEBUG)
else()
    target_compile_options(tilemap_chunk_codec_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET tilemap_chunk_codec_test LAYER ECS PRODUCT TEST ROLE DOMAIN)
