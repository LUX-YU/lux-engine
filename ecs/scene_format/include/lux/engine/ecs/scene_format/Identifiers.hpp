#pragma once
/**
 * @file Identifiers.hpp
 * @brief Stable identities used by the domain-neutral LXES scene format.
 */

#include <lux/cxx/algorithm/sha256.hpp>
#include <lux/engine/ecs/ComponentSchemaId.hpp>
#include <lux/engine/ecs/PersistentEntityId.hpp>
#include <lux/cxx/core/StableNameId.hpp>

#include <uuid.h>

#include <compare>
#include <string_view>

namespace lux::ecs::scene_format
{
    struct EntitySectionIdTag final {};
    using EntitySectionId = lux::ecs::UuidId<EntitySectionIdTag>;

    struct ContentTypeIdTag final {};

    using ContentTypeId = lux::cxx::StableNameId<ContentTypeIdTag>;

    [[nodiscard]] inline bool isCanonicalStableName(
        std::string_view name) noexcept
    {
        if (name.empty() || name.front() == '.' || name.back() == '.')
            return false;

        bool has_dot = false;
        bool previous_dot = false;
        for (const char value : name)
        {
            const bool dot = value == '.';
            if (dot)
            {
                if (previous_dot)
                    return false;
                has_dot = true;
            }
            else if (!((value >= 'a' && value <= 'z') ||
                       (value >= '0' && value <= '9') ||
                       value == '_' || value == '-'))
            {
                return false;
            }
            previous_dot = dot;
        }
        return has_dot;
    }

    template <class Tag>
    [[nodiscard]] bool isValidStableId(
        const lux::cxx::StableNameId<Tag>& id) noexcept
    {
        return id.isValid() && isCanonicalStableName(id.name());
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
} // namespace lux::ecs::scene_format
