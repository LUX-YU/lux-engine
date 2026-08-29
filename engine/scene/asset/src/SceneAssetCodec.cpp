#include <lux/engine/scene/SceneAssetCodec.hpp>

#include <lux/engine/serialization/BinaryReader.hpp>
#include <lux/engine/serialization/BinaryWriter.hpp>

#include <array>
#include <memory>
#include <new>
#include <span>
#include <utility>
#include <vector>

namespace lux::scene
{
    namespace
    {
        inline constexpr std::uint32_t kVersion = 1U;
        inline constexpr std::size_t kWireSize = 40U;

        [[nodiscard]] lux::cxx::expected<std::vector<std::byte>, lux::asset::EAssetCodecError> encode(
            const void* payload,
            const lux::asset::AssetEncodeContext& context
        ) noexcept
        {
            if (payload == nullptr || context.limits.max_encoded_bytes < kWireSize)
                return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
            const auto& scene = *static_cast<const SceneDescription*>(payload);
            if (scene.world.isNull() || scene.simulation.isNull())
                return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
            try
            {
                std::vector<std::byte> bytes;
                bytes.reserve(kWireSize);
                lux::serialization::BinaryWriter writer(bytes);
                if (!writer.writeUnsigned(SceneAssetPrimaryMagic) ||
                    !writer.writeUnsigned(kVersion) ||
                    !writer.writeBytes(scene.world.bytes()) ||
                    !writer.writeBytes(scene.simulation.bytes()))
                {
                    return lux::cxx::unexpected(lux::asset::EAssetCodecError::OUT_OF_MEMORY);
                }
                return bytes;
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(lux::asset::EAssetCodecError::OUT_OF_MEMORY);
            }
        }

        [[nodiscard]] lux::cxx::expected<lux::asset::DecodedAsset, lux::asset::EAssetCodecError> decode(
            std::span<const std::byte> input,
            const lux::asset::AssetDecodeContext& context
        ) noexcept
        {
            if (input.size() != kWireSize || input.size() > context.limits.max_input_bytes ||
                sizeof(SceneDescription) > context.limits.max_decoded_bytes)
            {
                return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
            }
            lux::serialization::BinaryReader reader(input);
            auto magic = reader.readUnsigned<std::uint32_t>();
            auto version = reader.readUnsigned<std::uint32_t>();
            std::array<std::uint8_t, 16U> world{};
            std::array<std::uint8_t, 16U> simulation{};
            if (!magic || !version || *magic != SceneAssetPrimaryMagic || *version != kVersion ||
                !reader.readBytes(std::as_writable_bytes(std::span(world))) ||
                !reader.readBytes(std::as_writable_bytes(std::span(simulation))) ||
                reader.remaining() != 0U)
            {
                return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
            }
            auto scene = std::make_shared<SceneDescription>(
                SceneDescription{lux::asset::AssetId(world), lux::asset::AssetId(simulation)}
            );
            if (scene->world.isNull() || scene->simulation.isNull())
                return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
            return lux::asset::DecodedAsset{std::move(scene), sizeof(SceneDescription)};
        }
    } // namespace

    lux::asset::AssetCodecDescriptor sceneAssetCodecDescriptor(std::shared_ptr<const void> code_lifetime)
    {
        return {
            lux::asset::AssetTypeId::fromName(SceneAssetCanonicalName),
            std::string(SceneAssetCanonicalName),
            SceneAssetPrimaryMagic,
            0U,
            lux::cxx::typeToken<SceneDescription>(),
            &decode,
            &encode,
            std::move(code_lifetime)
        };
    }
} // namespace lux::scene
