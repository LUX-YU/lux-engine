#pragma once

#include <uuid.h>

namespace lux::ecs
{
    /// Stable Tilemap-domain content identity. It carries no World or
    /// residency semantics; scene-local storage is represented by TilemapHandle.
    class TilemapId final
    {
    public:
        TilemapId() = default;
        explicit TilemapId(uuids::uuid value) noexcept : value_(value) {}

        [[nodiscard]] const uuids::uuid& value() const noexcept
        {
            return value_;
        }
        [[nodiscard]] bool empty() const noexcept { return value_.is_nil(); }

        friend bool operator==(const TilemapId&, const TilemapId&) = default;

    private:
        uuids::uuid value_{};
    };
} // namespace lux::ecs
