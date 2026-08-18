#pragma once
/**
 * @file PersistentEntityId.hpp
 * @brief ECS identity that remains stable across registry unload/reload cycles.
 */

#include <uuid.h>

#include <utility>

namespace lux::ecs
{
    template <class Tag>
    class UuidId final
    {
    public:
        UuidId() = default;

        explicit UuidId(uuids::uuid value) noexcept
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

        friend bool operator==(const UuidId&, const UuidId&) = default;

    private:
        uuids::uuid value_{};
    };

    struct PersistentEntityIdTag final {};
    using PersistentEntityId = UuidId<PersistentEntityIdTag>;

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
} // namespace lux::ecs
