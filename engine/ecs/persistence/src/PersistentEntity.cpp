#include <lux/engine/ecs/PersistentEntity.hpp>

#include <algorithm>
#include <array>
#include <cstring>

namespace lux::ecs
{
    namespace
    {
        [[nodiscard]] auto uuidBytes(const PersistentEntityId& id) noexcept
        {
            return id.value.as_bytes();
        }

        [[nodiscard]] bool lessId(
            const PersistentEntityId& left,
            const PersistentEntityId& right
        ) noexcept
        {
            const auto lhs = uuidBytes(left);
            const auto rhs = uuidBytes(right);
            return std::lexicographical_compare(
                lhs.begin(), lhs.end(), rhs.begin(), rhs.end()
            );
        }

        class PersistentIdEncode final : public ComponentEncodePort
        {
          public:
            explicit PersistentIdEncode(ComponentEncodePort& target) : target_(&target) {}

            lux::cxx::expected<void, EComponentCodecError> write(
                std::string_view name,
                EComponentWireType type,
                std::span<const std::byte> bytes
            ) noexcept override
            {
                return target_->write(name, type, bytes);
            }

            lux::cxx::expected<void, EComponentCodecError> writeEntity(
                std::string_view name,
                Entity entity
            ) noexcept override
            {
                return target_->writeEntity(name, entity);
            }

            lux::cxx::expected<void, EComponentCodecError> writeStableReference(
                std::string_view name,
                std::span<const std::byte> stable_id
            ) noexcept override
            {
                return target_->writeStableReference(name, stable_id);
            }

          private:
            ComponentEncodePort* target_{};
        };

        lux::cxx::expected<void, EComponentCodecError> encodePersistentId(
            const ComponentSchema&,
            const World& world,
            Entity entity,
            ComponentEncodePort& port
        ) noexcept
        {
            const auto* value = world.find<PersistentId>(entity);
            if (value == nullptr)
                return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
            const auto bytes = value->value.value.as_bytes();
            return port.write("value", EComponentWireType::STABLE_REFERENCE, bytes);
        }

        lux::cxx::expected<void, EComponentCodecError> decodePersistentId(
            const ComponentSchema&,
            WorldEdit& edit,
            Entity entity,
            std::uint32_t version,
            ComponentDecodePort& port
        ) noexcept
        {
            if (version != 1)
                return lux::cxx::unexpected(EComponentCodecError::UNSUPPORTED_VERSION);
            EncodedPropertyView property;
            while (port.next(property))
            {
                if (property.name == "value" && property.bytes.size() == 16)
                {
                    std::array<std::uint8_t, 16> bytes{};
                    std::memcpy(bytes.data(), property.bytes.data(), bytes.size());
                    edit.emplace<PersistentId>(
                        entity,
                        PersistentEntityId{uuids::uuid(bytes)}
                    );
                    return {};
                }
            }
            return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
        }
    } // namespace

    lux::cxx::expected<PersistentEntityIndex, EPersistentEntityIndexError>
    PersistentEntityIndex::build(const World& world) noexcept
    {
        try
        {
            PersistentEntityIndex result;
            for (auto [entity, id] : world.view<const PersistentId>().each())
            {
                if (id.value.value.is_nil())
                    return lux::cxx::unexpected(EPersistentEntityIndexError::INVALID_ID);
                result.entries_.push_back({id.value, entity});
            }
            std::sort(
                result.entries_.begin(), result.entries_.end(),
                [](const auto& left, const auto& right)
                {
                    return lessId(left.first, right.first);
                }
            );
            for (std::size_t index = 1; index < result.entries_.size(); ++index)
            {
                if (result.entries_[index - 1].first == result.entries_[index].first)
                    return lux::cxx::unexpected(EPersistentEntityIndexError::DUPLICATE_ID);
            }
            return result;
        }
        catch (...)
        {
            return lux::cxx::unexpected(EPersistentEntityIndexError::ALLOCATION_FAILURE);
        }
    }

    Entity PersistentEntityIndex::find(PersistentEntityId id) const noexcept
    {
        const auto iterator = std::lower_bound(
            entries_.begin(), entries_.end(), id,
            [](const auto& entry, const PersistentEntityId& value)
            {
                return lessId(entry.first, value);
            }
        );
        return iterator != entries_.end() && iterator->first == id
            ? iterator->second
            : NullEntity;
    }

    std::size_t PersistentEntityIndex::size() const noexcept
    {
        return entries_.size();
    }

    ComponentSchema persistentIdComponentSchema()
    {
        return makeComponentSchema<PersistentId>(
            componentSchemaId("lux.ecs.PersistentId"),
            1,
            ComponentSnapshotMode::Copy,
            ComponentCodec{&encodePersistentId, &decodePersistentId}
        );
    }
} // namespace lux::ecs
