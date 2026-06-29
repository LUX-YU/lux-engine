# shadergen 中性核测试（设备无关，self-checking，exit 0 = pass）。
# 启用：-DENABLE_SHADERGEN_TEST=ON

add_executable(shadergen_lower_material_test lower_material_test.cpp)
target_link_libraries(shadergen_lower_material_test
    PRIVATE
    shadergen
    lux::engine::resource::description
)
