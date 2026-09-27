#pragma once

#include <lux/engine/function/visibility.h>
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>

#include <cstdint>
#include <memory>
#include <span>

namespace lux::ui
{
    namespace detail
    {
        class VulkanRenderer;
    }

    enum class ECaptureError : std::uint8_t
    {
        NONE,
        WRONG_THREAD,
        FRAME_OPEN,
        INPUT_PENDING,
        NO_FRAME,
        INVALID_INPUT,
        UNSUPPORTED_CALLBACK,
        ALLOCATION_FAILURE
    };

    // Owns the finished draw lists. Texture tokens describe usage, not texture ownership.
    class LUX_FUNCTION_PUBLIC DrawData final
    {
    public:
        DrawData() noexcept;
        ~DrawData() noexcept;
        DrawData(DrawData&&) noexcept;
        DrawData& operator=(DrawData&&) noexcept;
        DrawData(const DrawData&) = delete;
        DrawData& operator=(const DrawData&) = delete;

        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] std::span<const render::RTextureHandle> textures() const noexcept;

    private:
        friend class Root;
        friend class detail::VulkanRenderer;
        [[nodiscard]] ECaptureError captureCurrent() noexcept;
        [[nodiscard]] const void* nativeDrawData() const noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::ui
