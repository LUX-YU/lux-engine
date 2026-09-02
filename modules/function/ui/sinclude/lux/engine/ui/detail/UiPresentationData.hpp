#pragma once

#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace lux::ui
{
    class UISession;
}

namespace lux::ui::detail
{
    struct UISessionPresentationAccess;

    struct UiFontAtlasSnapshot final
    {
        std::vector<std::uint8_t> pixels;
        int width{};
        int height{};
        void* context{};
    };

    class LUX_FUNCTION_PUBLIC UiDrawDataSnapshot final
    {
    public:
        UiDrawDataSnapshot();
        ~UiDrawDataSnapshot();
        UiDrawDataSnapshot(UiDrawDataSnapshot&&) noexcept;
        UiDrawDataSnapshot& operator=(UiDrawDataSnapshot&&) noexcept;
        UiDrawDataSnapshot(const UiDrawDataSnapshot&) = delete;
        UiDrawDataSnapshot& operator=(const UiDrawDataSnapshot&) = delete;

        [[nodiscard]] bool valid() const noexcept;

    private:
        friend struct UISessionPresentationAccess;
        friend class UiVulkanRenderer;
        void captureCurrent();
        [[nodiscard]] const void* nativeDrawData() const noexcept;

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    [[nodiscard]] LUX_FUNCTION_PUBLIC UiDrawDataSnapshot captureUiDrawData(UISession& session);
    [[nodiscard]] LUX_FUNCTION_PUBLIC UiFontAtlasSnapshot captureUiFontAtlas(UISession& session);
} // namespace lux::ui::detail
