#pragma once
/**
 * @file EntitySceneIdentifiers.hpp
 * @brief Stable identities used by cooked EntityScene documents.
 */

#include <lux/cxx/algorithm/Sha256.hpp>
#include <lux/engine/core/extension_abi/StableId.hpp>

#include <uuid.h>

#include <compare>
#include <string>
#include <string_view>

namespace lux::entity_scene
{
    template <class Tag>
    class BasicUuid final
    {
    public:
        BasicUuid() = default;

        explicit BasicUuid(uuids::uuid value) noexcept
            : value_(value)
        {}

        [[nodiscard]] const uuids::uuid& value() const noexcept
        {
            return value_;
        }

        [[nodiscard]] bool empty() const noexcept
        {
            return value_.is_nil();
        }

        friend bool operator==(const BasicUuid&, const BasicUuid&) = default;

    private:
        uuids::uuid value_{};
    };

    struct EntitySceneIdTag final {};
    struct EntitySectionIdTag final {};
    struct PersistentEntityIdTag final {};

    using EntitySceneId = BasicUuid<EntitySceneIdTag>;
    using EntitySectionId = BasicUuid<EntitySectionIdTag>;
    using PersistentEntityId = BasicUuid<PersistentEntityIdTag>;

    /// Stable cross-Section entity reference. Unlike an EntityOrdinal this is
    /// not relocated to an entt::entity while a Section is staged: the target
    /// may be absent today and become resident later. Domain systems resolve
    /// it through PersistentEntityIndex only when they actually need it.
    struct PersistentEntityRef final
    {
        PersistentEntityId id;

        [[nodiscard]] bool valid() const noexcept
        {
            return !id.empty();
        }

        friend bool operator==(
            const PersistentEntityRef&,
            const PersistentEntityRef&) = default;
    };

    struct ContentTypeIdTag final {};
    struct DemandChannelIdTag final {};
    struct SectionGeneratorIdTag final {};
    struct ComponentSchemaIdTag final {};

    using ContentTypeId = lux::cxx::StableNameId<ContentTypeIdTag>;
    using DemandChannelId = lux::cxx::StableNameId<DemandChannelIdTag>;
    using SectionGeneratorId = lux::cxx::StableNameId<SectionGeneratorIdTag>;
    using ComponentSchemaId = lux::cxx::StableNameId<ComponentSchemaIdTag>;

    template <class Tag>
    [[nodiscard]] bool isValidEntitySceneId(
        const lux::cxx::StableNameId<Tag>& id) noexcept
    {
        return id.isValid() &&
            lux::extensions::isCanonicalStableName(id.name());
    }

    struct ContentBlobId final
    {
        lux::cxx::algorithm::Sha256Digest digest;

        [[nodiscard]] bool empty() const noexcept
        {
            return digest == lux::cxx::algorithm::Sha256Digest{};
        }

        friend bool operator==(const ContentBlobId&, const ContentBlobId&) =
            default;
        friend auto operator<=>(const ContentBlobId&, const ContentBlobId&) =
            default;
    };
}
