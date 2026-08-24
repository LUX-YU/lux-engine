#pragma once

#include <uuid.h>

namespace lux::ecs
{
    /// Stable Pixel-domain content identity. It carries no World or residency
    /// semantics; scene-local storage is represented by PixelFieldHandle.
    class PixelFieldId final
    {
    public:
        PixelFieldId() = default;
        explicit PixelFieldId(uuids::uuid value) noexcept : value_(value) {}

        [[nodiscard]] const uuids::uuid& value() const noexcept
        {
            return value_;
        }
        [[nodiscard]] bool empty() const noexcept { return value_.is_nil(); }

        friend bool operator==(const PixelFieldId&, const PixelFieldId&) =
            default;

    private:
        uuids::uuid value_{};
    };
} // namespace lux::ecs
