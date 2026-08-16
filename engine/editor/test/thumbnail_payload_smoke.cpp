// ============================================================================
//  thumbnail_payload_smoke.cpp
//
//  CPU-only smoke test for the thumbnail STORAGE layer (no GPU):
//    1. PNG encode/decode is lossless for RGBA8.
//    2. downscale + BGRA↔RGBA swizzle helpers behave.
//    3. ThumbnailSet (multi-size) encode/decode round-trips; best() picks right.
//    4. END-TO-END: a thumbnail set stored as a generic asset Payload survives
//       a real `.luxasset` export → re-import (this also finally verifies T1's
//       payload persistence, which had no consumer before).
//
//  0 = PASS, non-zero = FAIL.
// ============================================================================

#include <lux/engine/editor/thumbnail/ThumbnailSet.hpp>
#include <lux/engine/editor/thumbnail/ImageCodec.hpp>

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/MeshAsset.hpp>
#include <lux/engine/resource/asset/MeshSerDeser.hpp>
#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/description/Vertex.hpp>
#include <lux/engine/math/AABB.hpp>

#include <uuid.h>
#include <Eigen/Core>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

using namespace lux::editor;

static int g_fail = 0;
static void check(bool c, const char* n)
{
    std::printf(c ? "  [PASS] %s\n" : "  [FAIL] %s\n", n);
    if (!c) ++g_fail;
}

static std::vector<std::byte> gradient(std::uint32_t w, std::uint32_t h)
{
    std::vector<std::byte> px(static_cast<std::size_t>(w) * h * 4);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
        {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            px[i + 0] = std::byte(x * 255 / (w > 1 ? w - 1 : 1));
            px[i + 1] = std::byte(y * 255 / (h > 1 ? h - 1 : 1));
            px[i + 2] = std::byte(128);
            px[i + 3] = std::byte(255);
        }
    return px;
}

int main()
{
    std::printf("=== thumbnail_payload_smoke ===\n");

    // 1. PNG round-trip (lossless for RGBA8).
    const auto src = gradient(16, 12);
    const auto png = encodePngRgba8(src.data(), 16, 12);
    check(!png.empty(), "encodePngRgba8 produced bytes");
    const auto dec = decodePngRgba8(png);
    check(dec.has_value(), "decodePngRgba8 ok");
    check(dec && dec->width == 16 && dec->height == 12, "decoded dims match");
    check(dec && dec->rgba8 == src, "PNG pixels round-trip exactly (lossless)");

    // 2. downscale + swizzle sanity.
    const auto small = downscaleRgba8(src.data(), 16, 12, 8, 6);
    check(small.size() == static_cast<std::size_t>(8) * 6 * 4, "downscale byte count");
    auto swz = src;
    swizzleBgraRgba(swz.data(), 16 * 12);
    check(swz[0] == src[2] && swz[2] == src[0] && swz[1] == src[1] && swz[3] == src[3],
          "swizzle swapped B/R, kept G/A");

    // 3. ThumbnailSet encode/decode round-trip + best().
    ThumbnailSet set;
    for (auto wh : {std::pair<std::uint32_t, std::uint32_t>{16, 12}, {8, 6}})
    {
        const auto g = gradient(wh.first, wh.second);
        ThumbnailImage img;
        img.width = wh.first; img.height = wh.second;
        img.encoding = EThumbnailEncoding::PNG;
        img.bytes = encodePngRgba8(g.data(), wh.first, wh.second);
        set.images.push_back(std::move(img));
    }
    const auto blob = encodeThumbnailSet(set);
    const auto set2 = decodeThumbnailSet(blob);
    check(set2.has_value(), "decodeThumbnailSet ok");
    check(set2 && set2->images.size() == 2, "set count round-trips");
    check(set2 && set2->images[0].width == 16 && set2->images[1].width == 8, "set dims round-trip");
    check(set2 && set2->images[0].bytes == set.images[0].bytes, "set image bytes round-trip");
    check(set2 && set2->best(10, 10) && set2->best(10, 10)->width == 16, "best(10,10) -> 16");
    check(set2 && set2->best(4, 4)   && set2->best(4, 4)->width == 8,    "best(4,4) -> 8");
    check(decodeThumbnailSet(std::span<const std::byte>{}).has_value() == false,
          "decode rejects empty blob");

    // 4. End-to-end: payload survives a real .luxasset export → re-import.
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "lux_thumb_payload_smoke.luxasset";
    std::error_code ec; fs::remove(tmp, ec);

    auto parsed = uuids::uuid::from_string("00000000-0000-4000-8000-123456789abc");
    check(parsed.has_value(), "parse test UUID");
    const lux::asset::asset_id_t id = parsed.value_or(lux::asset::asset_id_t{});

    {   // export
        auto mgr_ref = std::make_shared<lux::asset::AssetManager>(
            lux::asset::runtimeAssetCodecCatalog());
        using V3 = Eigen::Vector3f; using V2 = Eigen::Vector2f;
        std::vector<lux::rdesc::Vertex> verts = {
            {V3(0,0,0), V3(0,0,1), V3(1,0,0), V2(0,0), V3(0,1,0)},
            {V3(1,0,0), V3(0,0,1), V3(1,0,0), V2(1,0), V3(0,1,0)},
            {V3(0,1,0), V3(0,0,1), V3(1,0,0), V2(0,1), V3(0,1,0)},
        };
        std::vector<std::uint32_t> idx = {0, 1, 2};
        lux::rdesc::Mesh mesh{std::move(verts), std::move(idx),
                              lux::math::AABB(V3(0,0,0), V3(1,1,0))};

        auto info  = std::make_unique<lux::asset::AssetInfo>();
        info->id   = id;
        info->type = lux::asset::EAssetType::MESH;
        auto asset = std::make_unique<lux::asset::MeshAsset>(std::move(info));
        asset->setData(std::make_unique<lux::rdesc::Mesh>(std::move(mesh)));
        writeThumbnailSet(*asset, set);                 // payload set BEFORE register/export
        check(asset->hasPayload(kThumbnailPayloadTag), "payload present pre-export");
        check(mgr_ref->registerAsset(std::move(asset)), "registerAsset");

        lux::asset::MeshSerDeser ser(mgr_ref);
        const auto err = ser.exportAsLuxAsset(id, tmp);
        check(err == lux::asset::EAssetError::SUCCESS, "exportAsLuxAsset SUCCESS");
    }

    {   // re-import into a FRESH manager + verify the thumbnail payload survived
        auto mgr_ref = std::make_shared<lux::asset::AssetManager>(
            lux::asset::runtimeAssetCodecCatalog());
        lux::asset::MeshSerDeser ser(mgr_ref);
        auto res = ser.fromLuxAsset(tmp);
        check(static_cast<bool>(res), "fromLuxAsset ok");
        if (res)
        {
            const lux::asset::asset_id_t id2 = res.value().second;
            check(id2 == id, "round-tripped asset id matches");

            const auto* asset = mgr_ref->fetchAssetAs<lux::asset::MeshAsset>(id2);
            check(asset != nullptr, "asset fetchable after import");
            if (asset)
            {
                const auto rt = readThumbnailSet(*asset);
                check(rt.has_value(), "thumbnail payload survived round-trip");
                check(rt && rt->images.size() == 2, "round-tripped set count");
                check(rt && rt->images[0].bytes == set.images[0].bytes,
                      "round-tripped image[0] bytes match");
                check(rt && rt->images[1].bytes == set.images[1].bytes,
                      "round-tripped image[1] bytes match");
                if (rt && !rt->images.empty())
                {
                    const auto px = decodePngRgba8(rt->images[0].bytes);
                    check(px && px->rgba8 == gradient(16, 12),
                          "decoded round-tripped PNG matches original pixels");
                }
            }
        }
    }
    fs::remove(tmp, ec);

    std::printf("=== thumbnail_payload_smoke %s (fails=%d) ===\n",
                g_fail == 0 ? "PASSED" : "FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
