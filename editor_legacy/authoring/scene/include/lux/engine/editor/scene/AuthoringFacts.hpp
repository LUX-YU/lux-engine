#pragma once
#include <lux/engine/editor/sessions/ContentStamp.hpp>
#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>
#include <lux/engine/world/WorldDescription.hpp>

namespace lux::editor::scene
{
    enum class EApplicability : std::uint8_t { SUPPORTED, NOT_APPLICABLE, TEMPORARILY_UNAVAILABLE };
    enum class EApplicabilityReason : std::uint8_t
    {
        NONE, UNDECLARED_SCHEMA, MISSING_PROVIDER, NON_AUTHOR_VALUE, NO_DEFAULT_VALUE,
        PARTITION_MISMATCH, INDEX_REBUILD_REQUIRED, ADMISSION
    };
    // A synchronous observation, never a cache or another World owner. Capture inside the Session
    // read scope. Installed registrations alone do not declare persistent author data.
    struct AuthoringFacts final
    {
        sessions::ContentStamp based_on;
        std::span<const world::WorldDataSchemaId> schemas;
        const simulation::ecs::ComponentSchemaSet& registrations;
        std::string_view partition;
        std::uint32_t partition_version{};
        bool indexed{};
        bool available{true};
    };
    struct ApplicabilityRequirements final
    {
        std::span<const std::string_view> schemas;
        bool default_values{};
        bool edit_content{};
        std::string_view partition;
        std::uint32_t partition_version{};
    };
    struct ApplicabilityResult final
    {
        EApplicability status{EApplicability::SUPPORTED};
        EApplicabilityReason reason{EApplicabilityReason::NONE};
        sessions::ContentStamp based_on;
        std::string subject;
        [[nodiscard]] bool supported() const noexcept { return status == EApplicability::SUPPORTED; }
    };
    [[nodiscard]] bool isAuthorComponent(const simulation::ecs::ComponentSchema&) noexcept;
    [[nodiscard]] bool declaresAuthorSchema(std::span<const world::WorldDataSchemaId>, std::string_view) noexcept;
    [[nodiscard]] AuthoringFacts authoringFacts(
        const world::WorldDescription&, const simulation::ecs::ComponentSchemaSet&,
        sessions::ContentStamp = {}, bool available = true
    ) noexcept;
    [[nodiscard]] ApplicabilityResult queryApplicability(const AuthoringFacts&, const ApplicabilityRequirements&);
}
