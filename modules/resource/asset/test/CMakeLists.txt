# Asset module tests. Enable with -DENABLE_ASSET_TEST=ON.
#
# animation_asset_test: in-memory round-trip of SkeletonAsset /
#   AnimationClipAsset codecs (always runs) + env-var-gated import
#   integration test for a real .glb / .fbx model
#   (LUX_ANIMATION_TEST_MODEL=path/to/file).

add_executable(
    animation_asset_test
    animation_asset_test.cpp
)

target_link_libraries(
    animation_asset_test
      PRIVATE
      asset_codecs     # runtime codecs + asset core
      lux::engine::toolchain::toolchain_model_importer
)

target_include_directories(
    animation_asset_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../pinclude   # for *DescriptionCodec.hpp
)

# mesh_asset_test: in-memory round-trip of the MeshDescriptionCodec —
# skinned verts + indices + bounds, no-bounds, empty, and bad-magic rejection.
add_executable(
    mesh_asset_test
    mesh_asset_test.cpp
)

target_link_libraries(
    mesh_asset_test
    PRIVATE
    asset_codecs     # runtime codecs + asset core
)

target_include_directories(
    mesh_asset_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../pinclude   # for MeshDescriptionCodec.hpp
)

# asset_lifecycle_test: AssetManager::setBroadcast 回调缝的语义(批E:归零/
# 失效/内容变更三事实互不混用;批E2 起 onLoaded/onWillUnload 订阅面已退役)。
add_executable(
    asset_lifecycle_test
    asset_lifecycle_test.cpp
)

target_link_libraries(
    asset_lifecycle_test
    PRIVATE
    asset_codecs     # runtime codecs + asset core
)

# shell_from_file_test: W2b — makeShellFromFile reads only a .luxasset header and
# produces an info-only shell for lazy types (v2 + legacy v1 upgrade), rejects
# non-lazy types / truncation / bad version / missing file. No SerDeser, no data.
add_executable(
    shell_from_file_test
    shell_from_file_test.cpp
)

target_link_libraries(
    shell_from_file_test
    PRIVATE
    asset_codecs     # makeShellFromFile + MeshAsset + header structs
)

# ── CTest registration ────────────────────────────────────────────────────────
# All eight are CPU-only self-contained binaries (no GPU, no window); the "cpu"
# label folds them into the same suite as the render module's cpu tests, so the
# one command a verification run already uses — `ctest -L cpu` — now exercises
# the codec/VFS/pak layers too instead of relying on someone remembering to run
# them by hand (they had never been registered).
foreach(t
    animation_asset_test
    mesh_asset_test
    material_graph_codec_test
    asset_lifecycle_test
    shell_from_file_test)
endforeach()
