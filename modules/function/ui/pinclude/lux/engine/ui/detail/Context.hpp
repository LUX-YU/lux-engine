#pragma once

#include <array>
#include <imgui_internal.h>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/function_ref.hpp>
#include <lux/engine/ui/DrawData.hpp>
#include <lux/engine/ui/FontAtlas.hpp>
#include <lux/engine/ui/FontSource.hpp>
#include <lux/engine/ui/InputEvent.hpp>
#include <lux/engine/ui/Theme.hpp>
#include <memory>
#include <optional>
#include <vector>

namespace lux::ui
{
    struct RootConfig;
    struct FrameInfo;
    struct UpdateStatistics;
} // namespace lux::ui

namespace lux::ui::detail
{
    // Private ImGui owner. No object tree, window owner, focus target or renderer lives here.
    class Context final
    {
    public:
        using CreateResult = cxx::expected<std::unique_ptr<Context>, EInitError>;
        [[nodiscard]] static CreateResult create(const RootConfig&) noexcept;
        ~Context() noexcept;
        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;
        Context(Context&&) = delete;
        Context& operator=(Context&&) = delete;

        [[nodiscard]] ImGuiContext* native() const noexcept
        {
            return native_;
        }
        [[nodiscard]] const Theme& theme() const noexcept
        {
            return theme_;
        }
        [[nodiscard]] float scale() const noexcept
        {
            return scale_;
        }
        [[nodiscard]] cxx::expected<FontAtlas, EInitError> fontAtlas() const noexcept;
        void bindWindow(void*) noexcept;
        [[nodiscard]] cxx::expected<void, ECaptureError> beginFrame(FrameInfo) noexcept;
        [[nodiscard]] cxx::expected<void, ECaptureError> endFrame(DrawData&, UpdateStatistics&) noexcept;
        [[nodiscard]] cxx::expected<void, EInputError> feedInput(const VInputEvent&, std::uint64_t) noexcept;
        [[nodiscard]] bool closeInput() noexcept;
        [[nodiscard]] InputSnapshot inputSnapshot(bool composing, bool pointer_captured) const noexcept;
        [[nodiscard]] static EKey keyFromNative(ImGuiKey) noexcept;
        [[nodiscard]] bool hasInput() const noexcept
        {
            return input_pending_;
        }
        [[nodiscard]] bool takeFocusLoss() noexcept;
        [[nodiscard]] bool windowFocused() const noexcept
        {
            return window_focused_;
        }
        void setWindowFocused(bool value) noexcept
        {
            window_focused_ = value;
        }
        void cancelAdoptedInput() noexcept
        {
            cancelled_input_ = adopted_input_;
        }
        void
            consumeInput(cxx::function_ref<void(const ImGuiInputEvent&)>, cxx::function_ref<void(ECompositionStage)>) noexcept;

    private:
        Context() noexcept = default;
        struct InputRecord final
        {
            unsigned first{}, end{};
            std::uint64_t sequence{};
            std::optional<ECompositionStage> composition;
        };
        ImGuiContext* native_{};
        std::vector<std::uint8_t> font_bytes_;
        std::vector<ImWchar> font_ranges_;
        Theme theme_;
        float scale_{1.f};
        std::array<bool, 6> modifier_keys_{};
        int input_capacity_{};
        bool window_focused_{true}, input_pending_{};
        std::vector<InputRecord> input_records_;
        std::uint64_t accepted_input_{}, adopted_input_{}, focus_loss_{}, cancelled_input_{};
    };
} // namespace lux::ui::detail
