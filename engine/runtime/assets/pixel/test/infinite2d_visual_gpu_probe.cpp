#include "Infinite2DTestHarness.hpp"

#include "DeviceRenderFixture.hpp"

#include <lux/engine/ecs/components/ResolvedTransform2DComponent.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/render/RenderExtractionResources.hpp>
#include <lux/engine/ecs/render/SceneRenderBinding.hpp>
#include <lux/engine/ecs/render/components/PrimaryCameraTag.hpp>
#include <lux/engine/ecs/render/components/RenderViewBindingComponent.hpp>
#include <lux/engine/ecs/render/components/2d/Camera2DCacheComponent.hpp>
#include <lux/engine/ecs/render/components/2d/Camera2DComponent.hpp>
#include <lux/engine/ecs/render/subsystems/2d/Camera2DUploadSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/2d/PixelField2DSubsystem.hpp>
#include <lux/engine/ecs/pixel/components/PixelField2DComponent.hpp>
#include <lux/engine/function/render/client/FeatureCatalog.hpp>
#include <lux/engine/function/render/client/genops/Canvas2DOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/math/eigen_extend.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace
{
    constexpr std::uint32_t kExtent = 128u;
    constexpr std::uint32_t kForegroundRgba = 0xff65b96bu;
    constexpr std::uint32_t kLandmarkRgba = 0xffd66b5fu;
    constexpr std::uint32_t kSandRgba = 0xffc2b280u;
    constexpr std::uint32_t kWaterRgba = 0xff3060c0u;
    constexpr std::uint32_t kPlayerRgba = 0xffff4fd8u;

    [[nodiscard]] std::uint8_t linearToSrgb(std::uint8_t value) noexcept
    {
        const auto linear = static_cast<double>(value) / 255.0;
        const auto srgb = linear <= 0.0031308
            ? linear * 12.92
            : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
        return static_cast<std::uint8_t>(std::lround(srgb * 255.0));
    }

    [[nodiscard]] bool approximately(
        const std::uint8_t* pixel,
        std::uint32_t linear_rgba) noexcept
    {
        // The material table uploads little-endian RGBA8_UNORM. SceneColor is
        // BGRA8_SRGB, so device readback both swizzles R/B and contains the
        // attachment's linear-to-sRGB conversion.
        static constexpr std::array<std::uint32_t, 4u> kRgbaToBgraShift{
            16u, 8u, 0u, 24u};
        for (std::size_t channel = 0u; channel != 3u; ++channel)
        {
            const auto value = static_cast<int>(pixel[channel]);
            const auto linear = static_cast<std::uint8_t>(
                (linear_rgba >> kRgbaToBgraShift[channel]) & 0xffu);
            const auto reference = static_cast<int>(linearToSrgb(linear));
            if (std::abs(value - reference) > 4)
                return false;
        }
        return std::abs(
            static_cast<int>(pixel[3u]) -
            static_cast<int>((linear_rgba >> 24u) & 0xffu)) <= 4;
    }

    [[nodiscard]] std::vector<std::uint8_t> semanticMask(
        std::span<const std::uint8_t> pixels)
    {
        std::vector<std::uint8_t> result(kExtent * kExtent, 0u);
        if (pixels.size() != result.size() * 4u)
            return {};
        for (std::size_t index = 0u; index != result.size(); ++index)
        {
            const auto* pixel = pixels.data() + index * 4u;
            if (approximately(pixel, kForegroundRgba))
                result[index] = 1u;
            else if (approximately(pixel, kLandmarkRgba))
                result[index] = 2u;
            else if (approximately(pixel, kSandRgba))
                result[index] = 3u;
            else if (approximately(pixel, kWaterRgba))
                result[index] = 4u;
            else if (approximately(pixel, kPlayerRgba))
                result[index] = 5u;
        }
        return result;
    }

    [[nodiscard]] std::size_t colorPixelCount(
        std::span<const std::uint8_t> pixels,
        std::uint32_t color) noexcept
    {
        if (pixels.size() != kExtent * kExtent * 4u)
            return 0u;
        std::size_t result = 0u;
        for (std::size_t index = 0u; index != pixels.size(); index += 4u)
            result += approximately(pixels.data() + index, color) ? 1u : 0u;
        return result;
    }

    [[nodiscard]] bool hasFourWaySeamCoverage(
        std::span<const std::uint8_t> mask) noexcept
    {
        if (mask.size() != kExtent * kExtent)
            return false;
        std::array<std::uint32_t, 4u> counts{};
        for (std::uint32_t y = 0u; y != kExtent; ++y)
        {
            for (std::uint32_t x = 0u; x != kExtent; ++x)
            {
                const auto quadrant = (y >= kExtent / 2u ? 2u : 0u) +
                    (x >= kExtent / 2u ? 1u : 0u);
                counts[quadrant] += mask[y * kExtent + x] == 1u ? 1u : 0u;
            }
        }
        return std::ranges::all_of(
            counts,
            [](std::uint32_t count) noexcept
            {
                return count >= 512u;
            });
    }

    [[nodiscard]] bool regionEquivalent(
        std::span<const std::uint8_t> left,
        std::span<const std::uint8_t> right) noexcept
    {
        if (left.size() != right.size() || left.empty())
            return false;
        std::size_t different = 0u;
        for (std::size_t index = 0u; index != left.size(); ++index)
            different += left[index] != right[index] ? 1u : 0u;
        // Terrain fixtures and marker placement are translation invariant.
        // Sparse cave perturbation and a different number of simulation steps
        // intentionally vary some cells, so compare the full semantic color
        // classification with a tolerance rather than a whole-image hash.
        return different * 100u <= left.size() * 20u;
    }

    void writeU16(std::ofstream& stream, std::uint16_t value)
    {
        stream.put(static_cast<char>(value & 0xffu));
        stream.put(static_cast<char>((value >> 8u) & 0xffu));
    }

    void writeU32(std::ofstream& stream, std::uint32_t value)
    {
        for (std::uint32_t shift = 0u; shift != 32u; shift += 8u)
            stream.put(static_cast<char>((value >> shift) & 0xffu));
    }

    void writeI32(std::ofstream& stream, std::int32_t value)
    {
        writeU32(stream, static_cast<std::uint32_t>(value));
    }

    void writeBmp(
        const std::filesystem::path& path,
        std::span<const std::uint8_t> pixels)
    {
        if (pixels.size() != kExtent * kExtent * 4u)
            return;
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream)
            return;
        constexpr std::uint32_t header_bytes = 14u + 40u;
        writeU16(stream, 0x4d42u);
        writeU32(stream, header_bytes + static_cast<std::uint32_t>(pixels.size()));
        writeU16(stream, 0u);
        writeU16(stream, 0u);
        writeU32(stream, header_bytes);
        writeU32(stream, 40u);
        writeI32(stream, static_cast<std::int32_t>(kExtent));
        writeI32(stream, -static_cast<std::int32_t>(kExtent));
        writeU16(stream, 1u);
        writeU16(stream, 32u);
        writeU32(stream, 0u);
        writeU32(stream, static_cast<std::uint32_t>(pixels.size()));
        writeI32(stream, 2835);
        writeI32(stream, 2835);
        writeU32(stream, 0u);
        writeU32(stream, 0u);
        stream.write(
            reinterpret_cast<const char*>(pixels.data()),
            static_cast<std::streamsize>(pixels.size()));
    }

    class Infinite2DVisualExtension final
        : public lux::runtime::assets::pixel::testing::
              Infinite2DTestExtension
    {
    public:
        bool install(
            lux::ecs::World& world,
            lux::ecs::Schedule&,
            lux::ecs::PixelFieldRuntime& pixels) override
        {
            lux::rendertest::DeviceRenderFixture::Options options{};
            options.enable_validation = true;
            options.validation_errors = &validation_errors_;
            fixture_ = std::make_unique<lux::rendertest::DeviceRenderFixture>(
                kExtent,
                kExtent,
                "Infinite2D EntityScene GPU probe",
                options);
            if (!fixture_->ok())
            {
                skipped_ = true;
                fixture_.reset();
                return true;
            }

            scene_ = fixture_->makeSceneWithView(
                "Infinite2DEntityScene",
                "main");
            const auto camera_registration = fixture_->awaitControl(
                fixture_->control().registerFeatureType(
                    lux::render::kViewCameraFeatureFactory));
            fixture_->awaitControl(fixture_->control().addFeature(
                scene_.scene_id,
                camera_registration.feature_type_id,
                lux::render::ViewCameraCommTag{}));
            const auto canvas_registration = fixture_->awaitControl(
                fixture_->control().registerFeatureType(
                    lux::render::kCanvas2DFeatureFactory));
            fixture_->awaitControl(fixture_->control().addFeature(
                scene_.scene_id,
                canvas_registration.feature_type_id,
                lux::render::Canvas2DCommConfig{}));
            features_.injectForTest(
                "StandardViewCamera",
                std::span<const lux::render::TypeId>{
                    camera_registration.ops,
                    camera_registration.op_count});
            features_.injectForTest(
                "Canvas2D",
                std::span<const lux::render::TypeId>{
                    canvas_registration.ops,
                    canvas_registration.op_count});

            binding_ = std::make_unique<lux::ecs::SceneRenderBinding>(
                fixture_->session(),
                fixture_->control(),
                fixture_->uploadClientForTest(),
                scene_.scene_id);
            binding_->setCatalog(features_);
            active_view_ = std::make_unique<lux::ecs::ActiveRenderView>(
                scene_.view);

            auto& registry = world.registry();
            camera_entity_ = registry.create();
            registry.emplace<lux::ecs::PrimaryCameraTag>(camera_entity_);
            auto& camera = registry.emplace<lux::ecs::Camera2DComponent>(
                camera_entity_);
            camera.units_per_view_height = 128.0f;
            camera.aspect = 1.0f;
            registry.emplace<lux::ecs::ResolvedTransform2DComponent>(
                camera_entity_);
            auto& cache = registry.emplace<lux::ecs::Camera2DCacheComponent>(
                camera_entity_);
            cache.effective_aspect = 1.0f;
            cache.view = Eigen::Matrix4f::Identity();
            cache.proj = LuxEigenExt::TOrthographicProjection<float>(
                -64.0f,
                64.0f,
                -64.0f,
                64.0f,
                -1024.0f,
                1024.0f);
            cache.proj(1, 1) = -cache.proj(1, 1);
            cache.view_proj = cache.proj * cache.view;
            cache.prev_view_proj = cache.view_proj;
            registry.emplace<lux::ecs::RenderViewBindingComponent>(
                camera_entity_,
                fixture_->control().adoptView(scene_.scene_id, scene_.view));

            camera_upload_ =
                std::make_unique<lux::ecs::Camera2DUploadSubsystem>();
            pixels_ = std::make_unique<lux::ecs::PixelField2DSubsystem>(
                &pixels);
            camera_upload_->onAdded(lux::ecs::SystemSetupContext{registry, {}});
            pixels_->onAdded(lux::ecs::SystemSetupContext{registry, {}});
            return true;
        }

        void afterTick(lux::ecs::World& world) noexcept override
        {
            if (skipped_ || !fixture_ || !binding_ || !active_view_)
                return;
            auto& registry = world.registry();
            // The CPU scenario intentionally registers only the authored
            // Pixel schemas. In a composed scene Transform2DSystem supplies
            // this derived fact; the render-only extension supplies the same
            // origin fact at the post-barrier safe point without changing the
            // cooked Section or the streaming state machine.
            for (const auto entity :
                 registry.view<const lux::ecs::PixelField2DComponent>())
            {
                if (!registry.all_of<
                        lux::ecs::ResolvedTransform2DComponent>(entity))
                {
                    registry.emplace<
                        lux::ecs::ResolvedTransform2DComponent>(entity);
                }
            }
            lux::ecs::RenderSubsystemContext context{
                registry,
                {},
                *binding_,
                *active_view_,
                0.0f,
                frame_index_++};
            camera_upload_->prepare(context);
            camera_upload_->extract(context);
            pixels_->prepare(context);
            pixels_->extract(context);
            fixture_->flush();
        }

        bool checkpoint(
            lux::runtime::assets::pixel::testing::EInfinite2DCheckpoint
                checkpoint,
            lux::ecs::World& world,
            lux::ecs::PixelFieldRuntime&,
            lux::ecs::PixelFieldHandle,
            lux::math::GridCoord2i64 center) override
        {
            if (skipped_)
                return true;
            auto& registry = world.registry();
            const lux::math::Position2d position{
                static_cast<double>(center.x) * 64.0,
                static_cast<double>(center.y) * 64.0};
            registry.patch<lux::ecs::ResolvedTransform2DComponent>(
                camera_entity_,
                [position](auto& transform) noexcept
                {
                    transform.position = position;
                });
            registry.patch<lux::ecs::Camera2DCacheComponent>(
                camera_entity_,
                [position](auto& cache) noexcept
                {
                    cache.render_origin = position;
                });

            // Atlas creation, palette upload, nine active chunk instances and
            // their first dirty uploads all have independent replies.
            for (std::uint32_t frame = 0u; frame != 16u; ++frame)
                afterTick(world);
            auto image = fixture_->readback(scene_);
            if (fixture_->lastReadback().status != 0u ||
                fixture_->lastReadback().bytes_written != image.size())
            {
                return false;
            }
            auto mask = semanticMask(image);
            if (!hasFourWaySeamCoverage(mask))
                return false;
            if (colorPixelCount(image, kLandmarkRgba) < 2u ||
                colorPixelCount(image, kSandRgba) < 16u ||
                colorPixelCount(image, kWaterRgba) < 16u ||
                colorPixelCount(image, kPlayerRgba) < 2u)
            {
                return false;
            }

            if (const char* directory = std::getenv(
                    "LUX_INFINITE2D_SCREENSHOT_DIR");
                directory && *directory)
            {
                std::error_code directory_error;
                std::filesystem::create_directories(
                    directory,
                    directory_error);
                const char* name = checkpoint ==
                        lux::runtime::assets::pixel::
                        testing::EInfinite2DCheckpoint::ORIGIN_READY
                    ? "infinite2d_origin.bmp"
                    : checkpoint == lux::runtime::assets::pixel::testing::
                        EInfinite2DCheckpoint::FAR_READY
                        ? "infinite2d_far.bmp"
                        : "infinite2d_origin_recovered.bmp";
                writeBmp(std::filesystem::path{directory} / name, image);
            }

            switch (checkpoint)
            {
            case lux::runtime::assets::pixel::testing::
                    EInfinite2DCheckpoint::ORIGIN_READY:
                origin_mask_ = std::move(mask);
                break;
            case lux::runtime::assets::pixel::testing::
                    EInfinite2DCheckpoint::FAR_READY:
                if (!regionEquivalent(origin_mask_, mask))
                    return false;
                break;
            case lux::runtime::assets::pixel::testing::
                    EInfinite2DCheckpoint::ORIGIN_RECOVERED:
                if (!regionEquivalent(origin_mask_, mask))
                    return false;
                break;
            }
            return true;
        }

        void shutdown(lux::ecs::World& world) noexcept override
        {
            if (skipped_ || !fixture_)
                return;
            auto& registry = world.registry();
            lux::ecs::RenderSubsystemContext context{
                registry,
                {},
                *binding_,
                *active_view_,
                0.0f,
                frame_index_++};
            pixels_->prepare(context);
            pixels_->close(context);
            camera_upload_->close(context);
            pixels_->onRemoved(lux::ecs::SystemRemovalContext{registry});
            camera_upload_->onRemoved(
                lux::ecs::SystemRemovalContext{registry});
            if (registry.valid(camera_entity_))
                registry.destroy(camera_entity_);
            fixture_->flush(4);
            pixels_.reset();
            camera_upload_.reset();
            active_view_.reset();
            binding_.reset();
            fixture_.reset();
            clean_ = validation_errors_.load(std::memory_order_acquire) == 0;
        }

        [[nodiscard]] bool clean() const noexcept
        {
            return skipped_ || clean_;
        }

    private:
        std::atomic<int> validation_errors_{0};
        std::unique_ptr<lux::rendertest::DeviceRenderFixture> fixture_;
        lux::rendertest::DeviceRenderFixture::SceneView scene_{};
        lux::render::FeatureCatalog features_;
        std::unique_ptr<lux::ecs::SceneRenderBinding> binding_;
        std::unique_ptr<lux::ecs::ActiveRenderView> active_view_;
        std::unique_ptr<lux::ecs::Camera2DUploadSubsystem> camera_upload_;
        std::unique_ptr<lux::ecs::PixelField2DSubsystem> pixels_;
        entt::entity camera_entity_{entt::null};
        std::vector<std::uint8_t> origin_mask_;
        std::uint64_t frame_index_{0u};
        bool skipped_{false};
        bool clean_{false};
    };
}

int main()
{
    Infinite2DVisualExtension extension;
    const auto result =
        lux::runtime::assets::pixel::testing::runInfinite2DScenario(
            &extension);
    return result == 0 && extension.clean() ? 0 : 1;
}
