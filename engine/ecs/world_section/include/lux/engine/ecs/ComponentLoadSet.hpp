#pragma once

#include <lux/engine/ecs/ComponentLoadBinding.hpp>
#include <lux/engine/ecs/ComponentSchemaSet.hpp>
#include <lux/engine/ecs/WorldSectionTypes.hpp>
#include <lux/engine/ecs/world_section/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <memory>
#include <span>
#include <string_view>

namespace lux::ecs
{
    class LUX_ENGINE_ECS_WORLD_SECTION_PUBLIC ComponentLoadSet final
    {
      public:
        ComponentLoadSet() noexcept = default;

        [[nodiscard]] static lux::cxx::expected<
            ComponentLoadSet,
            WorldSectionFailure>
        build(
            const ComponentSchemaSet& schemas,
            std::span<const ComponentLoadContribution> contributions
        ) noexcept;

        [[nodiscard]] const ComponentLoadBinding* find(
            std::uint64_t schema_hash,
            std::string_view schema_name
        ) const noexcept;

        [[nodiscard]] const ComponentLoadBinding* find(
            const ComponentSchemaId& schema
        ) const noexcept;

        [[nodiscard]] std::span<const ComponentLoadBinding> all() const noexcept;
        [[nodiscard]] bool empty() const noexcept;

      private:
        struct Impl;
        explicit ComponentLoadSet(std::shared_ptr<const Impl> impl) noexcept;

        std::shared_ptr<const Impl> impl_;
    };
} // namespace lux::ecs
