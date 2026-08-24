#include <lux/engine/ecs/entity_scene/EntityBatchDecoder.hpp>

#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>

#include "EntityBatchInternal.hpp"

#include <utility>

namespace lux::ecs::entity_scene
{
    lux::cxx::expected<DecodedEntityBatch, EntityBatchFailure>
    EntityBatchDecoder::decode(
        lux::cxx::SharedBytes<> encoded,
        std::uint64_t generation) const noexcept
    {
        if (encoded.empty() || generation == 0u)
        {
            return lux::cxx::unexpected(detail::makeFailure(
                EEntityBatchError::INVALID_ARGUMENT,
                {},
                generation,
                "LXES bytes and a non-zero runtime generation are required"));
        }

        auto image = lux::ecs::scene_format::decodeEntitySectionImage(
            encoded.view());
        if (!image)
        {
            return lux::cxx::unexpected(detail::makeFailure(
                EEntityBatchError::CODEC_FAILURE,
                {},
                generation,
                image.error().detail));
        }
        return DecodedEntityBatch{
            std::move(*image), generation, encoded.size()};
    }
}
