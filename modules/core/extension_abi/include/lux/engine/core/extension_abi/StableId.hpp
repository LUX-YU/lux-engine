#pragma once

#include <lux/cxx/core/StableNameId.hpp>

#include <cstdint>
#include <string_view>

namespace lux::extensions
{
    struct ExtensionIdTag final {};
    struct ContributionIdTag final {};

    using ExtensionIdView = lux::cxx::StableNameIdView<ExtensionIdTag>;
    using ContributionIdView = lux::cxx::StableNameIdView<ContributionIdTag>;
    using ExtensionId = lux::cxx::StableNameId<ExtensionIdTag>;
    using ContributionId = lux::cxx::StableNameId<ContributionIdTag>;

    template <class Tag>
    [[nodiscard]] inline bool sameStableId(
        lux::cxx::StableNameIdView<Tag> lhs,
        lux::cxx::StableNameIdView<Tag> rhs) noexcept
    {
        return lhs == rhs;
    }

    template <class Tag>
    [[nodiscard]] inline bool stableIdCollision(
        lux::cxx::StableNameIdView<Tag> lhs,
        lux::cxx::StableNameIdView<Tag> rhs) noexcept
    {
        return lhs.hash() == rhs.hash() && lhs.name() != rhs.name();
    }

    [[nodiscard]] constexpr ExtensionIdView extensionId(
        std::string_view name) noexcept
    {
        return ExtensionIdView{name};
    }

    [[nodiscard]] constexpr ContributionIdView contributionId(
        std::string_view name) noexcept
    {
        return ContributionIdView{name};
    }

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
}
