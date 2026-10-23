#pragma once

#include <lux/engine/description/UiDraw.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>

namespace lux::ui
{
    namespace detail
    {
        class Context;
    } // namespace detail

    enum class ECaptureError : std::uint8_t
    {
        NONE,
        WRONG_THREAD,
        FRAME_OPEN,
        INPUT_PENDING,
        NO_FRAME,
        INVALID_INPUT,
        UNSUPPORTED_CALLBACK,
    };

    // Owns the finished draw lists. Texture tokens describe usage, not texture ownership.
    class LUX_FUNCTION_PUBLIC DrawData final
    {
    public:
        DrawData() noexcept;
        // Capture directly into a complete-frame arena. These trivial arrays live until the caller
        // resets that arena, even if DrawData itself leaves scope. No intermediate ImDrawList copy.
        void setFrameMemory(std::pmr::memory_resource&) noexcept;
        ~DrawData() noexcept;
        DrawData(DrawData&&) noexcept;
        DrawData& operator=(DrawData&&) noexcept;
        DrawData(const DrawData&) = delete;
        DrawData& operator=(const DrawData&) = delete;

        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] std::span<const rdesc::UiImageToken> textures() const noexcept;
        [[nodiscard]] const rdesc::UiDrawData& data() const noexcept;

    private:
        friend class detail::Context;
        [[nodiscard]] ECaptureError captureCurrent() noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::ui
