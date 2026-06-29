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
    asset            # pulls in description + meta + math transitively
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
    asset            # pulls in description + meta + math transitively
)

target_include_directories(
    mesh_asset_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../pinclude   # for MeshDescriptionCodec.hpp
)

# material_graph_codec_test: in-memory round-trip of the MaterialGraphCodec —
# encode -> decode -> byte-identical re-encode + structural equality + bad-magic
# rejection (the binary persistence behind the material asset's concrete graph).
add_executable(
    material_graph_codec_test
    material_graph_codec_test.cpp
)

target_link_libraries(
    material_graph_codec_test
    PRIVATE
    asset            # pulls in description (rdesc::MaterialGraph) transitively
)

target_include_directories(
    material_graph_codec_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../pinclude   # for MaterialGraphCodec.hpp
)

# asset_lifecycle_test: AssetManager onLoaded / onWillUnload / unsubscribe
# lifecycle subscription callbacks (the AssetHandle<T> wrapper that built on
# them was retired in the async-asset redesign).
add_executable(
    asset_lifecycle_test
    asset_lifecycle_test.cpp
)

target_link_libraries(
    asset_lifecycle_test
    PRIVATE
    asset            # pulls in description + meta + math transitively
)

# asset_vfs_test: VP-P2 — VirtualPath grammar tables, LooseDirProvider scan/
# resolve/open/collision determinism, AssetVfs mount-priority + tombstone
# shadowing, AssetManager findAssetByPath/ensureAsset round-trip.
add_executable(
    asset_vfs_test
    asset_vfs_test.cpp
)

target_link_libraries(
    asset_vfs_test
    PRIVATE
    asset            # pulls in description + meta + math transitively
)

# asset_pak_test: VP-P3 — cook rejection table, pak determinism, loose-vs-pak
# parity (resolve/open byte-identical), ensureAsset through a pak mount, and
# the negative decode table (truncation/corruption/unknown-section/tombstone).
add_executable(
    asset_pak_test
    asset_pak_test.cpp
)

target_link_libraries(
    asset_pak_test
    PRIVATE
    asset            # pulls in description + meta + math transitively
)

target_include_directories(
    asset_pak_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../pinclude   # for PakCodec.hpp (index surgery)
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
    asset            # makeShellFromFile + MeshAsset + header structs
)
