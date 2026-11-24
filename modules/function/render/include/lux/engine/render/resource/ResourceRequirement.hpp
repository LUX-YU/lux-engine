#pragma once

#include <lux/cxx/core/Schema.hpp>
#include <cstdint>
#include <string>

namespace lux::render
{
    struct ResourceNameTag;
    using ResourceName = cxx::SchemaId<ResourceNameTag>;
    using ResourceNameView = cxx::SchemaIdView<ResourceNameTag>;

    enum class EResourceDomain : std::uint8_t
    {
        DEVICE,
        SCENE,
        VIEW,
        JOB
    };

    enum class EResourceContentLifetime : std::uint8_t { PERSISTENT, PER_RUN };

    // Logical instance required by a capability. Several shader projections can refer to one binding.
    struct ResourceRequirement final
    {
        std::string_view binding;
        ResourceNameView type;
        EResourceDomain domain = EResourceDomain::DEVICE;
        EResourceContentLifetime lifetime = EResourceContentLifetime::PERSISTENT;
        // A default instance is addressed by its type within the scope. Named inputs of the same
        // type use independent instances keyed by their logical binding.
        bool scope_default = false;
        [[nodiscard]] bool isValid() const noexcept
        {
            return !binding.empty() && type.isValid() && domain <= EResourceDomain::JOB &&
                lifetime <= EResourceContentLifetime::PER_RUN;
        }

        bool operator==(const ResourceRequirement&) const noexcept = default;
    };
} // namespace lux::render
