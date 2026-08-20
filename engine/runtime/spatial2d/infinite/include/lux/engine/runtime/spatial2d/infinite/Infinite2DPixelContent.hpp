#pragma once
/**
 * @file Infinite2DPixelContent.hpp
 * @brief Pixel leaf provider for procedurally addressed EntitySections.
 */

#include <lux/engine/ecs/PersistentEntityId.hpp>
#include <lux/engine/ecs/ComponentSchemaId.hpp>
#include <lux/engine/ecs/pixel/systems/PixelFieldRuntime.hpp>
#include <lux/engine/scene/ScenePackage.hpp>
#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/math/Grid.hpp>
#include <lux/engine/runtime/entity_scene/EntitySectionGeneratorCatalog.hpp>
#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>
#include <lux/engine/runtime/spatial2d/infinite/Spatial2DSectionIndex.hpp>
#include <lux/engine/runtime/spatial2d/infinite/pixel_visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <memory>

namespace lux::runtime::spatial2d
{
    inline constexpr std::uint32_t kInfinite2DPixelChunkContentVersion = 2u;

    enum class EInfinite2DPixelContentError : std::uint8_t
    {
        INVALID_CONFIG,
        INVALID_SOURCE,
        INVALID_PARAMETERS,
        SECTION_ID_MISMATCH,
        ENCODE_FAILED
    };

    struct Infinite2DPixelContentFailure final
    {
        EInfinite2DPixelContentError code{
            EInfinite2DPixelContentError::INVALID_CONFIG};
        lux::math::GridCoord2i64 coordinate;
    };

    struct Infinite2DPixelSectionConfig final
    {
        lux::ecs::PersistentEntityId field;
        lux::scene::SectionGeneratorId generator{
            "lux.pixel.infinite2d.chunk"};
        lux::ecs::ComponentSchemaId chunk_schema{
            lux::ecs::componentSchemaId("lux.pixel.chunk2d")};
        lux::ecs::scene_format::ContentTypeId content_type{
            "lux.pixel.chunk"};
        lux::scene::DemandChannelId demand_channel{
            "lux.spatial2d.resident"};
        std::uint64_t seed{0u};
        lux::ecs::MaterialId foreground_material{1u};
        lux::ecs::MaterialId landmark_material{2u};
        lux::ecs::MaterialId sand_material{3u};
        lux::ecs::MaterialId water_material{4u};
        lux::ecs::MaterialId player_material{5u};

        [[nodiscard]] bool valid() const noexcept;
    };

    /// Shared immutable provider state is used by both the 2D address adapter
    /// (which emits owning records) and the generic background generator
    /// catalog (which emits LXES). This keeps record digest/size and generated
    /// bytes on one implementation without putting Pixel switches in either
    /// EntityScene or SpatialPartition.
    class LUX_ENGINE_RUNTIME_INFINITE2D_PIXEL_PUBLIC
    Infinite2DPixelSectionSource final
    {
    public:
        [[nodiscard]] static lux::cxx::expected<
            Infinite2DPixelSectionSource,
            Infinite2DPixelContentFailure>
        create(Infinite2DPixelSectionConfig config);

        [[nodiscard]] lux::runtime::entity_scene::
            EntitySectionGeneratorDescriptor generatorDescriptor() const;

        [[nodiscard]] Spatial2DSectionRecordFactory recordFactory() const;

        [[nodiscard]] lux::cxx::expected<
            lux::scene::SectionRecord,
            Infinite2DPixelContentFailure>
        record(lux::math::GridCoord2i64 coordinate) const;

        [[nodiscard]] const Infinite2DPixelSectionConfig& config() const
            noexcept;

    private:
        struct State;

        explicit Infinite2DPixelSectionSource(
            std::shared_ptr<const State> state) noexcept
            : state_(std::move(state))
        {}

        std::shared_ptr<const State> state_;
    };

    /// Expand one compact deterministic attachment into the Pixel runtime's
    /// owning 256x256 adoption value. The returned chunk is always inactive;
    /// SpatialInterest2DSystem is the sole authority which later publishes
    /// presentation and simulation activity.
    [[nodiscard]] LUX_ENGINE_RUNTIME_INFINITE2D_PIXEL_PUBLIC
    lux::cxx::expected<
        lux::ecs::PixelChunkLoad,
        Infinite2DPixelContentFailure>
    decodeInfinite2DPixelChunk(
        const lux::runtime::entity_scene::ContentBlobLease& content);

    [[nodiscard]] LUX_ENGINE_RUNTIME_INFINITE2D_PIXEL_PUBLIC
    lux::cxx::expected<
        lux::ecs::PixelChunkLoad,
        Infinite2DPixelContentFailure>
    decodeInfinite2DPixelChunk(
        lux::cxx::SharedBytes<> bytes,
        lux::ecs::scene_format::ContentBlobRef reference);
}
