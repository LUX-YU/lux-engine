#include <lux/engine/ecs/TransformSchema.hpp>

#include <lux/engine/ecs/Transform.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <string_view>

namespace lux::ecs
{
    namespace
    {
        template <std::size_t Count>
        [[nodiscard]] std::array<std::byte, Count * sizeof(float)> encodeFloats(
            const std::array<float, Count>& values
        ) noexcept
        {
            std::array<std::byte, Count * sizeof(float)> bytes{};
            for (std::size_t value_index{}; value_index < Count; ++value_index)
            {
                auto bits = std::bit_cast<std::uint32_t>(values[value_index]);
                for (std::size_t byte_index{}; byte_index < sizeof(float); ++byte_index)
                {
                    bytes[value_index * sizeof(float) + byte_index] =
                        static_cast<std::byte>(bits & 0xffu);
                    bits >>= 8u;
                }
            }
            return bytes;
        }

        template <std::size_t Count>
        [[nodiscard]] bool decodeFloats(
            std::span<const std::byte> bytes,
            std::array<float, Count>& values
        ) noexcept
        {
            if (bytes.size() != Count * sizeof(float))
                return false;
            for (std::size_t value_index{}; value_index < Count; ++value_index)
            {
                std::uint32_t bits{};
                for (std::size_t byte_index{}; byte_index < sizeof(float); ++byte_index)
                {
                    bits |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(
                        bytes[value_index * sizeof(float) + byte_index]
                    )) << (byte_index * 8u);
                }
                values[value_index] = std::bit_cast<float>(bits);
                if (!std::isfinite(values[value_index]))
                    return false;
            }
            return true;
        }

        template <std::size_t Count>
        [[nodiscard]] lux::cxx::expected<void, EComponentCodecError> writeFloats(
            ComponentEncodePort& port,
            std::string_view name,
            const std::array<float, Count>& values
        ) noexcept
        {
            for (const float value : values)
            {
                if (!std::isfinite(value))
                    return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
            }
            const auto bytes = encodeFloats(values);
            return port.write(name, EComponentWireType::FLOATING_POINT, bytes);
        }

        lux::cxx::expected<void, EComponentCodecError> encodeTransform2D(
            const ComponentSchema&,
            const World& world,
            Entity entity,
            ComponentEncodePort& port
        ) noexcept
        {
            const auto& value = world.get<Transform2D>(entity);
            auto result = writeFloats(
                port,
                "translation",
                std::array{value.translation.x(), value.translation.y()}
            );
            if (!result)
                return result;
            result = writeFloats(port, "rotation", std::array{value.rotation});
            if (!result)
                return result;
            return writeFloats(
                port,
                "scale",
                std::array{value.scale.x(), value.scale.y()}
            );
        }

        lux::cxx::expected<void, EComponentCodecError> decodeTransform2D(
            const ComponentSchema&,
            WorldEdit& edit,
            Entity entity,
            std::uint32_t version,
            ComponentDecodePort& port
        ) noexcept
        {
            if (version != 1u)
                return lux::cxx::unexpected(EComponentCodecError::UNSUPPORTED_VERSION);
            Transform2D value;
            EncodedPropertyView property;
            while (port.next(property))
            {
                if (property.type != EComponentWireType::FLOATING_POINT)
                    return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                if (property.name == "translation")
                {
                    std::array<float, 2> fields{};
                    if (!decodeFloats(property.bytes, fields))
                        return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                    value.translation = {fields[0], fields[1]};
                }
                else if (property.name == "rotation")
                {
                    std::array<float, 1> fields{};
                    if (!decodeFloats(property.bytes, fields))
                        return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                    value.rotation = fields[0];
                }
                else if (property.name == "scale")
                {
                    std::array<float, 2> fields{};
                    if (!decodeFloats(property.bytes, fields))
                        return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                    value.scale = {fields[0], fields[1]};
                }
            }
            edit.emplace<Transform2D>(entity, value);
            return {};
        }

        lux::cxx::expected<void, EComponentCodecError> encodeTransform3D(
            const ComponentSchema&,
            const World& world,
            Entity entity,
            ComponentEncodePort& port
        ) noexcept
        {
            const auto& value = world.get<Transform3D>(entity);
            auto result = writeFloats(
                port,
                "translation",
                std::array{
                    value.translation.x(),
                    value.translation.y(),
                    value.translation.z(),
                }
            );
            if (!result)
                return result;
            result = writeFloats(
                port,
                "rotation",
                std::array{
                    value.rotation.x(),
                    value.rotation.y(),
                    value.rotation.z(),
                    value.rotation.w(),
                }
            );
            if (!result)
                return result;
            return writeFloats(
                port,
                "scale",
                std::array{value.scale.x(), value.scale.y(), value.scale.z()}
            );
        }

        lux::cxx::expected<void, EComponentCodecError> decodeTransform3D(
            const ComponentSchema&,
            WorldEdit& edit,
            Entity entity,
            std::uint32_t version,
            ComponentDecodePort& port
        ) noexcept
        {
            if (version != 1u)
                return lux::cxx::unexpected(EComponentCodecError::UNSUPPORTED_VERSION);
            Transform3D value;
            EncodedPropertyView property;
            while (port.next(property))
            {
                if (property.type != EComponentWireType::FLOATING_POINT)
                    return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                if (property.name == "translation")
                {
                    std::array<float, 3> fields{};
                    if (!decodeFloats(property.bytes, fields))
                        return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                    value.translation = {fields[0], fields[1], fields[2]};
                }
                else if (property.name == "rotation")
                {
                    std::array<float, 4> fields{};
                    if (!decodeFloats(property.bytes, fields))
                        return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                    value.rotation = Eigen::Quaternionf{
                        fields[3], fields[0], fields[1], fields[2]};
                    if (value.rotation.squaredNorm() <=
                        std::numeric_limits<float>::epsilon())
                    {
                        return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                    }
                    value.rotation.normalize();
                }
                else if (property.name == "scale")
                {
                    std::array<float, 3> fields{};
                    if (!decodeFloats(property.bytes, fields))
                        return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                    value.scale = {fields[0], fields[1], fields[2]};
                }
            }
            edit.emplace<Transform3D>(entity, value);
            return {};
        }
    } // namespace

    std::span<const ComponentSchema> transformComponentSchemas() noexcept
    {
        static const std::array schemas{
            makeComponentSchema<Transform2D>(
                componentSchemaId("lux.ecs.Transform2D"),
                1,
                EComponentSnapshotPolicy::COPY,
                ComponentCodec{&encodeTransform2D, &decodeTransform2D}
            ),
            makeComponentSchema<WorldTransform2D>(
                componentSchemaId("lux.ecs.WorldTransform2D"),
                1,
                EComponentSnapshotPolicy::REBUILD
            ),
            makeComponentSchema<Transform3D>(
                componentSchemaId("lux.ecs.Transform3D"),
                1,
                EComponentSnapshotPolicy::COPY,
                ComponentCodec{&encodeTransform3D, &decodeTransform3D}
            ),
            makeComponentSchema<WorldTransform3D>(
                componentSchemaId("lux.ecs.WorldTransform3D"),
                1,
                EComponentSnapshotPolicy::REBUILD
            ),
        };
        return schemas;
    }
} // namespace lux::ecs
