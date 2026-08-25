#pragma once

#include <lux/engine/ecs/ComponentSchemaSet.hpp>
#include <lux/engine/ecs/ComponentSnapshotBinding.hpp>
#include <lux/engine/ecs/SnapshotTypes.hpp>
#include <lux/engine/ecs/snapshot/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <memory>
#include <span>

namespace lux::ecs
{
    namespace detail
    {
        struct ComponentSnapshotSetAccess;
    }

    class LUX_ENGINE_ECS_SNAPSHOT_PUBLIC ComponentSnapshotSet final
    {
      public:
        ComponentSnapshotSet() noexcept = default;

        [[nodiscard]] static lux::cxx::expected<
            ComponentSnapshotSet,
            SnapshotError>
        build(
            const ComponentSchemaSet& schemas,
            std::span<const ComponentSnapshotContribution> contributions
        ) noexcept;

        [[nodiscard]] std::span<const ComponentSnapshotBinding>
        all() const noexcept;

        [[nodiscard]] bool empty() const noexcept;

      private:
        struct Impl;
        explicit ComponentSnapshotSet(std::shared_ptr<const Impl> impl) noexcept;

        std::shared_ptr<const Impl> impl_;

        friend struct detail::ComponentSnapshotSetAccess;
    };
} // namespace lux::ecs
