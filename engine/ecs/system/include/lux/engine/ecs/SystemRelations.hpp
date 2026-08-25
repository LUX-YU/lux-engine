#pragma once

#include <lux/engine/ecs/SystemError.hpp>
#include <lux/engine/ecs/system/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/container/ScopeId.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lux::ecs
{
    struct SystemRelationsScopeTag;
    using SystemRelationsId = lux::cxx::ScopeId<SystemRelationsScopeTag>;

    namespace detail
    {
        struct SystemRelationsAccess;
    }

    /** Pure ordering data. Registry validity is intentionally checked at compile. */
    class LUX_ENGINE_ECS_SYSTEM_PUBLIC SystemRelations final
    {
    public:
        SystemRelations();
        ~SystemRelations();

        SystemRelations(SystemRelations&& other) noexcept;
        SystemRelations& operator=(SystemRelations&& other) noexcept;

        SystemRelations(const SystemRelations&) = delete;
        SystemRelations& operator=(const SystemRelations&) = delete;

        [[nodiscard]] lux::cxx::expected<void, SystemFailure> before(
            SystemId before,
            SystemId after
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<void, SystemFailure> after(
            SystemId after,
            SystemId before
        ) noexcept;

        [[nodiscard]] SystemRelationsId id() const noexcept;
        [[nodiscard]] std::size_t size() const noexcept;
        [[nodiscard]] std::uint64_t revision() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;

        friend struct detail::SystemRelationsAccess;
    };
}
