#include "DeviceRenderFixture.hpp"

#include <lux/engine/function/render/client/features/canvas2d/Canvas2DOperation.hpp>
#include <lux/engine/function/render/client/genops/Canvas2DOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string_view>
#include <vector>

namespace
{
    inline constexpr int kSkip = 77;

    [[nodiscard]] lux::render::Image2DInstanceData fullView(std::uint32_t texture) noexcept
    {
        lux::render::Image2DInstanceData result{};
        result.m[0] = 1.8F;
        result.m[3] = 1.8F;
        result.texture_bindless = texture;
        return result;
    }
}

int main(int argc, char** argv)
{
    using namespace lux;
    using namespace lux::asset;
    using namespace lux::render;
    const bool require_gpu = argc == 2 && std::string_view{argv[1]} == "--require-gpu";

    const auto provider = PakAssetProvider::loadFromFile(LUX_TEXTURE_QUALIFICATION_PAK);
    if (!provider)
    {
        std::fprintf(stderr, "texture qualification Pak cannot be opened\n");
        return 2;
    }
    auto vfs = std::make_shared<AssetVfs>();
    if (vfs->mount({"/Game", *provider, 0}) == kInvalidMountId)
        return 3;
    const auto requested = vfs->resolve("/Game/checker");
    if (requested.isNull())
        return 4;
    const auto blob = vfs->open(requested);
    if (!blob)
        return 5;
    const auto texture_asset = TAssetSerDeser<TextureAsset>::decode(
        requested,
        blob->bytes,
        AssetDecodeLimits{blob->bytes.size(), blob->bytes.size(), 4U}
    );
    if (!texture_asset)
        return 6;
    const auto& texture = (*texture_asset)->data();
    if (texture.pixelFormat() != rdesc::ETexturePixelFormat::BC3_SRGB || texture.mipCount() < 2U)
        return 7;

    std::atomic_int validation_errors{};
    std::uint32_t lit_pixels{};
    bool gpu_available{};
    bool upload_ready{};
    {
        lux::rendertest::DeviceRenderFixture fixture(
            128U,
            128U,
            "texture_asset_vulkan_qualification",
            {.enable_validation = true, .validation_errors = &validation_errors}
        );
        if (!fixture.ok())
        {
            std::fprintf(stderr, "no Vulkan device; texture qualification skipped\n");
            return kSkip;
        }
        gpu_available = true;

        std::vector<OwnedTextureMipLevel> mip_levels;
        mip_levels.reserve(texture.mipCount());
        for (std::uint32_t level = 0U; level < texture.mipCount(); ++level)
        {
            const auto& range = texture.mipRange(level);
            if (range.offset > texture.size() || range.size > texture.size() - range.offset)
                return 8;
            mip_levels.push_back(OwnedTextureMipLevel{
                texture.pixels().subspan(
                    static_cast<std::size_t>(range.offset),
                    static_cast<std::size_t>(range.size)
                ),
                range.width,
                range.height
            });
        }
        auto upload = fixture.uploadClientForTest().tryCreateTexture2DMips(
            std::move(mip_levels),
            texture.channel(),
            texture.pixelFormat(),
            false
        );
        if (!upload)
        {
            std::fprintf(stderr, "BC3 upload unsupported; texture qualification skipped\n");
            return kSkip;
        }
        const auto uploaded = fixture.awaitUpload(std::move(*upload));
        if (uploaded.status != 0U || uploaded.handle.isNull())
        {
            std::fprintf(stderr, "BC3 upload failed; texture qualification skipped\n");
            return kSkip;
        }
        upload_ready = true;

        const auto scene = fixture.makeSceneWithView("TypedTexture", "TypedTextureView");
        const auto camera_registration =
            fixture.awaitControl(fixture.control().registerFeatureType(kViewCameraFeatureFactory));
        fixture.awaitControl(fixture.control().addFeature(
            scene.scene_id,
            camera_registration.feature_type_id,
            ViewCameraCommTag{}
        ));
        const auto canvas_registration =
            fixture.awaitControl(fixture.control().registerFeatureType(kCanvas2DFeatureFactory));
        fixture.awaitControl(fixture.control().addFeature(
            scene.scene_id,
            canvas_registration.feature_type_id,
            Canvas2DCommConfig{}
        ));
        const auto camera_ops = ViewCameraOperationIds::fromOps(
            camera_registration.ops,
            camera_registration.op_count
        );
        const auto canvas_ops = Canvas2DOperationIds::fromOps(
            canvas_registration.ops,
            canvas_registration.op_count
        );
        if (!camera_ops.valid() || !canvas_ops.valid())
            return 9;
        Canvas2DProxy canvas{fixture.session(), canvas_ops};
        const auto image = fixture.await(addImage(
            canvas,
            scene.scene_id,
            fullView(uploaded.handle.index),
            0.0F
        ));
        if (image.status != ECanvas2DCreateStatus::Ok)
            return 10;

        const float identity[16] = {
            1.0F, 0.0F, 0.0F, 0.0F,
            0.0F, 1.0F, 0.0F, 0.0F,
            0.0F, 0.0F, 1.0F, 0.0F,
            0.0F, 0.0F, 0.0F, 1.0F
        };
        const float eye[3] = {0.0F, 0.0F, 0.0F};
        viewCameraUpdateTransient(
            ViewCameraProxy{fixture.session(), camera_ops},
            scene.scene_id,
            scene.view,
            identity,
            identity,
            eye
        );
        fixture.flush(8);
        const auto pixels = fixture.readback(scene);
        if (fixture.lastReadback().status != 0U)
            return 11;
        for (std::size_t offset = 0U; offset + 3U < pixels.size(); offset += 4U)
        {
            const auto brightness = static_cast<std::uint32_t>(pixels[offset]) +
                static_cast<std::uint32_t>(pixels[offset + 1U]) +
                static_cast<std::uint32_t>(pixels[offset + 2U]);
            if (brightness >= 48U)
                ++lit_pixels;
        }
        fixture.control().destroyTexture(uploaded.handle);
        fixture.flush(2);
    }

    std::printf(
        "gpu=%u format=BC3_SRGB mips=%u cooked_bytes=%zu upload=%u lit_pixels=%u validation_errors=%d\n",
        gpu_available ? 1U : 0U,
        texture.mipCount(),
        blob->bytes.size(),
        upload_ready ? 1U : 0U,
        lit_pixels,
        validation_errors.load()
    );
    const bool success = gpu_available && upload_ready && lit_pixels >= 64U && validation_errors.load() == 0;
    if (!success && !require_gpu)
        return kSkip;
    return success ? 0 : 12;
}
