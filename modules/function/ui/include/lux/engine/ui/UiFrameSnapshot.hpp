#pragma once

#include <lux/engine/function/visibility.h>
#include <lux/engine/ui/TextureHandle.hpp>

#include <cstdint>
#include <memory>
#include <span>

namespace lux::ui
{
namespace detail
{
class UiVulkanRenderer;
}

enum class EUiCaptureError : std::uint8_t
{
    NONE,
    WRONG_THREAD,
    FRAME_OPEN,
    NO_FRAME,
    UNSUPPORTED_CALLBACK,
    ALLOCATION_FAILURE
};

// Owns the finished draw lists. Texture tokens describe usage, not texture ownership.
class LUX_FUNCTION_PUBLIC UiFrameSnapshot final
{
  public:
    UiFrameSnapshot() noexcept;
    ~UiFrameSnapshot() noexcept;
    UiFrameSnapshot(UiFrameSnapshot &&) noexcept;
    UiFrameSnapshot &operator=(UiFrameSnapshot &&) noexcept;
    UiFrameSnapshot(const UiFrameSnapshot &) = delete;
    UiFrameSnapshot &operator=(const UiFrameSnapshot &) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::span<const TextureHandle> textures() const noexcept;

  private:
    friend class Context;
    friend class detail::UiVulkanRenderer;
    [[nodiscard]] EUiCaptureError captureCurrent();
    [[nodiscard]] const void *nativeDrawData() const noexcept;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace lux::ui
