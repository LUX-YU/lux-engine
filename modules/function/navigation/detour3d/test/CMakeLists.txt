find_package(Threads REQUIRED)

add_executable(
    navigation_detour3d_test
    navigation_detour3d_test.cpp
)
target_link_libraries(
    navigation_detour3d_test
    PRIVATE
        lux::engine::function::navigation_detour3d
        Threads::Threads
)
if(MSVC)
    target_compile_options(navigation_detour3d_test PRIVATE /UNDEBUG)
else()
    target_compile_options(navigation_detour3d_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  navigation_detour3d_test
    LAYER   FUNCTION
    PRODUCT TEST
    ROLE    DOMAIN
)
