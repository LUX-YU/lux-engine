#pragma once
/**
 * @file EntityBatchDecoder.hpp
 * @brief Pure owning LXES decode entry suitable for a background CPU arena.
 */

#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/engine/resource/entity_scene/EntitySection.hpp>
#include <lux/engine/runtime/entity_scene/EntityBatchTypes.hpp>
#include <lux/engine/runtime/entity_scene/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace lux::runtime::entity_scene
{
    class LUX_ENGINE_RUNTIME_ENTITY_SCENE_PUBLIC DecodedEntityBatch final
    {
    public:
        DecodedEntityBatch(DecodedEntityBatch&&) noexcept = default;
        DecodedEntityBatch& operator=(DecodedEntityBatch&&) noexcept = default;
        DecodedEntityBatch(const DecodedEntityBatch&) = delete;
        DecodedEntityBatch& operator=(const DecodedEntityBatch&) = delete;

        [[nodiscard]] const lux::entity_scene::EntitySectionId& section()
            const noexcept
        {
            return image_.section;
        }

        [[nodiscard]] std::uint64_t generation() const noexcept
        {
            return generation_;
        }

        [[nodiscard]] std::size_t entityCount() const noexcept
        {
            return image_.entities.size();
        }

        [[nodiscard]] std::size_t encodedBytes() const noexcept
        {
            return encoded_bytes_;
        }

    private:
        friend class EntityBatchDecoder;
        friend class EntityBatchStager;
        friend class EntityBatchMaterializer;

        DecodedEntityBatch(
            lux::entity_scene::EntitySectionImage image,
            std::uint64_t generation,
            std::size_t encoded_bytes) noexcept
            : image_(std::move(image)),
              generation_(generation),
              encoded_bytes_(encoded_bytes)
        {}

        lux::entity_scene::EntitySectionImage image_;
        std::uint64_t generation_{0u};
        std::size_t encoded_bytes_{0u};
    };

    class LUX_ENGINE_RUNTIME_ENTITY_SCENE_PUBLIC EntityBatchDecoder final
    {
    public:
        [[nodiscard]] lux::cxx::expected<
            DecodedEntityBatch,
            EntityBatchFailure>
        decode(
            lux::cxx::SharedBytes<> encoded,
            std::uint64_t generation) const noexcept;
    };
}
